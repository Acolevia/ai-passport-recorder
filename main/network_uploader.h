#pragma once

#include "app_types.h"
#include "network_config.h"
#include "recording_store.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

enum class NetworkState : uint8_t {
    kNotConfigured,
    kWifiConnecting,
    kWifiOffline,
    kServerOffline,
    kAuthError,
    kUploading,
    kOnline,
};

class NetworkUploader {
public:
    NetworkUploader(RecordingStore& store, NetworkConfig& config)
        : store_(store), config_(config) {}

    bool Start(QueueHandle_t app_queue, SemaphoreHandle_t wifi_lock);
    void SetPaused(bool paused);
    void Wake();

private:
    static void TaskEntry(void* argument);
    static void WifiEventHandler(void* argument, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data);

    void Task();
    void RunCycle(const NetworkSettings& settings);
    bool ConnectWifi(const NetworkSettings& settings);
    bool EnsureClock(const NetworkSettings& settings);
    void DisconnectWifi();
    bool CheckServer(const NetworkSettings& settings, bool& auth_error);
    bool UploadRecording(const NetworkSettings& settings, const RecordingInfo& recording,
                         bool& auth_error);
    bool QueryOffset(const std::string& url, const NetworkSettings& settings,
                     uint64_t& offset, bool& complete, bool& auth_error);
    bool PutFile(const std::string& url, const NetworkSettings& settings, FILE* file,
                 const RecordingHeader& header, uint64_t offset, uint64_t total_size,
                 bool& auth_error);
    std::string RecordingUrl(const NetworkSettings& settings, uint32_t sequence) const;
    void PostState(NetworkState state, uint32_t pending);

    RecordingStore& store_;
    NetworkConfig& config_;
    QueueHandle_t app_queue_ = nullptr;
    SemaphoreHandle_t wifi_lock_ = nullptr;
    TaskHandle_t task_ = nullptr;
    EventGroupHandle_t wifi_events_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
    std::atomic<bool> paused_{false};
    bool sntp_started_ = false;
    std::string device_id_;
};
