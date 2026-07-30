# WP2.5.i Aggregator Stack (Dockerized)

[Return to main](../README.md)

> [!CAUTION]
> To ensure full compatibility with the Raspberry-Pi CAN-hat by Waveshare, OS version must be one released on or before July 24, 2024.

Central hub software stack for the ElBussTransTherm project (KTH PhD research on thermal comfort and energy consumption in electric urban buses). This Raspberry Pi sits at the center of one bus: ESP32-S3 sensor nodes (WP2.1.i-WP2.6.i, WP2.TM) publish sensor readings over MQTT, this stack aggregates them once per second into a single JSON record per vehicle/trip, stores it in a time-series database, and republishes the combined record for any downstream consumer.

This directory is meant to be **copied as-is onto a fresh Raspberry Pi** (or any Debian-based aarch64/amd64 host) to bring the whole application stack up with one command. It replaces a previous bare-metal install (native Mosquitto + native PostgreSQL/TimescaleDB + a systemd unit running the aggregator) with three Docker containers, so a reinstall no longer means re-discovering the same handful of gotchas from scratch.

## Architecture

```
ESP32 sensor nodes  
--> MQTT (bus/env/#)
	--> [mosquitto container]
				|
                v
        [aggregator container]  
		--INSERT--> 
		        [timescaledb container]
                               |
                        +--republishes --> 
						               bus/unified/combined
```

- **mosquitto** — MQTT broker, listens on port 1883, no auth (trusted LAN only).
- **timescaledb** — PostgreSQL 16 + TimescaleDB, database `wp2data`, hypertable `wp2_records` keyed on `time`.
- **aggregator** — Python service (source: `aggregator.py`, unmodified from the original bare-metal version). Subscribes to `bus/env/#`, holds the latest reading per node in memory, assembles one combined JSON record per second, inserts it into `wp2_records`, and republishes it on `bus/unified/combined`.
  CAN bus and GPS data are intentionally stubbed out (`_data_source: "stub_no_can_hardware"` / `"stub_no_gps_hardware"`) until the CAN-HAT and SIM7600G-H modem are physically installed — this is expected, not a bug.

## What is NOT in this repo (must be configured on the host OS)

DHCP for the ESP32 nodes and the Pi's static IP are operating-system-level network configuration, not application configuration. Running a DHCP server (`dnsmasq`) inside a container requires `network_mode: host` and carries a real risk of breaking the Pi's own networking if misconfigured, so it is deliberately kept outside Docker. Configure it once per physical Pi, before bringing up this stack:

1. **Static IP on the sensor-facing interface** (adjust interface name/IP to your setup — the reference deployment uses `eth0` / `192.168.1.1`):

   ```bash
   sudo nmcli con mod "Wired connection 1" ipv4.addresses 192.168.1.1/24 \
       ipv4.method manual
   # or edit /etc/network/interfaces / netplan, depending on your OS image
   ```

2. **Install and configure dnsmasq** to hand out DHCP leases to the ESP32 nodes:

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

3. Verify with `ip addr show eth0` and confirm `dnsmasq` is `active (running)` before powering on any ESP32 node.

Everything below this point (Mosquitto, TimescaleDB, the aggregator) is fully containerized and does not touch host networking beyond publishing the ports each container needs.

## Prerequisites

- A Debian-based OS (Raspberry Pi OS / Debian) with the network setup above already done.
- Docker Engine + the Compose plugin. If not installed:

  ```bash
  # Try the distro package first.
  sudo apt-get update && sudo apt-get install -y docker.io docker-compose-v2

  # If those packages aren't available yet for your OS release (this has
  # happened on very new Debian releases before the distro repos catch up),
  # use Docker's official install script instead:
  curl -fsSL https://get.docker.com | sudo sh
  sudo apt-get install -y docker-compose-plugin

  sudo usermod -aG docker "$USER"   # log out/in for this to take effect
  sudo systemctl enable --now docker
  ```

## Quick start (fresh Pi)

