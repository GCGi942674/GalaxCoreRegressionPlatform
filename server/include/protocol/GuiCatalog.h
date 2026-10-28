#pragma once
#include "nlohmann/json.hpp"
#include <chrono>
#include <mutex>
#include <map>
#include <string>

namespace GCRP {
// Server-owned, bounded snapshot. Clients never supply filesystem paths.
class GuiCatalog {
public:
    nlohmann::json query(const nlohmann::json& request);

private:
    std::mutex mutex_;
    nlohmann::json snapshot_;
    std::map<uint64_t, nlohmann::json> recent_;
    uint64_t generation_{0};
    std::chrono::steady_clock::time_point created_{};
    nlohmann::json scan() const;
};
} // namespace GCRP
