#include "protocol/Message_codec.h"
#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

std::vector<char> MessageCodec::encode(const std::string& msg) {
    if (msg.size() > kMaxBodyLenght)
        throw std::length_error("protocol frame too large");
    uint32_t len = static_cast<uint32_t>(msg.size());
    uint32_t net_len = htonl(len);

    std::vector<char> packet(kHeaderLength + len);
    std::memcpy(packet.data(), &net_len, kHeaderLength);

    if (len > 0) {
        std::memcpy(packet.data() + kHeaderLength, msg.data(), len);
    }
    return packet;
}

MessageCodec::DecodeResult MessageCodec::Decoder::tryDecode(Buffer& buf, std::string& out_msg) {
    if (buf.readableBytes() < 4)
        return MessageCodec::DecodeResult::NeedMoreData;

    uint32_t net_len;
    std::memcpy(&net_len, buf.peek(), kHeaderLength);
    uint32_t body_len = ntohl(net_len);

    if (body_len > kMaxBodyLenght) {
        return MessageCodec::DecodeResult::Invalid;
    }

    if (buf.readableBytes() < kHeaderLength + body_len)
        return MessageCodec::DecodeResult::NeedMoreData;

    buf.retrieve(kHeaderLength);

    if (body_len == 0) {
        out_msg.clear();
        return MessageCodec::DecodeResult::Ok;
    }
    out_msg = buf.retrieveAsString(body_len);
    return MessageCodec::DecodeResult::Ok;
}
