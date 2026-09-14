#include "passport_board.h"

#include <button_adc.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

namespace {

constexpr char kTag[] = "PassportBoard";
constexpr gpio_num_t kDisplaySclk = GPIO_NUM_8;
constexpr gpio_num_t kDisplayMosi = GPIO_NUM_9;
constexpr gpio_num_t kDisplayCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayDc = GPIO_NUM_20;
constexpr gpio_num_t kDisplayBacklight = GPIO_NUM_21;
constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 320;

constexpr adc_unit_t kButtonAdcUnit = ADC_UNIT_1;
constexpr adc_channel_t kButtonAdcChannel = ADC_CHANNEL_0;
constexpr uint16_t kCw2017Address = 0x63;

enum ButtonIndex : uint8_t { kUpButton, kDownButton, kConfirmButton, kButtonCount };

constexpr uint16_t kButtonVoltageWindows[kButtonCount][2] = {
    {0, 150},
    {150, 447},
    {447, 1900},
};

struct PanelInitCommand {
    uint8_t command;
    uint8_t data[16];
    uint8_t data_length;
    uint16_t delay_ms;
};

constexpr PanelInitCommand kPanelInitCommands[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},
    {0xB7, {0x35}, 1, 0},
    {0xBB, {0x21}, 1, 0},
    {0xC0, {0x2C}, 1, 0},
    {0xC2, {0x01}, 1, 0},
    {0xC3, {0x0B}, 1, 0},
    {0xC4, {0x20}, 1, 0},
    {0xC6, {0x0F}, 1, 0},
    {0xD0, {0xA7, 0xA1}, 2, 0},
    {0xD0, {0xA4, 0xA1}, 2, 0},
    {0xD6, {0xA1}, 1, 0},
    {0xE0,
     {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43, 0x49, 0x09, 0x16, 0x15, 0x26, 0x2B},
     14,
     0},
    {0xE1,
     {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44, 0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A},
     14,
     10},
};

void SetBacklight(bool enabled) {
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << kDisplayBacklight,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_set_level(kDisplayBacklight, enabled ? 1 : 0));
}

}  // namespace

void PassportBoard::InitializeDisplay() {
    SetBacklight(false);
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = kDisplayMosi;
    bus_config.miso_io_num = GPIO_NUM_NC;
    bus_config.sclk_io_num = kDisplaySclk;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = kDisplayWidth * 64 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = kDisplayCs;
    io_config.dc_gpio_num = kDisplayDc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io_config, &panel_io));

    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    for (const auto& command : kPanelInitCommands) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel_io, command.command, command.data,
                                                  command.data_length));
        if (command.delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
        }
    }
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));

    lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    port_config.task_priority = 2;
    port_config.task_stack = 4096;
    port_config.task_affinity = 0;
    ESP_ERROR_CHECK(lvgl_port_init(&port_config));

    lvgl_port_display_cfg_t display_config = {};
    display_config.io_handle = panel_io;
    display_config.panel_handle = panel;
    display_config.buffer_size = kDisplayWidth * 24;
    display_config.double_buffer = false;
    display_config.hres = kDisplayWidth;
    display_config.vres = kDisplayHeight;
    display_config.color_format = LV_COLOR_FORMAT_RGB565;
    display_config.flags.buff_dma = 1;
    display_config.flags.swap_bytes = 1;
    ESP_ERROR_CHECK(lvgl_port_add_disp(&display_config) == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    const esp_err_t display_result = esp_lcd_panel_disp_on_off(panel, true);
    if (display_result != ESP_OK && display_result != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(display_result);
    }
    SetBacklight(true);
}

