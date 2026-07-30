# ElBussTransTherm Fleet Backend (Central Server)

[Return to main](../README.md)

Central hub for the ElBussTransTherm project (KTH PhD research on thermal
comfort and energy consumption in electric urban buses). While each bus has
its own Raspberry Pi doing local sensor aggregation and storage, this stack
runs on **one separate Linux machine** and is where data from *every* bus in
the fleet lands, gets stored long-term, and gets served through both a
web dashboard and a REST API, with per-user, per-bus access control.

This directory is meant to be **copied onto a fresh Linux host** (tested on
Ubuntu 26.04 / Debian-family, x86_64) to stand the whole backend up with a
handful of commands. It encodes a number of gotchas that were only found by
actually deploying and load-testing this stack — see
[Problems already solved](#problems-already-solved-kept-here-for-reference)
before you go rediscover them.

## Architecture

```
                         LTE, per-bus TLS + MQTT auth
Bus 1 Raspberry Pi  ──┐
  (forwarder)         │
Bus 2 Raspberry Pi  ──┼──►  [mosquitto :8883, TLS + dynamic-security]
  (forwarder)         │              │
Bus N Raspberry Pi  ──┘              │ fleet/{vehicle_id}/unified/combined
                                      ▼
                              [ingestor container]
                                      │ INSERT
                                      ▼
                          [timescaledb: fleetdata]
                                      ▲
                                      │ SQL (scoped per user/bus)
                              [api container : FastAPI]
                                    ▲   ▲
                        cookie session   Authorization: Bearer
                                    │   │
                          Browser (web dashboard)   Swagger / scripts
```

- **mosquitto** — the only thing bus Raspberry Pis talk to over the public
  network (LTE). TLS on port 8883, one MQTT username/password per bus,
  each restricted by ACL to publishing only under its own
  `fleet/<vehicle_id>/#`. No VPN daemon — TLS alone was the deliberate
  choice here (see the project's own bus-facing stack, where the
  ESP32-to-Pi link stays plaintext because it's a physically isolated LAN;
  this Pi-to-central link crosses the public internet over LTE, so it gets
  TLS).
- **timescaledb** — database `fleetdata`: `buses`, `app_users`
  (now with `password_changed_at`, used to invalidate old sessions the
  moment a password changes), `user_bus_assignments`, and hypertable
  `fleet_events`.
- **ingestor** — subscribes to `fleet/+/unified/combined`, extracts the
  vehicle_id from the *topic* (not the payload body — the topic is
  ACL-enforced, the payload isn't), and inserts into `fleet_events`.
  Silently drops (and logs) anything from a vehicle_id that isn't already
  a registered, active bus.
- **api** — FastAPI + Uvicorn, serving both a REST API and a server-rendered
  web dashboard from the same process (no separate frontend build, no
  CORS). `admin`/`user` roles, bus CRUD, user CRUD, bus↔user assignment,
  password management, a `GET /events` endpoint scoped to the caller's
  assigned buses (or everything, for admins), and CSV export.

## Web dashboard

Open `http://<this-host>:8000/login` in a browser. Pages:

- **`/dashboard`** — the events table, filterable by bus and date range.
  Every payload field is flattened into its own column (e.g.
  `bus_environment.wp21i.wp211_front.temperature_c`) rather than shown as
  raw JSON — the column set adjusts to whatever fields are present in the
  filtered rows, so the table scrolls horizontally when there are many. A
  **Download CSV** button exports the current filter (bus + date range) as
  a file, with the same flattened columns; a loading overlay covers the
  page while the table refreshes or a file is being generated, so it's
  never ambiguous whether something is still working.
- **`/account`** — every logged-in user (`admin` or `user`) can change
  their *own* password here (their current password is required).
- **`/admin`** — **`admin` role only**: create/delete users, reset *any*
  user's password directly (no old password needed), create/delete buses,
  and assign/unassign buses to users. A regular `user` never sees this
  page, and hitting its endpoints directly (not just the hidden UI) also
  gets `403` — checked, not just assumed.

Both roles: an `admin` is a "superadmin" in the sense the project asked
for (can manage every user and bus); there is no separate third tier.

**Branding placeholders**: `app/templates/base.html` reserves a logo slot
in the top navigation bar for the KTH logo, and `app/templates/login.html`
reserves one above the login form for the project's own logo. Both are
empty by design (`onerror` hides the broken-image icon if the file isn't
there yet) — drop the real files in as
`api/app/static/kth-logo.svg` and `api/app/static/project-logo.svg`
(rebuild with `docker compose up -d --build api` afterward; static assets
are baked into the image, not bind-mounted) and they'll appear with no
other changes. Colors already match KTH's official palette (intra.kth.se
graphic profile): KTH Blue `#004791`, Navy `#000061`, Sky Blue `#6298D2`,
Light Blue `#DEF0FF`, Sand `#EBE5E0`.

**Auth**: the dashboard uses an `HttpOnly`, `SameSite=Lax` session cookie
(2h, `WEB_SESSION_EXPIRE_MINUTES`) set on `/login`, separate from the
short-lived (20 min) bearer token Swagger/API clients get from
`/auth/token` — both are accepted by every endpoint, so scripts and the
UI can be used interchangeably without stepping on each other. State-
changing requests (create/delete user or bus, password changes) are
checked against `Origin`/`Referer` when cookie-authenticated (CSRF
defense) — this check is skipped entirely for `Authorization: Bearer`
requests, so it never affects Swagger or scripts.

## What each bus's Raspberry Pi needs (not part of this repo)

Each bus keeps running its own existing local stack (Mosquitto + aggregator
+ local TimescaleDB) unchanged. The only addition per bus is a `forwarder`
container in *that Pi's own* `docker-compose.yml`, which:

- polls that Pi's local `wp2_records` table for rows where
  `forwarded = false`,
- publishes each one to this central broker under
  `fleet/{VEHICLE_ID}/unified/combined`, and
- only marks a row `forwarded = true` once the central broker has
  acknowledged the publish (QoS 1).

This makes the **local Postgres row itself the durable queue** — an LTE
outage of any length just means the backlog gets sent once connectivity
returns; nothing is lost as long as local disk isn't. (An earlier version
of the forwarder just bridged the local MQTT topic to the central one
live — that approach was tested against a real broker outage and lost
every message published while disconnected. Don't go back to it; see
below.)

