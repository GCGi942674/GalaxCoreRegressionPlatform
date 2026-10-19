#pragma once
#include "RpcClient.h"
#include <stdexcept>

inline Json rerunTask(const Json& source,const std::string& batch,size_t index) {
    if(source.value("status",std::string())!="FAILED"&&source.value("status",std::string())!="TIMEOUT")
        throw std::invalid_argument("only failed/timeout tasks can be rerun here");
    auto payload=source.at("payload");
    if(payload.value("spec_version",0)!=2||!payload.contains("flow_config")||
        !payload["flow_config"].is_object()||payload["flow_config"].empty())
        throw std::invalid_argument("original flow snapshot unavailable; use Add Task explicitly");
    auto context=payload.value("context",Json::object());
    context.erase("date");context["source"]="manual";context["batch_id"]=batch;
    context["suite"]="rerun-task-"+std::to_string(source.at("task_id").get<uint64_t>());
    context["name"]="Rerun "+batch;
    payload["context"]=context;
    return {{"task_type",source.at("task_type")},{"target",source.at("target")},{"revision",source.at("revision")},
        {"priority",source.at("priority")},{"payload",payload},{"submission_key",batch+":"+std::to_string(index)}};
}
