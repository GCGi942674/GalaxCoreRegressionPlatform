#pragma once
#include "nlohmann/json.hpp"
#include <string>

namespace GCRP {
inline bool validFlowConfig(const nlohmann::json& flow) {
    if (!flow.is_object() || flow.empty() || flow.size() > 64)
        return false;
    for (auto it = flow.begin(); it != flow.end(); ++it) {
        const auto& key = it.key();
        if (key.empty() || key.size() > 64 ||
            !((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= 'A' && key[0] <= 'Z') ||
                key[0] == '_'))
            return false;
        for (unsigned char ch : key)
            if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
                !(ch >= '0' && ch <= '9') && ch != '_')
                return false;
        if (!it.value().is_number_integer())
            return false;
        const auto value = it.value().get<int64_t>();
        if (value < 0 || value > 1000000)
            return false;
    }
    return true;
}

inline bool validRegressionPayload(const nlohmann::json& p) {
    if (!p.is_object() || p.dump().size() > 4096)
        return false;
    if (p.empty())
        return true; // Legacy demo tasks.
    if (!p.contains("spec_version") || !p.contains("case") || !p.contains("flow") ||
        !p.contains("timeout_seconds"))
        return false;
    if (!p["spec_version"].is_number_integer() || !p["case"].is_string() ||
        !p["flow"].is_string() || !p["timeout_seconds"].is_number_unsigned())
        return false;
    if (p["spec_version"] == 1) {
        if (p.size() != 4)
            return false;
    } else if (p["spec_version"] == 2) {
        if ((p.size() != 5 && p.size() != 6) || !p.contains("flow_config") ||
            !validFlowConfig(p["flow_config"]))
            return false;
        if (p.contains("context")) {
            const auto& context = p["context"];
            if (!context.is_object() || context.size() > 6)
                return false;
            for (auto it = context.begin(); it != context.end(); ++it) {
                if (it.key() != "template" && it.key() != "suite" && it.key() != "batch_id" &&
                    it.key() != "name" && it.key() != "source" && it.key() != "date")
                    return false;
                if (!it.value().is_string() || it.value().get<std::string>().size() > 128)
                    return false;
            }
        }
    } else
        return false;
    const std::string path = p["case"].get<std::string>();
    const std::string flow = p["flow"].get<std::string>();
    if (path.empty() || path.size() > 512 || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string::npos || path.find("//") != std::string::npos ||
        path.size() < 9 || path.substr(path.size() - 8) != "/run.tcl")
        return false;
    size_t pos = 0;
    while (pos < path.size()) {
        const auto end = path.find('/', pos);
        const auto part = path.substr(pos, end == std::string::npos ? end : end - pos);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (unsigned char ch : part)
            if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
                !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-' && ch != '.' && ch != '+')
                return false;
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    if (flow.empty() || flow.size() > 64)
        return false;
    for (unsigned char ch : flow)
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') &&
            ch != '_' && ch != '-')
            return false;
    const auto seconds = p["timeout_seconds"].get<uint64_t>();
    return seconds >= 1 && seconds <= 86400;
}
} // namespace GCRP