## Prerequisites

- A Debian-family Linux host, reachable from the buses (LTE-routable IP,
  or a domain name once you have one).
- Docker Engine + Compose plugin:

  ```bash
  sudo apt-get update && sudo apt-get install -y docker.io docker-compose-plugin

  # If those aren't available yet for your OS release (this happened on a
  # very new Ubuntu release before Docker's own repo had packages for its
  # codename — see "Problems already solved" below), install from Docker's
  # official repo instead, pinning to the codename Docker actually
  # supports (e.g. "noble" for a too-new Ubuntu):
  sudo install -m 0755 -d /etc/apt/keyrings
  curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
  sudo chmod a+r /etc/apt/keyrings/docker.gpg
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu noble stable" \
    | sudo tee /etc/apt/sources.list.d/docker.list
  sudo apt-get update
  sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

  sudo systemctl enable --now docker
  sudo usermod -aG docker "$USER"   # log out/in for this to take effect
  ```

## Quick start (fresh machine)

### 1. Copy this directory over and generate the TLS certificate

```bash
scp -r fleet-central-stack/ user@central-host:~/
cd ~/fleet-central-stack
```

Generate a private CA and a server certificate for Mosquitto. **The
server cert's SAN must include every hostname any client will use to
reach it** — that means both the Docker Compose service name (`mosquitto`,
used by the internal `ingestor`/`api` containers) *and* whatever address
the buses will actually dial (a LAN IP for testing, a public IP or domain
once you have one):

