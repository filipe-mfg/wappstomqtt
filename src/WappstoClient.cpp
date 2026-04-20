#include "WappstoClient.hpp"
#include "Logger.hpp"

#include <nlohmann/json.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/evp.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <random>
#include <ctime>
#include <chrono>

using json = nlohmann::json;

// -----------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------

WappstoClient::WappstoClient(const WappstoConfig& cfg) : m_cfg(cfg) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    m_ctx = SSL_CTX_new(TLS_client_method());
    if (!m_ctx) throw std::runtime_error("SSL_CTX_new failed");

    // Always load system default CA paths — Wappsto's public endpoint uses
    // a publicly-issued cert (Let's Encrypt / DigiCert / ...). The bundle's
    // ca.crt is additionally loaded as a trusted root so private/staging
    // deployments also work.
    SSL_CTX_set_default_verify_paths(m_ctx);
    if (!m_cfg.ca_cert.empty()) {
        if (SSL_CTX_load_verify_locations(m_ctx, m_cfg.ca_cert.c_str(), nullptr) != 1)
            Logger::warn("[Wappsto] Could not load extra CA: %s", m_cfg.ca_cert.c_str());
    }
    SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, nullptr);

    // Load client certificate (mTLS)
    if (!m_cfg.client_cert.empty()) {
        if (SSL_CTX_use_certificate_file(m_ctx, m_cfg.client_cert.c_str(),
                                         SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("Cannot load client cert: " + m_cfg.client_cert);
    }
    if (!m_cfg.client_key.empty()) {
        if (SSL_CTX_use_PrivateKey_file(m_ctx, m_cfg.client_key.c_str(),
                                        SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("Cannot load client key: " + m_cfg.client_key);
        if (SSL_CTX_check_private_key(m_ctx) != 1)
            throw std::runtime_error("Client cert/key mismatch");
    }

    // Extract network UUID from certificate CN if set to "auto"
    if (m_cfg.network_uuid == "auto" && !m_cfg.client_cert.empty()) {
        FILE* f = fopen(m_cfg.client_cert.c_str(), "r");
        if (f) {
            X509* cert = PEM_read_X509(f, nullptr, nullptr, nullptr);
            fclose(f);
            if (cert) {
                X509_NAME* subj = X509_get_subject_name(cert);
                char cn[256] = {};
                X509_NAME_get_text_by_NID(subj, NID_commonName, cn, sizeof(cn));
                if (cn[0]) {
                    m_cfg.network_uuid = cn;
                    Logger::info("[Wappsto] Network UUID from cert CN: %s", cn);
                }
                X509_free(cert);
            }
        }
    }

    if (m_cfg.network_uuid == "auto" || m_cfg.network_uuid.empty()) {
        m_cfg.network_uuid = genUUID();
        Logger::warn("[Wappsto] No network UUID found – generated: %s",
                     m_cfg.network_uuid.c_str());
    }

    m_net.uuid = m_cfg.network_uuid;
}

WappstoClient::~WappstoClient() {
    stop();
    if (m_ctx) { SSL_CTX_free(m_ctx); m_ctx = nullptr; }
}

// -----------------------------------------------------------
// Connect & start
// -----------------------------------------------------------

bool WappstoClient::start(const std::string& networkName) {
    if (!tlsConnect()) return false;

    m_running = true;
    m_recvThread = std::thread([this] { recvLoop(); });
    m_pingThread = std::thread([this] { pingLoop(); });

    if (!provisionNetwork(networkName)) {
        Logger::error("[Wappsto] Network provisioning failed");
        stop();
        return false;
    }

    Logger::info("[Wappsto] Ready – network %s", m_net.uuid.c_str());
    return true;
}

void WappstoClient::stop() {
    m_running = false;
    m_connected = false;
    tlsDisconnect();
    if (m_recvThread.joinable()) m_recvThread.join();
    if (m_pingThread.joinable()) m_pingThread.join();
}

// -----------------------------------------------------------
// TLS connect / disconnect
// -----------------------------------------------------------

bool WappstoClient::tlsConnect() {
    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(m_cfg.port);
    int rc = getaddrinfo(m_cfg.host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        Logger::error("[Wappsto] getaddrinfo: %s", gai_strerror(rc));
        return false;
    }

    m_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (m_sock < 0) {
        Logger::error("[Wappsto] socket() failed");
        freeaddrinfo(res);
        return false;
    }

    if (connect(m_sock, res->ai_addr, res->ai_addrlen) != 0) {
        Logger::error("[Wappsto] connect() to %s:%d failed",
                      m_cfg.host.c_str(), m_cfg.port);
        freeaddrinfo(res);
        close(m_sock); m_sock = -1;
        return false;
    }
    freeaddrinfo(res);

    m_ssl = SSL_new(m_ctx);
    SSL_set_fd(m_ssl, m_sock);
    SSL_set_tlsext_host_name(m_ssl, m_cfg.host.c_str());

    if (SSL_connect(m_ssl) != 1) {
        unsigned long err = ERR_get_error();
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        Logger::error("[Wappsto] TLS handshake failed: %s", buf);
        SSL_free(m_ssl); m_ssl = nullptr;
        close(m_sock);   m_sock = -1;
        return false;
    }

    m_connected = true;
    Logger::info("[Wappsto] TLS connected to %s:%d", m_cfg.host.c_str(), m_cfg.port);
    return true;
}

void WappstoClient::tlsDisconnect() {
    if (m_ssl) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_sock >= 0) {
        close(m_sock);
        m_sock = -1;
    }
}

// -----------------------------------------------------------
// Low-level send / recv
// -----------------------------------------------------------

bool WappstoClient::sendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (!m_ssl || !m_connected) return false;

    // Wappsto's JSON-RPC transport is raw JSON back-to-back — no delimiter.
    int total = 0;
    int len   = static_cast<int>(line.size());
    while (total < len) {
        int written = SSL_write(m_ssl, line.c_str() + total, len - total);
        if (written <= 0) {
            Logger::error("[Wappsto] SSL_write failed");
            return false;
        }
        total += written;
    }
    Logger::debug("[Wappsto] SEND: %s", line.c_str());
    return true;
}

// Read one full JSON object by tracking brace depth, respecting string
// literals and escape sequences. Leading whitespace between messages is
// ignored. Returns an empty string on disconnect.
std::string WappstoClient::recvLine() {
    std::string msg;
    int  depth     = 0;
    bool inString  = false;
    bool escape    = false;
    bool started   = false;
    char c;

    while (m_running && m_ssl) {
        int rc = SSL_read(m_ssl, &c, 1);
        if (rc <= 0) return "";

        if (!started) {
            if (c == '{' || c == '[') {
                started = true;
                depth   = 1;
                msg    += c;
            }
            // Anything else outside a message is ignored (whitespace, etc.)
            continue;
        }

        msg += c;

        if (escape)          { escape = false; continue; }
        if (inString) {
            if (c == '\\')     escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (--depth == 0) return msg;
        }
    }
    return "";
}

// -----------------------------------------------------------
// JSON-RPC helpers
// -----------------------------------------------------------

std::string WappstoClient::sendRpc(const std::string& method,
                                   const std::string& url,
                                   const std::string& dataJson,
                                   int timeoutSec) {
    std::string id = genUUID();
    if (timeoutSec < 0) timeoutSec = m_cfg.rpc_timeout_sec;

    // Build JSON-RPC request. GETs must not include a "data" field — the
    // collector interprets GET-with-data as FETCH and rejects it.
    json params = { {"url", url} };
    if (method != "GET") {
        params["data"] = json::parse(dataJson, nullptr, false);
    }
    json req = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  method},
        {"params",  params}
    };

    // Register pending waiter
    auto pending = std::make_shared<PendingRpc>();
    {
        std::lock_guard<std::mutex> lk(m_rpcMutex);
        m_pending[id] = pending;
    }

    if (!sendLine(req.dump())) {
        std::lock_guard<std::mutex> lk(m_rpcMutex);
        m_pending.erase(id);
        return "";
    }

    // Wait for response
    std::unique_lock<std::mutex> lk(m_rpcMutex);
    bool timedOut = !pending->cv.wait_for(lk,
        std::chrono::seconds(timeoutSec),
        [&]{ return pending->done; });
    m_pending.erase(id);

    if (timedOut) {
        Logger::warn("[Wappsto] RPC timeout for %s %s", method.c_str(), url.c_str());
        return "";
    }
    if (!pending->ok) {
        Logger::warn("[Wappsto] RPC error for %s %s: %s",
            method.c_str(), url.c_str(), pending->result.c_str());
        return "";
    }
    return pending->result;
}

