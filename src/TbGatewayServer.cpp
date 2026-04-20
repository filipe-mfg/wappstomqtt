#include "TbGatewayServer.hpp"
#include "Logger.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

// -----------------------------------------------------------

TbGatewayServer::TbGatewayServer(const Config& cfg,
                                 ThingsBoardClient& tb,
                                 WappstoClient& wappsto)
    : m_cfg(cfg), m_tb(tb), m_wappsto(wappsto)
{
}

// -----------------------------------------------------------
// Provision device + values, prime the cache
// -----------------------------------------------------------

bool TbGatewayServer::start() {
    if (!m_cfg.gateway_config.enabled) {
        Logger::info("[TbSrv] Disabled in config");
        return true;
    }
    if (m_cfg.gateway_config.entries.empty()) {
        Logger::warn("[TbSrv] Enabled but no entries configured");
        return true;
    }

    // Create the Wappsto device that groups all config values
    std::string deviceUuid = m_wappsto.ensureDevice(m_cfg.gateway_config.wappsto_device_name);
    if (deviceUuid.empty()) {
        Logger::error("[TbSrv] Failed to provision config device");
        return false;
    }

    // Provision one Wappsto value per config entry
    for (const auto& e : m_cfg.gateway_config.entries) {
        WappstoValue wv;
        wv.name       = e.wappsto_value_name;
        wv.type       = "blob";      // JSON config — can exceed 64-byte string limit
        wv.permission = "rw";

        std::string valueUuid = m_wappsto.ensureValue(deviceUuid, wv);
        if (valueUuid.empty()) {
            Logger::error("[TbSrv] Failed to provision value: %s",
                          e.wappsto_value_name.c_str());
            continue;
        }

        // Find the state UUIDs produced by ensureValue
        ConfigEntry entry;
        entry.tb_key             = e.tb_key;
        entry.wappsto_value_uuid = valueUuid;
        entry.shared             = e.shared;

        const auto& net = m_wappsto.network();
        for (const auto& [dn, dd] : net.devices) {
            if (dd.uuid != deviceUuid) continue;
            auto vit = dd.values.find(wv.name);
            if (vit != dd.values.end()) {
                entry.report_state_uuid  = vit->second.report_state_uuid;
                entry.control_state_uuid = vit->second.control_state_uuid;
            }
        }

        // Load initial cache value with clear priority:
        //   1. Control state — user-set desired config (never overwritten by bridge)
        //   2. Report state  — last known running config (written by handleAttrBroadcast)
        //   3. default_json  — only when BOTH are "NA" (truly first time ever)
        //
        // This ensures bridge restarts never clobber configs the user or gateway set.
        std::string ctrlData = m_wappsto.getStateData(entry.control_state_uuid);
        std::string rptData  = m_wappsto.getStateData(entry.report_state_uuid);

        std::string cur;
        if (!ctrlData.empty() && ctrlData != "NA") {
            cur = ctrlData;
            Logger::info("[TbSrv] %s: using control-state config", e.tb_key.c_str());
        } else if (!rptData.empty() && rptData != "NA") {
            cur = rptData;
            Logger::info("[TbSrv] %s: using report-state config (gateway's last known)",
                         e.tb_key.c_str());
        } else {
            cur = e.default_json;
            Logger::info("[TbSrv] %s: seeding control state with default (first run)",
                         e.tb_key.c_str());
            // Write default only to control state as a starting point for the user.
            // Report state is left NA — will be set by handleAttrBroadcast.
            if (!entry.control_state_uuid.empty())
                m_wappsto.reportValue(entry.control_state_uuid, cur);
        }
        entry.cached_json = cur;

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_byTbKey[e.tb_key] = entry;
            if (!entry.control_state_uuid.empty())
                m_byControlState[entry.control_state_uuid] = e.tb_key;
        }
        Logger::info("[TbSrv] Loaded %s (value=%s)",
                     e.tb_key.c_str(), valueUuid.c_str());
    }

    return true;
}

