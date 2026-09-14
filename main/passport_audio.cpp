#include "passport_audio.h"

#include "recording_store.h"

#include <algorithm>

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "PassportAudio";
constexpr gpio_num_t kMclk = GPIO_NUM_6;
constexpr gpio_num_t kBclk = GPIO_NUM_5;
constexpr gpio_num_t kWordSelect = GPIO_NUM_3;
constexpr gpio_num_t kDataOut = GPIO_NUM_2;
constexpr gpio_num_t kDataIn = GPIO_NUM_4;
constexpr gpio_num_t kI2cSda = GPIO_NUM_10;
constexpr gpio_num_t kI2cScl = GPIO_NUM_7;
constexpr uint8_t kEs8311Address = ES8311_CODEC_DEFAULT_ADDR;

}  // namespace

PassportAudio::~PassportAudio() {
    if (device_ != nullptr) {
        esp_codec_dev_close(device_);
        esp_codec_dev_delete(device_);
    }
    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
    }
    if (rx_handle_ != nullptr) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
    }
    if (i2c_bus_ != nullptr) {
        i2c_del_master_bus(i2c_bus_);
    }
}

bool PassportAudio::Initialize() {
    return InitializeI2c() && InitializeI2s() && InitializeCodec();
}

bool PassportAudio::InitializeI2c() {
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = kI2cSda,
        .scl_io_num = kI2cScl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {.enable_internal_pullup = 1, .allow_pd = 0},
    };
    const esp_err_t result = i2c_new_master_bus(&config, &i2c_bus_);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "I2C init failed: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}

bool PassportAudio::InitializeI2s() {
    const i2s_chan_config_t channel_config = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 12,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
    };
    if (i2s_new_channel(&channel_config, &tx_handle_, &rx_handle_) != ESP_OK) {
        return false;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRecordingSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = kMclk,
            .bclk = kBclk,
            .ws = kWordSelect,
            .dout = kDataOut,
            .din = kDataIn,
            .invert_flags = {},
        },
    };
    if (i2s_channel_init_std_mode(tx_handle_, &standard_config) != ESP_OK ||
        i2s_channel_init_std_mode(rx_handle_, &standard_config) != ESP_OK ||
        i2s_channel_enable(tx_handle_) != ESP_OK || i2s_channel_enable(rx_handle_) != ESP_OK) {
        ESP_LOGE(kTag, "I2S init failed");
        return false;
    }
    return true;
}

bool PassportAudio::InitializeCodec() {
    audio_codec_i2s_cfg_t i2s_config = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
        .clk_src = 0,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_config);

    audio_codec_i2c_cfg_t i2c_config = {
        .port = I2C_NUM_0,
        .addr = kEs8311Address,
        .bus_handle = i2c_bus_,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_config);
    gpio_if_ = audio_codec_new_gpio();
    if (data_if_ == nullptr || ctrl_if_ == nullptr || gpio_if_ == nullptr) {
        return false;
    }

    uint8_t reset = 0x1f;
    if (ctrl_if_->write_reg(ctrl_if_, 0x00, 1, &reset, 1) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "ES8311 reset failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    es8311_codec_cfg_t codec_config = {};
    codec_config.ctrl_if = ctrl_if_;
    codec_config.gpio_if = gpio_if_;
    codec_config.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    codec_config.pa_pin = GPIO_NUM_NC;
    codec_config.use_mclk = true;
    codec_config.hw_gain.pa_voltage = 5.0f;
    codec_config.hw_gain.codec_dac_voltage = 3.3f;
    codec_if_ = es8311_codec_new(&codec_config);
    if (codec_if_ == nullptr) {
        return false;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    device_ = esp_codec_dev_new(&device_config);
    if (device_ == nullptr) {
        return false;
    }
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = kRecordingSampleRate,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(device_, &sample_info) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_gain(device_, 30.0f) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(device_, volume_) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "ES8311 open failed");
        return false;
    }
    ESP_LOGI(kTag, "ES8311 ready at %lu Hz", static_cast<unsigned long>(kRecordingSampleRate));
    return true;
}

bool PassportAudio::Read(int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr &&
           esp_codec_dev_read(device_, samples, count * sizeof(int16_t)) == ESP_CODEC_DEV_OK;
}

bool PassportAudio::Write(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_ != nullptr &&
           esp_codec_dev_write(device_, const_cast<int16_t*>(samples),
                               count * sizeof(int16_t)) == ESP_CODEC_DEV_OK;
}

void PassportAudio::SetVolume(int volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    volume_ = std::clamp(volume, 0, 100);
    if (device_ != nullptr) {
        esp_codec_dev_set_out_vol(device_, volume_);
    }
}
