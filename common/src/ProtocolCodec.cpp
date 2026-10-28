#include "protocol/ProtocolCodec.h"
#include "protocol/MessageType.h"
#include "protocol/ProtocolVersion.h"
#include "task/TaskType.h"
#include "task/TaskStatus.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "Logging.h"

using namespace GCRP;

namespace GCRP::Protocol {

std::string ProtocolCodec::messageTypeToString(MessageType type) {
    return toString(type);
}

bool ProtocolCodec::validateEnvelope(
    const nlohmann::json& j, ProtocolMessage& message, ProtocolError& error) {
    // Validate version
    if (!j.contains("version")) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing required field: version";
        return false;
    }

    if (!j["version"].is_number_unsigned()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_TYPE;
        error.message = "Invalid field type: version must be unsigned integer";
        return false;
    }

    uint64_t version = j["version"].get<uint64_t>();
    if (version != PROTOCOL_VERSION) {
        error.code = ProtocolErrorCode::UNSUPPORTED_VERSION;
        error.message = "Unsupported protocol version: " + std::to_string(version);
        return false;
    }
    message.version = version;

    // Validate type
    if (!j.contains("type")) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing required field: type";
        return false;
    }

    if (!j["type"].is_string()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_TYPE;
        error.message = "Invalid field type: type must be string";
        return false;
    }

    std::string type_str = j["type"].get<std::string>();
    MessageType type = messageTypeFromString(type_str);
    if (type == MessageType::UNKNOWN) {
        error.code = ProtocolErrorCode::UNKNOWN_MESSAGE_TYPE;
        error.message = "Unknown message type: " + type_str;
        return false;
    }
    message.type = type;

    // Validate request_id
    if (!j.contains("request_id")) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing required field: request_id";
        return false;
    }

    if (!j["request_id"].is_number_unsigned()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_TYPE;
        error.message = "Invalid field type: request_id must be unsigned integer";
        return false;
    }
    message.request_id = j["request_id"].get<uint64_t>();

    // Validate data
    if (!j.contains("data")) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing required field: data";
        return false;
    }

    if (!j["data"].is_object()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_TYPE;
        error.message = "Invalid field type: data must be object";
        return false;
    }

    message.data = j["data"];
    return true;
}

bool ProtocolCodec::decode(
    const std::string& json_text, ProtocolMessage& message, ProtocolError& error) {
    error.code = ProtocolErrorCode::NONE;
    error.message.clear();

    try {
        nlohmann::json j = nlohmann::json::parse(json_text);

        if (!validateEnvelope(j, message, error)) {
            if (error.code != ProtocolErrorCode::NONE) {
                LOG_WARN(LogModule::NETWORK, "Protocol decode failed: " + error.message);
            }
            return false;
        }

        return true;
    } catch (const nlohmann::json::exception& e) {
        error.code = ProtocolErrorCode::INVALID_JSON;
        error.message = "JSON parse error: " + std::string(e.what());
        LOG_WARN(LogModule::NETWORK,
            "JSON parse error for request_id " + std::to_string(message.request_id) + ": " +
                error.message);
        return false;
    } catch (const std::exception& e) {
        error.code = ProtocolErrorCode::INVALID_JSON;
        error.message = "Unexpected error: " + std::string(e.what());
        LOG_WARN(LogModule::NETWORK, "Unexpected error during decode: " + error.message);
        return false;
    }
}

bool ProtocolCodec::encode(
    const ProtocolMessage& message, std::string& json_text, ProtocolError& error) {
    error.code = ProtocolErrorCode::NONE;
    error.message.clear();

    try {
        nlohmann::json j;
        j["version"] = message.version;
        j["type"] = messageTypeToString(message.type);
        j["request_id"] = message.request_id;
        j["data"] = message.data;

        json_text = j.dump();
        return true;
    } catch (const nlohmann::json::exception& e) {
        error.code = ProtocolErrorCode::INVALID_JSON;
        error.message = "JSON encode error: " + std::string(e.what());
        return false;
    } catch (const std::exception& e) {
        error.code = ProtocolErrorCode::INVALID_JSON;
        error.message = "Unexpected error: " + std::string(e.what());
        return false;
    }
}

} // namespace GCRP::Protocol