void PassportBoard::InitializeButtons(QueueHandle_t event_queue) {
    const button_config_t button_config = {
        .long_press_time = 900,
        .short_press_time = 40,
    };
    for (uint8_t index = 0; index < kButtonCount; ++index) {
        const button_adc_config_t adc_config = {
            .adc_handle = nullptr,
            .unit_id = kButtonAdcUnit,
            .adc_channel = kButtonAdcChannel,
            .button_index = index,
            .min = kButtonVoltageWindows[index][0],
            .max = kButtonVoltageWindows[index][1],
        };
        ESP_ERROR_CHECK(iot_button_new_adc_device(&button_config, &adc_config, &buttons_[index]));
    }

    callback_contexts_ = {{{event_queue, AppEvent::kUp},
                           {event_queue, AppEvent::kDown},
                           {event_queue, AppEvent::kConfirm},
                           {event_queue, AppEvent::kConfirmLong},
                           {event_queue, AppEvent::kDeleteLong},
                           {event_queue, AppEvent::kExportLong}}};
    ESP_ERROR_CHECK(iot_button_register_cb(buttons_[kUpButton], BUTTON_SINGLE_CLICK, nullptr,
                                            ButtonCallback, &callback_contexts_[0]));
    ESP_ERROR_CHECK(iot_button_register_cb(buttons_[kDownButton], BUTTON_SINGLE_CLICK, nullptr,
                                            ButtonCallback, &callback_contexts_[1]));
    ESP_ERROR_CHECK(iot_button_register_cb(buttons_[kConfirmButton], BUTTON_SINGLE_CLICK, nullptr,
                                            ButtonCallback, &callback_contexts_[2]));
    ESP_ERROR_CHECK(iot_button_register_cb(buttons_[kConfirmButton], BUTTON_LONG_PRESS_START,
                                            nullptr, ButtonCallback, &callback_contexts_[3]));
    ESP_ERROR_CHECK(iot_button_register_cb(buttons_[kUpButton], BUTTON_LONG_PRESS_START, nullptr,
                                            ButtonCallback, &callback_contexts_[4]));
    ESP_ERROR_CHECK(iot_button_register_cb(buttons_[kDownButton], BUTTON_LONG_PRESS_START, nullptr,
                                            ButtonCallback, &callback_contexts_[5]));
    ESP_LOGI(kTag, "ADC buttons ready");
}

void PassportBoard::InitializeBattery(i2c_master_bus_handle_t i2c_bus) {
    if (i2c_bus == nullptr) {
        ESP_LOGW(kTag, "Cannot initialize battery gauge without I2C bus");
        return;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kCw2017Address,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = {},
    };
    if (i2c_master_bus_add_device(i2c_bus, &config, &battery_) != ESP_OK) {
        battery_ = nullptr;
        ESP_LOGW(kTag, "Failed to add CW2017 battery gauge");
        return;
    }
    uint8_t register_address = 0x00;
    uint8_t version = 0;
    if (i2c_master_transmit_receive(battery_, &register_address, 1, &version, 1, 100) != ESP_OK) {
        i2c_master_bus_rm_device(battery_);
        battery_ = nullptr;
        ESP_LOGW(kTag, "CW2017 battery gauge not detected");
        return;
    }
    ESP_LOGI(kTag, "CW2017 battery gauge ready, version=0x%02x", version);
}

int PassportBoard::BatteryLevel() {
    if (battery_ == nullptr) {
        return -1;
    }
    uint8_t register_address = 0x04;
    uint8_t state_of_charge[2] = {};
    if (i2c_master_transmit_receive(battery_, &register_address, 1, state_of_charge,
                                    sizeof(state_of_charge), 100) != ESP_OK ||
        state_of_charge[0] > 100) {
        ESP_LOGW(kTag, "Failed to read CW2017 state of charge");
        return -1;
    }
    return state_of_charge[0];
}

void PassportBoard::ButtonCallback(void*, void* user_data) {
    const auto* context = static_cast<CallbackContext*>(user_data);
    if (context == nullptr || context->queue == nullptr) {
        return;
    }
    const AppMessage message = {.event = context->event};
    xQueueSend(context->queue, &message, 0);
}
