#include "app_types.h"
#include "network_config.h"
#include "network_uploader.h"
#include "passport_audio.h"
#include "passport_board.h"
#include "recorder_ui.h"
#include "recording_store.h"
#include "web_export.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_opus_enc.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace {

constexpr char kTag[] = "RecorderApp";
constexpr size_t kPcmFrameSamples = kOpusFrameSamples;
constexpr uint64_t kMinimumFreeBytes = 16 * 1024;
constexpr TickType_t kBatteryPollInterval = pdMS_TO_TICKS(15000);
constexpr char kNvsNamespace[] = "recorder";
constexpr char kVolumeKey[] = "volume";
constexpr int kDefaultVolume = 65;
constexpr uint32_t kAudioTaskStackBytes = 20 * 1024;

enum class AppState : uint8_t { kIdle, kRecording, kPlaying, kDeleteConfirm, kExport };
enum class AudioCommandType : uint8_t { kStartRecording, kStop, kPlay, kSetVolume };

struct AudioCommand {
    AudioCommandType type = AudioCommandType::kStop;
    uint32_t sequence = 0;
    int volume = kDefaultVolume;
};

class RecorderApp {
public:
    void Run() {
        InitializeNvs();
        event_queue_ = xQueueCreate(16, sizeof(AppMessage));
        audio_queue_ = xQueueCreate(4, sizeof(AudioCommand));
        wifi_lock_ = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(event_queue_ == nullptr || audio_queue_ == nullptr || wifi_lock_ == nullptr
                            ? ESP_ERR_NO_MEM
                            : ESP_OK);

        board_.InitializeDisplay();
        ui_.Initialize();
        ui_.ShowMessage("STARTING", "Initializing storage and audio");
        if (!store_.Initialize()) {
            ui_.ShowMessage("STORAGE ERROR", "Flash filesystem unavailable");
            Halt();
        }
        if (!audio_.Initialize()) {
            ui_.ShowMessage("AUDIO ERROR", "ES8311 initialization failed");
            Halt();
        }
        volume_ = LoadVolume();
        audio_.SetVolume(volume_);
        board_.InitializeBattery(audio_.i2c_bus());
        battery_level_ = board_.BatteryLevel();
        ui_.SetBatteryLevel(battery_level_);
        last_battery_poll_ = xTaskGetTickCount();
        board_.InitializeButtons(event_queue_);
        web_export_.SetWifiLock(wifi_lock_);
        if (!uploader_.Start(event_queue_, wifi_lock_)) {
            ESP_LOGE(kTag, "Unable to start background uploader");
        }
        RefreshRecordings(true);
        ESP_LOGI(kTag, "Offline recorder ready");

        while (true) {
            AppMessage message;
            if (xQueueReceive(event_queue_, &message, pdMS_TO_TICKS(1000)) == pdTRUE) {
                HandleMessage(message);
            }
            PollBattery();
        }
    }

private:
    static void AudioTaskEntry(void* argument) {
        static_cast<RecorderApp*>(argument)->AudioTask();
    }

    bool StartAudioTask(const AudioCommand& command) {
        if (audio_task_ != nullptr) {
            return false;
        }
        xQueueReset(audio_queue_);
        if (xQueueSend(audio_queue_, &command, 0) != pdTRUE) {
            return false;
        }
        if (xTaskCreate(AudioTaskEntry, "recorder_audio", kAudioTaskStackBytes, this, 8,
                        &audio_task_) != pdPASS) {
            audio_task_ = nullptr;
            xQueueReset(audio_queue_);
            return false;
        }
        return true;
    }

