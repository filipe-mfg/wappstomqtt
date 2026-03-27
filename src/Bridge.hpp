#pragma once

#include "Config.hpp"
#include "ThingsBoardClient.hpp"
#include "WappstoClient.hpp"
#include <string>
#include <map>
#include <mutex>
#include <memory>

// -----------------------------------------------------------
// Runtime state for a single mapped value
// -----------------------------------------------------------
struct RuntimeValue {
    std::string tb_key;
    std::string wappsto_value_uuid;
    std::string report_state_uuid;
    std::string control_state_uuid;
    std::string rpc_method;
    std::string rpc_params_template;
};

// -----------------------------------------------------------
// Runtime state for a single mapped device
// -----------------------------------------------------------
struct RuntimeDevice {
    std::string tb_device_name;
    std::string wappsto_device_uuid;

    // keyed by ThingsBoard telemetry key
    std::map<std::string, RuntimeValue> valuesByTbKey;

    // keyed by Wappsto control_state_uuid (for reverse mapping)
    std::map<std::string, RuntimeValue*> valuesByControlState;
};

// -----------------------------------------------------------
// Bridge
//
// Orchestrates ThingsBoardClient ↔ WappstoClient.
//
//  TB telemetry  →  Wappsto Report state
//  TB RPC        ←  Wappsto Control state
// -----------------------------------------------------------
class Bridge {
public:
    explicit Bridge(const Config& cfg);
    ~Bridge() = default;

    // Start both clients
    bool start();

    // Block until stopped
    void run();

    // Signal graceful shutdown
    void stop();

private:
    Config m_cfg;
    std::unique_ptr<ThingsBoardClient> m_tb;
    std::unique_ptr<WappstoClient>     m_wappsto;

    std::mutex m_devicesMutex;
    // keyed by ThingsBoard device name
    std::map<std::string, RuntimeDevice> m_devices;

    std::atomic<bool> m_running{false};

    // ThingsBoard message handlers
    void onTbMessage(const TbMessage& msg);
    void handleTelemetry(const std::string& payload);
    void handleConnect(const std::string& payload);
    void handleRpcFromServer(const std::string& payload);

    // Wappsto control handler
    void onWappstoControl(const WappstoControl& ctrl);

    // Device/value provisioning
    RuntimeDevice& ensureDevice(const std::string& tbDevice,
                                const std::string& preferredUuid = "",
                                const std::string& preferredName = "");

    void ensureValue(RuntimeDevice& dev, const std::string& tbKey,
                     const ValueMapping* hint = nullptr);

    // ThingsBoard RPC publish
    void sendTbRpc(const std::string& tbDevice,
                   const std::string& method,
                   const std::string& paramsJson);

    // Timestamp from ThingsBoard (ms epoch) to ISO 8601
    static std::string msToISO8601(int64_t ms);
};
