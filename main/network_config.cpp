#include "network_config.h"

#include <array>

#include <esp_log.h>
#include <nvs.h>

namespace {

constexpr char kTag[] = "NetworkConfig";
constexpr char kNamespace[] = "upload_cfg";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "wifi_pass";
constexpr char kServerKey[] = "server";
constexpr char kTokenKey[] = "token";

template <size_t Size>
std::string ReadString(nvs_handle_t handle, const char* key) {
    std::array<char, Size> value = {};
    size_t length = value.size();
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) {
        return {};
    }
    return value.data();
}

bool WriteString(nvs_handle_t handle, const char* key, const std::string& value) {
    return nvs_set_str(handle, key, value.c_str()) == ESP_OK;
}

}  // namespace

NetworkSettings NetworkConfig::Load() const {
    NetworkSettings settings;
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return settings;
    }
    settings.wifi_ssid = ReadString<33>(handle, kSsidKey);
    settings.wifi_password = ReadString<65>(handle, kPasswordKey);
    settings.server_url = ReadString<193>(handle, kServerKey);
    settings.upload_token = ReadString<129>(handle, kTokenKey);
    nvs_close(handle);
    return settings;
}

bool NetworkConfig::Save(const NetworkSettings& settings) const {
    if (!IsValid(settings)) {
        return false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const bool written = WriteString(handle, kSsidKey, settings.wifi_ssid) &&
                         WriteString(handle, kPasswordKey, settings.wifi_password) &&
                         WriteString(handle, kServerKey, settings.server_url) &&
                         WriteString(handle, kTokenKey, settings.upload_token);
    const esp_err_t result = written ? nvs_commit(handle) : ESP_FAIL;
    nvs_close(handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save network settings");
        return false;
    }
    ESP_LOGI(kTag, "Network settings saved for SSID %s", settings.wifi_ssid.c_str());
    return true;
}

bool NetworkConfig::IsValid(const NetworkSettings& settings) {
    const bool server_scheme = settings.server_url.rfind("http://", 0) == 0 ||
                               settings.server_url.rfind("https://", 0) == 0;
    return !settings.wifi_ssid.empty() && settings.wifi_ssid.size() <= 32 &&
           settings.wifi_password.size() <= 64 && settings.server_url.size() <= 192 &&
           server_scheme && !settings.upload_token.empty() && settings.upload_token.size() <= 128;
}