void WappstoClient::sendReply(const std::string& id, bool ok,
                              const std::string& resultJson) {
    json rep = {{"jsonrpc", "2.0"}, {"id", id}};
    if (ok)
        rep["result"] = json::parse(resultJson, nullptr, false);
    else
        rep["error"]  = {{"code", -1}, {"message", resultJson}};
    sendLine(rep.dump());
}

// -----------------------------------------------------------
// Background receive loop
// -----------------------------------------------------------

void WappstoClient::recvLoop() {
    while (m_running) {
        std::string line = recvLine();
        if (line.empty()) {
            if (m_running) Logger::warn("[Wappsto] Connection lost");
            break;
        }
        Logger::debug("[Wappsto] RECV: %s", line.c_str());
        handleMessage(line);
    }
    m_connected = false;
}

// -----------------------------------------------------------
// Message dispatcher
// -----------------------------------------------------------

void WappstoClient::handleMessage(const std::string& raw) {
    auto j = json::parse(raw, nullptr, false);
    if (j.is_discarded()) {
        Logger::warn("[Wappsto] Invalid JSON: %s", raw.c_str());
        return;
    }

    // It is a response to one of our pending RPCs
    if (j.contains("result") || j.contains("error")) {
        std::string id = j.value("id", "");
        std::lock_guard<std::mutex> lk(m_rpcMutex);
        auto it = m_pending.find(id);
        if (it != m_pending.end()) {
            it->second->ok   = j.contains("result");
            it->second->result = j.contains("result")
                ? j["result"].dump()
                : j["error"].dump();
            it->second->done = true;
            it->second->cv.notify_one();
        }
        return;
    }

    // Server-sent request (control command, refresh, etc.)
    if (j.contains("method") && j.contains("params")) {
        std::string method = j["method"].get<std::string>();
        std::string id     = j.value("id", "");
        auto& params = j["params"];
        std::string url    = params.value("url", "");

        // Acknowledge immediately
        sendReply(id, true);

        // Control state update: PUT /state/{uuid}
        if ((method == "PUT" || method == "PATCH") &&
            url.find("/state/") != std::string::npos) {
            // Extract state uuid from URL
            std::string stateUuid = url.substr(url.rfind('/') + 1);
            auto& data = params["data"];

            WappstoControl ctrl;
            ctrl.state_uuid = stateUuid;
            ctrl.data       = data.value("data", "");
            ctrl.timestamp  = data.value("timestamp", "");

            Logger::info("[Wappsto] Control: state=%s data=%s",
                ctrl.state_uuid.c_str(), ctrl.data.c_str());

            if (m_onControl) m_onControl(ctrl);
        }
    }
}

