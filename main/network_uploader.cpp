#include "network_uploader.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <ctime>

#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>

namespace {

constexpr char kTag[] = "NetworkUploader";
constexpr EventBits_t kWifiConnected = BIT0;
constexpr EventBits_t kWifiFailed = BIT1;
constexpr TickType_t kRetryInterval = pdMS_TO_TICKS(30000);
constexpr TickType_t kConnectTimeout = pdMS_TO_TICKS(12000);
constexpr size_t kUploadBufferBytes = 4096;

struct HeaderContext {
    uint64_t offset = 0;
    bool saw_offset = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || event->user_data == nullptr ||
        event->header_key == nullptr || event->header_value == nullptr) {
        return ESP_OK;
    }
    auto* context = static_cast<HeaderContext*>(event->user_data);
    if (strcasecmp(event->header_key, "Upload-Offset") == 0) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(event->header_value, &end, 10);
        if (end != event->header_value && *end == '\0') {
            context->offset = parsed;
            context->saw_offset = true;
        }
    }
    return ESP_OK;
}

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

}  // namespace

bool NetworkUploader::Start(QueueHandle_t app_queue, SemaphoreHandle_t wifi_lock) {
    if (task_ != nullptr || app_queue == nullptr || wifi_lock == nullptr) {
        return false;
    }
    app_queue_ = app_queue;
    wifi_lock_ = wifi_lock;
    wifi_events_ = xEventGroupCreate();
    if (wifi_events_ == nullptr) {
        return false;
    }
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char identifier[13];
    std::snprintf(identifier, sizeof(identifier), "%02X%02X%02X%02X%02X%02X", mac[0],
                  mac[1], mac[2], mac[3], mac[4], mac[5]);
    device_id_ = identifier;
    if (xTaskCreate(TaskEntry, "record_uploader", 10240, this, 5, &task_) != pdPASS) {
        task_ = nullptr;
        vEventGroupDelete(wifi_events_);
        wifi_events_ = nullptr;
        return false;
    }
    return true;
}

void NetworkUploader::SetPaused(bool paused) {
    paused_.store(paused);
    if (paused && wifi_events_ != nullptr) {
        // Let hotspot export pre-empt a pending station connection immediately.
        xEventGroupSetBits(wifi_events_, kWifiFailed);
    }
    Wake();
}

void NetworkUploader::Wake() {
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
}

void NetworkUploader::TaskEntry(void* argument) {
    static_cast<NetworkUploader*>(argument)->Task();
}

void NetworkUploader::WifiEventHandler(void* argument, esp_event_base_t event_base,
                                       int32_t event_id, void*) {
    auto* uploader = static_cast<NetworkUploader*>(argument);
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(uploader->wifi_events_, kWifiConnected);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(uploader->wifi_events_, kWifiFailed);
    }
}

