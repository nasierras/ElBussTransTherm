# WP2.5.i Aggregator Stack (Dockerized)

Central hub software stack for the ElBussTransTherm project (KTH PhD research on
thermal comfort and energy consumption in electric urban buses). This Raspberry Pi
sits at the center of one bus: ESP32-S3 sensor nodes (WP2.1.i-WP2.6.i, WP2.TM)
publish sensor readings over MQTT, a PiCAN3 HAT provides real CAN-bus data, this
stack aggregates everything once per second into a single JSON record per
vehicle/trip, stores it in a local time-series database, and forwards it to the
central fleet broker over LTE.

This README is written as an **ordered checklist**. Follow it top to bottom on a
freshly imaged Raspberry Pi — do not skip ahead to Docker before Step 1 is done
and verified, since a flaky CAN-HAT is much easier to diagnose in isolation than
underneath three running containers.

## Architecture

```
ESP32 sensor nodes --MQTT (bus/env/#)--> [mosquitto container] --> [aggregator container] --INSERT--> [timescaledb container]
                                                ^                        ^                                    |
                                    [emulator container]     PiCAN3 (SPI, can0)                                |
                                    (dev/test only,                                                            v
                                     bus/env/# + bus/test/#)                                  [forwarder container] --TLS/LTE--> Central fleet broker (AWS/KTH)
```

- **mosquitto** — local MQTT broker, port 1883, no auth (trusted LAN only — the
  ESP32 nodes and this Pi share a private wired network, see Step 3).
- **timescaledb** — PostgreSQL 16 + TimescaleDB, database `wp2data`, hypertable
  `wp2_records`.
- **aggregator** — subscribes to `bus/env/#` and `bus/test/#`, reads live CAN
  data from `can0` (see Step 1), merges everything into one combined record
  per second, inserts it into `wp2_records`, and republishes it locally on
  `bus/unified/combined`.
- **forwarder** — reads rows from `wp2_records` that haven't been sent yet and
  relays them to the **central** fleet broker over TLS, one bus at a time,
  under topic `fleet/<VEHICLE_ID>/unified/combined`. Durable against LTE
  outages: a row is only marked as sent after the central broker acknowledges
  it, so a dead-zone just delays delivery instead of losing data.
- **emulator** — dev/test-only web tool (port 8090), see "Sensor emulator"
  below. Not part of the real data path; publishes to the same local broker
  an ESP32 node or the CAN-HAT would, so the rest of the pipeline can't tell
  the difference.

---

## Step 1 — CAN-HAT: install, configure, and stress-test in isolation

Do this **before** touching Docker. The previous CAN-HAT on this project failed
intermittently in a way that was very easy to mistake for a Docker/software
problem — always rule the HAT out on its own first.

1. **Mount the PiCAN3** on the Pi 5's 40-pin header, connect CAN-H/CAN-L to the
   vehicle harness (or a loopback test rig), and connect the SMPS power input
   if you're powering the Pi from the vehicle's 12V/24V system.

2. **Enable SPI and add the overlay** in `/boot/firmware/config.txt`:
   ```
   dtparam=spi=on
   dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
   ```

