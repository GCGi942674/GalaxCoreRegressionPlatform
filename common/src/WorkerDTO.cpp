#include "protocol/dto/WorkerDTO.h"
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include <limits>

namespace GCRP::Protocol::DTO {

// WorkerRegisterRequest
bool fromJson(const nlohmann::json& j, WorkerRegisterRequest& out, ProtocolError& error) {
    if (!j.contains("worker_id") || !j["worker_id"].is_string()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: worker_id";
        return false;
    }
    out.worker_id = j["worker_id"].get<std::string>();
    if (out.worker_id.empty()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
        error.message = "worker_id cannot be empty";
        return false;
    }

    if (!j.contains("hostname") || !j["hostname"].is_string()) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: hostname";
        return false;
    }
    out.hostname = j["hostname"].get<std::string>();
    if (out.hostname.empty()) {
        error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
        error.message = "hostname cannot be empty";
        return false;
    }

    if (j.contains("machine_id") != j.contains("slot_id")) {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "machine_id and slot_id must be supplied together";
        return false;
    }
    out.explicit_identity = j.contains("machine_id");
    for (const auto& field : {"machine_id", "slot_id"}) {
        if (j.contains(field) && (!j[field].is_string() || j[field].get<std::string>().empty() ||
                                     j[field].get<std::string>().size() > 64)) {
            error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
            error.message = std::string("invalid field: ") + field;
            return false;
        }
    }
    out.machine_id = out.explicit_identity ? j["machine_id"].get<std::string>() : "";
    out.slot_id = out.explicit_identity ? j["slot_id"].get<std::string>() : "";

    if (j.contains("slots") && j["slots"].is_number_unsigned()) {
        const auto slots = j["slots"].get<uint64_t>();
        if (slots == 0 || slots > std::numeric_limits<uint32_t>::max()) {
            error.code = ProtocolErrorCode::INVALID_FIELD_VALUE;
            error.message = "slots must be greater than 0";
            return false;
        }
        out.slots = static_cast<uint32_t>(slots);
    } else {
        error.code = ProtocolErrorCode::MISSING_FIELD;
        error.message = "Missing or invalid field: slots";
        return false;
    }

    if (j.contains("timeout_version")) {
        if (!j["timeout_version"].is_number_unsigned() || j["timeout_version"] != 1) {
            error.message = "unsupported timeout observation version";
            return false;
        }
        out.timeout_v1 = true;
    }
    out.control_v1 = false;
    if (j.contains("control_version")) {
        if (!j["control_version"].is_number_unsigned() || j["control_version"] != 1 ||
            out.slots != 1) {
            error.message = "control v1 requires a single-slot Worker";
            return false;
        }
        out.control_v1 = true;
    }
    return true;
}

nlohmann::json toJson(const WorkerRegisterRequest& value) {
    nlohmann::json j;
    j["worker_id"] = value.worker_id;
    j["hostname"] = value.hostname;
    if (value.explicit_identity) {
        j["machine_id"] = value.machine_id;
        j["slot_id"] = value.slot_id;
    }
    j["slots"] = value.slots;
    if (value.timeout_v1)
        j["timeout_version"] = 1;
    if (value.control_v1)
        j["control_version"] = 1;
    return j;
}

// WorkerHeartbeatRequest
bool fromJson(const nlohmann::json& j, WorkerHeartbeatRequest& out) {
    if (!j.contains("worker_id") || !j["worker_id"].is_string()) {
        return false;
    }
    out.worker_id = j["worker_id"].get<std::string>();
    return !out.worker_id.empty();
}

nlohmann::json toJson(const WorkerHeartbeatRequest& value) {
    nlohmann::json j;
    j["worker_id"] = value.worker_id;
    return j;
}

} // namespace GCRP::Protocol::DTO
