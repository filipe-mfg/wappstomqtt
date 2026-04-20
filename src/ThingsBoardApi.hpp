#pragma once

// -----------------------------------------------------------
// ThingsBoardApi
//
// Thin wrapper around the ThingsBoard REST API used to:
//  1. Authenticate and obtain a JWT session token
//  2. Resolve a device name to its UUID
//  3. Trigger server-side RPC calls on gateway-managed devices
//
// The Wappsto → ThingsBoard control direction CANNOT be done
// via the Gateway MQTT API (that API only lets ThingsBoard push
// RPCs to the gateway, not the other way around). Instead we
// call the ThingsBoard HTTP REST API.
// -----------------------------------------------------------

#include "Config.hpp"   // declares struct ThingsBoardApiConfig
#include <string>
#include <map>
#include <mutex>

class ThingsBoardApi {
public:
    explicit ThingsBoardApi(const ThingsBoardApiConfig& cfg);

    // Authenticate (called once; token is cached and refreshed)
    bool authenticate();

    // Trigger a one-way (fire-and-forget) RPC on a device
    // device_name is used to resolve the device UUID on first call
    bool sendRpc(const std::string& deviceName,
                 const std::string& method,
                 const std::string& paramsJson);

    bool isAuthenticated() const { return !m_token.empty(); }

private:
    ThingsBoardApiConfig m_cfg;
    std::string          m_token;
    std::mutex           m_mutex;

    // device name → device UUID cache
    std::map<std::string, std::string> m_deviceIdCache;

    // Resolve device name to UUID via GET /api/tenant/devices?deviceName=...
    std::string resolveDeviceId(const std::string& name);

    // Low-level HTTP call (returns response body or empty on error)
    std::string httpPost(const std::string& path,
                         const std::string& body,
                         const std::string& extraHeaders = "");
    std::string httpGet(const std::string& path);

    std::string buildRequest(const std::string& method,
                             const std::string& path,
                             const std::string& body,
                             const std::string& extraHeaders);
    std::string sendReceive(const std::string& request);
};
