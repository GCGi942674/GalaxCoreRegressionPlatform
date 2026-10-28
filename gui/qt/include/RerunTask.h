#pragma once
#include "RpcClient.h"
#include <stdexcept>

inline Json rerunTask(
    const Json& source, const std::string& batch, size_t index, const std::string& revision = "") {
    if (source.value("status", std::string()) != "FAILED" &&
        source.value("status", std::string()) != "TIMEOUT")
        throw std::invalid_argument("only failed/timeout tasks can be rerun here");
    auto payload = source.at("payload");
    if (payload.value("spec_version", 0) != 2 || !payload.contains("flow_config") ||
        !payload["flow_config"].is_object() || payload["flow_config"].empty())
        throw std::invalid_argument("original flow snapshot unavailable; use Add Task explicitly");
    auto context = payload.value("context", Json::object());
    context.erase("date");
    context["source"] = "manual";
    context["batch_id"] = batch;
    context["suite"] = "rerun-task-" + std::to_string(source.at("task_id").get<uint64_t>());
    const auto original = source.at("revision").get<std::string>();
    const auto selected = revision.empty() ? original : revision;
    if (selected.empty() || selected.size() > 32 ||
        selected.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("rerun revision must be 1-32 decimal digits");
    context["name"] = "人工复测 · r" + original + " -> r" + selected;
    payload["context"] = context;
    return {{"task_type", source.at("task_type")},
        {"target", source.at("target")},
        {"revision", selected},
        {"priority", source.at("priority")},
        {"payload", payload},
        {"submission_key", batch + ":" + std::to_string(index)}};
}
