#include "volc_binary_protocol.hpp"

#include <algorithm>

namespace smart_fridge {
namespace audio {
namespace volc {

namespace {

void append_be32(std::vector<uint8_t> &output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    output.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    output.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    output.push_back(static_cast<uint8_t>(value & 0xff));
}

uint32_t read_be32(const uint8_t *data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

bool read_u32(const uint8_t *data, size_t size, size_t &offset,
              uint32_t &value)
{
    if (offset + 4 > size) {
        return false;
    }
    value = read_be32(data + offset);
    offset += 4;
    return true;
}

bool parse_optional_id_and_payload(const uint8_t *data, size_t size,
                                   size_t &offset, std::string &id,
                                   std::vector<uint8_t> &payload)
{
    uint32_t first = 0;
    if (!read_u32(data, size, offset, first)) {
        return false;
    }

    // Events without an ID store payload size directly. Events carrying an ID
    // store id size, id, then payload size. Distinguish them by checking
    // whether the first value exactly consumes the remainder.
    if (offset + first == size) {
        payload.assign(data + offset, data + size);
        return true;
    }

    if (offset + first + 4 > size) {
        return false;
    }
    id.assign(reinterpret_cast<const char *>(data + offset), first);
    offset += first;

    uint32_t payload_size = 0;
    if (!read_u32(data, size, offset, payload_size) ||
        offset + payload_size > size) {
        return false;
    }
    payload.assign(data + offset, data + offset + payload_size);
    return true;
}

}  // namespace

bool is_connection_event(uint32_t event)
{
    return event == 1 || event == 2 || event == 50 || event == 51 ||
           event == 52;
}

std::vector<uint8_t> encode_event(
    message_type_t type, uint32_t event, const std::string &session_id,
    const uint8_t *payload, size_t payload_size, bool json_payload)
{
    std::vector<uint8_t> output;
    output.reserve(16 + session_id.size() + payload_size);
    output.push_back(0x11);
    output.push_back(
        static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | 0x04));
    output.push_back(static_cast<uint8_t>(json_payload ? 0x10 : 0x00));
    output.push_back(0x00);
    append_be32(output, event);

    if (!is_connection_event(event)) {
        append_be32(output, static_cast<uint32_t>(session_id.size()));
        output.insert(output.end(), session_id.begin(), session_id.end());
    }

    append_be32(output, static_cast<uint32_t>(payload_size));
    if (payload && payload_size > 0) {
        output.insert(output.end(), payload, payload + payload_size);
    }
    return output;
}

bool decode_packet(const uint8_t *data, size_t size, packet_t &packet,
                   std::string &error)
{
    packet = {};
    error.clear();
    if (!data || size < 8) {
        error = "协议帧过短";
        return false;
    }

    const uint8_t version = data[0] >> 4;
    const size_t header_size = (data[0] & 0x0f) * 4;
    if (version != 1 || header_size < 4 || header_size > size) {
        error = "协议版本或头长度无效";
        return false;
    }

    packet.type = static_cast<message_type_t>(data[1] >> 4);
    packet.flags = data[1] & 0x0f;
    packet.serialization = data[2] >> 4;
    packet.compression = data[2] & 0x0f;
    size_t offset = header_size;

    if (packet.type == message_type_t::ERROR) {
        if (!read_u32(data, size, offset, packet.error_code)) {
            error = "错误帧缺少错误码";
            return false;
        }
        uint32_t payload_size = 0;
        if (!read_u32(data, size, offset, payload_size) ||
            offset + payload_size > size) {
            error = "错误帧长度无效";
            return false;
        }
        packet.payload.assign(data + offset, data + offset + payload_size);
        return true;
    }

    if ((packet.flags & 0x01) != 0) {
        uint32_t sequence = 0;
        if (!read_u32(data, size, offset, sequence)) {
            error = "协议帧缺少序号";
            return false;
        }
        packet.sequence = static_cast<int32_t>(sequence);
    }

    if ((packet.flags & 0x04) != 0) {
        if (!read_u32(data, size, offset, packet.event)) {
            error = "协议帧缺少事件编号";
            return false;
        }
    }

    std::string id;
    if (!parse_optional_id_and_payload(data, size, offset, id,
                                       packet.payload)) {
        error = "协议帧 ID 或负载长度无效";
        return false;
    }
    if (is_connection_event(packet.event)) {
        packet.connection_id = id;
    } else {
        packet.session_id = id;
    }
    return true;
}

}  // namespace volc
}  // namespace audio
}  // namespace smart_fridge
