#include "Bridge.hpp"
#include "Logger.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <csignal>
#include <ctime>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

// -----------------------------------------------------------
// Construction
// -----------------------------------------------------------

Bridge::Bridge(const Config& cfg)
    : m_cfg(cfg)
    , m_tb(std::make_unique<ThingsBoardClient>(cfg.thingsboard))
    , m_wappsto(std::make_unique<WappstoClient>(cfg.wappsto))
{
}

// -----------------------------------------------------------
// Start
// -----------------------------------------------------------

bool Bridge::start() {
    // Pre-populate static device mappings so we have UUIDs ready
    if (m_cfg.mapping.mode == MappingMode::STATIC) {
        for (const auto& dm : m_cfg.mapping.devices) {
            auto& rdev = ensureDevice(dm.tb_device,
                                      dm.wappsto_device_uuid,
                                      dm.wappsto_device_name);
            for (const auto& vm : dm.values) {
                ensureValue(rdev, vm.tb_key, &vm);
            }
        }
    }

    // Wire callbacks
    m_tb->setMessageCallback([this](const TbMessage& msg) { onTbMessage(msg); });
    m_wappsto->setControlCallback([this](const WappstoControl& c) { onWappstoControl(c); });

    // Start Wappsto first (so devices are ready before TB messages arrive)
    Logger::info("[Bridge] Starting Wappsto client…");
    if (!m_wappsto->start("ThingsBoard-Wappsto Bridge")) {
        Logger::error("[Bridge] Failed to start Wappsto client");
        return false;
    }

    // Static mode: provision devices/values in Wappsto right away
    if (m_cfg.mapping.mode == MappingMode::STATIC) {
        std::lock_guard<std::mutex> lk(m_devicesMutex);
        for (auto& [name, rdev] : m_devices) {
            std::string devUuid = m_wappsto->ensureDevice(
                m_cfg.mapping.devices[0].wappsto_device_name.empty()
                    ? name : name,
                rdev.wappsto_device_uuid);
            rdev.wappsto_device_uuid = devUuid;

            for (auto& [key, rv] : rdev.valuesByTbKey) {
                WappstoValue wv;
                wv.uuid = rv.wappsto_value_uuid;
                wv.name = rv.tb_key;
                // Try to find hint from config
                for (const auto& dm : m_cfg.mapping.devices) {
                    if (dm.tb_device == name) {
                        for (const auto& vm : dm.values) {
                            if (vm.tb_key == key) {
                                wv.name       = vm.wappsto_value_name;
                                wv.type       = vm.type;
                                wv.min        = vm.min;
                                wv.max        = vm.max;
                                wv.step       = vm.step;
                                wv.unit       = vm.unit;
                                wv.permission = vm.permission;
                                break;
                            }
                        }
                    }
                }
                std::string vuuid = m_wappsto->ensureValue(devUuid, wv);
                rv.wappsto_value_uuid = vuuid;
                const auto& net = m_wappsto->network();
                for (const auto& [dn, dd] : net.devices) {
                    auto vit = dd.values.find(wv.name);
                    if (vit != dd.values.end()) {
                        rv.report_state_uuid  = vit->second.report_state_uuid;
                        rv.control_state_uuid = vit->second.control_state_uuid;
                        if (!rv.control_state_uuid.empty())
                            rdev.valuesByControlState[rv.control_state_uuid] = &rv;
                    }
                }
            }
        }
    }

    Logger::info("[Bridge] Starting ThingsBoard client…");
    if (!m_tb->start()) {
        Logger::error("[Bridge] Failed to start ThingsBoard client");
        return false;
    }

    m_running = true;
    return true;
}

// -----------------------------------------------------------
// Run (blocking)
// -----------------------------------------------------------

void Bridge::run() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void Bridge::stop() {
    m_running = false;
    m_tb->stop();
    m_wappsto->stop();
    Logger::info("[Bridge] Stopped");
}

// -----------------------------------------------------------
// ThingsBoard message dispatch
// -----------------------------------------------------------

void Bridge::onTbMessage(const TbMessage& msg) {
    const auto& tc = m_cfg.thingsboard;

    if (msg.topic == tc.telemetry_topic)
        handleTelemetry(msg.payload);
    else if (msg.topic == tc.connect_topic)
        handleConnect(msg.payload);
    else if (msg.topic == tc.rpc_topic)
        handleRpcFromServer(msg.payload);
    // attributes and disconnect are not forwarded by default
}

