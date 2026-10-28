#ifndef GCRP_WORKER_DTO_H
#define GCRP_WORKER_DTO_H

#include "../ProtocolError.h"
#include "nlohmann/json.hpp"
#include <string>
#include <cstdint>

namespace GCRP::Protocol::DTO {

struct WorkerRegisterRequest {
    std::string worker_id;
    std::string hostname;
    std::string machine_id;
    std::string slot_id;
    bool explicit_identity{false};
    bool control_v1{false};
    bool timeout_v1{false};
    uint32_t slots{0};
};

struct WorkerHeartbeatRequest {
    std::string worker_id;
};

// Conversion functions
bool fromJson(const nlohmann::json& j, WorkerRegisterRequest& out, ProtocolError& error);
nlohmann::json toJson(const WorkerRegisterRequest& value);

bool fromJson(const nlohmann::json& j, WorkerHeartbeatRequest& out);
nlohmann::json toJson(const WorkerHeartbeatRequest& value);

} // namespace GCRP::Protocol::DTO

#endif // GCRP_WORKER_DTO_H