// -----------------------------------------------------------
// Ping / keepalive loop
// -----------------------------------------------------------

void WappstoClient::pingLoop() {
    while (m_running) {
        for (int i = 0; i < m_cfg.ping_interval_sec && m_running; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!m_running || !m_connected) break;

        Logger::debug("[Wappsto] Sending ping");
        sendRpc("GET", "/network/" + m_net.uuid, "{}", 5);
    }
}

// -----------------------------------------------------------
// Provisioning
// -----------------------------------------------------------

bool WappstoClient::provisionNetwork(const std::string& name) {
    m_net.name = name;

    json netData = {
        {"meta", {{"id",   m_net.uuid},
                  {"type", "network"},
                  {"version", "2.1"}}},
        {"name", m_net.name}
    };

    // Try create-then-update so this works whether the network exists
    // or was deleted from the UI. POST to the collection creates with
    // meta.id; PUT on the item URL updates the existing one.
    std::string colUrl  = "/network";
    std::string itemUrl = colUrl + "/" + m_net.uuid;
    std::string res = sendRpc("POST", colUrl, netData.dump());
    if (res.empty()) res = sendRpc("PUT", itemUrl, netData.dump());
    return !res.empty();
}

std::string WappstoClient::ensureDevice(const std::string& name,
                                        const std::string& preferredUuid) {
    // Check if already exists
    auto it = m_net.devices.find(name);
    if (it != m_net.devices.end()) return it->second.uuid;

    WappstoDevice dev;
    dev.name = name;
    // Deterministic UUID derived from network UUID + device name so it
    // survives restarts without re-creating duplicate objects.
    dev.uuid = !preferredUuid.empty()
               ? preferredUuid
               : uuid5(m_net.uuid, name);

    json devData = {
        {"meta",     {{"id",   dev.uuid},
                      {"type", "device"},
                      {"version", "2.1"}}},
        {"name",     dev.name},
        {"protocol", "ThingsBoard-MQTT"},
        {"communication", "mqtt"}
    };

    // Wappsto expects creations to POST to the COLLECTION URL; the UUID is
    // taken from meta.id in the body. Use PUT on the specific URL to update
    // (or create) a resource that already has that UUID assigned.
    std::string colUrl  = "/network/" + m_net.uuid + "/device";
    std::string itemUrl = colUrl + "/" + dev.uuid;
    std::string res = sendRpc("POST", colUrl, devData.dump());
    if (res.empty()) res = sendRpc("PUT", itemUrl, devData.dump());

    if (!res.empty()) {
        m_net.devices[name] = dev;
        Logger::info("[Wappsto] Device provisioned: %s (%s)", name.c_str(), dev.uuid.c_str());
        return dev.uuid;
    }
    Logger::error("[Wappsto] Failed to provision device: %s", name.c_str());
    return "";
}

