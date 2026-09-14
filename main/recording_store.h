#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <wear_levelling.h>

constexpr uint32_t kRecordingMagic = 0x43455246;  // "FREC"
constexpr uint16_t kRecordingVersion = 2;
constexpr uint32_t kRecordingSampleRate = 16000;
constexpr uint32_t kRecordingBitrate = 24000;
constexpr uint16_t kOpusFrameSamples = 320;  // 20 ms at 16 kHz.
constexpr size_t kMaxOpusPacketBytes = 1536;

enum class RecordingCodec : uint32_t {
    kOpus = 2,
};

#pragma pack(push, 1)
struct RecordingHeader {
    uint32_t magic = kRecordingMagic;
    uint16_t version = kRecordingVersion;
    uint16_t header_size = sizeof(RecordingHeader);
    uint32_t sample_rate = kRecordingSampleRate;
    uint32_t sample_count = 0;
    uint32_t sequence = 0;
    uint32_t data_bytes = 0;
    RecordingCodec codec = RecordingCodec::kOpus;
    uint32_t bitrate = kRecordingBitrate;
};
#pragma pack(pop)

static_assert(sizeof(RecordingHeader) == 32);

struct RecordingInfo {
    uint32_t sequence = 0;
    uint32_t sample_count = 0;
    uint32_t data_bytes = 0;
    std::string path;
};

class RecordingStore {
public:
    ~RecordingStore();

    bool Initialize();
    bool Begin();
    bool WriteOpusPacket(const uint8_t* data, size_t bytes);
    bool Sync();
    bool Finish();
    void Cancel();

    std::vector<RecordingInfo> List() const;
    bool Get(uint32_t sequence, RecordingInfo& info) const;
    FILE* Open(uint32_t sequence, RecordingHeader& header) const;
    bool Delete(uint32_t sequence);
    uint64_t FreeBytes() const;
    uint64_t TotalBytes() const;
    uint32_t active_samples() const { return active_sample_count_; }
    uint32_t active_data_bytes() const { return active_data_bytes_; }

private:
    static std::string PathFor(uint32_t sequence);
    bool LoadHeader(const std::string& path, RecordingHeader& header) const;
    bool RecoverTemporary();
    uint32_t NextSequence() const;

    mutable std::mutex mutex_;
    wl_handle_t wl_handle_ = WL_INVALID_HANDLE;
    FILE* active_file_ = nullptr;
    uint32_t active_sequence_ = 0;
    uint32_t active_data_bytes_ = 0;
    uint32_t active_sample_count_ = 0;
};

bool ReadOpusPacket(FILE* file, uint32_t& remaining_bytes, uint8_t* packet,
                    size_t packet_capacity, size_t& packet_size);
