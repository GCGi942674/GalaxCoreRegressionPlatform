#include "progress/ProgressService.h"
#include "task/TaskType.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace GCRP {
namespace {
std::string stringField(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key) || !value[key].is_string())
        return {};
    return value[key].get<std::string>();
}

std::string numericRevision(const std::string& value) {
    if (value.empty() || value.size() > 32 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        }))
        return {};
    const auto first = value.find_first_not_of('0');
    return first == std::string::npos ? "0" : value.substr(first);
}

struct RevisionLess {
    bool operator()(const std::string& a, const std::string& b) const {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    }
};

bool validText(const nlohmann::json& input,
    const char* key,
    std::string& result,
    size_t max_length,
    bool required) {
    if (!input.contains(key))
        return !required;
    if (!input[key].is_string())
        return false;
    result = input[key].get<std::string>();
    return !result.empty() && result.size() <= max_length;
}

bool validLimit(const nlohmann::json& input, size_t& limit, size_t maximum) {
    if (!input.contains("limit"))
        return true;
    if (!input["limit"].is_number_integer())
        return false;
    const auto value = input["limit"].get<int64_t>();
    if (value < 1 || value > static_cast<int64_t>(maximum))
        return false;
    limit = static_cast<size_t>(value);
    return true;
}

const char* outcome(const Task& task) {
    const auto status = task.getStatus();
    if (status == TaskStatus::SUCCEEDED &&
        stringField(task.getExecutionSummary(), "case_status") == "PASS")
        return "PASS";
    if (status == TaskStatus::FAILED &&
        stringField(task.getExecutionSummary(), "case_status") == "FAIL")
        return "FAIL";
    if (status == TaskStatus::PENDING || status == TaskStatus::QUEUED ||
        status == TaskStatus::ASSIGNED || status == TaskStatus::RUNNING)
        return "ACTIVE";
    if (status == TaskStatus::TIMEOUT)
        return "TIMEOUT";
    if (status == TaskStatus::CANCELLED)
        return "CANCELLED";
    if (status == TaskStatus::FAILED) {
        if (stringField(task.getExecutionSummary(), "case_status") == "CRASH")
            return "CRASH";
        return "ERROR";
    }
    return "UNKNOWN";
}

using Cases = std::map<std::string, Task>;
using Revisions = std::map<std::string, Cases, RevisionLess>;

std::string caseKey(const Task& task) {
    const auto path = stringField(task.getPayload(), "case");
    if (path.empty())
        return {};
    return path + '\0' + stringField(task.getPayload(), "flow");
}

Revisions collect(
    const std::vector<Task>& tasks, const std::string& target, const std::string& flow) {
    Revisions revisions;
    for (const auto& task : tasks) {
        if (task.getType() != TaskType::REGRESSION || task.getTarget() != target)
            continue;
        const auto revision = numericRevision(task.getRevision());
        const auto key = caseKey(task);
        if (revision.empty() || key.empty() ||
            (!flow.empty() && stringField(task.getPayload(), "flow") != flow))
            continue;
        auto& cases = revisions[revision];
        const auto prior = cases.find(key);
        if (prior == cases.end() || prior->second.getId() < task.getId())
            cases.insert_or_assign(key, task);
    }
    return revisions;
}

std::pair<size_t, size_t> transitions(const Cases& previous, const Cases& current) {
    size_t rollbacks = 0, fixes = 0;
    for (const auto& entry : current) {
        const auto old = previous.find(entry.first);
        if (old == previous.end())
            continue;
        const std::string before = outcome(old->second), after = outcome(entry.second);
        if (before == "PASS" && after == "FAIL")
            ++rollbacks;
        if (before == "FAIL" && after == "PASS")
            ++fixes;
    }
    return {rollbacks, fixes};
}
} // namespace

std::optional<ProgressQuery> ProgressQuery::parse(const nlohmann::json& request) {
    if (!request.is_object() || request.size() > 4)
        return std::nullopt;
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "target" && it.key() != "flow" && it.key() != "limit" &&
            it.key() != "before_revision")
            return std::nullopt;
    ProgressQuery result;
    if (!validText(request, "target", result.target, 128, true) ||
        !validText(request, "flow", result.flow, 64, false) ||
        !validLimit(request, result.limit, 20) ||
        !validText(request, "before_revision", result.before_revision, 32, false))
        return std::nullopt;
    if (!result.before_revision.empty()) {
        result.before_revision = numericRevision(result.before_revision);
        if (result.before_revision.empty())
            return std::nullopt;
    }
    return result;
}

std::optional<RollbackQuery> RollbackQuery::parse(const nlohmann::json& request) {
    if (!request.is_object() || request.size() > 6)
        return std::nullopt;
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "target" && it.key() != "flow" && it.key() != "from_revision" &&
            it.key() != "to_revision" && it.key() != "limit" && it.key() != "after_task_id")
            return std::nullopt;
    RollbackQuery result;
    if (!validText(request, "target", result.target, 128, true) ||
        !validText(request, "flow", result.flow, 64, false) ||
        !validText(request, "from_revision", result.from_revision, 32, true) ||
        !validText(request, "to_revision", result.to_revision, 32, true) ||
        !validLimit(request, result.limit, 50))
        return std::nullopt;
    result.from_revision = numericRevision(result.from_revision);
    result.to_revision = numericRevision(result.to_revision);
    if (result.from_revision.empty() || result.to_revision.empty() ||
        !RevisionLess{}(result.from_revision, result.to_revision))
        return std::nullopt;
    if (request.contains("after_task_id")) {
        if (!request["after_task_id"].is_number_integer())
            return std::nullopt;
        if (!request["after_task_id"].is_number_unsigned() &&
            request["after_task_id"].get<int64_t>() <= 0)
            return std::nullopt;
        result.after_task_id = request["after_task_id"].get<TaskId>();
        if (!result.after_task_id)
            return std::nullopt;
    }
    return result;
}

