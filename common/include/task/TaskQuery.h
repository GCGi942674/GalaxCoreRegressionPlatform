#pragma once
#include "task/Task.h"
namespace GCRP {
inline std::string taskContext(const Task& task,const char* key){
    const auto& p=task.getPayload();
    if(!p.is_object()||!p.contains("context")||!p["context"].is_object())return {};
    const auto& c=p["context"];return c.contains(key)&&c[key].is_string()?c[key].get<std::string>():std::string{};
}
inline bool validQueryDate(const std::string& value){
    if(value.size()!=10||value[4]!='-'||value[7]!='-')return false;
    for(size_t i=0;i<10;++i)if(i!=4&&i!=7&&(value[i]<'0'||value[i]>'9'))return false;
    int y=std::stoi(value.substr(0,4)),m=std::stoi(value.substr(5,2)),d=std::stoi(value.substr(8,2));
    if(y<1||m<1||m>12)return false;const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return d>=1&&d<=days[m-1]+(m==2&&y%4==0&&(y%100!=0||y%400==0));
}
inline std::string taskBusinessOutcome(const Task& task){
    auto state=toString(task.getStatus());const auto& e=task.getExecutionSummary();
    const auto cs=e.is_object()&&e.contains("case_status")&&e["case_status"].is_string()?e["case_status"].get<std::string>():std::string{};
    return state=="SUCCEEDED"?(cs=="PASS"?"PASS":"UNKNOWN"):
        state=="FAILED"?(cs=="FAIL"?"FAIL":cs=="CRASH"?"CRASH":"ERROR"):state;
}
struct TaskGroupSnapshot { Task representative; nlohmann::json summary; int64_t created_at_ms; };
}
