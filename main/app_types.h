#pragma once

#include <cstdint>

enum class AppEvent : uint8_t {
    kUp,
    kDown,
    kConfirm,
    kConfirmLong,
    kDeleteLong,
    kExportLong,
    kAudioProgress,
    kAudioFinished,
    kNetworkStatus,
};

struct AppMessage {
    AppEvent event = AppEvent::kUp;
    uint32_t value = 0;
    uint32_t value2 = 0;
    bool success = true;
};
