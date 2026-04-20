#pragma once

#include "Config.hpp"
#include "ThingsBoardClient.hpp"
#include "WappstoClient.hpp"

#include <string>
#include <map>
#include <mutex>

// -----------------------------------------------------------
// TbGatewayServer
//
// Implements the ThingsBoard-server side of the MQTT protocol
// from the point of view of a ThingsBoard IoT Gateway device.
//
// The gateway (a Python process running in Docker, for example)
// needs a ThingsBoard-compatible endpoint to:
//   - negotiate session limits (getSessionLimits RPC)
//   - fetch its own configuration (attributes/request)
//   - receive configuration updates (shared attribute push)
//
// We get rid of the real ThingsBoard and serve all of this from
// Wappsto: each config type (general_configuration, OPCUA, ...)
// is a Wappsto Value with rw permission, whose control state
// holds the JSON payload the user edits in the Wappsto UI.
//
// Data flow:
//
//   Gateway  ───► v1/devices/me/rpc/request/1 ───► Bridge
//   Bridge   ───► v1/devices/me/rpc/response/1 ──► Gateway   (session limits, hardcoded)
//
//   Gateway  ───► v1/devices/me/attributes/request/N ───► Bridge
//   Bridge   ───► read control-state data from Wappsto for each tb_key
//   Bridge   ───► v1/devices/me/attributes/response/N ───► Gateway
//
//   Wappsto  ───► PUT /state/{control_uuid} ───► Bridge    (user edited config)
//   Bridge   ───► v1/devices/me/attributes ──────► Gateway (shared attr update)
// -----------------------------------------------------------
class TbGatewayServer {
public:
    TbGatewayServer(const Config& cfg,
                    ThingsBoardClient& tb,
                    WappstoClient& wappsto);

    // Provision the "Gateway Config" device and its values in Wappsto
    // (idempotent — uses deterministic UUIDs). Also seeds the cache
    // with the current control-state data.
    bool start();

    // Dispatch a TB MQTT message. Returns true if it was handled (so
    // the Bridge can skip its own telemetry/gateway-device logic).
    bool onTbMessage(const TbMessage& msg);

    // Notification that a Wappsto control state was updated. Returns
    // true if the control state belongs to a config entry and was
    // handled here (shared attribute pushed to gateway).
    bool onWappstoControl(const std::string& controlStateUuid,
                          const std::string& data);

private:
    const Config&        m_cfg;
    ThingsBoardClient&   m_tb;
    WappstoClient&       m_wappsto;

    // One runtime record per configured tb_key
    struct ConfigEntry {
        std::string tb_key;
        std::string wappsto_value_uuid;
        std::string report_state_uuid;
        std::string control_state_uuid;
        bool        shared = false;
        std::string cached_json;   // current JSON payload
    };

    std::mutex m_mutex;
    std::map<std::string, ConfigEntry> m_byTbKey;             // tb_key -> entry
    std::map<std::string, std::string> m_byControlState;      // control_state_uuid -> tb_key

    // Handlers
    void handleRpcRequest(const std::string& topic, const std::string& payload);
    void handleAttrRequest(const std::string& topic, const std::string& payload);

    // Build response payload for a given set of keys. If keys is empty,
    // returns every non-shared entry (the gateway's initial load). If
    // `sharedOnly` is true, returns only entries with shared=true.
    std::string buildAttrResponse(const std::vector<std::string>& keys,
                                  bool sharedOnly);

    // Publish a "shared attribute update" to the gateway (push pattern)
    void publishSharedUpdate(const std::string& tb_key,
                             const std::string& json);

    // Extract the request id from topics like ".../request/N"
    static std::string extractRequestId(const std::string& topic,
                                        const std::string& prefix);
};
