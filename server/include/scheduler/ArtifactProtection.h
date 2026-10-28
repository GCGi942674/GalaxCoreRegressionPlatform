#pragma once
#include "nlohmann/json.hpp"
#include <set>
#include <string>
#include <ctime>

namespace GCRP {
class StateStore;

// Called under Scheduler's mutex. The separate flock is shared with the ZIP cleaner.
class ArtifactProtection {
public:
    ArtifactProtection(StateStore&,
        std::string file,
        std::string root,
        const std::string& clock_state,
        int grace_seconds = 1800);
    void reserve(const std::string& revision);
    void daily(const nlohmann::json& manifest, bool complete);
    void refresh(const std::set<std::string>& active,
        const std::set<std::string>& stopped_batches,
        std::time_t now = std::time(nullptr));

private:
    StateStore& store_;
    std::string file_, root_;
    int grace_;
    nlohmann::json state_;
    void write(nlohmann::json next, std::time_t now);
    void checkArchive(const std::string&) const;
};
} // namespace GCRP
