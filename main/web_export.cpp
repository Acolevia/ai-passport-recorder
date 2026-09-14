#include "web_export.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_opus_dec.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "WebExport";
constexpr char kPasswordAlphabet[] = "0123456789";
constexpr size_t kPasswordLength = 8;  // WPA2-PSK requires at least eight characters.
constexpr size_t kOggPageHeaderBytes = 28;  // Fixed header plus one lacing byte.
constexpr uint32_t kOggGranuleSamplesPerFrame = 960;  // Opus granules always use 48 kHz.
constexpr uint32_t kOggSerialBase = 0x4f4c4f46;       // Stable stream serial base.

std::string GeneratePassword() {
    std::string password(kPasswordLength, 'A');
    for (char& character : password) {
        character = kPasswordAlphabet[esp_random() % (sizeof(kPasswordAlphabet) - 1)];
    }
    return password;
}

#pragma pack(push, 1)
struct WaveHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t riff_size = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t sample_rate = kRecordingSampleRate;
    uint32_t byte_rate = kRecordingSampleRate * 2;
    uint16_t block_align = 2;
    uint16_t bits_per_sample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)

static_assert(sizeof(WaveHeader) == 44);

constexpr char kPageHead[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI Passport Recorder</title><style>
body{font-family:system-ui,sans-serif;background:#10141d;color:#f2f5f8;max-width:720px;margin:auto;padding:24px}
h1{font-size:26px}.sub{color:#9aa8b8}.card{background:#1c2533;border-radius:14px;padding:16px;margin:12px 0}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}audio{width:100%;margin:10px 0}
a,button{background:#267bd9;color:white;border:0;border-radius:9px;padding:9px 13px;text-decoration:none;font-size:14px}
button.delete{background:#b73845}input{box-sizing:border-box;width:100%;margin:5px 0 12px;padding:9px;border-radius:7px;border:1px solid #47566a;background:#10141d;color:#f2f5f8}label{font-size:13px;color:#9aa8b8}code{color:#42d392}</style></head><body>
<h1>AI Passport Recorder</h1><p class="sub">Recordings are stored only on this badge. Downloads are converted to standard PCM WAV.</p>
)HTML";

constexpr char kPageTail[] = R"HTML(<p class="sub">Keep this page open only while Wi-Fi export mode is enabled.</p></body></html>)HTML";

esp_err_t SendChunk(httpd_req_t* request, const std::string& text) {
    return httpd_resp_send_chunk(request, text.data(), text.size());
}

std::string HtmlEscape(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (char character : input) {
        switch (character) {
            case '&': output += "&amp;"; break;
            case '<': output += "&lt;"; break;
            case '>': output += "&gt;"; break;
            case '"': output += "&quot;"; break;
            case '\'': output += "&#39;"; break;
            default: output += character; break;
        }
    }
    return output;
}

int HexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool UrlDecode(const std::string& input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '+') {
            output += ' ';
        } else if (input[index] == '%' && index + 2 < input.size()) {
            const int high = HexValue(input[index + 1]);
            const int low = HexValue(input[index + 2]);
            if (high < 0 || low < 0) return false;
            output += static_cast<char>((high << 4) | low);
            index += 2;
        } else if (input[index] == '%') {
            return false;
        } else {
            output += input[index];
        }
    }
    return true;
}

bool ParseForm(const std::string& body, std::map<std::string, std::string>& values) {
    size_t start = 0;
    while (start <= body.size()) {
        const size_t end = body.find('&', start);
        const std::string field = body.substr(start, end == std::string::npos ? end : end - start);
        const size_t separator = field.find('=');
        if (separator == std::string::npos) return false;
        std::string key;
        std::string value;
        if (!UrlDecode(field.substr(0, separator), key) ||
            !UrlDecode(field.substr(separator + 1), value)) {
            return false;
        }
        values[key] = value;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool LogStartFailure(const char* step, esp_err_t error) {
    ESP_LOGE(kTag, "%s failed: %s (free heap=%lu, minimum=%lu)", step,
             esp_err_to_name(error), static_cast<unsigned long>(esp_get_free_heap_size()),
             static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
    return false;
}

struct HttpByteRange {
    uint64_t first = 0;
    uint64_t last = 0;
    bool partial = false;
};

bool ParseUnsigned(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    value = std::strtoull(text, &end, 10);
    return end != text && *end == '\0';
}

bool ParseRange(httpd_req_t* request, uint64_t total_size, HttpByteRange& range) {
    range = {.first = 0, .last = total_size - 1, .partial = false};
    const size_t header_length = httpd_req_get_hdr_value_len(request, "Range");
    if (header_length == 0) {
        return true;
    }
    if (header_length > 63) {
        return false;
    }

    std::array<char, 64> header = {};
    if (httpd_req_get_hdr_value_str(request, "Range", header.data(), header.size()) != ESP_OK ||
        std::strncmp(header.data(), "bytes=", 6) != 0 ||
        std::strchr(header.data() + 6, ',') != nullptr) {
        return false;
    }
    char* first_text = header.data() + 6;
    char* dash = std::strchr(first_text, '-');
    if (dash == nullptr) {
        return false;
    }
    *dash = '\0';
    char* last_text = dash + 1;

    if (*first_text == '\0') {
        uint64_t suffix_length = 0;
        if (!ParseUnsigned(last_text, suffix_length) || suffix_length == 0) {
            return false;
        }
        suffix_length = std::min(suffix_length, total_size);
        range.first = total_size - suffix_length;
        range.last = total_size - 1;
    } else {
        if (!ParseUnsigned(first_text, range.first) || range.first >= total_size) {
            return false;
        }
        if (*last_text == '\0') {
            range.last = total_size - 1;
        } else if (!ParseUnsigned(last_text, range.last) || range.last < range.first) {
            return false;
        } else {
            range.last = std::min(range.last, total_size - 1);
        }
    }
    range.partial = true;
    return true;
}

esp_err_t SendAll(httpd_req_t* request, const void* data, size_t size) {
    const char* cursor = static_cast<const char*>(data);
    while (size > 0) {
        const int sent = httpd_send(request, cursor, size);
        if (sent <= 0) {
            return ESP_FAIL;
        }
        cursor += sent;
        size -= static_cast<size_t>(sent);
    }
    return ESP_OK;
}

void WriteLittleEndian16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void WriteLittleEndian32(uint8_t* destination, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        destination[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

void WriteLittleEndian64(uint8_t* destination, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        destination[index] = static_cast<uint8_t>(value >> (index * 8));
    }
}

uint32_t OggCrc(const uint8_t* data, size_t size) {
    uint32_t crc = 0;
    for (size_t index = 0; index < size; ++index) {
        crc ^= static_cast<uint32_t>(data[index]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    return crc;
}

size_t BuildOggPage(uint8_t* page, size_t capacity, const uint8_t* packet, size_t packet_size,
                    uint8_t flags, uint64_t granule_position, uint32_t serial,
                    uint32_t sequence) {
    // Recorder packets are bounded by the encoder's 160-byte output frame.
    // Keeping one packet and one lacing value per page makes Range responses
    // deterministic and lets browsers seek without an index in RAM.
    if (page == nullptr || packet == nullptr || packet_size == 0 || packet_size >= 255 ||
        capacity < kOggPageHeaderBytes + packet_size) {
        return 0;
    }
    std::memset(page, 0, kOggPageHeaderBytes);
    std::memcpy(page, "OggS", 4);
    page[4] = 0;
    page[5] = flags;
    WriteLittleEndian64(page + 6, granule_position);
    WriteLittleEndian32(page + 14, serial);
    WriteLittleEndian32(page + 18, sequence);
    page[26] = 1;
    page[27] = static_cast<uint8_t>(packet_size);
    std::memcpy(page + kOggPageHeaderBytes, packet, packet_size);
    WriteLittleEndian32(page + 22, OggCrc(page, kOggPageHeaderBytes + packet_size));
    return kOggPageHeaderBytes + packet_size;
}

esp_err_t SendRangeIntersection(httpd_req_t* request, const uint8_t* data, size_t size,
                                uint64_t offset, const HttpByteRange& range) {
    if (size == 0 || offset > range.last || offset + size - 1 < range.first) {
        return ESP_OK;
    }
    const uint64_t first = std::max(offset, range.first);
    const uint64_t last = std::min(offset + size - 1, range.last);
    return SendAll(request, data + (first - offset), static_cast<size_t>(last - first + 1));
}

}  // namespace

WebExport::~WebExport() {
    Stop();
}

bool WebExport::Start() {
    if (server_ != nullptr) {
        return true;
    }
    if (wifi_lock_ == nullptr ||
        xSemaphoreTake(wifi_lock_, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(kTag, "Wi-Fi is busy");
        return false;
    }
    owns_wifi_lock_ = true;
    ESP_LOGI(kTag, "Starting export AP (free heap=%lu)",
             static_cast<unsigned long>(esp_get_free_heap_size()));
    const esp_err_t netif_result = esp_netif_init();
    if (netif_result != ESP_OK && netif_result != ESP_ERR_INVALID_STATE) {
        Stop();
        return LogStartFailure("esp_netif_init", netif_result);
    }
    const esp_err_t event_result = esp_event_loop_create_default();
    if (event_result != ESP_OK && event_result != ESP_ERR_INVALID_STATE) {
        Stop();
        return LogStartFailure("esp_event_loop_create_default", event_result);
    }
    ap_netif_ = esp_netif_create_default_wifi_ap();
    if (ap_netif_ == nullptr) {
        Stop();
        return LogStartFailure("esp_netif_create_default_wifi_ap", ESP_ERR_NO_MEM);
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    // The badge has no PSRAM and only serves up to two local clients. The default
    // 10/32/32 buffer profile needlessly competes with LVGL and the audio pipeline.
    init_config.static_rx_buf_num = 4;
    init_config.dynamic_rx_buf_num = 8;
    init_config.dynamic_tx_buf_num = 8;
    init_config.mgmt_sbuf_num = 8;
    init_config.ampdu_rx_enable = 0;
    init_config.ampdu_tx_enable = 0;
    init_config.rx_ba_win = 0;
    esp_err_t result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        Stop();
        return LogStartFailure("esp_wifi_init", result);
    }
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) {
        Stop();
        return LogStartFailure("esp_wifi_set_storage", result);
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    std::snprintf(ssid, sizeof(ssid), "Passport-Recorder-%02X%02X", mac[4], mac[5]);
    ssid_ = ssid;
    password_ = GeneratePassword();

    wifi_config_t wifi_config = {};
    std::memcpy(wifi_config.ap.ssid, ssid_.data(), ssid_.size());
    wifi_config.ap.ssid_len = ssid_.size();
    std::memcpy(wifi_config.ap.password, password_.data(), password_.size());
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 2;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = false;
    result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result != ESP_OK) {
        Stop();
        return LogStartFailure("esp_wifi_set_mode", result);
    }
    result = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (result != ESP_OK) {
        Stop();
        return LogStartFailure("esp_wifi_set_config", result);
    }
    result = esp_wifi_start();
    if (result != ESP_OK) {
        Stop();
        return LogStartFailure("esp_wifi_start", result);
    }

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 6;
    // lwIP exposes five sockets and the HTTP server reserves three internally.
    server_config.max_open_sockets = 2;
    // On-demand Opus decoding is stack-heavy. The recorder/player task is not
    // resident while export mode is active, leaving this memory available.
    server_config.stack_size = 20 * 1024;
    result = httpd_start(&server_, &server_config);
    if (result != ESP_OK) {
        Stop();
        return LogStartFailure("httpd_start", result);
    }
    const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = IndexHandler, .user_ctx = this};
    const httpd_uri_t download_uri = {
        .uri = "/download", .method = HTTP_GET, .handler = DownloadHandler, .user_ctx = this};
    const httpd_uri_t play_uri = {
        .uri = "/play", .method = HTTP_GET, .handler = PlayHandler, .user_ctx = this};
    const httpd_uri_t delete_uri = {
        .uri = "/delete", .method = HTTP_POST, .handler = DeleteHandler, .user_ctx = this};
    const httpd_uri_t settings_uri = {
        .uri = "/settings", .method = HTTP_POST, .handler = SettingsHandler, .user_ctx = this};
    httpd_register_uri_handler(server_, &index_uri);
    httpd_register_uri_handler(server_, &download_uri);
    httpd_register_uri_handler(server_, &play_uri);
    httpd_register_uri_handler(server_, &delete_uri);
    httpd_register_uri_handler(server_, &settings_uri);
    ESP_LOGI(kTag, "Export AP started: %s (free heap=%lu)", ssid_.c_str(),
             static_cast<unsigned long>(esp_get_free_heap_size()));
    return true;
}

void WebExport::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (ap_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(ap_netif_);
        ap_netif_ = nullptr;
    }
    if (owns_wifi_lock_) {
        xSemaphoreGive(wifi_lock_);
        owns_wifi_lock_ = false;
    }
}

esp_err_t WebExport::IndexHandler(httpd_req_t* request) {
    return static_cast<WebExport*>(request->user_ctx)->SendIndex(request);
}

esp_err_t WebExport::DownloadHandler(httpd_req_t* request) {
    return static_cast<WebExport*>(request->user_ctx)->SendWave(request, true);
}

esp_err_t WebExport::PlayHandler(httpd_req_t* request) {
    return static_cast<WebExport*>(request->user_ctx)->SendOggOpus(request);
}

esp_err_t WebExport::DeleteHandler(httpd_req_t* request) {
    return static_cast<WebExport*>(request->user_ctx)->DeleteRecording(request);
}

esp_err_t WebExport::SettingsHandler(httpd_req_t* request) {
    return static_cast<WebExport*>(request->user_ctx)->SaveSettings(request);
}

esp_err_t WebExport::SendIndex(httpd_req_t* request) {
    ESP_LOGI(kTag, "Serving recording index");
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_send_chunk(request, kPageHead, HTTPD_RESP_USE_STRLEN);
    const auto recordings = store_.List();
    if (recordings.empty()) {
        SendChunk(request, "<div class=\"card\">No recordings yet.</div>");
    }
    for (const auto& recording : recordings) {
        const uint32_t seconds = recording.sample_count / kRecordingSampleRate;
        char item[768];
        std::snprintf(
            item, sizeof(item),
            "<div class=\"card\"><b>R%07lu</b> &nbsp; %02lu:%02lu"
            "<audio controls preload=\"none\" src=\"/play?seq=%lu\"></audio>"
            "<div class=\"row\"><a href=\"/download?seq=%lu\">Download WAV</a>"
            "<form method=\"post\" action=\"/delete?seq=%lu\" "
            "onsubmit=\"return confirm('Delete this recording?')\">"
            "<button class=\"delete\">Delete</button></form></div></div>",
            static_cast<unsigned long>(recording.sequence),
            static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60),
            static_cast<unsigned long>(recording.sequence),
            static_cast<unsigned long>(recording.sequence),
            static_cast<unsigned long>(recording.sequence));
        httpd_resp_send_chunk(request, item, HTTPD_RESP_USE_STRLEN);
    }
    char status[192];
    const uint64_t total_bytes = store_.TotalBytes();
    const uint64_t free_bytes = store_.FreeBytes();
    const uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    std::snprintf(status, sizeof(status),
                  "<p class=\"sub\">%u recording(s), %.1f / %.1f MB used, %.1f MB free.</p>",
                  static_cast<unsigned>(recordings.size()),
                  static_cast<double>(used_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(total_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(free_bytes) / (1024.0 * 1024.0));
    httpd_resp_send_chunk(request, status, HTTPD_RESP_USE_STRLEN);
    const NetworkSettings settings = network_config_.Load();
    const std::string form =
        "<div class=\"card\"><b>Automatic server upload</b>"
        "<p class=\"sub\">Saved recordings upload in the background whenever this Wi-Fi and "
        "server are reachable.</p><form method=\"post\" action=\"/settings\">"
        "<label>Wi-Fi name</label><input name=\"ssid\" maxlength=\"32\" required value=\"" +
        HtmlEscape(settings.wifi_ssid) +
        "\"><label>Wi-Fi password (leave blank to keep saved password)</label>"
        "<input name=\"password\" maxlength=\"64\" type=\"password\">"
        "<label>Server URL</label><input name=\"server\" maxlength=\"192\" required "
        "placeholder=\"https://recorder.example.com\" value=\"" +
        HtmlEscape(settings.server_url) +
        "\"><label>Upload token (leave blank to keep saved token)</label>"
        "<input name=\"token\" maxlength=\"128\" type=\"password\">"
        "<button type=\"submit\">Save upload settings</button></form></div>";
    SendChunk(request, form);
    httpd_resp_send_chunk(request, kPageTail, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t WebExport::SaveSettings(httpd_req_t* request) {
    if (request->content_len <= 0 || request->content_len > 512) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid settings");
    }
    std::string body(static_cast<size_t>(request->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int result = httpd_req_recv(request, body.data() + received, body.size() - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += static_cast<size_t>(result);
    }
    std::map<std::string, std::string> values;
    if (!ParseForm(body, values)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
    }
    NetworkSettings settings = network_config_.Load();
    const std::string previous_ssid = settings.wifi_ssid;
    settings.wifi_ssid = values["ssid"];
    settings.server_url = values["server"];
    if (!values["password"].empty() || settings.wifi_password.empty() ||
        settings.wifi_ssid != previous_ssid) {
        settings.wifi_password = values["password"];
    }
    if (!values["token"].empty() || settings.upload_token.empty()) {
        settings.upload_token = values["token"];
    }
    if (!network_config_.Save(settings)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Check Wi-Fi, URL, and token values");
    }
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_sendstr(request, "Settings saved");
}

bool WebExport::ParseSequence(httpd_req_t* request, uint32_t& sequence) const {
    const size_t length = httpd_req_get_url_query_len(request);
    if (length == 0 || length > 48) {
        return false;
    }
    std::array<char, 49> query = {};
    std::array<char, 16> value = {};
    if (httpd_req_get_url_query_str(request, query.data(), query.size()) != ESP_OK ||
        httpd_query_key_value(query.data(), "seq", value.data(), value.size()) != ESP_OK) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.data(), &end, 10);
    if (end == value.data() || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
        return false;
    }
    sequence = static_cast<uint32_t>(parsed);
    return true;
}

esp_err_t WebExport::SendOggOpus(httpd_req_t* request) {
    uint32_t sequence = 0;
    if (!ParseSequence(request, sequence)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid recording");
    }
    RecordingHeader recording_header;
    FILE* file = store_.Open(sequence, recording_header);
    if (file == nullptr) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording not found");
    }

    std::array<uint8_t, 19> opus_head = {};
    std::memcpy(opus_head.data(), "OpusHead", 8);
    opus_head[8] = 1;  // OpusHead version.
    opus_head[9] = 1;  // Mono.
    WriteLittleEndian16(opus_head.data() + 10, 0);  // No samples are trimmed.
    WriteLittleEndian32(opus_head.data() + 12, kRecordingSampleRate);
    WriteLittleEndian16(opus_head.data() + 16, 0);  // Output gain.
    opus_head[18] = 0;                              // Mono/stereo mapping family.

    constexpr char kVendor[] = "AI Passport Recorder";
    std::array<uint8_t, 8 + 4 + sizeof(kVendor) - 1 + 4> opus_tags = {};
    std::memcpy(opus_tags.data(), "OpusTags", 8);
    WriteLittleEndian32(opus_tags.data() + 8, sizeof(kVendor) - 1);
    std::memcpy(opus_tags.data() + 12, kVendor, sizeof(kVendor) - 1);
    WriteLittleEndian32(opus_tags.data() + 12 + sizeof(kVendor) - 1, 0);

    const uint32_t frame_count = recording_header.sample_count / kOpusFrameSamples;
    const uint64_t headers_size = (kOggPageHeaderBytes + opus_head.size()) +
                                  (kOggPageHeaderBytes + opus_tags.size());
    // Stored frames have a two-byte length prefix; Ogg uses a 28-byte page
    // header for each of these sub-255-byte packets.
    const uint64_t total_size = headers_size + recording_header.data_bytes +
                                static_cast<uint64_t>(frame_count) *
                                    (kOggPageHeaderBytes - sizeof(uint16_t));
    HttpByteRange range;
    if (!ParseRange(request, total_size, range)) {
        std::fclose(file);
        char content_range[48];
        std::snprintf(content_range, sizeof(content_range), "bytes */%llu",
                      static_cast<unsigned long long>(total_size));
        httpd_resp_set_status(request, "416 Range Not Satisfiable");
        httpd_resp_set_hdr(request, "Content-Range", content_range);
        return httpd_resp_send(request, nullptr, 0);
    }

    ESP_LOGI(kTag, "Serving R%07lu as Ogg/Opus, bytes %llu-%llu/%llu",
             static_cast<unsigned long>(sequence),
             static_cast<unsigned long long>(range.first),
             static_cast<unsigned long long>(range.last),
             static_cast<unsigned long long>(total_size));
    const uint64_t response_size = range.last - range.first + 1;
    char response_header[512];
    const int response_header_size = range.partial
        ? std::snprintf(response_header, sizeof(response_header),
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Type: audio/ogg; codecs=opus\r\n"
                        "Content-Length: %llu\r\n"
                        "Content-Range: bytes %llu-%llu/%llu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Disposition: inline; filename=\"R%07lu.opus\"\r\n"
                        "Cache-Control: no-store\r\n\r\n",
                        static_cast<unsigned long long>(response_size),
                        static_cast<unsigned long long>(range.first),
                        static_cast<unsigned long long>(range.last),
                        static_cast<unsigned long long>(total_size),
                        static_cast<unsigned long>(sequence))
        : std::snprintf(response_header, sizeof(response_header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: audio/ogg; codecs=opus\r\n"
                        "Content-Length: %llu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Disposition: inline; filename=\"R%07lu.opus\"\r\n"
                        "Cache-Control: no-store\r\n\r\n",
                        static_cast<unsigned long long>(response_size),
                        static_cast<unsigned long>(sequence));
    if (response_header_size <= 0 ||
        static_cast<size_t>(response_header_size) >= sizeof(response_header) ||
        SendAll(request, response_header, static_cast<size_t>(response_header_size)) != ESP_OK) {
        std::fclose(file);
        return ESP_FAIL;
    }

    const uint32_t serial = kOggSerialBase ^ sequence;
    std::array<uint8_t, kOggPageHeaderBytes + kMaxOpusPacketBytes> page = {};
    uint64_t stream_offset = 0;
    size_t page_size = BuildOggPage(page.data(), page.size(), opus_head.data(), opus_head.size(),
                                    0x02, 0, serial, 0);
    esp_err_t result = page_size == 0
        ? ESP_FAIL
        : SendRangeIntersection(request, page.data(), page_size, stream_offset, range);
    stream_offset += page_size;

    if (result == ESP_OK) {
        page_size = BuildOggPage(page.data(), page.size(), opus_tags.data(), opus_tags.size(),
                                 frame_count == 0 ? 0x04 : 0, 0, serial, 1);
        result = page_size == 0
            ? ESP_FAIL
            : SendRangeIntersection(request, page.data(), page_size, stream_offset, range);
        stream_offset += page_size;
    }

    std::array<uint8_t, kMaxOpusPacketBytes> packet = {};
    uint32_t remaining = recording_header.data_bytes;
    uint64_t granule_position = 0;
    uint32_t page_sequence = 2;
    uint32_t frames_processed = 0;
    while (result == ESP_OK && remaining > 0 && stream_offset <= range.last) {
        size_t packet_size = 0;
        if (!ReadOpusPacket(file, remaining, packet.data(), packet.size(), packet_size)) {
            result = ESP_FAIL;
            break;
        }
        granule_position += kOggGranuleSamplesPerFrame;
        page_size = BuildOggPage(page.data(), page.size(), packet.data(), packet_size,
                                 remaining == 0 ? 0x04 : 0, granule_position, serial,
                                 page_sequence++);
        if (page_size == 0) {
            result = ESP_FAIL;
            break;
        }
        result = SendRangeIntersection(request, page.data(), page_size, stream_offset, range);
        stream_offset += page_size;
        if (++frames_processed % 64 == 0) {
            vTaskDelay(1);
        }
    }
    std::fclose(file);
    return result;
}

esp_err_t WebExport::SendWave(httpd_req_t* request, bool attachment) {
    uint32_t sequence = 0;
    if (!ParseSequence(request, sequence)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid recording");
    }
    ESP_LOGI(kTag, "Serving R%07lu as %s", static_cast<unsigned long>(sequence),
             attachment ? "download" : "playback");
    RecordingHeader recording_header;
    FILE* file = store_.Open(sequence, recording_header);
    if (file == nullptr) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording not found");
    }

    WaveHeader wave_header;
    wave_header.data_size = recording_header.sample_count * sizeof(int16_t);
    wave_header.riff_size = 36 + wave_header.data_size;
    const uint64_t total_size = sizeof(wave_header) + wave_header.data_size;
    HttpByteRange range;
    if (!ParseRange(request, total_size, range)) {
        std::fclose(file);
        char content_range[48];
        std::snprintf(content_range, sizeof(content_range), "bytes */%llu",
                      static_cast<unsigned long long>(total_size));
        httpd_resp_set_status(request, "416 Range Not Satisfiable");
        httpd_resp_set_hdr(request, "Content-Range", content_range);
        return httpd_resp_send(request, nullptr, 0);
    }

    const uint64_t response_size = range.last - range.first + 1;
    void* decoder = nullptr;
    if (range.last >= sizeof(WaveHeader)) {
        esp_opus_dec_cfg_t decoder_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
        decoder_config.sample_rate = kRecordingSampleRate;
        decoder_config.channel = 1;
        decoder_config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        decoder_config.self_delimited = false;
        if (esp_opus_dec_open(&decoder_config, sizeof(decoder_config), &decoder) !=
            ESP_AUDIO_ERR_OK) {
            std::fclose(file);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Opus decoder unavailable");
        }
    }
    char response_header[512];
    const int header_size = range.partial
        ? std::snprintf(response_header, sizeof(response_header),
                        "HTTP/1.1 206 Partial Content\r\n"
                        "Content-Type: audio/wav\r\n"
                        "Content-Length: %llu\r\n"
                        "Content-Range: bytes %llu-%llu/%llu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Disposition: %s; filename=\"R%07lu.wav\"\r\n"
                        "Cache-Control: no-store\r\n\r\n",
                        static_cast<unsigned long long>(response_size),
                        static_cast<unsigned long long>(range.first),
                        static_cast<unsigned long long>(range.last),
                        static_cast<unsigned long long>(total_size),
                        attachment ? "attachment" : "inline",
                        static_cast<unsigned long>(sequence))
        : std::snprintf(response_header, sizeof(response_header),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: audio/wav\r\n"
                        "Content-Length: %llu\r\n"
                        "Accept-Ranges: bytes\r\n"
                        "Content-Disposition: %s; filename=\"R%07lu.wav\"\r\n"
                        "Cache-Control: no-store\r\n\r\n",
                        static_cast<unsigned long long>(response_size),
                        attachment ? "attachment" : "inline",
                        static_cast<unsigned long>(sequence));
    if (header_size <= 0 || static_cast<size_t>(header_size) >= sizeof(response_header) ||
        SendAll(request, response_header, static_cast<size_t>(header_size)) != ESP_OK) {
        if (decoder != nullptr) {
            esp_opus_dec_close(decoder);
        }
        std::fclose(file);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    constexpr uint64_t wave_header_size = sizeof(WaveHeader);
    if (range.first < wave_header_size) {
        const uint64_t header_last = std::min(range.last, wave_header_size - 1);
        result = SendAll(request,
                         reinterpret_cast<const uint8_t*>(&wave_header) + range.first,
                         static_cast<size_t>(header_last - range.first + 1));
    }

    std::array<uint8_t, kMaxOpusPacketBytes> encoded = {};
    std::array<int16_t, kOpusFrameSamples> decoded = {};
    uint32_t remaining = recording_header.data_bytes;
    uint64_t decoded_offset = wave_header_size;
    uint32_t decoded_frames = 0;
    while (result == ESP_OK && remaining > 0 && decoded_offset <= range.last) {
        size_t packet_size = 0;
        if (!ReadOpusPacket(file, remaining, encoded.data(), encoded.size(), packet_size)) {
            result = ESP_FAIL;
            break;
        }
        esp_audio_dec_in_raw_t input = {};
        input.buffer = encoded.data();
        input.len = static_cast<uint32_t>(packet_size);
        esp_audio_dec_out_frame_t output = {};
        output.buffer = reinterpret_cast<uint8_t*>(decoded.data());
        output.len = static_cast<uint32_t>(decoded.size() * sizeof(int16_t));
        esp_audio_dec_info_t info = {};
        if (esp_opus_dec_decode(decoder, &input, &output, &info) != ESP_AUDIO_ERR_OK ||
            output.decoded_size == 0 || output.decoded_size > output.len) {
            result = ESP_FAIL;
            break;
        }
        const size_t decoded_size = output.decoded_size;
        const uint64_t decoded_last = decoded_offset + decoded_size - 1;
        if (decoded_last >= range.first) {
            const uint64_t send_first = std::max(decoded_offset, range.first);
            const uint64_t send_last = std::min(decoded_last, range.last);
            const auto* decoded_bytes = reinterpret_cast<const uint8_t*>(decoded.data());
            result = SendAll(request, decoded_bytes + (send_first - decoded_offset),
                             static_cast<size_t>(send_last - send_first + 1));
        }
        decoded_offset += decoded_size;
        // The ESP32-C3 is single-core. Long WAV responses can otherwise keep
        // the HTTP task runnable for more than the watchdog interval while it
        // decodes Opus faster than the socket drains. Let the idle task run
        // without materially reducing streaming throughput.
        if (++decoded_frames % 8 == 0) {
            vTaskDelay(1);
        }
    }
    if (decoder != nullptr) {
        esp_opus_dec_close(decoder);
    }
    std::fclose(file);
    return result;
}

esp_err_t WebExport::DeleteRecording(httpd_req_t* request) {
    uint32_t sequence = 0;
    if (!ParseSequence(request, sequence)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid recording");
    }
    if (!store_.Delete(sequence)) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Recording not found");
    }
    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", "/");
    return httpd_resp_send(request, nullptr, 0);
}
