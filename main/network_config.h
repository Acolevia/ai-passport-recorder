#pragma once

#include <string>

struct NetworkSettings {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string server_url;
    std::string upload_token;

    bool configured() const {
        return !wifi_ssid.empty() && !server_url.empty() && !upload_token.empty();
    }
};

class NetworkConfig {
public:
    NetworkSettings Load() const;
    bool Save(const NetworkSettings& settings) const;

private:
    static bool IsValid(const NetworkSettings& settings);
};