nlohmann::json ProgressService::queryProgress(const ProgressQuery& query) const {
    const auto history = scheduler_.historySnapshot("", {}, "", query.target);
    std::pair<uint64_t, std::vector<Task>> snapshot;
    snapshot.first = history.generation;
    for (const auto& task : history.tasks)
        if (query.flow.empty() || stringField(task.getPayload(), "flow") == query.flow)
            snapshot.second.push_back(task);
    const auto revisions = collect(snapshot.second, query.target, query.flow);
    nlohmann::json rows = nlohmann::json::array();
    bool has_more = false;
    for (auto it = revisions.rbegin(); it != revisions.rend(); ++it) {
        if (!query.before_revision.empty() && !RevisionLess{}(it->first, query.before_revision))
            continue;
        if (rows.size() == query.limit) {
            has_more = true;
            break;
        }
        nlohmann::json counts = {{"PASS", 0},
            {"FAIL", 0},
            {"ACTIVE", 0},
            {"TIMEOUT", 0},
            {"CRASH", 0},
            {"ERROR", 0},
            {"CANCELLED", 0},
            {"UNKNOWN", 0}};
        for (const auto& item : it->second) {
            const char* name = outcome(item.second);
            counts[name] = counts[name].get<size_t>() + 1;
        }
        const auto previous = std::next(it).base();
        const bool has_previous = previous != revisions.begin();
        const auto prior = has_previous ? std::prev(previous) : revisions.end();
        size_t new_cases = 0, missing_cases = 0;
        if (has_previous) {
            for (const auto& item : it->second)
                if (!prior->second.count(item.first))
                    ++new_cases;
            for (const auto& item : prior->second)
                if (!it->second.count(item.first))
                    ++missing_cases;
        }
        const auto changes = has_previous ? transitions(prior->second, it->second)
                                          : std::make_pair(size_t{0}, size_t{0});
        rows.push_back({{"revision", it->first},
            {"cases", it->second.size()},
            {"counts", counts},
            {"previous_revision",
                has_previous ? nlohmann::json(prior->first) : nlohmann::json(nullptr)},
            {"rollback_count", changes.first},
            {"fixed_count", changes.second},
            {"new_cases", new_cases},
            {"missing_cases", missing_cases}});
    }
    return {{"target", query.target},
        {"flow", query.flow},
        {"snapshot_generation", snapshot.first},
        {"total_revisions", revisions.size()},
        {"revisions", rows},
        {"has_more", has_more},
        {"next_before_revision",
            has_more ? nlohmann::json(rows.back()["revision"]) : nlohmann::json(nullptr)}};
}

nlohmann::json ProgressService::queryRollbacks(const RollbackQuery& query) const {
    const auto history = scheduler_.historySnapshot("", {}, "", query.target);
    std::pair<uint64_t, std::vector<Task>> snapshot;
    snapshot.first = history.generation;
    for (const auto& task : history.tasks) {
        const auto revision = numericRevision(task.getRevision());
        if ((revision == query.from_revision || revision == query.to_revision) &&
            (query.flow.empty() || stringField(task.getPayload(), "flow") == query.flow))
            snapshot.second.push_back(task);
    }
    const auto revisions = collect(snapshot.second, query.target, query.flow);
    const auto old = revisions.find(query.from_revision);
    const auto current = revisions.find(query.to_revision);
    nlohmann::json items = nlohmann::json::array();
    std::vector<std::pair<TaskId, nlohmann::json>> matches;
    if (old != revisions.end() && current != revisions.end()) {
        for (const auto& entry : current->second) {
            const auto prior = old->second.find(entry.first);
            if (prior == old->second.end() || std::string_view(outcome(prior->second)) != "PASS" ||
                std::string_view(outcome(entry.second)) != "FAIL")
                continue;
            const auto& task = entry.second;
            matches.push_back({task.getId(),
                {{"case", stringField(task.getPayload(), "case")},
                    {"flow", stringField(task.getPayload(), "flow")},
                    {"previous_task_id", prior->second.getId()},
                    {"current_task_id", task.getId()},
                    {"previous_revision", query.from_revision},
                    {"current_revision", query.to_revision}}});
        }
    }
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    bool has_more = false;
    for (const auto& item : matches) {
        if (item.first <= query.after_task_id)
            continue;
        if (items.size() == query.limit) {
            has_more = true;
            break;
        }
        items.push_back(item.second);
    }
    return {{"target", query.target},
        {"flow", query.flow},
        {"from_revision", query.from_revision},
        {"to_revision", query.to_revision},
        {"snapshot_generation", snapshot.first},
        {"from_available", old != revisions.end()},
        {"to_available", current != revisions.end()},
        {"total", matches.size()},
        {"items", items},
        {"has_more", has_more},
        {"next_after_task_id",
            has_more ? nlohmann::json(items.back()["current_task_id"]) : nlohmann::json(nullptr)}};
}

} // namespace GCRP
