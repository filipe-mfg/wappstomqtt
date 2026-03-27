#pragma once

#include "Config.hpp"
#include <string>
#include <functional>
#include <memory>
#include <atomic>

// Forward-declare mosquitto struct to avoid exposing the header
struct mosquitto;

// -----------------------------------------------------------
// Message received from ThingsBoard MQTT broker
// -----------------------------------------------------------
struct TbMessage {
    std::string topic;
    std::string payload;
};

// -----------------------------------------------------------
// ThingsBoardClient
//
// MQTT subscriber that connects to ThingsBoard broker using
// libmosquitto. Subscribes to configured topics and delivers
// messages via callback.
// -----------------------------------------------------------
class ThingsBoardClient {
public:
    using MessageCallback = std::function<void(const TbMessage&)>;
    using ConnectCallback = std::function<void(bool connected)>;

    explicit ThingsBoardClient(const ThingsBoardConfig& cfg);
    ~ThingsBoardClient();

    // Non-copyable
    ThingsBoardClient(const ThingsBoardClient&) = delete;
    ThingsBoardClient& operator=(const ThingsBoardClient&) = delete;

    void setMessageCallback(MessageCallback cb) { m_onMessage = std::move(cb); }
    void setConnectCallback(ConnectCallback cb) { m_onConnect = std::move(cb); }

    // Connect and start the background network loop (non-blocking)
    bool start();

    // Publish a message to the broker (e.g. RPC response)
    bool publish(const std::string& topic, const std::string& payload, int qos = 1);

    // Stop and disconnect
    void stop();

    bool isConnected() const { return m_connected; }

private:
    ThingsBoardConfig m_cfg;
    mosquitto*        m_mosq = nullptr;
    MessageCallback   m_onMessage;
    ConnectCallback   m_onConnect;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};

    // libmosquitto static callbacks
    static void onConnect(struct mosquitto*, void* obj, int rc);
    static void onDisconnect(struct mosquitto*, void* obj, int rc);
    static void onMessage(struct mosquitto*, void* obj,
                          const struct mosquitto_message* msg);
    static void onLog(struct mosquitto*, void* obj, int level, const char* str);
};
