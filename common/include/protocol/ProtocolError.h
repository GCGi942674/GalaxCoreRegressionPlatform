#ifndef GCRP_PROTOCOL_ERROR_H
#define GCRP_PROTOCOL_ERROR_H

#include <string>

namespace GCRP::Protocol {

enum class ProtocolErrorCode {
    NONE,
    INVALID_JSON,
    MISSING_FIELD,
    INVALID_FIELD_TYPE,
    INVALID_FIELD_VALUE,
    UNSUPPORTED_VERSION,
    UNKNOWN_MESSAGE_TYPE
};

struct ProtocolError {
    ProtocolErrorCode code{ProtocolErrorCode::NONE};
    std::string message;

    bool hasError() const {
        return code != ProtocolErrorCode::NONE;
    }
};

} // namespace GCRP::Protocol

#endif // GCRP_PROTOCOL_ERROR_H
