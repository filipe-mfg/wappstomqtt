#include "ThingsBoardApi.hpp"
#include "Logger.hpp"

#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <sstream>
#include <stdexcept>
#include <cstring>

using json = nlohmann::json;

// -----------------------------------------------------------
// Construction
// -----------------------------------------------------------

ThingsBoardApi::ThingsBoardApi(const ThingsBoardApiConfig& cfg) : m_cfg(cfg) {}

// -----------------------------------------------------------
// Authenticate
// POST /api/auth/login  { "username": "...", "password": "..." }
// Response: { "token": "...", "refreshToken": "..." }
// -----------------------------------------------------------

bool ThingsBoardApi::authenticate() {
    if (m_cfg.username.empty()) {
        Logger::warn("[TbApi] No credentials configured – RPC will be skipped");
        return false;
    }

    json body = {{"username", m_cfg.username}, {"password", m_cfg.password}};
    std::string resp = httpPost("/api/auth/login", body.dump());
    if (resp.empty()) {
        Logger::error("[TbApi] Authentication request failed");
        return false;
    }

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.contains("token")) {
        Logger::error("[TbApi] Unexpected auth response: %s", resp.c_str());
        return false;
    }

    m_token = j["token"].get<std::string>();
    Logger::info("[TbApi] Authenticated as %s", m_cfg.username.c_str());
    return true;
}

// -----------------------------------------------------------
// Send RPC
// POST /api/rpc/oneway/{deviceId}
// Body: { "method": "...", "params": {...} }
// -----------------------------------------------------------

bool ThingsBoardApi::sendRpc(const std::string& deviceName,
                             const std::string& method,
                             const std::string& paramsJson) {
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_token.empty()) {
        Logger::warn("[TbApi] Not authenticated – skipping RPC for %s", deviceName.c_str());
        return false;
    }

    std::string deviceId = resolveDeviceId(deviceName);
    if (deviceId.empty()) {
        Logger::error("[TbApi] Cannot resolve device ID for '%s'", deviceName.c_str());
        return false;
    }

    json params = json::parse(paramsJson, nullptr, false);
    if (params.is_discarded()) params = json::object();

    json body = {{"method", method}, {"params", params}, {"timeout", 10000}};
    std::string path = "/api/rpc/oneway/" + deviceId;
    std::string resp = httpPost(path, body.dump());

    if (resp.empty()) {
        // Try re-authenticating once
        Logger::warn("[TbApi] RPC failed – trying re-auth");
        m_token.clear();
        if (!authenticate()) return false;
        resp = httpPost(path, body.dump());
    }

    Logger::info("[TbApi] RPC sent: device=%s method=%s",
                 deviceName.c_str(), method.c_str());
    return !resp.empty();
}

// -----------------------------------------------------------
// Resolve device name → UUID
// GET /api/tenant/devices?deviceName=<name>
// -----------------------------------------------------------

std::string ThingsBoardApi::resolveDeviceId(const std::string& name) {
    auto it = m_deviceIdCache.find(name);
    if (it != m_deviceIdCache.end()) return it->second;

    std::string path = "/api/tenant/devices?deviceName=" + name;
    std::string resp = httpGet(path);
    if (resp.empty()) return "";

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded()) return "";

    // Response is a Device object with "id": { "id": "uuid", "entityType": "DEVICE" }
    std::string uuid;
    if (j.contains("id") && j["id"].contains("id"))
        uuid = j["id"]["id"].get<std::string>();

    if (!uuid.empty()) {
        m_deviceIdCache[name] = uuid;
        Logger::debug("[TbApi] Device '%s' = %s", name.c_str(), uuid.c_str());
    }
    return uuid;
}

// -----------------------------------------------------------
// Low-level HTTP POST / GET
// -----------------------------------------------------------

std::string ThingsBoardApi::httpPost(const std::string& path,
                                     const std::string& body,
                                     const std::string& /*extraHeaders*/) {
    std::string authHeader;
    if (!m_token.empty())
        authHeader = "X-Authorization: Bearer " + m_token + "\r\n";

    std::string req = buildRequest("POST", path, body, authHeader);
    return sendReceive(req);
}

std::string ThingsBoardApi::httpGet(const std::string& path) {
    std::string authHeader;
    if (!m_token.empty())
        authHeader = "X-Authorization: Bearer " + m_token + "\r\n";

    std::string req = buildRequest("GET", path, "", authHeader);
    return sendReceive(req);
}

std::string ThingsBoardApi::buildRequest(const std::string& method,
                                         const std::string& path,
                                         const std::string& body,
                                         const std::string& extraHeaders) {
    std::ostringstream oss;
    oss << method << " " << path << " HTTP/1.1\r\n"
        << "Host: " << m_cfg.host << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << extraHeaders
        << "\r\n"
        << body;
    return oss.str();
}

// -----------------------------------------------------------
// TCP/TLS send + receive HTTP response body
// -----------------------------------------------------------

std::string ThingsBoardApi::sendReceive(const std::string& request) {
    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(m_cfg.port);

    if (getaddrinfo(m_cfg.host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        Logger::error("[TbApi] DNS resolution failed for %s", m_cfg.host.c_str());
        return "";
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return ""; }

    // Set timeout via SO_RCVTIMEO / SO_SNDTIMEO
    struct timeval tv{ m_cfg.timeout_sec, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        Logger::error("[TbApi] connect() failed to %s:%d",
                      m_cfg.host.c_str(), m_cfg.port);
        freeaddrinfo(res); close(sock); return "";
    }
    freeaddrinfo(res);

    std::string body;

    if (m_cfg.tls) {
        // TLS path — verify the server certificate against the system's
        // trusted CA store. Without this we'd be vulnerable to MITM on the
        // path between the bridge and ThingsBoard, leaking the JWT and any
        // RPC payloads.
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, m_cfg.host.c_str());
        // Enable hostname verification at the OpenSSL layer
        X509_VERIFY_PARAM* vp = SSL_get0_param(ssl);
        X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        X509_VERIFY_PARAM_set1_host(vp, m_cfg.host.c_str(), 0);
        if (SSL_connect(ssl) != 1) {
            unsigned long err = ERR_get_error();
            char errbuf[256];
            ERR_error_string_n(err, errbuf, sizeof(errbuf));
            Logger::error("[TbApi] TLS handshake failed: %s", errbuf);
            SSL_free(ssl); SSL_CTX_free(ctx); close(sock); return "";
        }
        SSL_write(ssl, request.c_str(), static_cast<int>(request.size()));
        char buf[4096];
        std::string rawResp;
        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0)
            rawResp.append(buf, n);
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx);
        close(sock);
        // Extract body after \r\n\r\n
        auto pos = rawResp.find("\r\n\r\n");
        if (pos != std::string::npos) body = rawResp.substr(pos + 4);
    } else {
        // Plain TCP path
        send(sock, request.c_str(), request.size(), 0);
        char buf[4096];
        std::string rawResp;
        ssize_t n;
        while ((n = recv(sock, buf, sizeof(buf), 0)) > 0)
            rawResp.append(buf, static_cast<std::size_t>(n));
        close(sock);
        auto pos = rawResp.find("\r\n\r\n");
        if (pos != std::string::npos) body = rawResp.substr(pos + 4);
    }

    return body;
}
