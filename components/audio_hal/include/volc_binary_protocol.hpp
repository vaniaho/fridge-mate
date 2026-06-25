#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smart_fridge {
namespace audio {
namespace volc {

enum class message_type_t : uint8_t {
    FULL_CLIENT = 0x1,
    AUDIO_CLIENT = 0x2,
    FULL_SERVER = 0x9,
    AUDIO_SERVER = 0xB,
    ERROR = 0xF,
};

struct packet_t {
    message_type_t type = message_type_t::FULL_SERVER;
    uint8_t flags = 0;
    uint8_t serialization = 0;
    uint8_t compression = 0;
    int32_t sequence = 0;
    uint32_t event = 0;
    uint32_t error_code = 0;
    std::string connection_id;
    std::string session_id;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode_event(
    message_type_t type, uint32_t event, const std::string &session_id,
    const uint8_t *payload, size_t payload_size, bool json_payload);

inline std::vector<uint8_t> encode_json_event(
    uint32_t event, const std::string &session_id, const std::string &json)
{
    return encode_event(message_type_t::FULL_CLIENT, event, session_id,
                        reinterpret_cast<const uint8_t *>(json.data()),
                        json.size(), true);
}

inline std::vector<uint8_t> encode_audio_event(
    uint32_t event, const std::string &session_id, const uint8_t *audio,
    size_t audio_size)
{
    return encode_event(message_type_t::AUDIO_CLIENT, event, session_id,
                        audio, audio_size, false);
}

bool decode_packet(const uint8_t *data, size_t size, packet_t &packet,
                   std::string &error);

bool is_connection_event(uint32_t event);

}  // namespace volc
}  // namespace audio
}  // namespace smart_fridge
