#pragma once

#include "app_types.h"

#include <array>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/i2c_master.h>
#include <iot_button.h>

class PassportBoard {
public:
    void InitializeDisplay();
    void InitializeButtons(QueueHandle_t event_queue);
    void InitializeBattery(i2c_master_bus_handle_t i2c_bus);
    int BatteryLevel();

private:
    struct CallbackContext {
        QueueHandle_t queue = nullptr;
        AppEvent event = AppEvent::kUp;
    };

    static void ButtonCallback(void* button, void* user_data);

    std::array<button_handle_t, 3> buttons_{};
    std::array<CallbackContext, 6> callback_contexts_{};
    i2c_master_dev_handle_t battery_ = nullptr;
};
