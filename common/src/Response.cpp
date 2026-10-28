#include "protocol/dto/Response.h"

namespace GCRP::Protocol::DTO {

bool fromJson(const nlohmann::json& j, Response& out) {
    if (j.contains("code") && j["code"].is_number_integer()) {
        out.code = j["code"].get<int>();
    }

    if (j.contains("message") && j["message"].is_string()) {
        out.message = j["message"].get<std::string>();
    }

    if (j.contains("result") && j["result"].is_object()) {
        out.data = j["result"];
    } else if (j.contains("data") && j["data"].is_object()) {
        out.data = j["data"];
    }

    return true;
}

nlohmann::json toJson(const Response& value) {
    nlohmann::json j;
    j["code"] = value.code;
    j["message"] = value.message;
    j["result"] = value.data;
    // Additive, stable categories: callers must not branch on human messages.
    const char* reason = "INTERNAL_ERROR";
    switch (value.code) {
        case 0:
            reason = "OK";
            break;
        case 1:
            reason = "INVALID_REQUEST";
            break;
        case 2:
            reason = "NOT_FOUND";
            break;
        case 3:
            reason = "INVALID_STATE";
            break;
    }
    j["error_reason"] = reason;
    return j;
}

} // namespace GCRP::Protocol::DTO
