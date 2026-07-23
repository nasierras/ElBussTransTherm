# Work Projects (WP2)
## Raspberry Pi Setup Guide — MQTT Broker & ESP32 Network (WP2.5)

This guide documents the steps completed so far to turn the Raspberry Pi into the central MQTT broker and local network hub for all WP2.x ESP32 nodes.

**Target device:** Raspberry Pi 5, Raspberry Pi OS

**LAN subnet for ESP32 nodes:** `192.168.1.0/24`

**Pi's static LAN address:** `192.168.1.100`

## 1. Static IP on the Ethernet interface (`eth0`)
The Pi's Ethernet interface must be pinned to a fixed address, since every ESP32 node's firmware hardcodes `192.168.1.100` as the MQTT broker address.
Check which system manages your network first:

```bash
ls /etc/netplan/
```

If this Pi shows netplan-managed connections (e.g. `netplan-eth0`) but with a `passthrough:` block inside the `.yaml` file, NetworkManager is still the actual source of truth — configure via `nmcli`, not by hand-editing the yaml:

```bash
sudo nmcli connection modify netplan-eth0 ipv4.method manual ipv4.addresses 192.168.1.100/24
sudo nmcli connection down netplan-eth0
sudo nmcli connection up netplan-eth0
```

**Verify:**
```bash
ip addr show eth0
```
Expect to see `192.168.1.100/24` listed.

> No gateway is configured on `eth0` intentionally — this interface stays purely local. Internet uplink is handled separately by the SIM7600G-H (see Section 4, not yet configured).

## 2. DHCP for ESP32 nodes (`dnsmasq`)
The ESP32 nodes DHCP-request an address on boot, so the Pi needs to hand one out.
```bash
sudo apt install dnsmasq
```

Create `/etc/dnsmasq.d/esp32-lan.conf`:

```
interface=eth0
bind-interfaces
dhcp-range=192.168.1.101,192.168.1.150,255.255.255.0,24h
port=0
```

- `port=0` disables dnsmasq's DNS resolver — only DHCP is needed, since ESP32 firmware uses a hardcoded broker IP, not a hostname.

```bash
sudo systemctl enable --now dnsmasq
sudo systemctl restart dnsmasq   # required after creating/editing the conf file
```

**Verify DNS is actually off (should print nothing dnsmasq-related):**
```bash
sudo ss -lunp | grep :53
```

**Verify a real lease when a node boots:**
```bash
sudo journalctl -u dnsmasq -f
```
Expect a `DHCPDISCOVER` → `DHCPOFFER` → `DHCPREQUEST` → `DHCPACK` sequence with an address in `192.168.1.101–150`.

## 3. MQTT Broker (Mosquitto)
```bash
sudo apt install mosquitto mosquitto-clients
```

Create `/etc/mosquitto/conf.d/local.conf`:
```
listener 1883 192.168.1.100
listener 1883 127.0.0.1
allow_anonymous true
```

> ⚠️ **Known pitfall:** by default, Mosquitto only binds to `127.0.0.1` (localhost), which silently rejects every connection from the ESP32 nodes with a TCP reset (`Connection reset by peer` in the ESP32 logs), even though the broker itself is "running" and looks healthy from `systemctl status`. The explicit `listener 1883 192.168.1.100` line above is required for network clients to reach it at all.

```bash
sudo systemctl enable --now mosquitto
sudo systemctl restart mosquitto   # required after editing the listener config
```

**Verify it's listening on the LAN address, not just localhost:**
```bash
sudo ss -tlnp | grep 1883
```
Expect two lines: one for `192.168.1.100:1883` and one for `127.0.0.1:1883`.

**Manually confirm messages arrive (subscribe from the Pi itself):**
```bash
mosquitto_sub -h 192.168.1.100 -t "bus/test/heartbeat"
```

## 4. Validated end-to-end
A minimal "dummy" ESP32 test node (Ethernet + MQTT only, no sensors) was used to confirm the full chain:

**Link up → DHCP lease → IP obtained → TCP reaches broker → MQTT connects → publish succeeds**, once per second, with no drops.