```bash
# 1. Copy this directory onto the Pi, e.g.:
scp -r wp2-pi-stack-docker/ user@pi-host:~/

# 2. Create your real .env from the template
cd ~/wp2-pi-stack-docker
cp .env.example .env
nano .env   # set DB_PASSWORD, POSTGRES_SUPERUSER_PASSWORD, VEHICLE_ID, TRIP_ID

# 3. Build and start everything
docker compose up -d --build

# 4. Watch it come up
docker compose ps
docker compose logs -f aggregator
```

On first start, the `timescaledb` container automatically runs
`postgres/init/01-init-wp2-schema.sh`, which:
- enables the `timescaledb` extension,
- creates the `wp2_records` hypertable with the correct schema and indexes,
- creates the `wp2user` role with the password from your `.env`,
- grants it the privileges it needs on `wp2_records` and the `public` schema.

This init script only runs once, against an **empty** data volume. It will not re-run (and will not undo manual changes) on a container restart once data exists — that's standard Postgres Docker image behavior.

## Verifying it's working

```bash
# Containers healthy and running
docker compose ps

# Aggregator connected to both MQTT and the DB, publishing every second
docker compose logs --tail=30 aggregator
# Expect to see:
#   [DB] Connected to TimescaleDB.
#   [MQTT] Connected (reason_code=Success). Subscribing to bus/env/#
#   [AGGREGATOR] Published combined record @ ...

# Combined record arriving once a second
docker exec wp2-mosquitto mosquitto_sub -h localhost -t "bus/unified/combined"

# Rows landing in the database
docker exec wp2-timescaledb psql -U wp2user -d wp2data \
  -c "SELECT time, vehicle_id, trip_id FROM wp2_records ORDER BY time DESC LIMIT 5;"

# Raw traffic from real ESP32 nodes (once dnsmasq/static IP is set up and a node is powered on)
docker exec wp2-mosquitto mosquitto_sub -h localhost -t "bus/env/#" -v
```

## Restoring data from a previous (bare-metal or Docker) install

If you're reinstalling and have a backup of the old `wp2_records` table:

```bash
# On the OLD host, while its Postgres is still running:
sudo -u postgres pg_dump -d wp2data --data-only -t wp2_records \
  -f wp2_records_data_only.sql

# Copy that file to the new host, then, with the new stack already up:
docker cp wp2_records_data_only.sql wp2-timescaledb:/tmp/
docker exec wp2-timescaledb psql -U postgres -d wp2data \
  -f /tmp/wp2_records_data_only.sql
```

## Common issues (already solved in this repo, kept here for reference)

- **"permission denied for table wp2_records"** — the app user only had privileges from `GRANT ALL PRIVILEGES ON DATABASE ...`, which does *not* cascade to tables created by another role. Fixed by the explicit `GRANT ... ON TABLE`, `GRANT USAGE ON SCHEMA`, and `ALTER DEFAULT PRIVILEGES` statements already baked into `postgres/init/01-init-wp2-schema.sh`.
- **Mosquitto silently refusing external (ESP32) connections** — happens when Mosquitto's listener is bound to `127.0.0.1` only. Not an issue here: the container's `mosquitto.conf` uses a bare `listener 1883` (binds all interfaces inside the container), and `docker-compose.yml` publishes that port on the host's `0.0.0.0:1883`.
- **ESP32 nodes not appearing in the combined record** — check that the DHCP/static-IP prerequisite above is actually configured and that the node is powered on; the aggregator will still run and publish a combined record with empty node keys (`{}`) even with zero nodes connected.
- **No aggregator logs showing up** — the image sets `PYTHONUNBUFFERED=1`; without it, Python buffers stdout and `docker compose logs` shows nothing until the buffer flushes.

## Resetting everything (destructive)

```bash
docker compose down -v   # also deletes the named volumes (all DB + MQTT persisted data)
```

## Auto-start on boot

No separate systemd unit is needed for the application: every service in `docker-compose.yml` has `restart: unless-stopped`, and the Docker daemon itself starts on boot once enabled (`sudo systemctl enable docker`). As long as you don't manually stop the stack, it comes back up automatically after a reboot or power loss — which was the original motivation for moving off ad-hoc terminal-run processes in the first place.