// -----------------------------------------------------------
// Telemetry: TB → Wappsto
//
// TB payload format:
// {
//   "DeviceName": [
//     { "ts": 1234567890000, "values": { "key": value, ... } },
//     ...
//   ]
// }
// -----------------------------------------------------------
void Bridge::handleTelemetry(const std::string& payload) {
    auto j = json::parse(payload, nullptr, false);
    if (j.is_discarded()) {
        Logger::warn("[Bridge] Telemetry: invalid JSON");
        return;
    }

    for (auto& [deviceName, entries] : j.items()) {
        if (!entries.is_array()) continue;

        std::lock_guard<std::mutex> lk(m_devicesMutex);
        auto& rdev = ensureDevice(deviceName);

        for (auto& entry : entries) {
            int64_t ts = entry.value("ts", 0LL);
            std::string tsStr = ts > 0 ? msToISO8601(ts) : "";

            if (!entry.contains("values")) continue;
            auto& vals = entry["values"];

            for (auto& [key, val] : vals.items()) {
                ensureValue(rdev, key);
                auto it = rdev.valuesByTbKey.find(key);
                if (it == rdev.valuesByTbKey.end()) continue;
                RuntimeValue& rv = it->second;
                if (rv.report_state_uuid.empty()) continue;

                std::string dataStr;
                if (val.is_string())      dataStr = val.get<std::string>();
                else if (val.is_number()) dataStr = val.dump();
                else if (val.is_boolean()) dataStr = val.get<bool>() ? "1" : "0";
                else                       dataStr = val.dump();

                Logger::debug("[Bridge] %s/%s = %s", deviceName.c_str(),
                              key.c_str(), dataStr.c_str());
                m_wappsto->reportValue(rv.report_state_uuid, dataStr, tsStr);
            }
        }
    }
}

// -----------------------------------------------------------
// Device connect notification (dynamic mode)
//
// TB payload: { "device": "DeviceName", "type": "Device" }
// -----------------------------------------------------------
void Bridge::handleConnect(const std::string& payload) {
    auto j = json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.contains("device")) return;

    std::string devName = j["device"].get<std::string>();
    Logger::info("[Bridge] TB device connected: %s", devName.c_str());

    if (m_cfg.mapping.mode == MappingMode::DYNAMIC) {
        std::lock_guard<std::mutex> lk(m_devicesMutex);
        ensureDevice(devName);
    }
}

// -----------------------------------------------------------
// RPC from ThingsBoard server to gateway (bidirectional)
// We just log it; the real TB→gateway RPC is handled below.
// -----------------------------------------------------------
void Bridge::handleRpcFromServer(const std::string& payload) {
    Logger::debug("[Bridge] RPC from TB server: %s", payload.c_str());
    // No default action – this is for server-initiated RPCs
}

// -----------------------------------------------------------
// Wappsto Control → ThingsBoard RPC
// -----------------------------------------------------------
void Bridge::onWappstoControl(const WappstoControl& ctrl) {
    std::lock_guard<std::mutex> lk(m_devicesMutex);

    // Find which device/value owns this control state
    for (auto& [devName, rdev] : m_devices) {
        auto it = rdev.valuesByControlState.find(ctrl.state_uuid);
        if (it == rdev.valuesByControlState.end()) continue;

        RuntimeValue* rv = it->second;
        if (rv->rpc_method.empty()) {
            Logger::warn("[Bridge] Control for %s/%s has no rpc_method – ignoring",
                devName.c_str(), rv->tb_key.c_str());
            return;
        }

        // Build RPC params from template
        std::string params = rv->rpc_params_template;
        auto pos = params.find("{{value}}");
        if (pos != std::string::npos) params.replace(pos, 9, ctrl.data);

        Logger::info("[Bridge] Control → TB RPC: device=%s method=%s params=%s",
            devName.c_str(), rv->rpc_method.c_str(), params.c_str());

        sendTbRpc(devName, rv->rpc_method, params);
        return;
    }

    Logger::warn("[Bridge] Control state %s not mapped to any device",
                 ctrl.state_uuid.c_str());
}

