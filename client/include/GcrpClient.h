#pragma once
#include "protocol/ProtocolMessage.h"
#include <string>
#include <cstdint>

namespace GCRP {
// Synchronous one-request-per-connection API. No automatic retries for mutations.
// Numeric IPv4 only; timeout covers connect, complete send and complete response.
class GcrpClient {
public:
    GcrpClient(std::string host, uint16_t port, int timeout_ms = 5000);
    Protocol::ProtocolMessage request(Protocol::MessageType, const nlohmann::json&);
    Protocol::ProtocolMessage submit(
        const std::string& target, const std::string& revision, int priority = 0);
    Protocol::ProtocolMessage query(uint64_t task_id);
    Protocol::ProtocolMessage cancel(uint64_t task_id);
    Protocol::ProtocolMessage queryBatch(const std::string& batch_id);
    Protocol::ProtocolMessage cancelBatch(const std::string& batch_id);
    Protocol::ProtocolMessage queryRegression(const nlohmann::json& filter);
    Protocol::ProtocolMessage queryProgress(const nlohmann::json& filter);
    Protocol::ProtocolMessage queryRollback(const nlohmann::json& filter);

private:
    std::string host_;
    uint16_t port_;
    int timeout_ms_;
    uint64_t next_request_id_{1};
};
} // namespace GCRP
