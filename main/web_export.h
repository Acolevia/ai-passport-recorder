#pragma once

#include "network_config.h"
#include "recording_store.h"

#include <string>

#include <esp_http_server.h>
#include <esp_netif.h>
#include <freertos/semphr.h>

class WebExport {
public:
    WebExport(RecordingStore& store, NetworkConfig& network_config)
        : store_(store), network_config_(network_config) {}
    ~WebExport();

    void SetWifiLock(SemaphoreHandle_t wifi_lock) { wifi_lock_ = wifi_lock; }
    bool Start();
    void Stop();
    bool running() const { return server_ != nullptr; }
    const std::string& ssid() const { return ssid_; }
    const std::string& password() const { return password_; }

private:
    static esp_err_t IndexHandler(httpd_req_t* request);
    static esp_err_t DownloadHandler(httpd_req_t* request);
    static esp_err_t PlayHandler(httpd_req_t* request);
    static esp_err_t DeleteHandler(httpd_req_t* request);
    static esp_err_t SettingsHandler(httpd_req_t* request);

    esp_err_t SendIndex(httpd_req_t* request);
    esp_err_t SendOggOpus(httpd_req_t* request);
    esp_err_t SendWave(httpd_req_t* request, bool attachment);
    esp_err_t DeleteRecording(httpd_req_t* request);
    esp_err_t SaveSettings(httpd_req_t* request);
    bool ParseSequence(httpd_req_t* request, uint32_t& sequence) const;

    RecordingStore& store_;
    NetworkConfig& network_config_;
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    SemaphoreHandle_t wifi_lock_ = nullptr;
    bool owns_wifi_lock_ = false;
    std::string ssid_;
    std::string password_;
};