```bash
mkdir -p mosquitto/certs && cd mosquitto/certs

openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
  -subj "/O=ElBussTransTherm/CN=ElBussTransTherm Fleet CA" -out ca.crt

openssl genrsa -out server.key 2048
openssl req -new -key server.key -subj "/O=ElBussTransTherm/CN=fleet-central" -out server.csr

cat > server_ext.cnf <<EOF
subjectAltName = DNS:fleet-central, DNS:mosquitto, DNS:localhost, IP:203.0.113.10, IP:127.0.0.1
extendedKeyUsage = serverAuth
EOF
# ^ replace 203.0.113.10 with this host's real reachable IP (or add a DNS: entry once you have a domain)

openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 825 -sha256 -extfile server_ext.cnf
rm server.csr server_ext.cnf

# Mosquitto in the container runs as uid/gid 1883, not root -- the config
# and cert files must be readable (and, for dynamic-security.json,
# writable) by that exact uid or the broker fails to start.
cd ..
sudo chown -R 1883:1883 certs config 2>/dev/null || true
```

You'll need `mosquitto/certs/ca.crt` again later, on every bus's Pi (see
[Onboarding a new bus](#onboarding-a-new-bus)).

### 2. Bootstrap the dynamic-security config file

The dynamic-security plugin needs an initial admin identity created
*before* the broker first starts with it loaded:

```bash
mkdir -p mosquitto/config
DYNSEC_ADMIN_PASSWORD=$(openssl rand -base64 24 | tr -d '=+/\n' | cut -c1-24)
sudo docker run --rm -v "$PWD/mosquitto/config:/mosquitto/config" eclipse-mosquitto:2 \
  mosquitto_ctrl dynsec init /mosquitto/config/dynamic-security.json dynsec-admin "$DYNSEC_ADMIN_PASSWORD"
sudo chown 1883:1883 mosquitto/config/dynamic-security.json
echo "Save this for your .env's DYNSEC_ADMIN_PASSWORD: $DYNSEC_ADMIN_PASSWORD"
```

This is the *only* thing `mosquitto_ctrl` is used for in this whole stack
— everything else (creating a bus's MQTT client, its role, its ACL) is
done by the API itself talking the dynamic-security MQTT control protocol
directly. See [Problems already solved](#problems-already-solved-kept-here-for-reference)
for why.

### 3. Configure `.env`

```bash
cp .env.example .env
nano .env
```

Fill in: `DB_PASSWORD`, `POSTGRES_SUPERUSER_PASSWORD`, `JWT_SECRET` (a long
random string), `BOOTSTRAP_ADMIN_PASSWORD` (used once, to create the first
admin account if none exists yet), `DYNSEC_ADMIN_PASSWORD` (from step 2),
and `MQTT_INGESTOR_PASSWORD` (any strong password — the `ingestor` MQTT
identity itself is created automatically on first API startup).

Also present, with sane defaults you usually don't need to touch:
`WEB_SESSION_EXPIRE_MINUTES` (dashboard cookie lifetime, default 120) and
`COOKIE_SECURE` (default `false` — **leave this `false` until this API sits
behind TLS**; a `Secure` cookie is silently never sent by the browser over
plain HTTP, which breaks dashboard login with no visible error).

### 4. Bring it up

```bash
sudo docker compose up -d --build
sudo docker compose ps           # all four services should be healthy/running
sudo docker compose logs api     # look for "Dynamic-security roles bootstrapped."
```

On first startup the API:
- creates the bootstrap admin user (only if no admin exists yet),
- creates the `bus-publisher` and `fleet-subscriber` dynamic-security
  roles and their ACLs, and
- creates the `ingestor` MQTT client and assigns it the subscriber role.

All of this is idempotent — safe to see happen again on every restart.

Log in at `http://<this-host>:8000/login` with `BOOTSTRAP_ADMIN_USERNAME`
/ `BOOTSTRAP_ADMIN_PASSWORD` and change that password from `/account` right
away — it's the same value that's sitting in your `.env`.

## Managing the fleet

Everything below can be done either from `/admin` in the browser, or via
the API directly (useful for scripting bulk bus/user setup). The curl
examples show the API path.

Log in as admin and grab a token:

```bash
TOKEN=$(curl -s -X POST http://localhost:8000/auth/token \
  -d "username=admin&password=$BOOTSTRAP_ADMIN_PASSWORD" \
  -H "Content-Type: application/x-www-form-urlencoded" \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['access_token'])")
```

**Register a bus** (this also creates its MQTT credentials via
dynamic-security in the same request):

```bash
curl -s -X POST http://localhost:8000/buses -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"vehicle_id": "BUS_2034", "display_name": "Bus 2034 test line"}'
# => {"id":1,"vehicle_id":"BUS_2034",...,"mqtt_username":"BUS_2034","mqtt_password":"..."}
```

**Create a researcher account** and give them visibility into just one
bus:

```bash
curl -s -X POST http://localhost:8000/users -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"username": "researcher1", "password": "...", "role": "user"}'
# => {"id": 2, ...}

curl -s -X POST http://localhost:8000/buses/1/assignments/2 -H "Authorization: Bearer $TOKEN"
```

**Query events** (as that researcher, once they've logged in and gotten
their own token):

```bash
curl -s "http://localhost:8000/events?bus_id=1&limit=50" -H "Authorization: Bearer $USER_TOKEN"
```

`from`/`to` (ISO 8601) narrow the time range; default is the last 24h.
Asking for a bus not assigned to you returns `403`.

**Export events as CSV** (same scoping/filters as above):

```bash
curl -s "http://localhost:8000/events/export?bus_id=1&format=csv" \
  -H "Authorization: Bearer $USER_TOKEN" -o events.csv
```

`format=xlsx` also works (returns a real `.xlsx` file) — it's just not
wired to a button in the dashboard UI today, only reachable via the API
directly. Exports are capped at `EVENTS_EXPORT_MAX_LIMIT` rows (default
20,000) to keep memory use bounded.

**Password management**:
- Any user changes their own password from `/account`, or
  `PATCH /users/me/password` with `{current_password, new_password}`.
- An admin resets *anyone's* password from `/admin`, or
  `PUT /admin/users/{id}/password` with `{new_password}` (no old password
  needed).
- Either action immediately invalidates that user's existing sessions —
  a token/cookie issued before the change stops working, so a compromised
  account can actually be locked out, not just have its password quietly
  changed while an attacker's session stays live.

Removing a bus (`DELETE /buses/{id}`) also deletes its MQTT credentials
via dynamic-security in the same call — that bus's Pi will start failing
to authenticate immediately.

## Onboarding a new bus

1. `POST /buses` (or the "Create bus" form on `/admin`) — note the
   returned `mqtt_username` / `mqtt_password`.
2. Copy `mosquitto/certs/ca.crt` from this machine to that bus's Pi, e.g.
   into `~/wp2-pi-stack-docker/forwarder/certs/ca.crt`.
3. On that Pi, add to `~/wp2-pi-stack-docker/.env`:
   ```
   CENTRAL_MQTT_HOST=<this machine's reachable address>
   CENTRAL_MQTT_PORT=8883
   CENTRAL_MQTT_USERNAME=<the vehicle_id, e.g. BUS_2034>
   CENTRAL_MQTT_PASSWORD=<the mqtt_password from step 1>
   ```
4. Make sure that Pi's `docker-compose.yml` has the `forwarder` service
   (polls local `wp2_records`, publishes centrally — see
   [Architecture](#architecture) above) and run
   `docker compose up -d --build forwarder` there.
5. Verify: `sudo docker compose logs forwarder` on the Pi should show
   `[CENTRAL] Connected`, and this machine's
   `SELECT * FROM fleet_events WHERE vehicle_id='BUS_2034' ORDER BY time DESC LIMIT 5;`
   should start showing rows within a couple of seconds. Or just check
   `/dashboard` — the new bus will appear in the filter dropdown for
   whoever it's assigned to.

## Problems already solved (kept here for reference)

- **`mosquitto_ctrl` isn't available as a library dependency, only as a
  CLI, and not even reliably as that.** Neither Debian's nor Alpine's
  `mosquitto-clients` package includes it (both ship only
  `mosquitto_pub`/`sub`/`rr`) — it exists only in the official Eclipse
  Mosquitto Docker image, which is Alpine/musl-based and can't be copied
  into a glibc-based image (like this API's `python:3.13-slim`) and just
  run. The fix here: the API talks the dynamic-security **MQTT control
  protocol directly** (`app/dynsec_client.py`) — publish
  `{"commands":[{...}]}` to `$CONTROL/dynamic-security/v1`, read the
  matching entry back from `$CONTROL/dynamic-security/v1/response`'s
  `"responses"` array, an `"error"` key means failure. This was verified
  command-by-command against a live broker before being relied on, not
  assumed from documentation alone. `mosquitto_ctrl` is used exactly once
  in this whole stack: the one-time `dynsec init` bootstrap in step 2
  above, which only touches the config file directly and needs no running
  broker.
- **TLS cert SAN must include the Docker Compose service name, not just
  the host's real address.** The `ingestor` and `api` containers connect
  to the broker as `mosquitto` (the Compose service name, resolved via
  Docker's internal DNS) — if the server cert's SAN only lists the host's
  LAN/public IP, those *internal* connections fail hostname verification
  even though external bus connections (which use the real IP) work fine.
  Include both.
- **A pure MQTT bridge silently loses data during an outage.** The first
  version of the Pi-side forwarder subscribed to the local topic and
  republished live to the central broker. Tested against a real 10-second
  central-broker outage, it produced an exact 10-second gap in the central
  table — `paho-mqtt`'s `publish()` returns `MQTT_ERR_NO_CONN` and drops
  the message when disconnected; it does not queue outgoing publishes.
  `clean_session=False` doesn't help here either — durable sessions
  protect a *subscriber* that goes offline, not a *publisher*. The fix:
  the forwarder polls the bus's own local Postgres for
  `forwarded = false` rows and only flips that flag after the central
  broker acks the publish, making the already-durable local database row
  the queue. Re-tested against a 12-second outage afterward: zero rows
  lost, backlog delivered in one batch on reconnect.
- **`dynamic-security.json` and the cert files must be owned by uid/gid
  1883, not root.** The mosquitto container process runs as a
  non-root `mosquitto` user (uid 1883) even though `docker run` defaults
  to root for arbitrary commands in the image — bind-mounted files the
  broker needs to read (certs) or read *and write* (the dynsec state
  file) must match that uid or the broker fails silently/refuses to start.
- **A brand-new OS release may be too new for Docker's own apt repo.**
  If `docker-ce`/`docker-compose-plugin` aren't available for your distro
  codename yet, add Docker's repo pinned to the newest codename it does
  support (e.g. `noble` instead of a newer, unreleased-in-Docker's-repo
  codename) — Docker Engine itself doesn't care that the codename in the
  repo URL doesn't match the host's actual OS release.
- **A page route and a JSON API route can't share the exact same path.**
  The events *page* was originally also mapped to `GET /events`, the same
  path as the JSON API (which requires a `bus_id` query param) — since
  Starlette matches routes in registration order, the JSON route (added
  first) always won, so a plain browser visit to `/events` returned `422`
  instead of the page. Fixed by moving the page to `/dashboard` and
  leaving `/events` as the (unchanged) JSON API.
- **A `Secure` cookie on a plain-HTTP API breaks login with no error at
  all.** The session cookie was originally hardcoded `Secure=True`
  (correct once TLS is in front of this API) — but this API is plain HTTP
  today, and a `Secure` cookie is simply never sent back by the browser
  over HTTP, so login appeared to "succeed" (200/303) while nothing after
  it worked. Made configurable via `COOKIE_SECURE` (default `false`);
  flip it once a reverse proxy terminates TLS here.

## Resetting everything (destructive)

```bash
sudo docker compose down -v   # deletes all volumes: DB contents, mosquitto persistence
```

The dynamic-security state (`mosquitto/config/dynamic-security.json`) and
the TLS certs live on the host filesystem, not in a volume — delete them
manually too if you want a truly from-scratch reset, and re-run steps 1–2
above.

## Auto-start on boot

Every service has `restart: unless-stopped` and the Docker daemon itself
starts on boot once enabled (`sudo systemctl enable docker`) — no separate
systemd unit is needed for the stack to survive a reboot or power loss.