// -----------------------------------------------------------
// ThingsBoard RPC publish
//
// TB Gateway RPC topic: v1/gateway/rpc
// Payload: { "device": "Name", "data": { "id": N, "method": "m", "params": {} } }
// -----------------------------------------------------------
void Bridge::sendTbRpc(const std::string& tbDevice,
                       const std::string& method,
                       const std::string& paramsJson) {
    static int rpcId = 1;

    json paramsJ = json::parse(paramsJson, nullptr, false);
    if (paramsJ.is_discarded()) paramsJ = json::object();

    json payload = {
        {"device", tbDevice},
        {"data",   {
            {"id",     rpcId++},
            {"method", method},
            {"params", paramsJ}
        }}
    };

    m_tb->publish(m_cfg.thingsboard.rpc_resp_topic, payload.dump());
}

// -----------------------------------------------------------
// Device / value provisioning helpers
// -----------------------------------------------------------

RuntimeDevice& Bridge::ensureDevice(const std::string& tbDevice,
                                    const std::string& preferredUuid,
                                    const std::string& preferredName) {
    auto it = m_devices.find(tbDevice);
    if (it != m_devices.end()) return it->second;

    RuntimeDevice rdev;
    rdev.tb_device_name     = tbDevice;
    rdev.wappsto_device_uuid = preferredUuid;

    // Provision in Wappsto (if connected)
    if (m_wappsto->isConnected()) {
        std::string name = preferredName.empty() ? tbDevice : preferredName;
        rdev.wappsto_device_uuid = m_wappsto->ensureDevice(name, preferredUuid);
    }

    m_devices[tbDevice] = std::move(rdev);
    return m_devices[tbDevice];
}

void Bridge::ensureValue(RuntimeDevice& dev, const std::string& tbKey,
                         const ValueMapping* hint) {
    if (dev.valuesByTbKey.count(tbKey)) return;

    // In STATIC mode, only create values that are in the config
    if (m_cfg.mapping.mode == MappingMode::STATIC && !hint) return;

    // In STATIC mode with no hint and auto_create_values off, skip
    if (!m_cfg.mapping.auto_create_values && !hint) return;

    RuntimeValue rv;
    rv.tb_key = tbKey;

    WappstoValue wv;
    wv.name = tbKey;
    wv.type = "number";
    wv.permission = m_cfg.mapping.default_permission;
    wv.min  = m_cfg.mapping.default_min;
    wv.max  = m_cfg.mapping.default_max;
    wv.step = m_cfg.mapping.default_step;

    if (hint) {
        wv.name       = hint->wappsto_value_name;
        wv.uuid       = hint->wappsto_value_uuid;
        wv.type       = hint->type;
        wv.min        = hint->min;
        wv.max        = hint->max;
        wv.step       = hint->step;
        wv.unit       = hint->unit;
        wv.permission = hint->permission;
        rv.rpc_method          = hint->rpc_method;
        rv.rpc_params_template = hint->rpc_params_template;
    }

    if (m_wappsto->isConnected() && !dev.wappsto_device_uuid.empty()) {
        rv.wappsto_value_uuid = m_wappsto->ensureValue(dev.wappsto_device_uuid, wv);

        // Fetch state UUIDs from network model
        const auto& net = m_wappsto->network();
        for (const auto& [dn, dd] : net.devices) {
            if (dd.uuid != dev.wappsto_device_uuid) continue;
            auto vit = dd.values.find(wv.name);
            if (vit != dd.values.end()) {
                rv.report_state_uuid  = vit->second.report_state_uuid;
                rv.control_state_uuid = vit->second.control_state_uuid;
            }
        }
    }

    if (!rv.control_state_uuid.empty())
        dev.valuesByControlState[rv.control_state_uuid] = nullptr; // placeholder

    dev.valuesByTbKey[tbKey] = std::move(rv);

    // Fix up the pointer (must point into the map)
    RuntimeValue& stored = dev.valuesByTbKey[tbKey];
    if (!stored.control_state_uuid.empty())
        dev.valuesByControlState[stored.control_state_uuid] = &stored;
}

// -----------------------------------------------------------
// Utilities
// -----------------------------------------------------------

std::string Bridge::msToISO8601(int64_t ms) {
    std::time_t sec = static_cast<std::time_t>(ms / 1000);
    long        msec = static_cast<long>(ms % 1000);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&sec));
    std::snprintf(buf + 19, sizeof(buf) - 19, ".%03ldZ", msec);
    return buf;
}
