#include "Config.hpp"
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Helper: get field with default
template<typename T>
static T jget(const json& j, const std::string& key, T def) {
    if (j.contains(key) && !j[key].is_null()) return j[key].get<T>();
    return def;
}

static TbTlsConfig parseTbTls(const json& j) {
    TbTlsConfig t;
    t.enabled     = jget<bool>(j,        "enabled",     false);
    t.ca_cert     = jget<std::string>(j, "ca_cert",     "");
    t.client_cert = jget<std::string>(j, "client_cert", "");
    t.client_key  = jget<std::string>(j, "client_key",  "");
    return t;
}

static ThingsBoardConfig parseThingsBoard(const json& j) {
    ThingsBoardConfig c;
    c.host       = jget<std::string>(j, "host",      c.host);
    c.port       = jget<int>(j,         "port",      c.port);
    c.username   = jget<std::string>(j, "username",  "");
    c.password   = jget<std::string>(j, "password",  "");
    c.client_id  = jget<std::string>(j, "client_id", c.client_id);
    c.keepalive_sec = jget<int>(j,      "keepalive_sec", c.keepalive_sec);

    if (j.contains("subscribe_topics") && j["subscribe_topics"].is_array()) {
        c.subscribe_topics.clear();
        for (auto& t : j["subscribe_topics"]) c.subscribe_topics.push_back(t.get<std::string>());
    }

    c.telemetry_topic  = jget<std::string>(j, "telemetry_topic",  c.telemetry_topic);
    c.attributes_topic = jget<std::string>(j, "attributes_topic", c.attributes_topic);
    c.connect_topic    = jget<std::string>(j, "connect_topic",    c.connect_topic);
    c.disconnect_topic = jget<std::string>(j, "disconnect_topic", c.disconnect_topic);
    c.rpc_topic        = jget<std::string>(j, "rpc_topic",        c.rpc_topic);
    c.rpc_resp_topic   = jget<std::string>(j, "rpc_resp_topic",   c.rpc_resp_topic);

    if (j.contains("tls")) c.tls = parseTbTls(j["tls"]);

    if (j.contains("api")) {
        auto& a = j["api"];
        c.api.enabled  = jget<bool>(a,        "enabled",  false);
        c.api.host     = jget<std::string>(a, "host",     "");
        c.api.port     = jget<int>(a,         "port",     8080);
        c.api.tls      = jget<bool>(a,        "tls",      false);
        c.api.username = jget<std::string>(a, "username", "");
        c.api.password = jget<std::string>(a, "password", "");
        c.api.timeout_sec = jget<int>(a,      "timeout_sec", 10);
    }
    // Default: use same host as MQTT broker
    if (c.api.host.empty()) c.api.host = c.host;

    return c;
}

static WappstoConfig parseWappsto(const json& j) {
    WappstoConfig c;
    c.host         = jget<std::string>(j, "host",         c.host);
    c.port         = jget<int>(j,         "port",         c.port);
    c.ca_cert      = jget<std::string>(j, "ca_cert",      c.ca_cert);
    c.client_cert  = jget<std::string>(j, "client_cert",  c.client_cert);
    c.client_key   = jget<std::string>(j, "client_key",   c.client_key);
    c.network_uuid = jget<std::string>(j, "network_uuid", c.network_uuid);
    c.rpc_timeout_sec   = jget<int>(j, "rpc_timeout_sec",   c.rpc_timeout_sec);
    c.ping_interval_sec = jget<int>(j, "ping_interval_sec", c.ping_interval_sec);
    return c;
}

