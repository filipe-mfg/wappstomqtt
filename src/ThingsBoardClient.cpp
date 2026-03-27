#include "ThingsBoardClient.hpp"
#include "Logger.hpp"
#include <mosquitto.h>
#include <stdexcept>
#include <cstring>

// -----------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------

ThingsBoardClient::ThingsBoardClient(const ThingsBoardConfig& cfg)
    : m_cfg(cfg)
{
    mosquitto_lib_init();

    m_mosq = mosquitto_new(m_cfg.client_id.c_str(), true, this);
    if (!m_mosq) throw std::runtime_error("Failed to create mosquitto instance");

    // Callbacks
    mosquitto_connect_callback_set(m_mosq,    ThingsBoardClient::onConnect);
    mosquitto_disconnect_callback_set(m_mosq, ThingsBoardClient::onDisconnect);
    mosquitto_message_callback_set(m_mosq,    ThingsBoardClient::onMessage);
    mosquitto_log_callback_set(m_mosq,        ThingsBoardClient::onLog);

    // Credentials
    if (!m_cfg.username.empty()) {
        mosquitto_username_pw_set(m_mosq,
            m_cfg.username.c_str(),
            m_cfg.password.empty() ? nullptr : m_cfg.password.c_str());
    }

    // TLS
    if (m_cfg.tls.enabled) {
        const char* ca   = m_cfg.tls.ca_cert.empty()     ? nullptr : m_cfg.tls.ca_cert.c_str();
        const char* cert = m_cfg.tls.client_cert.empty() ? nullptr : m_cfg.tls.client_cert.c_str();
        const char* key  = m_cfg.tls.client_key.empty()  ? nullptr : m_cfg.tls.client_key.c_str();
        int rc = mosquitto_tls_set(m_mosq, ca, nullptr, cert, key, nullptr);
        if (rc != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error(std::string("mosquitto_tls_set failed: ") +
                                     mosquitto_strerror(rc));
        }
    }
}

ThingsBoardClient::~ThingsBoardClient() {
    stop();
    if (m_mosq) {
        mosquitto_destroy(m_mosq);
        m_mosq = nullptr;
    }
    mosquitto_lib_cleanup();
}

// -----------------------------------------------------------
// Connect & start
// -----------------------------------------------------------

bool ThingsBoardClient::start() {
    Logger::info("[TB] Connecting to %s:%d as '%s'",
        m_cfg.host.c_str(), m_cfg.port, m_cfg.client_id.c_str());

    int rc = mosquitto_connect(m_mosq,
        m_cfg.host.c_str(), m_cfg.port, m_cfg.keepalive_sec);
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::error("[TB] Connect failed: %s", mosquitto_strerror(rc));
        return false;
    }

    // Start threaded loop (POSIX thread inside libmosquitto)
    rc = mosquitto_loop_start(m_mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::error("[TB] loop_start failed: %s", mosquitto_strerror(rc));
        return false;
    }

    m_running = true;
    return true;
}

// -----------------------------------------------------------
// Publish
// -----------------------------------------------------------

bool ThingsBoardClient::publish(const std::string& topic,
                                const std::string& payload,
                                int qos) {
    if (!m_connected) {
        Logger::warn("[TB] publish skipped – not connected");
        return false;
    }
    int rc = mosquitto_publish(m_mosq, nullptr,
        topic.c_str(),
        static_cast<int>(payload.size()),
        payload.c_str(),
        qos, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        Logger::error("[TB] publish failed: %s", mosquitto_strerror(rc));
        return false;
    }
    return true;
}

// -----------------------------------------------------------
// Stop
// -----------------------------------------------------------

void ThingsBoardClient::stop() {
    if (m_running.exchange(false)) {
        if (m_mosq) {
            mosquitto_disconnect(m_mosq);
            mosquitto_loop_stop(m_mosq, true);
        }
    }
}

// -----------------------------------------------------------
// Static mosquitto callbacks
// -----------------------------------------------------------

void ThingsBoardClient::onConnect(struct mosquitto*, void* obj, int rc) {
    auto* self = static_cast<ThingsBoardClient*>(obj);

    if (rc != 0) {
        Logger::error("[TB] Connection refused (rc=%d)", rc);
        if (self->m_onConnect) self->m_onConnect(false);
        return;
    }

    Logger::info("[TB] Connected");
    self->m_connected = true;

    // Subscribe to all configured topics
    for (const auto& topic : self->m_cfg.subscribe_topics) {
        int sub_rc = mosquitto_subscribe(self->m_mosq, nullptr, topic.c_str(), 1);
        if (sub_rc != MOSQ_ERR_SUCCESS) {
            Logger::error("[TB] Subscribe '%s' failed: %s",
                topic.c_str(), mosquitto_strerror(sub_rc));
        } else {
            Logger::info("[TB] Subscribed: %s", topic.c_str());
        }
    }

    if (self->m_onConnect) self->m_onConnect(true);
}

void ThingsBoardClient::onDisconnect(struct mosquitto*, void* obj, int rc) {
    auto* self = static_cast<ThingsBoardClient*>(obj);
    self->m_connected = false;
    Logger::warn("[TB] Disconnected (rc=%d) – libmosquitto will reconnect", rc);
    if (self->m_onConnect) self->m_onConnect(false);
}

void ThingsBoardClient::onMessage(struct mosquitto*, void* obj,
                                  const struct mosquitto_message* msg) {
    auto* self = static_cast<ThingsBoardClient*>(obj);
    if (!msg || !msg->payload) return;

    TbMessage m;
    m.topic   = msg->topic;
    m.payload = std::string(static_cast<const char*>(msg->payload),
                            static_cast<std::size_t>(msg->payloadlen));

    Logger::debug("[TB] MSG topic=%s payload=%s", m.topic.c_str(), m.payload.c_str());

    if (self->m_onMessage) self->m_onMessage(m);
}

void ThingsBoardClient::onLog(struct mosquitto*, void* /*obj*/,
                               int level, const char* str) {
    // Map mosquitto log levels to our logger
    if (level & MOSQ_LOG_ERR)    Logger::error("[MQTT] %s", str);
    else if (level & MOSQ_LOG_WARNING) Logger::warn("[MQTT] %s", str);
    else if (level & MOSQ_LOG_DEBUG)   Logger::debug("[MQTT] %s", str);
    // INFO and NOTICE are suppressed to avoid noise
}