3. **Reboot and check the driver probed successfully:**
   ```bash
   sudo reboot
   ```
   ```bash
   dmesg | grep -i mcp251x
   ip link show can0
   ```
   Expect `mcp251x spi0.0 can0: MCP2515 successfully initialized.` and `can0`
   listed as a link (state `DOWN` is normal — it hasn't been brought up yet).

4. **Reboot 3-4 times in a row**, re-checking `dmesg | grep -i mcp251x` after
   each one. All four boots must show `successfully initialized` — a single
   successful boot proves nothing (this is exactly the failure mode the
   previous HAT had: it would initialize cleanly most of the time and fail
   intermittently). Do not proceed to step 5 until this is consistent.

5. **Bring the interface up and confirm it takes real traffic:**
   ```bash
   sudo apt-get install -y can-utils
   sudo ip link set can0 up type can bitrate 250000   # 250k is the J1939 default for most heavy-vehicle buses; confirm with the bus documentation if available
   sudo ifconfig can0 txqueuelen 65536
   candump can0
   ```
   With the HAT wired to the live vehicle bus, you should see frames scrolling
   immediately. If nothing appears, don't assume it's a bitrate problem before
   checking termination and wiring — see Troubleshooting below.

6. **Make it persistent across reboots.** SocketCAN interfaces don't
   auto-configure on their own; add a systemd-networkd or a small script.
   Simplest option, `/etc/systemd/system/can0-up.service`:
   ```ini
   [Unit]
   Description=Bring up can0 at boot
   After=network.target

   [Service]
   Type=oneshot
   ExecStart=/sbin/ip link set can0 up type can bitrate 250000
   RemainAfterExit=yes

   [Install]
   WantedBy=multi-user.target
   ```
   ```bash
   sudo systemctl enable --now can0-up.service
   ```

7. **CAN reading is wired into the aggregator; per-signal decoding is not.**
   `aggregator.py` runs a background thread (`_can_reader_thread()`) that
   opens `can0` via `python-can`'s SocketCAN backend and continuously reads
   real frames, with the same connect/retry/fall-back-gracefully pattern
   already used for MQTT and the database elsewhere in the file — if `can0`
   doesn't exist yet, isn't up, or gets unplugged mid-run, the aggregator
   logs it and keeps retrying every 5s instead of crashing, and the rest of
   the pipeline (ESP32 sensors, GPS stub, DB writes) keeps working
   regardless. This part needed no vehicle-specific knowledge and is done.

   **What's still a stub is turning those raw frames into the named
   signals** (`fuel_heater_hours`, `hvess_remaining_distance_km`,
   `operational_soc_percent`, and the rest) — that requires a DBC file (or
   at minimum a documented CAN ID / byte-offset / scaling-factor mapping)
   for this specific vehicle (a MAN electric bus), which does not exist in
   this repo yet. `get_can_data()` deliberately does not guess at that
   mapping — a wrong-but-plausible-looking decoded value would be worse
   than `None`, since it could silently corrupt the research dataset with
   no obvious sign anything was wrong. Budget time for this decoding step
   once a DBC becomes available; until then, `wp25i._data_source` tells you
   exactly why the signals are still `null`:
   - `"stub_no_can_hardware"` — `can0` isn't up / hasn't produced a frame
     yet (no HAT, HAT not wired to the vehicle bus, interface down).
   - `"stub_no_dbc_mapping"` — `can0` **is** up and receiving real traffic,
     but nothing decodes it yet, because no DBC/signal mapping has been
     provided. This is the one to look for once Step 5 (`ip link show
     can0`, `candump can0`) confirms real traffic is flowing but the DB
     still shows stub values — it means Step 1 hardware is fine, and the
     only remaining work is the decode step, not more hardware debugging.
   - `"live_can_hardware"` — reserved for once real decoding is
     implemented against an actual DBC; nothing produces this value today.

   **Docker networking note:** reading `can0` from inside a container
   requires the `aggregator` service to run with `network_mode: host` in
   `docker-compose.yml` (SocketCAN interfaces are kernel network devices
   scoped to a network namespace — the default bridge network puts the
   container in its own namespace that simply cannot see `can0`, and there
   is no per-device mapping that fixes this the way there would be for a
   USB serial device). This is already configured in this repo's
   `docker-compose.yml`. The one consequence to know about: a
   host-networked container can no longer resolve `mosquitto` /
   `timescaledb` by their Compose service names, so `aggregator`'s
   `MQTT_BROKER` / `DB_HOST` env vars point at `127.0.0.1` instead — both
   services already publish their ports to the host, so this just reaches
   them the same way any other host process would. If you ever add a
   service that the aggregator needs to reach by service name again,
   remember it needs its own host-published port too, or the aggregator
   won't be able to find it.

### Troubleshooting the CAN-HAT

- **`MCP251x didn't enter in conf mode after reset` / `Probe failed, err=110`,
  every boot, no exceptions** → hardware fault, not configuration. This exact
  signature is what led to returning the previous HAT — see the project's
  correspondence with Waveshare (support ticket #248595) for the full
  diagnostic trail if this recurs.
- **Same error, but intermittent (works most boots, fails occasionally)** →
  re-check wiring, 120Ω termination, and interrupt pin *before* suspecting the
  board again; this pattern was chased for a long time on the old HAT and
  ultimately was the unit itself, but don't skip re-verifying the basics.
- **`candump` shows nothing at all, no errors** → check termination resistance
  (should read ~60Ω between CAN-H/CAN-L with the bus unpowered, two 120Ω
  terminators in parallel) and that you're on the correct bitrate for this
  vehicle's bus.

---

## Step 2 — Host network prerequisites (dnsmasq / static IP)

DHCP for the ESP32 nodes and the Pi's static IP are operating-system-level
network configuration, not application configuration, and are deliberately
kept outside Docker (a containerized DHCP server needs `network_mode: host`
and risks breaking the Pi's own networking if misconfigured). Do this once per
physical Pi:

1. **Static IP on the sensor-facing interface** (adjust interface name/IP to
   your setup — the reference deployment uses `eth0` / `192.168.1.1`):
   ```bash
   sudo nmcli con mod "Wired connection 1" ipv4.addresses 192.168.1.1/24 \
       ipv4.method manual
   ```

2. **Install and configure dnsmasq:**
   ```bash
   sudo apt-get install -y dnsmasq
   ```
   `/etc/dnsmasq.d/wp2-sensors.conf`:
   ```
   interface=eth0
   dhcp-range=192.168.1.101,192.168.1.150,12h
   ```
   ```bash
   sudo systemctl enable --now dnsmasq
   ```

3. Verify with `ip addr show eth0` and confirm `dnsmasq` is `active (running)`
   before powering on any ESP32 node.

---

## Step 3 — Configure `.env` (bus identity, database, and broker credentials)

```bash
cd ~/wp2-pi-stack-docker
cp .env.example .env
nano .env
```

Fill in every value marked `change_me...`, plus:

- **`VEHICLE_ID`** — this bus's unique identifier ("bus ID" / "unit ID",
  equivalent to what you may know as `ServerAddress`-adjacent device identity
  on other hardware, e.g. the Cipia units). Set this uniquely per physical Pi
  **before first boot** — it tags every DB row, is used in the local MQTT
  payload, and namespaces this bus's topic on the central broker
  (`fleet/<VEHICLE_ID>/unified/combined`).
- **`CENTRAL_MQTT_HOST`** — address of the central fleet broker in AWS
  (this is the "ServerAddress" field). **Required** — the `forwarder`
  container will not start without it.
- **`CENTRAL_MQTT_PASSWORD`** — password for authenticating this bus to the
  central broker (this is the "ServerPassword" field). **Required**, same as
  above.
- **`CENTRAL_MQTT_USERNAME`** — optional; defaults to `VEHICLE_ID` if left
  unset/commented out, so each bus authenticates as itself automatically. Only
  set this explicitly if the central broker uses a different username scheme.
- **`CENTRAL_MQTT_PORT`** — defaults to `8883` (MQTT over TLS); only change if
  the central broker uses a non-standard port.

Note the deliberate split: the **local** Mosquitto broker on this Pi (used by
the aggregator and the ESP32 nodes) has no password by design — it's trusted
LAN traffic inside the bus. Only the **uplink** to the central broker is
authenticated. Don't add a password to the local broker; if you need to change
that decision, it means editing `mosquitto/config/mosquitto.conf`, not `.env`.

---

## Step 4 — Bring up the Docker stack

```bash
# Install Docker if not already present
curl -fsSL https://get.docker.com | sudo sh
sudo apt-get install -y docker-compose-plugin
sudo usermod -aG docker "$USER"   # log out/in for this to take effect
sudo systemctl enable --now docker
```

```bash
docker compose up -d --build
docker compose ps
```

Don't be alarmed that `aggregator` shows no `PORTS` column in `docker compose ps`
— it runs with `network_mode: host` (needed so its CAN reader can see the
host's `can0` interface, see Step 1.7), so it doesn't get its own published
ports the way the other services do; that's expected, not a sign something
failed to start.

On first start, `timescaledb` automatically runs
`postgres/init/01-init-wp2-schema.sh` (enables the TimescaleDB extension,
creates the `wp2_records` hypertable, creates the `wp2user` role from your
`.env`, grants the needed privileges). This only runs once against an empty
volume — it will not re-run on later restarts.

---

## Step 5 — Verify everything end to end

```bash
# All four containers healthy/running
docker compose ps

# Aggregator connected to MQTT + DB, and either connected to can0 or
# cleanly retrying if it isn't up yet (both are fine -- see below)
docker compose logs --tail=30 aggregator
# Expect:
#   [DB] Connected to TimescaleDB.
#   [MQTT] Connected (reason_code=Success). Subscribing to bus/env/#
#   [AGGREGATOR] Published combined record @ ...
# Plus one of:
#   [CAN] Connected to can0.                                  <- HAT wired and can0 is up
#   [CAN] Bus unavailable on can0 (...); retrying in 5s...     <- no HAT yet, or can0 isn't up -- expected, not an error

# Forwarder connected to the central broker
docker compose logs --tail=30 forwarder
# Expect:
#   [DB] Connected to local TimescaleDB.
#   [CENTRAL] Connected (reason_code=Success).
# If you instead see repeated "[CENTRAL] Disconnected", double check
# CENTRAL_MQTT_HOST / CENTRAL_MQTT_PASSWORD in .env and that this Pi has
# a working LTE/internet connection.

# Combined record arriving once a second, locally
docker exec wp2-mosquitto mosquitto_sub -h localhost -t "bus/unified/combined"

# Check the CAN block's _data_source to see exactly where things stand
docker exec wp2-timescaledb psql -U wp2user -d wp2data \
  -c "SELECT payload->'bus_environment'->'wp25i' FROM wp2_records ORDER BY time DESC LIMIT 1;"
# "stub_no_can_hardware"  -> can0 isn't up / no traffic yet (see Step 1)
# "stub_no_dbc_mapping"   -> can0 IS up and receiving real frames, but no
#                            DBC/signal mapping exists yet to decode them
#                            into the named fields (see Step 1.7) -- this
#                            is expected until a DBC is provided, not a bug
# "live_can_hardware"     -> real decoded values (not implemented yet)

# Raw traffic from real ESP32 nodes (once Step 2 is done and a node is powered on)
docker exec wp2-mosquitto mosquitto_sub -h localhost -t "bus/env/#" -v

# Diagnostics block (matches WP2_payload_schema.json exactly)
docker exec wp2-timescaledb psql -U wp2user -d wp2data \
  -c "SELECT payload->'diagnostics' FROM wp2_records ORDER BY time DESC LIMIT 1;"
# Expect: {"mqtt_connected": true, "sensor_faults": [...], "watchdog_triggered": false}
# sensor_faults will always include {"source": "wp24i", "message": "no_gps_hardware"}
# until the SIM7600 GPS module is wired up (get_gps_data() is still a full
# stub), plus {"source": "wp25i", "message": "no_can_hardware"} whenever
# can0 isn't up and no test override is active (see "Sensor emulator" below).

# Sensor emulator (dev/test tool) is reachable and connected to the broker
curl http://localhost:8090/health
# Expect: {"mqtt_connected": true}
```

---

## MQTT topics reference (for ESP32 / sensor firmware)

Every topic below is `bus/env/<name>`, published by the corresponding
physical node, subscribed to by `aggregator.py` via a single `bus/env/#`
wildcard. Field names in each payload must match `WP2_payload_schema.json`
exactly -- that file is the single source of truth for this project, not
this table (this table exists so firmware doesn't have to re-derive topic
names from the schema file, which only describes the combined record's
shape, not the per-node MQTT wire format).

| Topic | WP | Payload shape |
|---|---|---|
| `wp211_front`, `wp212_middle`, `wp213_rear` | WP2.1.i | flat: `{c1_temp_C_head_standing, c2_temp_C_head_seated, c3_temp_C_abdomen_seated, c4_temp_C_ankle_feet, k1_temp_C_control}` |
| `wp221_front`, `wp222_right`, `wp223_left` | WP2.2.i | flat: `{a1_iav_x_m_per_s, a1_iav_y_m_per_s, a1_iav_z_m_per_s, a2_co2_ppm, a2_temp_C, a2_RH_percent, b2_pm001_mum, b2_pm025_mum, b2_pm040_mum, b2_pm100_mum, a3_solar_irradiance_w_per_m2}` |
| `wp231_front`, `wp232_middle`, `wp233_rear` | WP2.3.i | flat: `{b1_RH_percent, d1_globe_temp_x_C, d1_globe_temp_y_C, d1_globe_temp_z_C, b2_open_door_state}` |
| `wp233_aux` | WP2.3.i (aux) | flat: same as above but **`b2_ignition_state` instead of `b2_open_door_state`** -- same field type (boolean), different sensed condition on this node |
| `thermal_dummy_1`, `thermal_dummy_2`, `thermal_dummy_3` | WP2.6.i | `{"thermal_dummy": {"surface": {head, neck, torso, abdomen, arms, legs, auxiliary}}}` -- 7 nested body-region objects, see `WP2_payload_schema.json`'s `thermal_dummies.thermal_dummy_wp26N.surface` for exact field names per region |
| `wp2tm_1`, `wp2tm_2`, `wp2tm_3` | WP2.TM | `{"internal_wp2tm": {...32 flat fields...}}` -- 16 body zones, each a `temp_*` float + its own `pwm_status_pad_*` number (actuator PWM level, so actuator power can be computed downstream -- **not** a plain on/off flag; this replaced the old boolean `on_status_pad_*` field) |

**Important for firmware:** do **not** publish an `"active"` field on the
`thermal_dummy_N` topics. Whether a dummy counts as active is computed
entirely by `aggregator.py` from how recently it last heard from that
topic (`THERMAL_DUMMY_ACTIVE_TIMEOUT_SEC`, currently 10s) -- a manikin
that stops publishing (unplugged, powered off) is automatically reported
as inactive within one timeout window. Firmware only ever needs to
publish `surface`/`internal_wp2tm` data when it has a real reading; it
never needs to declare its own active/inactive state.

---

## Sensor emulator (manual testing without real hardware)

`http://<this-pi's-IP>:8090` — a small web page (the `emulator` container,
already part of `docker-compose.yml`, no extra setup needed) that publishes
hand-built MQTT messages to the **local** broker, exactly like a real ESP32
node, the CAN-HAT, or a thermal manikin would. The aggregator picks these up
on its next cycle and processes them through the same real code path as
genuine hardware -- local DB insert, forward to the central broker,
everything. This tool never talks to the database or the central broker
directly. Every field in the form is named after `WP2_payload_schema.json`
exactly -- that file is the source of truth for this tool, not the other
way around.

What it lets you do:

- **Simulate any WP2.1.i / WP2.2.i / WP2.3.i node** (including `wp233_aux`)
  -- one card per node, "Send" publishes just that node, "Send all enabled
  sensor sections" publishes everything at once.
- **Simulate all 3 thermal dummies and their 3 paired internal WP2.TM
  manikins** -- 6 cards total, each always sendable ("active" is computed
  by `aggregator.py` from message recency, see the MQTT topics table
  above, so a single "Send" click only counts as active for
  `THERMAL_DUMMY_ACTIVE_TIMEOUT_SEC` (10s) before timing back out). Each
  thermal dummy card also has an **"Auto-send every 5s" checkbox** --
  check it to keep resending that card's current values on a timer, so it
  reads as continuously `active` for as long as it stays checked, without
  needing to keep clicking "Send" by hand. Unchecking it (or closing the
  page) just stops the timer; the dummy naturally times back out to
  `active: false` within 10s, same as a real manikin going quiet.
- **Inject dummy energy/CAN data** (`wp25i`) via `bus/test/can_override` --
  useful since there's no DBC file yet (see Step 1.7): "Set override"
  overrides `wp25i` with whatever values you type, tagged
  `_data_source: "test_override"`; "Clear override" reverts to the real
  CAN-hardware/stub behavior.
- **Inject one or more simulated sensor faults at once** via
  `bus/test/fault` -- build up several source+message rows ("+ Add another
  fault row") and "Add fault(s)" publishes all of them in a single
  message, landing together in the same cycle's `diagnostics.sensor_faults`;
  "Clear all injected faults" removes only the ones added this way (it does
  not touch the always-recomputed `no_can_hardware`/`no_gps_hardware`
  entries described above).

Both `bus/test/*` topics are handled directly inside `aggregator.py`
(`CAN_OVERRIDE_TOPIC`, `FAULT_TOPIC`), separate from the real `bus/env/*`
sensor topics, specifically so a wildcard subscriber watching only
`bus/env/#` never sees test traffic mixed in with real data.

---

## Populating test data (step-by-step sequence)

To build up one fully-populated combined record in `wp2_records` for
testing (dashboards, forwarder, central broker, etc.), publish to every
topic below in order via the emulator's `/publish` endpoint -- this is
exactly what the web UI's individual "Send" buttons do; scripted here as
`curl` for repeatable/automatable testing. Each publish lands on the
aggregator's next 1-second cycle, so everything ends up in the very next
`wp2_records` row (or the one after, if steps straddle a cycle boundary).
Replace `localhost` with the Pi's LAN IP if calling from another machine.

```bash
EMULATOR=http://localhost:8090

# 1. WP2.1.i -- cabin environment (3 nodes)
for node in wp211_front wp212_middle wp213_rear; do
  curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
    "topic": "bus/env/'"$node"'",
    "payload": {"c1_temp_C_head_standing": 23.4, "c2_temp_C_head_seated": 24.1, "c3_temp_C_abdomen_seated": 16.0, "c4_temp_C_ankle_feet": 25.0, "k1_temp_C_control": 16.0}
  }'
done

# 2. WP2.2.i -- air quality & velocity (3 nodes)
for node in wp221_front wp222_right wp223_left; do
  curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
    "topic": "bus/env/'"$node"'",
    "payload": {"a1_iav_x_m_per_s": 1.8, "a1_iav_y_m_per_s": 2.8, "a1_iav_z_m_per_s": 3.8, "a2_co2_ppm": 670, "a2_temp_C": 12.5, "a2_RH_percent": 65.8, "b2_pm001_mum": 30.0, "b2_pm025_mum": 50.0, "b2_pm040_mum": 75.0, "b2_pm100_mum": 120.0, "a3_solar_irradiance_w_per_m2": 810.0}
  }'
done

# 3. WP2.3.i -- humidity/3-axis globe temp/door (3 nodes), plus the aux node (ignition instead of door)
for node in wp231_front wp232_middle wp233_rear; do
  curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
    "topic": "bus/env/'"$node"'",
    "payload": {"b1_RH_percent": 23.4, "d1_globe_temp_x_C": 24.1, "d1_globe_temp_y_C": 24.1, "d1_globe_temp_z_C": 24.1, "b2_open_door_state": false}
  }'
done
curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
  "topic": "bus/env/wp233_aux",
  "payload": {"b1_RH_percent": 23.4, "d1_globe_temp_x_C": 24.1, "d1_globe_temp_y_C": 24.1, "d1_globe_temp_z_C": 24.1, "b2_ignition_state": true}
}'

# 4. Thermal dummies + their paired internal WP2.TM (1, 2, and 3)
for n in 1 2 3; do
  curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
    "topic": "bus/env/thermal_dummy_'"$n"'",
    "payload": {"thermal_dummy": {"surface": {
      "head": {"temp_c": 31.5, "rh_percent": 60, "iav_m_per_s_x": 2.3, "iav_m_per_s_y": 2.3, "globe_temp_c": 31.5},
      "neck": {"temp_c": 31.5, "iav_m_per_s_x": 2.3, "iav_m_per_s_y": 2.3},
      "torso": {"temp_c": 31.5, "globe_temp_c": 31.5},
      "abdomen": {"temp_c": 31.5},
      "arms": {"temp_c_left_upper_arm": 31.5, "temp_c_right_upper_arm": 31.5, "temp_c_left_forearm": 31.5, "temp_c_right_forearm": 31.5, "temp_c_left_hand": 31.5, "temp_c_right_hand": 31.5},
      "legs": {"temp_c_left_thigh": 31.5, "temp_c_right_thigh": 31.5, "temp_c_left_calf": 31.5, "temp_c_right_calf": 31.5, "temp_c_left_foot": 31.5, "temp_c_right_foot": 31.5, "globe_temp_c_feet": 31.5, "iav_m_per_s_x_feet": 2.3, "iav_m_per_s_y_feet": 2.3},
      "auxiliary": {"cushion_temp_c": 31.5, "iav_m_per_s_aux_z1": 2.3, "iav_m_per_s_aux_z2": 2.3}
    }}}
  }'
  curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
    "topic": "bus/env/wp2tm_'"$n"'",
    "payload": {"internal_wp2tm": {"temp_head": 35.8, "pwm_status_pad_head": 65, "temp_chest_left": 35.8, "pwm_status_pad_chest_left": 65, "temp_chest_right": 35.8, "pwm_status_pad_chest_right": 65, "temp_abdomen": 35.8, "pwm_status_pad_abdomen": 65, "temp_tight_left": 35.8, "pwm_status_pad_tight_left": 65, "temp_tight_right": 35.8, "pwm_status_pad_tight_right": 65, "temp_upper_left_arm": 35.8, "pwm_status_pad_upper_left_arm": 65, "temp_upper_right_arm": 35.8, "pwm_status_pad_upper_right_arm": 65, "temp_lower_left_arm": 35.8, "pwm_status_pad_lower_left_arm": 65, "temp_lower_right_arm": 35.8, "pwm_status_pad_lower_right_arm": 65, "temp_left_hand": 35.8, "pwm_status_pad_left_hand": 65, "temp_right_hand": 35.8, "pwm_status_pad_right_hand": 65, "temp_lower_left_leg": 35.8, "pwm_status_pad_lower_left_leg": 65, "temp_lower_right_leg": 35.8, "pwm_status_pad_lower_right_leg": 65, "temp_left_foot": 35.8, "pwm_status_pad_left_foot": 65, "temp_right_foot": 35.8, "pwm_status_pad_right_foot": 65}}
  }'
done

# 5. Energy/CAN dummy data (wp25i) -- optional, since no DBC file exists yet (see Step 1.7)
curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
  "topic": "bus/test/can_override",
  "payload": {"fuel_heater_hours": 15, "hvess_remaining_distance_km": 35, "operational_soc_percent": 85, "cumulated_energy_charged_kwh": 530.15, "regenerative_braking_cumulated_energy_kwh": 120.08, "hvac_cumulated_energy_kwh": 35.15, "air_compressor_cumulated_energy_kwh": 19.15, "auxiliaries_cumulated_energy_kwh": 4.18}
}'

# 6. (Optional) inject a couple of simulated faults in one batch
curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{
  "topic": "bus/test/fault",
  "payload": {"action": "add", "faults": [
    {"source": "wp21i", "message": "sensor_timeout_test"},
    {"source": "wp26i_2", "message": "surface_probe_disconnected"}
  ]}
}'
```

Verify the fully-populated record landed:

```bash
docker exec wp2-timescaledb psql -U wp2user -d wp2data \
  -c "SELECT payload FROM wp2_records ORDER BY time DESC LIMIT 1;"
```

`thermal_dummies.thermal_dummy_wp26N.active` only reads `true` within
`THERMAL_DUMMY_ACTIVE_TIMEOUT_SEC` (10s) of that dummy's last publish (see
the MQTT topics table above) -- query right after step 4, or re-publish
first, if you want to see `active: true` rather than it having already
timed back out to `false`.

**Clearing test data**: unlike the central node's dedicated test vehicle
(`BUS_2038`, with its own `scripts/clear_bus2038.sh`), this Pi's
`wp2_records` has no such destructive "clear" -- every row here is this
bus's real research data, with no test/real tag to filter on, so deleting
rows would risk deleting genuine data along with test rows. To back out
just the two `bus/test/*` injections (their *effect* on future rows, not
already-inserted ones):

```bash
curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{"topic": "bus/test/can_override", "payload": {}}'
curl -s -X POST $EMULATOR/publish -H "Content-Type: application/json" -d '{"topic": "bus/test/fault", "payload": {"action": "clear"}}'
```

---

## Restoring data from a previous install

For a lightweight migration where you just want the historical rows moved
over and are otherwise setting the new host up from scratch (Steps 1-5
above):

```bash
# On the OLD host, while its Postgres is still running:
sudo -u postgres pg_dump -d wp2data --data-only -t wp2_records \
  -f wp2_records_data_only.sql

# Copy that file to the new host, then, with the new stack already up:
docker cp wp2_records_data_only.sql wp2-timescaledb:/tmp/
docker exec wp2-timescaledb psql -U postgres -d wp2data \
  -f /tmp/wp2_records_data_only.sql
```

## Full disaster-recovery backup and restore (code + .env + all volumes)

Use this instead of the data-only method above when a Pi needs to be fully
reproduced elsewhere (or after a reimage/OS downgrade on the same Pi) --
this restores the exact code, secrets, and historical data, not just the
`wp2_records` rows. This is the actual procedure used to recover this
project after a Debian downgrade wiped a Pi; it's written up here so any
other bus's Pi that needs the same recovery follows the same steps instead
of improvising a new method each time.

**Taking the backup** (run on the host you're backing up, before wiping
or downgrading it):

```bash
cd ~
mkdir -p backup_pre_downgrade/volumes
cd wp2-pi-stack-docker
docker compose ps > ~/backup_pre_downgrade/containers.txt
docker compose images > ~/backup_pre_downgrade/images.txt
docker network ls > ~/backup_pre_downgrade/networks.txt
docker volume ls > ~/backup_pre_downgrade/volumes.txt

# The whole project directory, including the real .env (this backup
# contains live secrets -- handle the resulting .tar.gz like you would
# any other file with production credentials in it).
cp -r ~/wp2-pi-stack-docker ~/backup_pre_downgrade/project

# Every named volume, tarred up from inside a throwaway container so file
# ownership (e.g. postgres's uid 999, mosquitto's uid 1883) is preserved
# exactly -- this matters, see "Problems already solved" below.
for vol in mosquitto_data mosquitto_log timescaledb_data; do
  docker run --rm \
    -v "wp2-pi-stack-docker_${vol}:/data" \
    -v ~/backup_pre_downgrade/volumes:/backup \
    alpine sh -c "cd /data && tar -czf /backup/${vol}.tar.gz ."
done

cd ~ && tar -czf backup_pre_downgrade_full.tar.gz backup_pre_downgrade/
```

**Restoring onto a fresh/blank Pi:**

```bash
# 1. Copy the code + .env back (keep the directory named exactly
#    "wp2-pi-stack-docker" -- Compose derives volume names from this
#    directory name, and they need to match what's inside the volume
#    tarballs, e.g. "wp2-pi-stack-docker_timescaledb_data")
mkdir -p ~/wp2-pi-stack-docker
cp -r backup_pre_downgrade/project/. ~/wp2-pi-stack-docker/
chmod 600 ~/wp2-pi-stack-docker/.env

# 2. Install Docker (see Step 4 above), then create the containers +
#    named volumes WITHOUT starting anything yet, so there's something
#    to restore data into:
cd ~/wp2-pi-stack-docker
docker compose create

# 3. Populate each volume from its backup tarball, again via a
#    throwaway container so the original ownership is preserved:
for vol in mosquitto_data mosquitto_log timescaledb_data; do
  docker run --rm \
    -v "wp2-pi-stack-docker_${vol}:/data" \
    -v ~/backup_pre_downgrade/volumes:/backup \
    alpine sh -c "tar -xzf /backup/${vol}.tar.gz -C /data"
done

# 4. Bring everything up
docker compose up -d
docker compose ps
```

Then continue from Step 5 (verify everything end to end) as normal. Note
this restores the bus's identity (`VEHICLE_ID`, its central-broker MQTT
credentials in `.env`) along with everything else -- the central fleet
broker doesn't need the bus re-registered, since as far as it's concerned
this is the same bus reconnecting, not a new one.

## Common issues (already solved in this repo, kept here for reference)

- **"permission denied for table wp2_records"** — fixed by the explicit
  `GRANT`/`ALTER DEFAULT PRIVILEGES` statements already in
  `postgres/init/01-init-wp2-schema.sh`.
- **Mosquitto silently refusing external (ESP32) connections** — not an issue
  here: `mosquitto.conf` uses a bare `listener 1883` and the port is published
  on `0.0.0.0:1883`.
- **ESP32 nodes not appearing in the combined record** — check Step 2 is
  actually done and the node is powered on; the aggregator still publishes
  with empty node keys (`{}`) even with zero nodes connected.
- **No aggregator logs showing up** — the image sets `PYTHONUNBUFFERED=1`;
  this is already handled, but if you rebuild the Dockerfile from scratch,
  don't drop it.
- **`forwarder` container restarting in a loop right after `docker compose up`**
  — almost always means `CENTRAL_MQTT_HOST` or `CENTRAL_MQTT_PASSWORD` is
  missing from `.env` (see Step 3); both are required with no default.
- **`[CAN] Bus unavailable on can0 ([Errno 19] No such device)` forever,
  even though the HAT is installed and `ip link show can0` works fine on
  the host** — the `aggregator` container isn't on `network_mode: host`
  (already set in this repo's `docker-compose.yml`; check nothing removed
  it if you're seeing this). Without it, the container is in its own
  network namespace and `can0` genuinely doesn't exist from its point of
  view — no `devices:` mapping fixes this, since SocketCAN interfaces
  aren't `/dev` files.
- **DB/MQTT connection errors after adding `network_mode: host` to a
  service** — a host-networked container can't resolve other services by
  their Compose name (`mosquitto`, `timescaledb`) anymore; point it at
  `127.0.0.1` instead and make sure the target service actually publishes
  its port to the host (see the Step 1.7 Docker networking note above).

## Resetting everything (destructive)

```bash
docker compose down -v   # also deletes the named volumes (all DB + MQTT persisted data)
```

## Auto-start on boot

Every service in `docker-compose.yml` has `restart: unless-stopped`, and the
Docker daemon starts on boot once enabled (`sudo systemctl enable docker`). As
long as you don't manually stop the stack, it comes back up automatically
after a reboot or power loss. The `can0-up.service` from Step 1.6 handles
bringing the CAN interface up independently, before Docker needs it.