static ValueMapping parseValue(const json& j) {
    ValueMapping v;
    v.tb_key              = jget<std::string>(j, "tb_key",              "");
    v.wappsto_value_name  = jget<std::string>(j, "wappsto_value_name",  v.tb_key);
    v.wappsto_value_uuid  = jget<std::string>(j, "wappsto_value_uuid",  "");
    v.type                = jget<std::string>(j, "type",                "number");
    v.min                 = jget<double>(j,      "min",                 0.0);
    v.max                 = jget<double>(j,      "max",                 100.0);
    v.step                = jget<double>(j,      "step",                1.0);
    v.unit                = jget<std::string>(j, "unit",                "");
    v.permission          = jget<std::string>(j, "permission",          "r");
    v.rpc_method          = jget<std::string>(j, "rpc_method",          "");
    v.rpc_params_template = jget<std::string>(j, "rpc_params_template", "{\"value\": {{value}}}");
    return v;
}

static DeviceMapping parseDevice(const json& j) {
    DeviceMapping d;
    d.tb_device           = jget<std::string>(j, "tb_device",           "");
    d.wappsto_device_name = jget<std::string>(j, "wappsto_device_name", d.tb_device);
    d.wappsto_device_uuid = jget<std::string>(j, "wappsto_device_uuid", "");

    if (j.contains("values") && j["values"].is_array()) {
        for (auto& v : j["values"]) d.values.push_back(parseValue(v));
    }
    return d;
}

static MappingConfig parseMapping(const json& j) {
    MappingConfig m;
    std::string mode = jget<std::string>(j, "mode", "dynamic");
    m.mode = (mode == "static") ? MappingMode::STATIC : MappingMode::DYNAMIC;

    m.auto_create_values = jget<bool>(j,        "auto_create_values", m.auto_create_values);
    m.default_min        = jget<double>(j,      "default_min",        m.default_min);
    m.default_max        = jget<double>(j,      "default_max",        m.default_max);
    m.default_step       = jget<double>(j,      "default_step",       m.default_step);
    m.default_permission = jget<std::string>(j, "default_permission", m.default_permission);

    if (j.contains("devices") && j["devices"].is_array()) {
        for (auto& d : j["devices"]) m.devices.push_back(parseDevice(d));
    }
    return m;
}

static GatewayConfigEntry parseGatewayEntry(const json& j) {
    GatewayConfigEntry e;
    e.tb_key             = jget<std::string>(j, "tb_key",             "");
    e.wappsto_value_name = jget<std::string>(j, "wappsto_value_name", e.tb_key);
    e.shared             = jget<bool>(j,        "shared",             false);
    // default_json can be either a JSON object/array or a JSON string
    if (j.contains("default_json")) {
        if (j["default_json"].is_string()) {
            e.default_json = j["default_json"].get<std::string>();
        } else {
            e.default_json = j["default_json"].dump();
        }
    }
    return e;
}

static GatewayConfigMapping parseGatewayConfig(const json& j) {
    GatewayConfigMapping g;
    g.enabled             = jget<bool>(j,        "enabled",             false);
    g.wappsto_device_name = jget<std::string>(j, "wappsto_device_name", g.wappsto_device_name);

    if (j.contains("session_limits")) {
        auto& s = j["session_limits"];
        g.session_limits.maxPayloadSize      = jget<int>(s, "maxPayloadSize",      65536);
        g.session_limits.maxInflightMessages = jget<int>(s, "maxInflightMessages", 100);
    }
    if (j.contains("entries") && j["entries"].is_array()) {
        for (auto& e : j["entries"]) g.entries.push_back(parseGatewayEntry(e));
    }
    return g;
}

Config Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open config file: " + path);

    json j;
    try {
        f >> j;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("JSON parse error: ") + e.what());
    }

    Config c;
    if (j.contains("thingsboard"))    c.thingsboard    = parseThingsBoard(j["thingsboard"]);
    if (j.contains("wappsto"))        c.wappsto        = parseWappsto(j["wappsto"]);
    if (j.contains("mapping"))        c.mapping        = parseMapping(j["mapping"]);
    if (j.contains("gateway_config")) c.gateway_config = parseGatewayConfig(j["gateway_config"]);
    c.log_level = jget<std::string>(j, "log_level", "info");

    if (c.thingsboard.host.empty())
        throw std::runtime_error("thingsboard.host is required");

    return c;
}