// -----------------------------------------------------------
// TB → Bridge dispatch
// -----------------------------------------------------------

bool TbGatewayServer::onTbMessage(const TbMessage& msg) {
    if (!m_cfg.gateway_config.enabled) return false;

    const auto& tb = m_cfg.thingsboard;

    if (msg.topic.rfind(tb.dev_rpc_request_prefix, 0) == 0) {
        handleRpcRequest(msg.topic, msg.payload);
        return true;
    }
    if (msg.topic.rfind(tb.dev_attr_request_prefix, 0) == 0) {
        handleAttrRequest(msg.topic, msg.payload);
        return true;
    }
    if (msg.topic == tb.dev_attr_topic) {
        // Gateway reporting its running config — mirror into cache + Wappsto
        // report states so subsequent attribute requests return the real values.
        handleAttrBroadcast(msg.payload);
        return true;
    }
    if (msg.topic == tb.dev_telemetry_topic) {
        Logger::debug("[TbSrv] Gateway self-telemetry: %s", msg.payload.c_str());
        return true;  // consumed — Bridge shouldn't forward this
    }
    return false;
}

// -----------------------------------------------------------
// RPC from gateway  (mainly: getSessionLimits)
// -----------------------------------------------------------

void TbGatewayServer::handleRpcRequest(const std::string& topic,
                                       const std::string& payload) {
    std::string reqId = extractRequestId(topic, m_cfg.thingsboard.dev_rpc_request_prefix);
    Logger::info("[TbSrv] RPC request id=%s payload=%s", reqId.c_str(), payload.c_str());

    auto j = json::parse(payload, nullptr, false);
    std::string method;
    if (!j.is_discarded() && j.contains("method"))
        method = j["method"].get<std::string>();

    json resp;
    if (method == "getSessionLimits") {
        const auto& sl = m_cfg.gateway_config.session_limits;
        resp = {
            {"maxPayloadSize",      sl.maxPayloadSize},
            {"maxInflightMessages", sl.maxInflightMessages},
            {"rateLimits",        {{"messages", nullptr},
                                   {"telemetryMessages", nullptr},
                                   {"telemetryDataPoints", nullptr}}},
            {"gatewayRateLimits", {{"messages", nullptr},
                                   {"telemetryMessages", nullptr},
                                   {"telemetryDataPoints", nullptr}}}
        };
    } else {
        Logger::warn("[TbSrv] Unknown RPC method '%s' — empty response", method.c_str());
        resp = json::object();
    }

    std::string respTopic = m_cfg.thingsboard.dev_rpc_response_prefix + reqId;
    m_tb.publish(respTopic, resp.dump());
}

// -----------------------------------------------------------
// Attribute request from gateway
//   body vary:
//     {}                                → all client attributes
//     {"sharedKeys": "OPCUA"}           → specific shared attrs
//     {"clientKeys": "x,y", "sharedKeys": "z"}
// -----------------------------------------------------------