void NetworkUploader::Task() {
    while (true) {
        const NetworkSettings settings = config_.Load();
        if (!settings.configured()) {
            PostState(NetworkState::kNotConfigured, store_.List().size());
        } else if (!paused_.load() && xSemaphoreTake(wifi_lock_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            RunCycle(settings);
            xSemaphoreGive(wifi_lock_);
        }
        ulTaskNotifyTake(pdTRUE, kRetryInterval);
    }
}

void NetworkUploader::RunCycle(const NetworkSettings& settings) {
    const auto recordings = store_.List();
    uint32_t pending = recordings.size();
    PostState(NetworkState::kWifiConnecting, pending);
    if (!ConnectWifi(settings)) {
        PostState(NetworkState::kWifiOffline, pending);
        DisconnectWifi();
        return;
    }
    if (!EnsureClock(settings)) {
        PostState(NetworkState::kServerOffline, pending);
        DisconnectWifi();
        return;
    }

    bool auth_error = false;
    if (!CheckServer(settings, auth_error)) {
        PostState(auth_error ? NetworkState::kAuthError : NetworkState::kServerOffline, pending);
        DisconnectWifi();
        return;
    }

    for (const auto& recording : recordings) {
        if (paused_.load()) {
            break;
        }
        PostState(NetworkState::kUploading, pending);
        if (!UploadRecording(settings, recording, auth_error)) {
            PostState(auth_error ? NetworkState::kAuthError : NetworkState::kServerOffline,
                      pending);
            DisconnectWifi();
            return;
        }
        if (pending > 0) {
            --pending;
        }
    }
    if (!paused_.load()) {
        PostState(NetworkState::kOnline, pending);
    }
    DisconnectWifi();
}

bool NetworkUploader::ConnectWifi(const NetworkSettings& settings) {
    const esp_err_t netif_result = esp_netif_init();
    if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    const esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE) {
        return false;
    }
    sta_netif_ = esp_netif_create_default_wifi_sta();
    if (sta_netif_ == nullptr) {
        return false;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    init_config.static_rx_buf_num = 4;
    init_config.dynamic_rx_buf_num = 8;
    init_config.dynamic_tx_buf_num = 8;
    init_config.mgmt_sbuf_num = 8;
    init_config.ampdu_rx_enable = 0;
    init_config.ampdu_tx_enable = 0;
    init_config.rx_ba_win = 0;
    if (esp_wifi_init(&init_config) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
        return false;
    }
    if (esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                            WifiEventHandler, this, &wifi_handler_) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventHandler,
                                            this, &ip_handler_) != ESP_OK) {
        return false;
    }

    wifi_config_t wifi_config = {};
    std::memcpy(wifi_config.sta.ssid, settings.wifi_ssid.data(), settings.wifi_ssid.size());
    std::memcpy(wifi_config.sta.password, settings.wifi_password.data(),
                settings.wifi_password.size());
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    xEventGroupClearBits(wifi_events_, kWifiConnected | kWifiFailed);
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK || esp_wifi_start() != ESP_OK ||
        esp_wifi_connect() != ESP_OK) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(wifi_events_, kWifiConnected | kWifiFailed,
                                                  pdTRUE, pdFALSE, kConnectTimeout);
    return (bits & kWifiConnected) != 0;
}

bool NetworkUploader::EnsureClock(const NetworkSettings& settings) {
    if (settings.server_url.rfind("https://", 0) != 0 || std::time(nullptr) > 1700000000) {
        return true;
    }
    if (!sntp_started_) {
        const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&config) != ESP_OK) {
            return false;
        }
        sntp_started_ = true;
    } else if (esp_netif_sntp_start() != ESP_OK) {
        return false;
    }
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK ||
           std::time(nullptr) > 1700000000;
}

void NetworkUploader::DisconnectWifi() {
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (wifi_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                              wifi_handler_);
        wifi_handler_ = nullptr;
    }
    if (ip_handler_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
        ip_handler_ = nullptr;
    }
    esp_wifi_deinit();
    if (sta_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(sta_netif_);
        sta_netif_ = nullptr;
    }
}

bool NetworkUploader::CheckServer(const NetworkSettings& settings, bool& auth_error) {
    HeaderContext context;
    const std::string url = TrimTrailingSlash(settings.server_url) + "/api/v1/status";
    esp_http_client_config_t client_config = {};
    client_config.url = url.c_str();
    client_config.timeout_ms = 5000;
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
    client_config.event_handler = HttpEventHandler;
    client_config.user_data = &context;
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == nullptr) {
        return false;
    }
    const std::string authorization = "Bearer " + settings.upload_token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    auth_error = status == 401 || status == 403;
    return result == ESP_OK && status == 200;
}

bool NetworkUploader::UploadRecording(const NetworkSettings& settings,
                                      const RecordingInfo& recording, bool& auth_error) {
    RecordingHeader header;
    FILE* file = store_.Open(recording.sequence, header);
    if (file == nullptr) {
        return false;
    }
    const uint64_t total_size = sizeof(RecordingHeader) + header.data_bytes;
    const std::string url = RecordingUrl(settings, recording.sequence);
    uint64_t offset = 0;
    bool complete = false;
    bool success = QueryOffset(url, settings, offset, complete, auth_error);
    if (success && !complete) {
        success = offset <= total_size &&
                  PutFile(url, settings, file, header, offset, total_size, auth_error);
    }
    std::fclose(file);
    return success;
}

