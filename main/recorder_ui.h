#pragma once

#include "recording_store.h"
#include "network_uploader.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <lvgl.h>

class RecorderUi {
public:
    void Initialize();
    void ShowReady(const std::vector<RecordingInfo>& recordings, size_t selected,
                   uint64_t free_bytes, uint64_t total_bytes);
    void ShowRecording(uint32_t seconds, uint32_t recording_bytes, uint64_t free_bytes,
                       uint64_t total_bytes);
    void ShowPlaying(const RecordingInfo& recording, uint32_t seconds, int volume);
    void ShowDeleteConfirm(const RecordingInfo& recording);
    void ShowExport(const std::string& ssid, const std::string& password);
    void ShowMessage(const char* title, const char* detail);
    void SetBatteryLevel(int level);
    void SetNetworkStatus(NetworkState state, uint32_t pending);

private:
    void Render(const char* state, const char* primary, const char* secondary,
                const char* footer, uint32_t accent);
    void UpdateBatteryLabel();
    void UpdateNetworkLabel();

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    int battery_level_ = -1;
    NetworkState network_state_ = NetworkState::kNotConfigured;
    uint32_t pending_uploads_ = 0;
};
