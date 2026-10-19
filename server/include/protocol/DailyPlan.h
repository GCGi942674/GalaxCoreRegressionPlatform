#pragma once
#include "protocol/ApiQueries.h"
#include "task/TaskQuery.h"

namespace GCRP::Api {
// Counts are pinned before the first testcase is submitted. They are not
// inferred from the number of task records that happen to exist afterwards.
inline void validateDailyPlan(const Json& plan) {
    validate(plan,{"date","suite_id","plan_sha256","batches"});
    if(plan.size()!=4||!validQueryDate(text(plan,"date",10)))
        throw std::invalid_argument("invalid daily plan date");
    text(plan,"suite_id",128);
    const auto digest=text(plan,"plan_sha256",64);
    if(digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::invalid_argument("invalid daily plan digest");
    const auto& batches=plan.at("batches");
    if(!batches.is_array()||batches.empty()||batches.size()>1000)
        throw std::invalid_argument("invalid daily batch count");
    std::set<std::string> ids;uint64_t total=0;
    for(const auto& batch:batches){
        validate(batch,{"batch_id","template","name","revision","planned"});
        if(batch.size()!=5||!ids.insert(text(batch,"batch_id",128)).second)
            throw std::invalid_argument("duplicate daily batch");
        text(batch,"template",64);text(batch,"name",128);text(batch,"revision",128);
        const auto n=positive(batch,"planned",0);
        if(n>100000)throw std::invalid_argument("invalid batch planned count");
        total+=n;
        if(!batch.contains("planned")||total>100000)
            throw std::invalid_argument("daily plan exceeds 100000 cases");
    }
}
} // namespace GCRP::Api