void TbGatewayServer::handleAttrRequest(const std::string& topic,
                                        const std::string& payload) {
    std::string reqId = extractRequestId(topic, m_cfg.thingsboard.dev_attr_request_prefix);
    Logger::info("[TbSrv] Attr request id=%s payload=%s",
                 reqId.c_str(), payload.c_str());

    auto j = json::parse(payload, nullptr, false);
    if (j.is_discarded()) j = json::object();

    auto splitKeys = [](const std::string& csv) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : csv) {
            if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };

    std::vector<std::string> clientKeys, sharedKeys;
    if (j.contains("clientKeys") && j["clientKeys"].is_string())
        clientKeys = splitKeys(j["clientKeys"].get<std::string>());
    if (j.contains("sharedKeys") && j["sharedKeys"].is_string())
        sharedKeys = splitKeys(j["sharedKeys"].get<std::string>());

    json resp = json::object();

    if (!sharedKeys.empty()) {
        json shared = json::object();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (const auto& k : sharedKeys) {
                auto it = m_byTbKey.find(k);
                if (it == m_byTbKey.end()) continue;
                auto cj = json::parse(it->second.cached_json, nullptr, false);
                shared[k] = cj.is_discarded() ? json(it->second.cached_json) : cj;
            }
        }
        resp["shared"] = shared;
    }

    // Client attributes (including the empty-body case)
    bool wantAllClient = sharedKeys.empty() && clientKeys.empty();
    if (wantAllClient || !clientKeys.empty()) {
        json client = json::object();
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            for (auto& [k, e] : m_byTbKey) {
                if (e.shared) continue;
                if (!wantAllClient &&
                    std::find(clientKeys.begin(), clientKeys.end(), k) == clientKeys.end())
                    continue;
                auto cj = json::parse(e.cached_json, nullptr, false);
                client[k] = cj.is_discarded() ? json(e.cached_json) : cj;
            }
        }
        // When only client keys are requested the payload is the client map
        // at the root level. Otherwise it's nested under "client".
        if (wantAllClient && sharedKeys.empty()) {
            resp = client;
        } else {
            resp["client"] = client;
        }
    }

    std::string respTopic = m_cfg.thingsboard.dev_attr_response_prefix + reqId;
    m_tb.publish(respTopic, resp.dump());
    Logger::debug("[TbSrv] Attr response (%zu bytes) → %s",
                  resp.dump().size(), respTopic.c_str());
}

// -----------------------------------------------------------
// Wappsto control update → shared attribute push to gateway
// -----------------------------------------------------------

bool TbGatewayServer::onWappstoControl(const std::string& controlStateUuid,
                                       const std::string& data) {
    std::string key;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_byControlState.find(controlStateUuid);
        if (it == m_byControlState.end()) return false;
        key = it->second;
        m_byTbKey[key].cached_json = data;
    }

    Logger::info("[TbSrv] Config '%s' updated from Wappsto (%zu bytes)",
                 key.c_str(), data.size());

    publishSharedUpdate(key, data);

    // Mirror into report state so the UI shows what's actually in use
    auto it = m_byTbKey.find(key);
    if (it != m_byTbKey.end() && !it->second.report_state_uuid.empty())
        m_wappsto.reportValue(it->second.report_state_uuid, data);

    return true;
}

// -----------------------------------------------------------
// Publish shared attribute update (push pattern)
// -----------------------------------------------------------

void TbGatewayServer::publishSharedUpdate(const std::string& tb_key,
                                          const std::string& jsonData) {
    auto cj = json::parse(jsonData, nullptr, false);
    json payload;
    payload[tb_key] = cj.is_discarded() ? json(jsonData) : cj;
    m_tb.publish(m_cfg.thingsboard.dev_attr_topic, payload.dump());
    Logger::debug("[TbSrv] Pushed shared update for %s", tb_key.c_str());
}

// -----------------------------------------------------------
// Mirror gateway's running config into cache + Wappsto report states
// -----------------------------------------------------------

void TbGatewayServer::handleAttrBroadcast(const std::string& payload) {
    auto j = json::parse(payload, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [key, val] : j.items()) {
        if (key == "ts") continue;   // ThingsBoard timestamp field, not a config key

        auto it = m_byTbKey.find(key);
        if (it == m_byTbKey.end()) continue;

        std::string valStr = val.is_string() ? val.get<std::string>() : val.dump();
        it->second.cached_json = valStr;

        Logger::info("[TbSrv] Cache updated from gateway broadcast: %s", key.c_str());

        // Mirror into Wappsto report state so the UI shows what's really running
        if (!it->second.report_state_uuid.empty())
            m_wappsto.reportValue(it->second.report_state_uuid, valStr);
    }
}

// -----------------------------------------------------------

std::string TbGatewayServer::extractRequestId(const std::string& topic,
                                              const std::string& prefix) {
    if (topic.size() <= prefix.size()) return "0";
    return topic.substr(prefix.size());
}
