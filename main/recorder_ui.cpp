#include "recorder_ui.h"

#include <cstdio>

#include <esp_lvgl_port.h>

namespace {

constexpr uint32_t kBackground = 0x10141D;
constexpr uint32_t kPanel = 0x1C2533;
constexpr uint32_t kText = 0xF2F5F8;
constexpr uint32_t kMuted = 0x9AA8B8;
constexpr uint32_t kBlue = 0x4FA3FF;
constexpr uint32_t kRed = 0xFF4F5E;
constexpr uint32_t kGreen = 0x42D392;
constexpr uint32_t kOrange = 0xFFB347;

void FormatTime(uint32_t seconds, char* output, size_t size) {
    std::snprintf(output, size, "%02lu:%02lu", static_cast<unsigned long>(seconds / 60),
                  static_cast<unsigned long>(seconds % 60));
}

void FormatFileSize(uint32_t bytes, char* output, size_t size) {
    if (bytes < 1024 * 1024) {
        std::snprintf(output, size, "%lu KB",
                      static_cast<unsigned long>((bytes + 1023) / 1024));
    } else {
        std::snprintf(output, size, "%.2f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
}

}  // namespace

void RecorderUi::Initialize() {
    lvgl_port_lock(0);
    screen_ = lv_screen_active();
    lv_obj_set_style_bg_color(screen_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lvgl_port_unlock();
}

void RecorderUi::Render(const char* state, const char* primary, const char* secondary,
                        const char* footer, uint32_t accent) {
    lvgl_port_lock(0);
    battery_label_ = nullptr;
    network_label_ = nullptr;
    lv_obj_clean(screen_);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

    lv_obj_t* title = lv_label_create(screen_);
    lv_label_set_text(title, "FOLO RECORDER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kMuted), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 18);

    battery_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_font(battery_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_label_, LV_ALIGN_TOP_RIGHT, -12, 18);
    UpdateBatteryLabel();

    network_label_ = lv_label_create(screen_);
    lv_obj_set_style_text_font(network_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(network_label_, LV_ALIGN_TOP_LEFT, 12, 38);
    UpdateNetworkLabel();

    lv_obj_t* panel = lv_obj_create(screen_);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 212, 186);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(kPanel), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(accent), 0);
    lv_obj_set_style_radius(panel, 18, 0);

    lv_obj_t* state_label = lv_label_create(panel);
    lv_label_set_text(state_label, state);
    lv_obj_set_style_text_font(state_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(state_label, lv_color_hex(accent), 0);
    lv_obj_align(state_label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t* primary_label = lv_label_create(panel);
    lv_label_set_text(primary_label, primary);
    lv_obj_set_width(primary_label, 186);
    lv_obj_set_style_text_align(primary_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(primary_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(primary_label, lv_color_hex(kText), 0);
    lv_obj_align(primary_label, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t* secondary_label = lv_label_create(panel);
    lv_label_set_text(secondary_label, secondary);
    lv_obj_set_width(secondary_label, 186);
    lv_obj_set_style_text_align(secondary_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(secondary_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(secondary_label, lv_color_hex(kMuted), 0);
    lv_obj_align(secondary_label, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_t* footer_label = lv_label_create(screen_);
    lv_label_set_text(footer_label, footer);
    lv_obj_set_width(footer_label, 224);
    lv_obj_set_style_text_align(footer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(footer_label, lv_color_hex(kMuted), 0);
    lv_obj_align(footer_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lvgl_port_unlock();
}

void RecorderUi::UpdateBatteryLabel() {
    if (battery_label_ == nullptr) {
        return;
    }
    char text[16];
    if (battery_level_ < 0) {
        std::snprintf(text, sizeof(text), "BAT --");
    } else {
        std::snprintf(text, sizeof(text), "BAT %d%%", battery_level_);
    }
    lv_label_set_text(battery_label_, text);
    const uint32_t color = battery_level_ >= 0 && battery_level_ <= 15 ? kRed : kMuted;
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(color), 0);
    lv_obj_align(battery_label_, LV_ALIGN_TOP_RIGHT, -12, 18);
}

void RecorderUi::SetBatteryLevel(int level) {
    battery_level_ = level;
    lvgl_port_lock(0);
    UpdateBatteryLabel();
    lvgl_port_unlock();
}

void RecorderUi::UpdateNetworkLabel() {
    if (network_label_ == nullptr) {
        return;
    }
    const char* text = "NET NOT SET";
    uint32_t color = kMuted;
    char upload_text[24];
    switch (network_state_) {
        case NetworkState::kWifiConnecting:
            text = "WIFI CONNECTING";
            color = kOrange;
            break;
        case NetworkState::kWifiOffline:
            text = "WIFI OFFLINE";
            color = kRed;
            break;
        case NetworkState::kServerOffline:
            text = "SERVER OFFLINE";
            color = kRed;
            break;
        case NetworkState::kAuthError:
            text = "SERVER AUTH ERROR";
            color = kRed;
            break;
        case NetworkState::kUploading:
            std::snprintf(upload_text, sizeof(upload_text), "UPLOADING %lu",
                          static_cast<unsigned long>(pending_uploads_));
            text = upload_text;
            color = kOrange;
            break;
        case NetworkState::kOnline:
            text = "SERVER ONLINE";
            color = kGreen;
            break;
        case NetworkState::kNotConfigured:
            break;
    }
    lv_label_set_text(network_label_, text);
    lv_obj_set_style_text_color(network_label_, lv_color_hex(color), 0);
    lv_obj_align(network_label_, LV_ALIGN_TOP_LEFT, 12, 38);
}

void RecorderUi::SetNetworkStatus(NetworkState state, uint32_t pending) {
    network_state_ = state;
    pending_uploads_ = pending;
    lvgl_port_lock(0);
    UpdateNetworkLabel();
    lvgl_port_unlock();
}

void RecorderUi::ShowReady(const std::vector<RecordingInfo>& recordings, size_t selected,
                           uint64_t free_bytes, uint64_t total_bytes) {
    char primary[64];
    char secondary[128];
    const uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    const double used_mb = static_cast<double>(used_bytes) / (1024.0 * 1024.0);
    const double total_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    if (recordings.empty()) {
        std::snprintf(primary, sizeof(primary), "NO RECORDINGS");
        std::snprintf(secondary, sizeof(secondary), "USED %.1f / %.1f MB", used_mb, total_mb);
    } else {
        const auto& recording = recordings[selected];
        char duration[16];
        char file_size[20];
        FormatTime(recording.sample_count / kRecordingSampleRate, duration, sizeof(duration));
        FormatFileSize(recording.data_bytes, file_size, sizeof(file_size));
        std::snprintf(primary, sizeof(primary), "R%07lu", static_cast<unsigned long>(recording.sequence));
        std::snprintf(secondary, sizeof(secondary), "%s   %u/%u   %s\nUSED %.1f / %.1f MB",
                      duration, static_cast<unsigned>(selected + 1),
                      static_cast<unsigned>(recordings.size()), file_size, used_mb, total_mb);
    }
    Render("READY", primary, secondary,
           "OK: record   UP/DOWN: select\nHold OK: play   Hold UP: delete\nHold DOWN: Wi-Fi",
           kBlue);
}

void RecorderUi::ShowRecording(uint32_t seconds, uint32_t recording_bytes, uint64_t free_bytes,
                               uint64_t total_bytes) {
    char time[16];
    char secondary[80];
    char file_size[20];
    FormatTime(seconds, time, sizeof(time));
    FormatFileSize(recording_bytes, file_size, sizeof(file_size));
    const uint64_t used_bytes = total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    std::snprintf(secondary, sizeof(secondary), "THIS %s\nUSED %.1f / %.1f MB", file_size,
                  static_cast<double>(used_bytes) / (1024.0 * 1024.0),
                  static_cast<double>(total_bytes) / (1024.0 * 1024.0));
    Render("RECORDING", time, secondary, "Press OK to stop and save", kRed);
}

void RecorderUi::ShowPlaying(const RecordingInfo& recording, uint32_t seconds, int volume) {
    char primary[40];
    char secondary[48];
    char elapsed[16];
    char total[16];
    FormatTime(seconds, elapsed, sizeof(elapsed));
    FormatTime(recording.sample_count / kRecordingSampleRate, total, sizeof(total));
    std::snprintf(primary, sizeof(primary), "%s / %s", elapsed, total);
    std::snprintf(secondary, sizeof(secondary), "R%07lu   VOL %d%%",
                  static_cast<unsigned long>(recording.sequence), volume);
    Render("PLAYING", primary, secondary, "UP/DOWN: volume   Hold OK: stop", kGreen);
}

void RecorderUi::ShowDeleteConfirm(const RecordingInfo& recording) {
    char primary[40];
    char secondary[48];
    char file_size[20];
    FormatFileSize(recording.data_bytes, file_size, sizeof(file_size));
    std::snprintf(primary, sizeof(primary), "R%07lu",
                  static_cast<unsigned long>(recording.sequence));
    std::snprintf(secondary, sizeof(secondary), "%s will be removed", file_size);
    Render("DELETE?", primary, secondary, "OK: delete   UP/DOWN: cancel", kOrange);
}

void RecorderUi::ShowExport(const std::string& ssid, const std::string& password) {
    char secondary[96];
    std::snprintf(secondary, sizeof(secondary), "PASS: %s\n192.168.4.1", password.c_str());
    Render("WI-FI EXPORT", ssid.c_str(), secondary, "Connect and open browser\nHold DOWN to exit", kGreen);
}

void RecorderUi::ShowMessage(const char* title, const char* detail) {
    Render(title, detail, "", "", kRed);
}
