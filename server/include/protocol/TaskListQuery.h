#pragma once
#include "protocol/ApiQueries.h"
#include <map>
#include <deque>

namespace GCRP::Api {
// Immutable, bounded per-query snapshots. A heartbeat/task transition does not
// invalidate a page already being read. Router calls are host-serialized.
class TaskListQuery {
    struct Snapshot {
        uint64_t id, generation;
        std::string filter;
        Json rows;
        std::chrono::steady_clock::time_point expires;
        size_t bytes;
    };

    std::deque<Snapshot> snapshots_;
    uint64_t next_{0};
    static constexpr size_t budget_ = 64 * 1024 * 1024;

public:
    Json query(const Json& input, const Scheduler& scheduler) {
        validate(input,
            {"status",
                "target",
                "revision",
                "batch_id",
                "limit",
                "before_task_id",
                "snapshot_generation",
                "view"});
        const auto count = limit(input), before = positive(input, "before_task_id", 0);
        const auto token = positive(input, "snapshot_generation", 0);
        auto filter = input;
        filter.erase("limit");
        filter.erase("before_task_id");
        filter.erase("snapshot_generation");
        const auto identity = filter.dump();
        const auto now = std::chrono::steady_clock::now();
        while (!snapshots_.empty() && snapshots_.front().expires <= now)
            snapshots_.pop_front();
        Snapshot* current = nullptr;
        if (token) {
            for (auto& snapshot : snapshots_)
                if (snapshot.id == token && snapshot.filter == identity)
                    current = &snapshot;
            if (!current)
                throw std::invalid_argument("task snapshot expired; restart query from first page");
        } else if (before) {
            for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it)
                if (it->filter == identity) {
                    current = &*it;
                    break;
                }
            if (!current)
                throw std::invalid_argument("task snapshot expired; restart query from first page");
        } else {
            const auto generation = scheduler.taskGeneration();
            for (const auto& saved : snapshots_)
                if (saved.generation == generation && saved.filter == identity) {
                    auto reuse = input;
                    reuse["snapshot_generation"] = saved.id;
                    return query(reuse, scheduler);
                }
            const auto view = text(input, "view", 16);
            if (!view.empty() && view != "groups")
                throw std::invalid_argument("unknown task view");
            const auto status = text(input, "status", 16), target = text(input, "target", 128),
                       revision = text(input, "revision", 128),
                       batch = text(input, "batch_id", 128);
            if (!status.empty() && !std::set<std::string>{"PENDING",
                                       "QUEUED",
                                       "ASSIGNED",
                                       "RUNNING",
                                       "SUCCEEDED",
                                       "FAILED",
                                       "TIMEOUT",
                                       "CANCELLED"}
                                        .count(status))
                throw std::invalid_argument("unknown status");
            Json rows = Json::array();
            std::map<std::string, Json> groups;
            size_t retained = 0;
            auto rowOf = [](const Task& task) {
                const auto summary = taskListSummary(task);
                Json row = {{"task_id", task.getId()},
                    {"task_type", toString(task.getType())},
                    {"status", toString(task.getStatus())},
                    {"target", task.getTarget()},
                    {"revision", task.getRevision()},
                    {"batch_id", taskContext(task, "batch_id")},
                    {"worker_id", task.getWorkerId()},
                    {"run_id", task.getRunId()},
                    {"priority", task.getPriority()},
                    {"retry_count", task.getRetryCount()}};
                for (auto it = summary.begin(); it != summary.end(); ++it)
                    row[it.key()] = it.value();
                return row;
            };
            uint64_t captured = 0;
            const auto sql_groups =
                view == "groups" ? scheduler.taskGroups(target, revision, batch) : std::nullopt;
            if (sql_groups) {
                captured = sql_groups->first;
                for (const auto& group : sql_groups->second) {
                    if (!status.empty() && group.summary["counts"].value(status, uint64_t(0)) == 0)
                        continue;
                    auto row = rowOf(group.representative);
                    row["created_at_ms"] = group.created_at_ms;
                    row["group_summary"] = group.summary;
                    retained += row.dump().size();
                    if (retained > budget_)
                        throw std::invalid_argument("task query too broad; narrow filters");
                    rows.push_back(std::move(row));
                }
            } else
                captured = scheduler.visitTasks(
                    [&](const Task& task) {
                        const auto summary = taskListSummary(task);
                        const auto context = summary.value("payload", Json::object())
                                                 .value("context", Json::object());
                        const auto group = context.value("batch_id", std::string{});
                        if ((!target.empty() && task.getTarget() != target) ||
                            (!revision.empty() && task.getRevision() != revision) ||
                            (!batch.empty() && group != batch))
                            return;
                        Json row = rowOf(task);
                        if (view == "groups") {
                            auto& parent =
                                groups[group.empty() ? "single-" + std::to_string(task.getId())
                                                     : group];
                            if (parent.is_null()) {
                                retained += row.dump().size() + 512;
                                if (retained > budget_)
                                    throw std::invalid_argument(
                                        "task query too broad; narrow filters");
                                parent = {{"representative", row},
                                    {"counts", Json::object()},
                                    {"outcomes", Json::object()},
                                    {"total", 0},
                                    {"created_at_ms", row["created_at_ms"]}};
                            }
                            if (row["created_at_ms"].get<int64_t>() <
                                parent["created_at_ms"].get<int64_t>())
                                parent["created_at_ms"] = row["created_at_ms"];
                            if (parent["representative"]["task_id"].get<uint64_t>() < task.getId())
                                parent["representative"] = row;
                            auto& counts = parent["counts"];
                            const auto s = toString(task.getStatus());
                            counts[s] = counts.value(s, uint64_t(0)) + 1;
                            parent["total"] = parent["total"].get<uint64_t>() + 1;
                            auto business = s;
                            const auto execution = task.getExecutionSummary();
                            if (s == "SUCCEEDED")
                                business = execution.is_object() && execution.value("case_status",
                                                                        std::string{}) == "PASS"
                                               ? "PASS"
                                               : "UNKNOWN";
                            else if (s == "FAILED") {
                                const auto cs = execution.is_object()
                                                    ? execution.value("case_status", std::string{})
                                                    : std::string{};
                                business = cs == "FAIL"    ? "FAIL"
                                           : cs == "CRASH" ? "CRASH"
                                                           : "ERROR";
                            }
                            auto& outcomes = parent["outcomes"];
                            outcomes[business] = outcomes.value(business, uint64_t(0)) + 1;
                        } else if (status.empty() || toString(task.getStatus()) == status) {
                            retained += row.dump().size();
                            if (retained > budget_)
                                throw std::invalid_argument("task query too broad; select a batch");
                            rows.push_back(std::move(row));
                        }
                    },
                    batch);
            if (view == "groups")
                for (auto& entry : groups) {
                    if (!status.empty() && entry.second["counts"].value(status, uint64_t(0)) == 0)
                        continue;
                    auto row = entry.second["representative"];
                    row["created_at_ms"] = entry.second["created_at_ms"];
                    row["group_summary"] = {{"counts", entry.second["counts"]},
                        {"outcomes", entry.second["outcomes"]},
                        {"total", entry.second["total"]}};
                    rows.push_back(std::move(row));
                }
            std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
                return a["task_id"].get<uint64_t>() > b["task_id"].get<uint64_t>();
            });
            const auto bytes = rows.dump().size();
            if (bytes > budget_)
                throw std::invalid_argument(
                    "task query too broad; select a batch or use grouped view");
            size_t used = bytes;
            for (const auto& s : snapshots_)
                used += s.bytes;
            while (!snapshots_.empty() && (snapshots_.size() >= 8 || used > budget_)) {
                used -= snapshots_.front().bytes;
                snapshots_.pop_front();
            }
            snapshots_.push_back({++next_,
                captured,
                identity,
                std::move(rows),
                now + std::chrono::minutes(2),
                bytes});
            current = &snapshots_.back();
        }
        Json items = Json::array();
        bool more = false;
        for (const auto& row : current->rows) {
            if (before && row["task_id"].get<uint64_t>() >= before)
                continue;
            if (items.size() == count) {
                more = true;
                break;
            }
            items.push_back(row);
        }
        return {{"items", items},
            {"total", current->rows.size()},
            {"has_more", more},
            {"next_before_task_id", more ? items.back()["task_id"] : Json(nullptr)},
            {"snapshot_generation", current->id},
            {"snapshot_scope", "immutable_query"}};
    }
};
} // namespace GCRP::Api
