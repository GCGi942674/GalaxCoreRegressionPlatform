#ifndef MESSAGE_CODEC_H
#define MESSAGE_CODEC_H

#include "Buffer.h"
#include <cstdint>
#include <string>
#include <vector>

class Buffer;

class MessageCodec {
public:
    static constexpr uint32_t kHeaderLength = 4;
    static constexpr uint32_t kMaxBodyLenght = 1024 * 1024; // 1MB

    enum class DecodeResult { Ok, NeedMoreData, Invalid };

    static std::vector<char> encode(const std::string& msg);

    class Decoder {
    public:
        static DecodeResult tryDecode(Buffer& buf, std::string& out_msg);
    };
};

#endif