std::string WappstoClient::ensureValue(const std::string& deviceUuid,
                                       const WappstoValue& val) {
    // Find the device
    WappstoDevice* dev = nullptr;
    for (auto& [k, d] : m_net.devices) {
        if (d.uuid == deviceUuid) { dev = &d; break; }
    }
    if (!dev) {
        Logger::error("[Wappsto] ensureValue: unknown device %s", deviceUuid.c_str());
        return "";
    }

    // Already exists?
    auto it = dev->values.find(val.name);
    if (it != dev->values.end()) return it->second.uuid;

    WappstoValue v = val;
    if (v.uuid.empty()) v.uuid = uuid5(deviceUuid, v.name);

    // Build number/string schema
    json schema;
    if (v.type == "number") {
        schema = {{"min", v.min}, {"max", v.max}, {"step", v.step}, {"unit", v.unit}};
    } else if (v.type == "blob") {
        schema = {{"max", 65536}, {"encoding", "utf-8"}};
    } else {
        // "string" — Wappsto limits to 64 chars in the UI; use blob for larger payloads
        schema = {{"max", 64}, {"encoding", "utf-8"}};
    }

    json valData = {
        {"meta",      {{"id",   v.uuid},
                       {"type", "value"},
                       {"version", "2.1"}}},
        {"name",      v.name},
        {"type",      v.type},
        {"permission", v.permission},
        {v.type,      schema}
    };

    std::string valColUrl  = "/network/" + m_net.uuid + "/device/" + deviceUuid + "/value";
    std::string valItemUrl = valColUrl + "/" + v.uuid;
    std::string res = sendRpc("POST", valColUrl, valData.dump());
    if (res.empty()) res = sendRpc("PUT", valItemUrl, valData.dump());

    if (res.empty()) {
        Logger::error("[Wappsto] Failed to provision value: %s", v.name.c_str());
        return "";
    }

    std::string stateColUrl = valItemUrl + "/state";

    // Create Report state (if readable)
    if (v.permission == "r" || v.permission == "rw") {
        v.report_state_uuid = uuid5(v.uuid, "Report");
        json stData = {
            {"meta",      {{"id",   v.report_state_uuid},
                           {"type", "state"},
                           {"version", "2.1"}}},
            {"type",      "Report"},
            {"data",      "NA"},
            {"timestamp", nowISO8601()}
        };
        std::string sItemUrl = stateColUrl + "/" + v.report_state_uuid;
        std::string sres = sendRpc("POST", stateColUrl, stData.dump());
        if (sres.empty()) sendRpc("PUT", sItemUrl, stData.dump());
    }

    // Create Control state (if writable)
    if (v.permission == "w" || v.permission == "rw") {
        v.control_state_uuid = uuid5(v.uuid, "Control");
        json stData = {
            {"meta",      {{"id",   v.control_state_uuid},
                           {"type", "state"},
                           {"version", "2.1"}}},
            {"type",      "Control"},
            {"data",      "NA"},
            {"timestamp", nowISO8601()}
        };
        std::string sItemUrl = stateColUrl + "/" + v.control_state_uuid;
        std::string sres = sendRpc("POST", stateColUrl, stData.dump());
        if (sres.empty()) sendRpc("PUT", sItemUrl, stData.dump());
    }

    dev->values[v.name] = v;
    Logger::info("[Wappsto] Value provisioned: %s/%s (%s)",
        dev->name.c_str(), v.name.c_str(), v.uuid.c_str());
    return v.uuid;
}

