#pragma once

#include "Config.hpp"
#include <string>
#include <functional>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <openssl/ssl.h>

// -----------------------------------------------------------
// Wappsto object model
// -----------------------------------------------------------

struct WappstoState {
    std::string uuid;
    std::string type;       // "Report" | "Control"
    std::string data;
    std::string timestamp;
};

struct WappstoValue {
    std::string uuid;
    std::string name;
    std::string type;       // "number" | "string" | "blob"
    std::string permission; // "r" | "w" | "rw"
    double      min  = 0;
    double      max  = 100;
    double      step = 1;
    std::string unit;

    std::string report_state_uuid;
    std::string control_state_uuid;
};

struct WappstoDevice {
    std::string uuid;
    std::string name;
    std::map<std::string, WappstoValue> values; // keyed by value name
};

struct WappstoNetwork {
    std::string uuid;
    std::string name;
    std::map<std::string, WappstoDevice> devices; // keyed by device name
};

// -----------------------------------------------------------
// Control event from Wappsto (a "Control" state was updated)
// -----------------------------------------------------------
struct WappstoControl {
    std::string state_uuid;
    std::string data;
    std::string timestamp;
};

// -----------------------------------------------------------
// WappstoClient
//
// Connects to Wappsto via mTLS and exchanges JSON-RPC 2.0
// messages to manage a Network/Device/Value/State hierarchy.
// -----------------------------------------------------------
class WappstoClient {
public:
    using ControlCallback = std::function<void(const WappstoControl&)>;

    explicit WappstoClient(const WappstoConfig& cfg);
    ~WappstoClient();

    WappstoClient(const WappstoClient&) = delete;
    WappstoClient& operator=(const WappstoClient&) = delete;

    void setControlCallback(ControlCallback cb) { m_onControl = std::move(cb); }

    // Connect, provision network, start receive loop
    bool start(const std::string& networkName = "TB-Wappsto Bridge");
    void stop();

    bool isConnected() const { return m_connected; }

    // Provision or update a device; returns device uuid
    std::string ensureDevice(const std::string& name,
                             const std::string& preferredUuid = "");

    // Provision or update a value on a device; returns value uuid
    std::string ensureValue(const std::string& deviceUuid,
                            const WappstoValue& val);

    // Report a new measurement for a value (Report state)
    bool reportValue(const std::string& stateUuid,
                     const std::string& data,
                     const std::string& timestamp = "");

    // Fetch the current data payload of a state (GET /state/{uuid})
    // Returns empty string on error
    std::string getStateData(const std::string& stateUuid);

    // Deterministic UUID generation (v5 SHA-1 based).
    //   namespace: any UUID (we use the network UUID as root)
    //   name:      any string, e.g. "Gateway Config", "OPCUA", "Report"
    // Same inputs always produce the same output UUID, so restarts
    // reuse the same Wappsto objects.
    static std::string uuid5(const std::string& namespaceUuid,
                             const std::string& name);

    const std::string& networkUuid() const { return m_net.uuid; }
    const WappstoNetwork& network() const { return m_net; }

private:
    WappstoConfig    m_cfg;
    SSL_CTX*         m_ctx  = nullptr;
    SSL*             m_ssl  = nullptr;
    int              m_sock = -1;
    std::thread      m_recvThread;
    std::thread      m_pingThread;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};

    WappstoNetwork   m_net;
    std::mutex       m_sendMutex;
    ControlCallback  m_onControl;

    // Pending RPC calls waiting for response
    struct PendingRpc {
        std::string  result;
        bool         done   = false;
        bool         ok     = false;
        std::condition_variable cv;
    };
    std::mutex m_rpcMutex;
    std::map<std::string, std::shared_ptr<PendingRpc>> m_pending;

    // Low-level I/O
    bool tlsConnect();
    void tlsDisconnect();
    bool sendLine(const std::string& line);
    std::string recvLine();

    // JSON-RPC helpers
    std::string sendRpc(const std::string& method,
                        const std::string& url,
                        const std::string& dataJson = "{}",
                        int timeoutSec = -1);
    void sendReply(const std::string& id, bool ok,
                   const std::string& resultJson = "\"ok\"");

    // Background threads
    void recvLoop();
    void pingLoop();

    // Message dispatcher
    void handleMessage(const std::string& json);

    // UUID utilities
    static std::string genUUID();
    static std::string nowISO8601();

    // Provision helpers
    bool provisionNetwork(const std::string& name);
};
