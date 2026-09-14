#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

#include <driver/i2c_master.h>
#include <driver/i2s_std.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

class PassportAudio {
public:
    ~PassportAudio();

    bool Initialize();
    bool Read(int16_t* samples, size_t count);
    bool Write(const int16_t* samples, size_t count);
    void SetVolume(int volume);
    i2c_master_bus_handle_t i2c_bus() const { return i2c_bus_; }

private:
    bool InitializeI2c();
    bool InitializeI2s();
    bool InitializeCodec();

    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    esp_codec_dev_handle_t device_ = nullptr;
    std::mutex mutex_;
    int volume_ = 65;
};