bool WappstoClient::reportValue(const std::string& stateUuid,
                                const std::string& data,
                                const std::string& timestamp) {
    std::string ts = timestamp.empty() ? nowISO8601() : timestamp;
    json stData = {{"data", data}, {"timestamp", ts}};
    std::string url = "/state/" + stateUuid;
    std::string res = sendRpc("PATCH", url, stData.dump());
    return !res.empty();
}

// -----------------------------------------------------------
// Fetch current state data (GET /state/{uuid})
// -----------------------------------------------------------

std::string WappstoClient::getStateData(const std::string& stateUuid) {
    if (stateUuid.empty()) return "";
    std::string res = sendRpc("GET", "/state/" + stateUuid);
    if (res.empty()) return "";

    auto j = json::parse(res, nullptr, false);
    if (j.is_discarded()) return "";

    // Wappsto wraps the state object in { "value": <state> }
    const json& state = j.contains("value") ? j["value"] : j;
    if (!state.is_object() || !state.contains("data")) return "";

    const json& d = state["data"];
    // data may be a string (stored as-is) or any JSON value (blob type)
    if (d.is_string()) return d.get<std::string>();
    if (d.is_null())   return "";
    return d.dump();  // object/array/number — serialise back to string
}

// -----------------------------------------------------------
// Utilities
// -----------------------------------------------------------

std::string WappstoClient::genUUID() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    uint32_t a = dist(rng), b = dist(rng), c = dist(rng), d = dist(rng);
    // Set version 4 and variant bits
    b = (b & 0xFFFF0FFF) | 0x00004000;
    c = (c & 0x3FFFFFFF) | 0x80000000;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%04x%08x",
        a,
        (b >> 16) & 0xFFFF,
        b & 0xFFFF,
        (c >> 16) & 0xFFFF,
        c & 0xFFFF,
        d);
    return buf;
}

// -----------------------------------------------------------
// UUID v5 (SHA-1 based, deterministic from namespace + name)
// -----------------------------------------------------------

std::string WappstoClient::uuid5(const std::string& namespaceUuid,
                                 const std::string& name) {
    // Parse namespace UUID into 16 raw bytes
    unsigned char nsBytes[16] = {};
    const char* s = namespaceUuid.c_str();
    int bi = 0;
    for (int i = 0; s[i] && bi < 16; ++i) {
        if (s[i] == '-') continue;
        char hex[3] = { s[i], s[i+1], 0 };
        nsBytes[bi++] = static_cast<unsigned char>(std::strtoul(hex, nullptr, 16));
        ++i;
    }

    // SHA-1(namespace || name) via EVP (non-deprecated API)
    unsigned char digest[20];
    unsigned int  digestLen = sizeof(digest);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, nsBytes, 16);
    EVP_DigestUpdate(ctx, name.data(), name.size());
    EVP_DigestFinal_ex(ctx, digest, &digestLen);
    EVP_MD_CTX_free(ctx);

    // Take first 16 bytes; set version=5 and RFC4122 variant
    digest[6] = (digest[6] & 0x0F) | 0x50;
    digest[8] = (digest[8] & 0x3F) | 0x80;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        digest[0],  digest[1],  digest[2],  digest[3],
        digest[4],  digest[5],  digest[6],  digest[7],
        digest[8],  digest[9],  digest[10], digest[11],
        digest[12], digest[13], digest[14], digest[15]);
    return buf;
}

std::string WappstoClient::nowISO8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));
    std::snprintf(buf + 19, sizeof(buf) - 19, ".%03ldZ",
                  static_cast<long>(ms.count()));
    return buf;
}
