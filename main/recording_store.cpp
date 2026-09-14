#include "recording_store.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <nvs.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr char kTag[] = "RecordingStore";
constexpr char kMountPath[] = "/rec";
constexpr char kPartitionLabel[] = "recordings";
constexpr char kTemporaryPath[] = "/rec/ACTIVE.TMP";
constexpr char kNvsNamespace[] = "recorder";
constexpr char kNextSequenceKey[] = "next_seq";

uint32_t ReserveSequence(uint32_t disk_next) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return disk_next;
    }
    uint32_t saved_next = 1;
    if (nvs_get_u32(handle, kNextSequenceKey, &saved_next) != ESP_OK) {
        saved_next = 1;
    }
    const uint32_t selected = std::max(disk_next, saved_next);
    const esp_err_t set_result = nvs_set_u32(handle, kNextSequenceKey, selected + 1);
    const esp_err_t commit_result = set_result == ESP_OK ? nvs_commit(handle) : set_result;
    nvs_close(handle);
    if (commit_result != ESP_OK) {
        ESP_LOGW(kTag, "Unable to persist recording sequence: %s",
                 esp_err_to_name(commit_result));
    }
    return selected;
}

bool ParseRecordingName(const char* name, uint32_t& sequence) {
    unsigned parsed = 0;
    char tail = 0;
    if (std::sscanf(name, "R%7u.FRC%c", &parsed, &tail) != 1 || parsed == 0) {
        return false;
    }
    sequence = parsed;
    return true;
}

bool IsValidHeader(const RecordingHeader& header) {
    return header.magic == kRecordingMagic && header.version == kRecordingVersion &&
           header.header_size == sizeof(RecordingHeader) &&
           header.sample_rate == kRecordingSampleRate &&
           header.codec == RecordingCodec::kOpus;
}

}  // namespace

RecordingStore::~RecordingStore() {
    Cancel();
    if (wl_handle_ != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(kMountPath, wl_handle_);
    }
}

bool RecordingStore::Initialize() {
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    const esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(
        kMountPath, kPartitionLabel, &mount_config, &wl_handle_);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount recording partition: %s", esp_err_to_name(result));
        return false;
    }
    RecoverTemporary();
    ESP_LOGI(kTag, "Recording storage mounted, free=%llu bytes",
             static_cast<unsigned long long>(FreeBytes()));
    return true;
}

std::string RecordingStore::PathFor(uint32_t sequence) {
    char path[32];
    std::snprintf(path, sizeof(path), "/rec/R%07lu.FRC",
                  static_cast<unsigned long>(sequence));
    return path;
}

bool RecordingStore::LoadHeader(const std::string& path, RecordingHeader& header) const {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    const bool valid = std::fread(&header, 1, sizeof(header), file) == sizeof(header) &&
                       IsValidHeader(header);
    std::fclose(file);
    return valid;
}

std::vector<RecordingInfo> RecordingStore::List() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecordingInfo> recordings;
    DIR* directory = opendir(kMountPath);
    if (directory == nullptr) {
        return recordings;
    }
    while (dirent* entry = readdir(directory)) {
        uint32_t sequence = 0;
        if (!ParseRecordingName(entry->d_name, sequence)) {
            continue;
        }
        const std::string path = PathFor(sequence);
        RecordingHeader header;
        if (LoadHeader(path, header)) {
            recordings.push_back({sequence, header.sample_count, header.data_bytes, path});
        }
    }
    closedir(directory);
    std::sort(recordings.begin(), recordings.end(), [](const auto& left, const auto& right) {
        return left.sequence < right.sequence;
    });
    return recordings;
}

uint32_t RecordingStore::NextSequence() const {
    uint32_t next = 1;
    for (const auto& recording : List()) {
        next = std::max(next, recording.sequence + 1);
    }
    return next;
}

bool RecordingStore::RecoverTemporary() {
    struct stat status = {};
    if (stat(kTemporaryPath, &status) != 0) {
        return true;
    }
    if (status.st_size <= static_cast<off_t>(sizeof(RecordingHeader))) {
        remove(kTemporaryPath);
        return true;
    }

    const uint32_t sequence = ReserveSequence(NextSequence());
    FILE* file = std::fopen(kTemporaryPath, "r+b");
    if (file == nullptr) {
        return false;
    }
    RecordingHeader header;
    if (std::fread(&header, 1, sizeof(header), file) != sizeof(header) || !IsValidHeader(header)) {
        std::fclose(file);
        remove(kTemporaryPath);
        return false;
    }
    uint32_t remaining = static_cast<uint32_t>(status.st_size - sizeof(RecordingHeader));
    uint32_t valid_data_bytes = 0;
    uint32_t sample_count = 0;
    std::array<uint8_t, kMaxOpusPacketBytes> packet = {};
    while (remaining > 0) {
        const uint32_t before = remaining;
        size_t packet_size = 0;
        if (!ReadOpusPacket(file, remaining, packet.data(), packet.size(), packet_size)) {
            break;
        }
        valid_data_bytes += before - remaining;
        sample_count += kOpusFrameSamples;
    }
    if (sample_count == 0 || ftruncate(fileno(file), sizeof(RecordingHeader) + valid_data_bytes) != 0) {
        std::fclose(file);
        remove(kTemporaryPath);
        return false;
    }
    header.sequence = sequence;
    header.data_bytes = valid_data_bytes;
    header.sample_count = sample_count;
    std::rewind(file);
    const bool written = std::fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    std::fflush(file);
    fsync(fileno(file));
    std::fclose(file);
    if (!written || rename(kTemporaryPath, PathFor(sequence).c_str()) != 0) {
        return false;
    }
    ESP_LOGW(kTag, "Recovered interrupted recording R%07lu", static_cast<unsigned long>(sequence));
    return true;
}

