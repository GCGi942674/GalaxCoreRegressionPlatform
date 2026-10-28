#pragma once
#include "nlohmann/json.hpp"
#include "Sha256.h"
#include <algorithm>
#include <string>

namespace GCRP {
inline bool timingInteger(const nlohmann::json& j, const char* key, int64_t lo, int64_t hi) {
    const auto p = j.find(key);
    if (p == j.end() || !p->is_number_integer())
        return false;
    if (p->is_number_unsigned() && p->get<uint64_t>() > static_cast<uint64_t>(hi))
        return false;
    const auto n = p->get<int64_t>();
    return n >= lo && n <= hi;
}

inline bool timingDigest(const nlohmann::json& j, const char* key) {
    const auto p = j.find(key);
    if (p == j.end() || !p->is_string())
        return false;
    const auto s = p->get<std::string>();
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

inline nlohmann::json cleanExecutionTiming(const nlohmann::json& value) {
    if (!value.is_object() || value.value("version", nlohmann::json()) != 1 ||
        value.value("scope", nlohmann::json()) != "single_case_runner_wall_v1")
        return nullptr;
    nlohmann::json out = {{"version", 1}, {"scope", "single_case_runner_wall_v1"}};
    for (const char* k :
        {"prepare_wall_ms", "command_wall_ms", "reap_wall_ms", "finalize_wall_ms"}) {
        if (!timingInteger(value, k, 0, 604800000))
            return nullptr;
        out[k] = value[k];
    }
    if (!timingInteger(value, "effective_timeout_seconds", 1, 86400) ||
        !timingDigest(value, "script_fingerprint") || !timingDigest(value, "archive_sha256"))
        return nullptr;
    for (const char* k : {"effective_timeout_seconds", "script_fingerprint", "archive_sha256"})
        out[k] = value[k];
    if (timingInteger(value, "runner_runtime_ms", 0, 604800000))
        out["runner_runtime_ms"] = value["runner_runtime_ms"];
    for (const char* k : {"supervisor_reason", "fallback_reason"}) {
        const auto i = value.find(k);
        if (i != value.end() && i->is_string() && i->get<std::string>().size() <= 128)
            out[k] = *i;
    }
    return out;
}

inline std::string timeoutWorkloadKey(const nlohmann::json& p) {
    if (!p.is_object() || !p.contains("case") || !p.contains("flow_config"))
        return {};
    const auto data = nlohmann::json({{"case", p["case"]},
                                         {"flow_config", p["flow_config"]},
                                         {"scope", "single_case_runner_wall_v1"}})
                          .dump();
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.hex();
}

inline std::string timeoutGuardKey(
    const std::string& workload, const std::string& revision, const std::string& machine) {
    return workload + ":" + revision + ":" + machine;
}
} // namespace GCRP
