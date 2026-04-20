#pragma once

#include <string>
#include <vector>
#include <optional>

// -----------------------------------------------------------
// ThingsBoard REST API configuration (used for RPC triggering)
// -----------------------------------------------------------
struct ThingsBoardApiConfig {
    bool        enabled  = false;
    std::string host;        // defaults to thingsboard.host
    int         port     = 8080;
    bool        tls      = false;
    std::string username;    // ThingsBoard user email
    std::string password;
    int         timeout_sec = 10;
};

// -----------------------------------------------------------
// ThingsBoard MQTT configuration
// -----------------------------------------------------------
struct TbTlsConfig {
    bool        enabled     = false;
    std::string ca_cert;
    std::string client_cert;
    std::string client_key;
};

struct ThingsBoardConfig {
    std::string host            = "localhost";
    int         port            = 1883;
    std::string username;           // Gateway access token (used as MQTT username)
    std::string password;
    std::string client_id       = "tb-wappsto-bridge";
    int         keepalive_sec   = 60;

    // Topics the bridge subscribes to on the broker. Defaults cover
    // both sides of the gateway-device protocol, so the bridge can
    // emulate the ThingsBoard server.
    std::vector<std::string> subscribe_topics = {
        "v1/devices/me/rpc/request/+",
        "v1/devices/me/attributes/request/+",
        "v1/devices/me/attributes",
        "v1/devices/me/telemetry",
        "v1/gateway/telemetry",
        "v1/gateway/attributes",
        "v1/gateway/attributes/request/+",
        "v1/gateway/connect",
        "v1/gateway/disconnect",
        "v1/gateway/rpc"
    };

    // Gateway-device (downstream) topics
    std::string telemetry_topic   = "v1/gateway/telemetry";
    std::string attributes_topic  = "v1/gateway/attributes";
    std::string connect_topic     = "v1/gateway/connect";
    std::string disconnect_topic  = "v1/gateway/disconnect";
    std::string rpc_topic         = "v1/gateway/rpc";
    std::string rpc_resp_topic    = "v1/gateway/rpc";

    // Gateway-itself (device) topics — used to emulate the TB server
    std::string dev_rpc_request_prefix     = "v1/devices/me/rpc/request/";
    std::string dev_rpc_response_prefix    = "v1/devices/me/rpc/response/";
    std::string dev_attr_request_prefix    = "v1/devices/me/attributes/request/";
    std::string dev_attr_response_prefix   = "v1/devices/me/attributes/response/";
    std::string dev_attr_topic             = "v1/devices/me/attributes";
    std::string dev_telemetry_topic        = "v1/devices/me/telemetry";

    TbTlsConfig        tls;
    ThingsBoardApiConfig api;
};

// -----------------------------------------------------------
// Wappsto connection configuration
// -----------------------------------------------------------
struct WappstoConfig {
    std::string host         = "wappsto.com";
    int         port         = 443;
    std::string ca_cert      = "certs/ca.crt";
    std::string client_cert  = "certs/client.crt";
    std::string client_key   = "certs/client.key";

    // UUID of the Wappsto network (read from cert CN if "auto")
    std::string network_uuid = "auto";

    int rpc_timeout_sec  = 5;
    int ping_interval_sec = 30;
};

// -----------------------------------------------------------
// Gateway configuration serving (ThingsBoard server-side protocol)
// -----------------------------------------------------------
//
// The bridge acts as a ThingsBoard server from the gateway's point
// of view. It serves configuration stored as values in Wappsto, so
// the operator can edit OPC-UA / Modbus / ... configs through the
// Wappsto UI (control state) and the gateway reloads automatically.
// -----------------------------------------------------------

struct GatewayConfigEntry {
    std::string tb_key;              // e.g. "general_configuration", "OPCUA"
    std::string wappsto_value_name;  // value name in Wappsto
    bool        shared = false;      // true = responds to request with "sharedKeys"
    std::string default_json = "{}"; // used to initialise the value if it's empty
};

struct SessionLimits {
    int maxPayloadSize            = 65536;
    int maxInflightMessages       = 100;
    // Rate limits left null (the gateway interprets null as "unlimited")
};

struct GatewayConfigMapping {
    bool   enabled = false;
    std::string wappsto_device_name = "Gateway Config";

    SessionLimits session_limits;

    // Attribute key → Wappsto value mapping
    std::vector<GatewayConfigEntry> entries;
};

// -----------------------------------------------------------
// Value mapping (ThingsBoard key ↔ Wappsto value)
// -----------------------------------------------------------
struct ValueMapping {
    std::string tb_key;              // ThingsBoard telemetry key
    std::string wappsto_value_name;  // Name shown in Wappsto
    std::string wappsto_value_uuid;  // Leave empty to auto-generate
    std::string type = "number";     // number | string | blob
    double      min  = 0.0;
    double      max  = 100.0;
    double      step = 1.0;
    std::string unit;
    std::string permission = "r";    // r | w | rw

    // When control changes in Wappsto, call this RPC method on the TB device
    std::string rpc_method;          // empty = no control mapping
    std::string rpc_params_template; // JSON template, use {{value}} placeholder
};

// -----------------------------------------------------------
// Device mapping (ThingsBoard device ↔ Wappsto device)
// -----------------------------------------------------------
struct DeviceMapping {
    std::string tb_device;           // ThingsBoard device name
    std::string wappsto_device_name; // Name shown in Wappsto
    std::string wappsto_device_uuid; // Leave empty to auto-generate
    std::vector<ValueMapping> values;
};

// -----------------------------------------------------------
// Mapping configuration
// -----------------------------------------------------------
enum class MappingMode { STATIC, DYNAMIC };

struct MappingConfig {
    MappingMode mode = MappingMode::DYNAMIC;

    // Used in DYNAMIC mode: auto-create Wappsto values for all telemetry keys
    bool   auto_create_values = true;
    double default_min        = -1e9;
    double default_max        =  1e9;
    double default_step       = 1.0;
    std::string default_permission = "r";

    // Used in STATIC mode (or to override dynamic behaviour for specific devices)
    std::vector<DeviceMapping> devices;
};

// -----------------------------------------------------------
// Top-level configuration
// -----------------------------------------------------------
struct Config {
    ThingsBoardConfig    thingsboard;
    WappstoConfig        wappsto;
    MappingConfig        mapping;
    GatewayConfigMapping gateway_config;
    std::string          log_level = "info";

    // Load from JSON file; throws std::runtime_error on failure
    static Config load(const std::string& path);
};