bool RecordingStore::Begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_file_ != nullptr) {
        return false;
    }
    active_sequence_ = 1;
    DIR* directory = opendir(kMountPath);
    if (directory != nullptr) {
        while (dirent* entry = readdir(directory)) {
            uint32_t sequence = 0;
            if (ParseRecordingName(entry->d_name, sequence)) {
                active_sequence_ = std::max(active_sequence_, sequence + 1);
            }
        }
        closedir(directory);
    }
    active_sequence_ = ReserveSequence(active_sequence_);
    remove(kTemporaryPath);
    active_file_ = std::fopen(kTemporaryPath, "w+b");
    active_data_bytes_ = 0;
    active_sample_count_ = 0;
    if (active_file_ == nullptr) {
        ESP_LOGE(kTag, "Cannot create recording: %s", std::strerror(errno));
        return false;
    }
    setvbuf(active_file_, nullptr, _IOFBF, 4096);
    RecordingHeader header;
    header.sequence = active_sequence_;
    if (std::fwrite(&header, 1, sizeof(header), active_file_) != sizeof(header)) {
        Cancel();
        return false;
    }
    return true;
}

bool RecordingStore::WriteOpusPacket(const uint8_t* data, size_t bytes) {
    if (active_file_ == nullptr || data == nullptr || bytes == 0 ||
        bytes > kMaxOpusPacketBytes || bytes > UINT16_MAX) {
        return false;
    }
    const uint16_t packet_size = static_cast<uint16_t>(bytes);
    if (std::fwrite(&packet_size, 1, sizeof(packet_size), active_file_) != sizeof(packet_size) ||
        std::fwrite(data, 1, bytes, active_file_) != bytes) {
        ESP_LOGE(kTag, "Recording write failed: %s", std::strerror(errno));
        return false;
    }
    active_data_bytes_ += sizeof(packet_size) + static_cast<uint32_t>(bytes);
    active_sample_count_ += kOpusFrameSamples;
    return true;
}

bool RecordingStore::Sync() {
    if (active_file_ == nullptr) {
        return false;
    }
    return std::fflush(active_file_) == 0 && fsync(fileno(active_file_)) == 0;
}

bool RecordingStore::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_file_ == nullptr) {
        return false;
    }
    RecordingHeader header;
    header.sequence = active_sequence_;
    header.data_bytes = active_data_bytes_;
    header.sample_count = active_sample_count_;
    const bool seek_ok = std::fseek(active_file_, 0, SEEK_SET) == 0;
    const bool header_ok = seek_ok &&
                           std::fwrite(&header, 1, sizeof(header), active_file_) == sizeof(header);
    std::fflush(active_file_);
    fsync(fileno(active_file_));
    std::fclose(active_file_);
    active_file_ = nullptr;
    if (!header_ok || rename(kTemporaryPath, PathFor(active_sequence_).c_str()) != 0) {
        ESP_LOGE(kTag, "Failed to finalize recording");
        return false;
    }
    ESP_LOGI(kTag, "Saved R%07lu, %lu samples", static_cast<unsigned long>(active_sequence_),
             static_cast<unsigned long>(header.sample_count));
    return true;
}

void RecordingStore::Cancel() {
    if (active_file_ != nullptr) {
        std::fclose(active_file_);
        active_file_ = nullptr;
    }
}

bool RecordingStore::Get(uint32_t sequence, RecordingInfo& info) const {
    const std::string path = PathFor(sequence);
    RecordingHeader header;
    if (!LoadHeader(path, header) || header.sequence != sequence) {
        return false;
    }
    info = {sequence, header.sample_count, header.data_bytes, path};
    return true;
}

FILE* RecordingStore::Open(uint32_t sequence, RecordingHeader& header) const {
    const std::string path = PathFor(sequence);
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr || std::fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        !IsValidHeader(header) || header.sequence != sequence) {
        if (file != nullptr) {
            std::fclose(file);
        }
        return nullptr;
    }
    return file;
}

bool RecordingStore::Delete(uint32_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordingHeader header;
    const std::string path = PathFor(sequence);
    return LoadHeader(path, header) && header.sequence == sequence && remove(path.c_str()) == 0;
}

uint64_t RecordingStore::FreeBytes() const {
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kMountPath, &total_bytes, &free_bytes) != ESP_OK) {
        return 0;
    }
    return free_bytes;
}

uint64_t RecordingStore::TotalBytes() const {
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kMountPath, &total_bytes, &free_bytes) != ESP_OK) {
        return 0;
    }
    return total_bytes;
}

bool ReadOpusPacket(FILE* file, uint32_t& remaining_bytes, uint8_t* packet,
                    size_t packet_capacity, size_t& packet_size) {
    packet_size = 0;
    if (file == nullptr || packet == nullptr || remaining_bytes < sizeof(uint16_t)) {
        return false;
    }
    uint16_t stored_size = 0;
    if (std::fread(&stored_size, 1, sizeof(stored_size), file) != sizeof(stored_size)) {
        return false;
    }
    remaining_bytes -= sizeof(stored_size);
    if (stored_size == 0 || stored_size > packet_capacity || stored_size > remaining_bytes) {
        return false;
    }
    if (std::fread(packet, 1, stored_size, file) != stored_size) {
        return false;
    }
    remaining_bytes -= stored_size;
    packet_size = stored_size;
    return true;
}