    static void InitializeNvs() {
        esp_err_t result = nvs_flash_init();
        if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            result = nvs_flash_init();
        }
        ESP_ERROR_CHECK(result);
    }

    static int LoadVolume() {
        nvs_handle_t handle = 0;
        uint8_t saved_volume = kDefaultVolume;
        if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
            if (nvs_get_u8(handle, kVolumeKey, &saved_volume) != ESP_OK || saved_volume > 100) {
                saved_volume = kDefaultVolume;
            }
            nvs_close(handle);
        }
        return saved_volume;
    }

    static void SaveVolume(int volume) {
        nvs_handle_t handle = 0;
        if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
            ESP_LOGW(kTag, "Unable to open NVS for volume");
            return;
        }
        const esp_err_t result = nvs_set_u8(handle, kVolumeKey, static_cast<uint8_t>(volume));
        const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
        nvs_close(handle);
        if (commit_result != ESP_OK) {
            ESP_LOGW(kTag, "Unable to save volume: %s", esp_err_to_name(commit_result));
        }
    }

    [[noreturn]] static void Halt() {
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    void RefreshRecordings(bool select_latest) {
        recordings_ = store_.List();
        if (recordings_.empty()) {
            selected_ = 0;
        } else if (select_latest) {
            selected_ = recordings_.size() - 1;
        } else {
            selected_ = std::min(selected_, recordings_.size() - 1);
        }
        ui_.ShowReady(recordings_, selected_, store_.FreeBytes(), store_.TotalBytes());
    }

    void HandleMessage(const AppMessage& message) {
        switch (message.event) {
            case AppEvent::kUp:
                if (state_ == AppState::kDeleteConfirm) {
                    CancelDelete();
                } else if (state_ == AppState::kPlaying) {
                    AdjustVolume(10);
                } else {
                    Select(-1);
                }
                break;
            case AppEvent::kDown:
                if (state_ == AppState::kDeleteConfirm) {
                    CancelDelete();
                } else if (state_ == AppState::kPlaying) {
                    AdjustVolume(-10);
                } else {
                    Select(1);
                }
                break;
            case AppEvent::kConfirm:
                if (state_ == AppState::kDeleteConfirm) {
                    ConfirmDelete();
                } else {
                    ToggleRecording();
                }
                break;
            case AppEvent::kConfirmLong:
                if (state_ == AppState::kDeleteConfirm) {
                    CancelDelete();
                } else {
                    TogglePlayback();
                }
                break;
            case AppEvent::kDeleteLong:
                BeginDelete();
                break;
            case AppEvent::kExportLong:
                ToggleExport();
                break;
            case AppEvent::kAudioProgress:
                ShowProgress(message.value);
                break;
            case AppEvent::kAudioFinished:
                FinishAudio(message.success);
                break;
            case AppEvent::kNetworkStatus:
                ui_.SetNetworkStatus(static_cast<NetworkState>(message.value), message.value2);
                break;
        }
    }

    void Select(int direction) {
        if (state_ != AppState::kIdle || recordings_.empty()) {
            return;
        }
        const int count = static_cast<int>(recordings_.size());
        selected_ = static_cast<size_t>((static_cast<int>(selected_) + count + direction) % count);
        ui_.ShowReady(recordings_, selected_, store_.FreeBytes(), store_.TotalBytes());
    }

    void ToggleRecording() {
        if (state_ == AppState::kRecording) {
            const AudioCommand command = {.type = AudioCommandType::kStop};
            xQueueSend(audio_queue_, &command, 0);
            return;
        }
        if (state_ != AppState::kIdle) {
            return;
        }
        if (store_.FreeBytes() < kMinimumFreeBytes) {
            ui_.ShowMessage("STORAGE FULL", "Delete recordings in Wi-Fi export mode");
            return;
        }
        const AudioCommand command = {.type = AudioCommandType::kStartRecording};
        uploader_.SetPaused(true);
        if (!StartAudioTask(command)) {
            uploader_.SetPaused(false);
            ui_.ShowMessage("AUDIO ERROR", "Unable to start recording task");
            return;
        }
        state_ = AppState::kRecording;
        ui_.ShowRecording(0, 0, store_.FreeBytes(), store_.TotalBytes());
    }

    void TogglePlayback() {
        if (state_ == AppState::kPlaying) {
            const AudioCommand command = {.type = AudioCommandType::kStop};
            xQueueSend(audio_queue_, &command, 0);
            return;
        }
        if (state_ != AppState::kIdle || recordings_.empty()) {
            return;
        }
        const AudioCommand command = {
            .type = AudioCommandType::kPlay, .sequence = recordings_[selected_].sequence};
        uploader_.SetPaused(true);
        if (!StartAudioTask(command)) {
            uploader_.SetPaused(false);
            ui_.ShowMessage("AUDIO ERROR", "Unable to start playback task");
            return;
        }
        state_ = AppState::kPlaying;
        playback_seconds_ = 0;
        ui_.ShowPlaying(recordings_[selected_], playback_seconds_, volume_);
    }

    void AdjustVolume(int delta) {
        if (state_ != AppState::kPlaying || recordings_.empty()) {
            return;
        }
        const int new_volume = std::clamp(volume_ + delta, 0, 100);
        if (new_volume == volume_) {
            return;
        }
        volume_ = new_volume;
        SaveVolume(volume_);
        ui_.ShowPlaying(recordings_[selected_], playback_seconds_, volume_);
        const AudioCommand command = {.type = AudioCommandType::kSetVolume, .volume = volume_};
        if (xQueueSend(audio_queue_, &command, 0) != pdTRUE) {
            ESP_LOGW(kTag, "Volume command queue full");
        }
    }

    void BeginDelete() {
        if (state_ != AppState::kIdle || recordings_.empty()) {
            return;
        }
        state_ = AppState::kDeleteConfirm;
        ui_.ShowDeleteConfirm(recordings_[selected_]);
    }

    void CancelDelete() {
        if (state_ != AppState::kDeleteConfirm) {
            return;
        }
        state_ = AppState::kIdle;
        RefreshRecordings(false);
    }

    void ConfirmDelete() {
        if (state_ != AppState::kDeleteConfirm || recordings_.empty()) {
            return;
        }
        const uint32_t sequence = recordings_[selected_].sequence;
        uploader_.SetPaused(true);
        const bool deleted = store_.Delete(sequence);
        uploader_.SetPaused(false);
        uploader_.Wake();
        state_ = AppState::kIdle;
        if (!deleted) {
            ESP_LOGE(kTag, "Failed to delete R%07lu", static_cast<unsigned long>(sequence));
            ui_.ShowMessage("DELETE ERROR", "Recording could not be removed");
            vTaskDelay(pdMS_TO_TICKS(800));
        } else {
            ESP_LOGI(kTag, "Deleted R%07lu", static_cast<unsigned long>(sequence));
        }
        RefreshRecordings(false);
    }

    void ToggleExport() {
        if (state_ == AppState::kExport) {
            web_export_.Stop();
            uploader_.SetPaused(false);
            uploader_.Wake();
            state_ = AppState::kIdle;
            RefreshRecordings(false);
            return;
        }
        if (state_ != AppState::kIdle) {
            return;
        }
        uploader_.SetPaused(true);
        if (!web_export_.Start()) {
            uploader_.SetPaused(false);
            uploader_.Wake();
            ui_.ShowMessage("WI-FI ERROR", "Unable to start export hotspot");
            return;
        }
        state_ = AppState::kExport;
        ui_.ShowExport(web_export_.ssid(), web_export_.password());
    }

    void ShowProgress(uint32_t seconds) {
        if (state_ == AppState::kRecording) {
            ui_.ShowRecording(seconds, store_.active_data_bytes(), store_.FreeBytes(),
                              store_.TotalBytes());
        } else if (state_ == AppState::kPlaying && !recordings_.empty()) {
            playback_seconds_ = seconds;
            ui_.ShowPlaying(recordings_[selected_], playback_seconds_, volume_);
        }
    }

    void FinishAudio(bool success) {
        const bool was_recording = state_ == AppState::kRecording;
        state_ = AppState::kIdle;
        uploader_.SetPaused(false);
        uploader_.Wake();
        if (!success) {
            ui_.ShowMessage("AUDIO ERROR", "Operation stopped unexpectedly");
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        RefreshRecordings(was_recording && success);
    }

    void PostAudioEvent(AppEvent event, uint32_t value = 0, bool success = true) {
        const AppMessage message = {
            .event = event, .value = value, .value2 = 0, .success = success};
        xQueueSend(event_queue_, &message, portMAX_DELAY);
    }

    void PollBattery() {
        const TickType_t now = xTaskGetTickCount();
        if (now - last_battery_poll_ < kBatteryPollInterval) {
            return;
        }
        last_battery_poll_ = now;
        const int level = board_.BatteryLevel();
        if (level != battery_level_) {
            battery_level_ = level;
            ui_.SetBatteryLevel(level);
        }
    }

    void AudioTask() {
        AudioCommand command;
        bool success = false;
        if (xQueueReceive(audio_queue_, &command, portMAX_DELAY) == pdTRUE) {
            if (command.type == AudioCommandType::kStartRecording) {
                success = RunRecording();
            } else if (command.type == AudioCommandType::kPlay) {
                success = RunPlayback(command.sequence);
            }
        }
        audio_task_ = nullptr;
        PostAudioEvent(AppEvent::kAudioFinished, 0, success);
        vTaskDelete(nullptr);
    }

    bool RunRecording() {
        esp_opus_enc_config_t encoder_config = ESP_OPUS_ENC_CONFIG_DEFAULT();
        encoder_config.sample_rate = kRecordingSampleRate;
        encoder_config.channel = 1;
        encoder_config.bits_per_sample = 16;
        encoder_config.bitrate = kRecordingBitrate;
        encoder_config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
        encoder_config.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
        encoder_config.complexity = 2;
        encoder_config.enable_fec = false;
        encoder_config.enable_dtx = false;
        encoder_config.enable_vbr = true;
        void* encoder = nullptr;
        if (esp_opus_enc_open(&encoder_config, sizeof(encoder_config), &encoder) !=
            ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "Failed to open Opus encoder");
            return false;
        }
        int input_bytes = 0;
        int output_bytes = 0;
        if (esp_opus_enc_get_frame_size(encoder, &input_bytes, &output_bytes) != ESP_AUDIO_ERR_OK ||
            input_bytes != static_cast<int>(kPcmFrameSamples * sizeof(int16_t)) ||
            output_bytes <= 0) {
            ESP_LOGE(kTag, "Unexpected Opus frame sizes: input=%d output=%d", input_bytes,
                     output_bytes);
            esp_opus_enc_close(encoder);
            return false;
        }
        if (!store_.Begin()) {
            esp_opus_enc_close(encoder);
            return false;
        }
        std::array<int16_t, kPcmFrameSamples> pcm = {};
        std::vector<uint8_t> encoded(static_cast<size_t>(output_bytes));
        ESP_LOGI(kTag, "Opus encoder ready: %d -> %d bytes, free heap=%lu", input_bytes,
                 output_bytes, static_cast<unsigned long>(esp_get_free_heap_size()));
        bool success = true;
        bool stopping = false;
        uint32_t last_second = 0;
        while (!stopping) {
            AudioCommand command;
            if (xQueueReceive(audio_queue_, &command, 0) == pdTRUE &&
                command.type == AudioCommandType::kStop) {
                break;
            }
            if (!audio_.Read(pcm.data(), pcm.size())) {
                success = false;
                break;
            }
            esp_audio_enc_in_frame_t input = {};
            input.buffer = reinterpret_cast<uint8_t*>(pcm.data());
            input.len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
            esp_audio_enc_out_frame_t output = {};
            output.buffer = encoded.data();
            output.len = static_cast<uint32_t>(encoded.size());
            if (esp_opus_enc_process(encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
                output.encoded_bytes == 0 || output.encoded_bytes > kMaxOpusPacketBytes ||
                !store_.WriteOpusPacket(encoded.data(), output.encoded_bytes)) {
                success = false;
                break;
            }
            const uint32_t seconds = store_.active_samples() / kRecordingSampleRate;
            if (seconds != last_second) {
                last_second = seconds;
                PostAudioEvent(AppEvent::kAudioProgress, seconds);
                if (seconds % 5 == 0) {
                    store_.Sync();
                }
                if (store_.FreeBytes() < kMinimumFreeBytes) {
                    stopping = true;
                }
            }
        }
        esp_opus_enc_close(encoder);
        success = store_.Finish() && success;
        ESP_LOGI(kTag, "Audio task stack remaining: %lu bytes",
                 static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
        return success;
    }

    bool RunPlayback(uint32_t sequence) {
        RecordingHeader header;
        FILE* file = store_.Open(sequence, header);
        if (file == nullptr) {
            return false;
        }
        esp_opus_dec_cfg_t decoder_config = ESP_OPUS_DEC_CONFIG_DEFAULT();
        decoder_config.sample_rate = kRecordingSampleRate;
        decoder_config.channel = 1;
        decoder_config.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        decoder_config.self_delimited = false;
        void* decoder = nullptr;
        if (esp_opus_dec_open(&decoder_config, sizeof(decoder_config), &decoder) !=
            ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "Failed to open Opus decoder");
            std::fclose(file);
            return false;
        }
        std::array<uint8_t, kMaxOpusPacketBytes> encoded = {};
        std::array<int16_t, kPcmFrameSamples> pcm = {};
        uint32_t bytes_remaining = header.data_bytes;
        uint32_t samples_played = 0;
        uint32_t last_second = 0;
        bool success = true;
        while (bytes_remaining > 0) {
            AudioCommand command;
            if (xQueueReceive(audio_queue_, &command, 0) == pdTRUE) {
                if (command.type == AudioCommandType::kStop) {
                    break;
                }
                if (command.type == AudioCommandType::kSetVolume) {
                    audio_.SetVolume(command.volume);
                    ESP_LOGI(kTag, "Playback volume applied: %d%%", command.volume);
                }
            }
            size_t packet_size = 0;
            if (!ReadOpusPacket(file, bytes_remaining, encoded.data(), encoded.size(), packet_size)) {
                success = false;
                break;
            }
            esp_audio_dec_in_raw_t input = {};
            input.buffer = encoded.data();
            input.len = static_cast<uint32_t>(packet_size);
            esp_audio_dec_out_frame_t output = {};
            output.buffer = reinterpret_cast<uint8_t*>(pcm.data());
            output.len = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
            esp_audio_dec_info_t info = {};
            if (esp_opus_dec_decode(decoder, &input, &output, &info) != ESP_AUDIO_ERR_OK ||
                output.decoded_size == 0 || output.decoded_size > output.len ||
                !audio_.Write(pcm.data(), output.decoded_size / sizeof(int16_t))) {
                success = false;
                break;
            }
            samples_played += output.decoded_size / sizeof(int16_t);
            const uint32_t seconds = samples_played / kRecordingSampleRate;
            if (seconds != last_second) {
                last_second = seconds;
                PostAudioEvent(AppEvent::kAudioProgress, seconds);
            }
        }
        esp_opus_dec_close(decoder);
        std::fclose(file);
        ESP_LOGI(kTag, "Audio task stack remaining: %lu bytes",
                 static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
        return success;
    }

    PassportBoard board_;
    PassportAudio audio_;
    RecordingStore store_;
    NetworkConfig network_config_;
    NetworkUploader uploader_{store_, network_config_};
    RecorderUi ui_;
    WebExport web_export_{store_, network_config_};
    QueueHandle_t event_queue_ = nullptr;
    QueueHandle_t audio_queue_ = nullptr;
    TaskHandle_t audio_task_ = nullptr;
    SemaphoreHandle_t wifi_lock_ = nullptr;
    AppState state_ = AppState::kIdle;
    std::vector<RecordingInfo> recordings_;
    size_t selected_ = 0;
    int battery_level_ = -1;
    TickType_t last_battery_poll_ = 0;
    uint32_t playback_seconds_ = 0;
    int volume_ = kDefaultVolume;
};

}  // namespace

extern "C" void app_main() {
    static RecorderApp app;
    app.Run();
}
