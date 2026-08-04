# Fleet Central Stack — Installation Guide

Portable installation kit to bring up the full stack (API + Ingestor + TimescaleDB + Mosquitto MQTT) on any Linux machine with Docker.

---

## Prerequisites

- Linux (Ubuntu/Debian recommended)
- Docker and Docker Compose installed

```bash
docker --version
docker compose version
```

If Docker isn't installed yet:

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
# Log out and back in for the group change to take effect
```

---

## 1. Extract the kit

This guide assumes the file `fleet-central-stack-install-kit.tar.gz` is already sitting in your `~/Downloads` folder.

```bash
# Create the destination folder and extract into it
mkdir -p ~/fleet-central-stack
tar xzf ~/Downloads/fleet-central-stack-install-kit.tar.gz -C ~/fleet-central-stack
cd ~/fleet-central-stack
```

> If the file isn't there, double check the exact folder name — some systems use `~/Descargas` instead of `~/Downloads` depending on language settings:
> ```bash
> ls ~/Downloads/ 2>/dev/null
> ls ~/Descargas/ 2>/dev/null
> ```
>
> If you're working on a remote server over SSH and downloaded the file through a browser on your local machine, it will land on your **local machine**, not the server. Transfer it over first with `scp`:
> ```bash
> scp ~/Downloads/fleet-central-stack-install-kit.tar.gz user@server_ip:~/Downloads/
> ```

---

## 2. Configure environment variables

The kit includes a template (`.env.example`) but not the real `.env` — each installation must create its own:

```bash
cp .env.example .env
nano .env
```

**Important: don't reuse the placeholder values from `.env.example` as-is.** They're only there to show the expected format, not real credentials.

- **Passwords and secrets — always generate fresh, unique values** (never reuse the example): `DB_PASSWORD`, `POSTGRES_SUPERUSER_PASSWORD`, `JWT_SECRET`, `BOOTSTRAP_ADMIN_PASSWORD`, `DYNSEC_ADMIN_PASSWORD`, `MQTT_INGESTOR_PASSWORD`.

  Generate a strong random value for each one with:
  ```bash
  openssl rand -base64 24
  ```
  Run it once per password field and paste the result in.

- **Names/config values — fine to keep as-is or adjust to preference**: `DB_NAME`, `DB_USER`, `BOOTSTRAP_ADMIN_USERNAME`, `MQTT_INGESTOR_USERNAME`, `COOKIE_SECURE`. These aren't secrets, just configuration choices.

---

## 3. Generate TLS certificates

For security reasons, the kit does not include real certificates — they're generated fresh on each install:

```bash
chmod +x generate_certs.sh
./generate_certs.sh ./mosquitto/certs
```

This creates a local CA and a self-signed server certificate. If the broker will be reachable from the internet, consider using certificates from a real CA (e.g. Let's Encrypt) instead.

---

## 4. Set up Mosquitto dynamic security

The kit includes an empty template (`dynamic-security.json.example`). Copy it to the active filename:

```bash
cp mosquitto/config/dynamic-security.json.example mosquitto/config/dynamic-security.json
```

---

## 5. Bring the stack up

```bash
docker compose up -d
docker compose ps
```

All services (`fleet-api`, `fleet-ingestor`, `fleet-timescaledb`, `fleet-mosquitto`) should show as `Up`.

---

## 6. Create the first Mosquitto admin user

Replace `<admin_username>` and `<secure_password>` below with your own values — they're placeholders, not literal text to copy. Pick any username you want, and generate a strong password the same way as before:

```bash
openssl rand -base64 24
```

Then run:

```bash
docker exec -it fleet-mosquitto mosquitto_ctrl dynsec createClient <admin_username> --password <secure_password>
docker exec -it fleet-mosquitto mosquitto_ctrl dynsec addClientRole <admin_username> admin
```

For example, if you chose the username `fleetadmin` and password `Xk29fL...`, the command would look like:

```bash
docker exec -it fleet-mosquitto mosquitto_ctrl dynsec createClient fleetadmin --password Xk29fL...
docker exec -it fleet-mosquitto mosquitto_ctrl dynsec addClientRole fleetadmin admin
```

---

## 7. Automated backups (enabled by default)

The kit sets up nightly backups automatically as part of the install — you don't need to configure anything manually. The command below adds the cron entry without opening an editor:

```bash
chmod +x backup_mosquitto.sh

# Add the nightly 2 AM backup job automatically (idempotent — safe to run more than once)
CRON_JOB="0 2 * * * $(pwd)/backup_mosquitto.sh >> $(pwd)/backup.log 2>&1"
( crontab -l 2>/dev/null | grep -vF "backup_mosquitto.sh" ; echo "$CRON_JOB" ) | crontab -

# Verify it was added
crontab -l | grep backup_mosquitto.sh
```

This runs `backup_mosquitto.sh` every night at 2 AM and logs output to `backup.log` in the project folder.

Before it runs for the first time, double check the variables at the top of `backup_mosquitto.sh` (`BASE_DIR`, `BACKUP_DIR`, etc.) match your actual paths — adjust with `nano backup_mosquitto.sh` if needed.

### Disabling automated backups

If you don't want the nightly backup, remove the cron entry:

```bash
crontab -l | grep -vF "backup_mosquitto.sh" | crontab -
```

---

## 8. Final verification

```bash
# Are the containers running and healthy?
docker ps -a

# Does the broker's TLS port respond?
openssl s_client -connect localhost:8883 -CAfile ./mosquitto/certs/ca.crt
```

---

## Kit contents

| File | Purpose |
|---|---|
| `docker-compose.yml` | Orchestrates the whole stack |
| `.env.example` | Environment variable template |
| `generate_certs.sh` | Generates self-signed TLS certificates |
| `backup_mosquitto.sh` | Automated MQTT broker backup |
| `mosquitto/config/mosquitto.conf` | Broker configuration |
| `mosquitto/config/dynamic-security.json.example` | User/role template |
| `api/`, `ingestor/`, `timescaledb/` | Source code for each service |

---

## Security notes

- `.env` and real certificates are **never** included in the kit — they're generated/configured per installation.
- Never share your `.env` or your generated `.key` files.
- Periodically check that your backups (`backup_mosquitto.sh`) are actually running.
