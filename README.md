# ThingsBoard ↔ Wappsto Bridge

A POSIX C++17 daemon that bridges **ThingsBoard** IoT platform (via MQTT) and **Wappsto** (via JSON-RPC over mTLS).

## Architecture

```
ThingsBoard MQTT Broker          Wappsto (wappsto.com)
        │                                │
  v1/gateway/#  ◄──────────────►  JSON-RPC / TLS
        │                                │
        └──────────  Bridge  ────────────┘
                      │
               config.json
```

### Data flow

| Direction | Protocol | Description |
|-----------|----------|-------------|
| ThingsBoard → Wappsto | MQTT → JSON-RPC | Telemetry/attributes become Wappsto Report states |
| Wappsto → ThingsBoard | JSON-RPC → MQTT | Control state changes trigger ThingsBoard RPC calls |

## Prerequisites

```bash
# Debian / Ubuntu
apt install libmosquitto-dev libssl-dev cmake build-essential

# Fedora / RHEL
dnf install mosquitto-devel openssl-devel cmake gcc-c++
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary will be at `build/tb_wappsto_bridge`.

## Configuration

Copy `config.example.json` to `config.json` and edit it:

```bash
cp config.example.json config.json
```

### ThingsBoard settings

| Key | Description |
|-----|-------------|
| `host` | MQTT broker hostname |
| `port` | MQTT broker port (default 1883, 8883 for TLS) |
| `username` | Gateway access token |
| `subscribe_topics` | MQTT topics to subscribe (default `["v1/gateway/#"]`) |
| `telemetry_topic` | Topic for device telemetry |
| `rpc_topic` | Topic for RPC commands |

### Wappsto settings

Download your IoT certificates from the **IoT Certificate Manager** app on [wappsto.com](https://wappsto.com) and place them in `certs/`:

```
certs/
  ca.crt
  client.crt
  client.key
```

Set `"network_uuid": "auto"` to read the UUID from the client certificate CN.

### Mapping modes

**Dynamic** (default): Devices and values are auto-created in Wappsto as telemetry arrives from ThingsBoard.

**Static**: Define explicit device/value mappings in `config.json`. Only listed keys are forwarded.

Mixed: Use `"mode": "dynamic"` and list specific devices in `"devices"` to override defaults for those devices.

#### Control mappings (Wappsto → ThingsBoard RPC)

For values with `"permission": "rw"`, set `rpc_method` to the ThingsBoard RPC method name:

```json
{
  "tb_key":   "setpoint",
  "permission": "rw",
  "rpc_method": "setSetpoint",
  "rpc_params_template": "{\"value\": {{value}}}"
}
```

When the value is changed in Wappsto, the bridge sends:
```json
{
  "device": "DeviceName",
  "data": { "id": 1, "method": "setSetpoint", "params": {"value": 42} }
}
```
on topic `v1/gateway/rpc`.

## Run

```bash
./build/tb_wappsto_bridge -c config.json -l info
```

Options:
- `-c <file>` – config file path (default: `config.json`)
- `-l <level>` – log level: `debug|info|warn|error`
- `-h` – help

## ThingsBoard Gateway MQTT topics

| Topic | Direction | Description |
|-------|-----------|-------------|
| `v1/gateway/connect` | TB→Bridge | Device connected |
| `v1/gateway/disconnect` | TB→Bridge | Device disconnected |
| `v1/gateway/telemetry` | TB→Bridge | Telemetry data |
| `v1/gateway/attributes` | TB→Bridge | Attribute updates |
| `v1/gateway/rpc` | Both | RPC commands / responses |

## Wappsto data model

```
Network (one per certificate)
  └── Device (one per ThingsBoard device)
        └── Value (one per telemetry key)
              ├── Report state (current reading from ThingsBoard)
              └── Control state (desired value set from Wappsto)
```
