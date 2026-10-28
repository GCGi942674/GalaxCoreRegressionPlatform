#ifndef GCRP_PROTOCOL_CODEC_H
#define GCRP_PROTOCOL_CODEC_H

#include "ProtocolMessage.h"
#include "ProtocolError.h"
#include "MessageType.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include <string>

namespace GCRP::Protocol {

class ProtocolCodec {
public:
    static bool decode(
        const std::string& json_text, ProtocolMessage& message, ProtocolError& error);

    static bool encode(
        const ProtocolMessage& message, std::string& json_text, ProtocolError& error);

private:
    static bool validateEnvelope(
        const nlohmann::json& j, ProtocolMessage& message, ProtocolError& error);

    static std::string messageTypeToString(MessageType type);
};

} // namespace GCRP::Protocol

#endif // GCRP_PROTOCOL_CODEC_H