bool NetworkUploader::QueryOffset(const std::string& url, const NetworkSettings& settings,
                                  uint64_t& offset, bool& complete, bool& auth_error) {
    HeaderContext context;
    esp_http_client_config_t client_config = {};
    client_config.url = url.c_str();
    client_config.timeout_ms = 5000;
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
    client_config.event_handler = HttpEventHandler;
    client_config.user_data = &context;
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == nullptr) {
        return false;
    }
    const std::string authorization = "Bearer " + settings.upload_token;
    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    auth_error = status == 401 || status == 403;
    if (result != ESP_OK || auth_error || (status != 200 && status != 404)) {
        return false;
    }
    offset = status == 404 ? 0 : context.offset;
    complete = status == 200 && !context.saw_offset;
    return status == 404 || complete || context.saw_offset;
}

bool NetworkUploader::PutFile(const std::string& url, const NetworkSettings& settings, FILE* file,
                              const RecordingHeader& header, uint64_t offset, uint64_t total_size,
                              bool& auth_error) {
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    HeaderContext context;
    esp_http_client_config_t client_config = {};
    client_config.url = url.c_str();
    client_config.timeout_ms = 10000;
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
    client_config.event_handler = HttpEventHandler;
    client_config.user_data = &context;
    esp_http_client_handle_t client = esp_http_client_init(&client_config);
    if (client == nullptr) {
        return false;
    }
    const std::string authorization = "Bearer " + settings.upload_token;
    char offset_text[24];
    char total_text[24];
    char samples_text[16];
    std::snprintf(offset_text, sizeof(offset_text), "%" PRIu64, offset);
    std::snprintf(total_text, sizeof(total_text), "%" PRIu64, total_size);
    std::snprintf(samples_text, sizeof(samples_text), "%lu",
                  static_cast<unsigned long>(header.sample_count));
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "Upload-Offset", offset_text);
    esp_http_client_set_header(client, "Upload-Length", total_text);
    esp_http_client_set_header(client, "X-Sample-Count", samples_text);
    esp_http_client_set_header(client, "X-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Codec", "opus-framed-v2");

    bool success =
        esp_http_client_open(client, static_cast<int>(total_size - offset)) == ESP_OK;
    std::array<uint8_t, kUploadBufferBytes> buffer = {};
    uint64_t sent = offset;
    while (success && sent < total_size && !paused_.load()) {
        const size_t wanted = static_cast<size_t>(
            std::min<uint64_t>(buffer.size(), total_size - sent));
        const size_t read = std::fread(buffer.data(), 1, wanted, file);
        if (read != wanted) {
            success = false;
            break;
        }
        size_t written = 0;
        while (written < read) {
            const int result = esp_http_client_write(
                client, reinterpret_cast<const char*>(buffer.data() + written), read - written);
            if (result <= 0) {
                success = false;
                break;
            }
            written += result;
            sent += result;
        }
        vTaskDelay(1);
    }
    if (success && sent == total_size) {
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        auth_error = status == 401 || status == 403;
        success = status == 201 || status == 204;
    } else {
        success = false;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

std::string NetworkUploader::RecordingUrl(const NetworkSettings& settings,
                                          uint32_t sequence) const {
    char recording_id[16];
    std::snprintf(recording_id, sizeof(recording_id), "R%07lu",
                  static_cast<unsigned long>(sequence));
    return TrimTrailingSlash(settings.server_url) + "/api/v1/uploads/" + device_id_ + "/" +
           recording_id;
}

void NetworkUploader::PostState(NetworkState state, uint32_t pending) {
    const AppMessage message = {.event = AppEvent::kNetworkStatus,
                                .value = static_cast<uint32_t>(state),
                                .value2 = pending};
    xQueueSend(app_queue_, &message, 0);
}
