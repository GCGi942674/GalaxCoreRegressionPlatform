#include "scheduler/AutomationService.h"
#include "persistence/StateStore.h"
#include "scheduler/Scheduler.h"
#include "task/TaskService.h"
#include "protocol/ApiQueries.h"
#include "Logging.h"
#include <regex>
#include <set>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace GCRP {
namespace {
using Json = nlohmann::json;

std::tm local(std::time_t now) {
    std::tm value{};
    localtime_r(&now, &value);
    return value;
}

std::string dayOf(const std::tm& value) {
    char text[16];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &value);
    return text;
}

std::string dueDay(const Json& p, std::time_t now) {
    if (!p.at("enabled").get<bool>())
        return {};
    const auto time = p["time"].get<std::string>();
    // Include the previous calendar day so 23:59 has the same five-minute grace.
    for (int previous = 0; previous <= 1; ++previous) {
        auto scheduled = local(now);
        scheduled.tm_mday -= previous;
        scheduled.tm_hour = std::stoi(time.substr(0, 2));
        scheduled.tm_min = std::stoi(time.substr(3, 2));
        scheduled.tm_sec = 0;
        scheduled.tm_isdst = -1;
        const auto timestamp = std::mktime(&scheduled);
        const auto age = std::difftime(now, timestamp);
        const int weekday = scheduled.tm_wday == 0 ? 7 : scheduled.tm_wday;
        if (age >= 0 && age < 300 &&
            std::find(p["weekdays"].begin(), p["weekdays"].end(), Json(weekday)) !=
                p["weekdays"].end())
            return dayOf(scheduled);
    }
    return {};
}

void validatePlans(const Json& plans) {
    if (!plans.is_array() || plans.size() > 32)
        throw std::invalid_argument("at most 32 plans");
    if (plans.dump().size() > 512 * 1024)
        throw std::invalid_argument("plan settings exceed 512 KiB; split or shorten case lists");
    std::set<std::string> ids;
    for (const auto& p : plans) {
        Api::validate(p,
            {"id",
                "name",
                "enabled",
                "weekdays",
                "time",
                "template",
                "scope",
                "cases",
                "revision",
                "priority",
                "timeout_seconds",
                "timeout_mode"});
        if (p.contains("timeout_mode") && p["timeout_mode"] != "fixed" &&
            p["timeout_mode"] != "auto")
            throw std::invalid_argument("invalid timeout mode");
        if (p.size() != (p.contains("cases") ? 11 : 10) + (p.contains("timeout_mode") ? 1 : 0))
            throw std::invalid_argument("all plan fields are required");
        const auto id = Api::text(p, "id", 64), name = Api::text(p, "name", 128);
        if (!std::regex_match(id, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")) ||
            !ids.insert(id).second || name.empty())
            throw std::invalid_argument("invalid or duplicate plan ID/name");
        if (!p["enabled"].is_boolean() || !p["weekdays"].is_array() || p["weekdays"].empty() ||
            p["weekdays"].size() > 7)
            throw std::invalid_argument("invalid enabled/weekdays");
        std::set<int> weekdays;
        for (const auto& d : p["weekdays"]) {
            if (!d.is_number_integer() || d < 1 || d > 7 || !weekdays.insert(d.get<int>()).second)
                throw std::invalid_argument("invalid weekday");
        }
        if (!std::regex_match(Api::text(p, "time", 5), std::regex("([01][0-9]|2[0-3]):[0-5][0-9]")))
            throw std::invalid_argument("time must be HH:mm");
        if (!std::regex_match(
                Api::text(p, "template", 64), std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
            throw std::invalid_argument("invalid template");
        if (!p.contains("scope") || !p["scope"].is_string() ||
            p["scope"].get_ref<const std::string&>().size() > 512)
            throw std::invalid_argument("invalid scope");
        const auto scope = p["scope"].get<std::string>();
        if (p.contains("cases")) {
            static const std::regex casePattern("[A-Za-z0-9_.+/-]+");
            if (!scope.empty() || !p["cases"].is_array() || p["cases"].empty() ||
                p["cases"].size() > 20000)
                throw std::invalid_argument(
                    "case-list plan requires empty scope and 1..20000 cases");
            std::set<std::string> selected;
            for (const auto& entry : p["cases"]) {
                if (!entry.is_string())
                    throw std::invalid_argument("case path must be a string");
                const auto path = entry.get<std::string>();
                if (path.size() < 9 || path.size() > 512 || path.front() == '/' ||
                    path.substr(path.size() - 8) != "/run.tcl" ||
                    path.find("//") != std::string::npos || !std::regex_match(path, casePattern))
                    throw std::invalid_argument("case must be a relative run.tcl path");
                std::istringstream components(path);
                std::string component;
                while (std::getline(components, component, '/'))
                    if (component == ".." || component == ".")
                        throw std::invalid_argument("case escapes test2");
                if (!selected.insert(path).second)
                    throw std::invalid_argument("duplicate case in plan");
            }
        } else {
            if (scope.empty() || scope.front() == '/' || scope.back() == '/' ||
                scope.find("//") != std::string::npos ||
                !std::regex_match(scope, std::regex("[A-Za-z0-9_.+/-]+")))
                throw std::invalid_argument("scope must be a relative testcase directory");
            std::istringstream parts(scope);
            std::string part;
            while (std::getline(parts, part, '/'))
                if (part == ".." || (part == "." && scope != "."))
                    throw std::invalid_argument("scope escapes test2");
        }
        if (!std::regex_match(Api::text(p, "revision", 32), std::regex("(latest|[0-9]{1,32})")))
            throw std::invalid_argument("revision must be latest or a ZIP version");
        if (!p["priority"].is_number_integer() || p["priority"] < -100 || p["priority"] > 100 ||
            !p["timeout_seconds"].is_number_integer() || p["timeout_seconds"] < 1 ||
            p["timeout_seconds"] > 86400)
            throw std::invalid_argument("invalid priority/timeout");
    }
}
} // namespace

AutomationService::AutomationService(StateStore* store, Scheduler& scheduler, TaskService& tasks)
    : store_(store)
    , scheduler_(scheduler)
    , tasks_(tasks) {
    state_ = store ? store->loadAutomation() : Json(nullptr);
    if (state_.is_null())
        state_ = {{"version", 1},
            {"revision", uint64_t(0)},
            {"daily_enabled", true},
            {"plans", Json::array()},
            {"runs", Json::array()}};
    if (!state_.is_object() || state_.value("version", 0) != 1 || !state_.contains("runs") ||
        !state_["runs"].is_array() || state_["runs"].size() > 1100)
        throw std::runtime_error("invalid persisted automation state");
    validatePlans(state_.at("plans"));
    if (!state_.at("daily_enabled").is_boolean() || !state_.at("revision").is_number_unsigned())
        throw std::runtime_error("invalid persisted automation settings");
}

AutomationService::~AutomationService() {
    stop();
}

void AutomationService::start() {
    if (!store_ || thread_.joinable())
        return;
    thread_ = std::thread([this] {
        while (!stopping_) {
            bool failed = false;
            try {
                tick();
                std::lock_guard<std::mutex> lock(mutex_);
                runner_error_.clear();
            } catch (const std::exception& e) {
                failed = true;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    runner_error_ = std::string(e.what()).substr(0, 512);
                }
                LOG_ERROR(LogModule::SERVER, std::string("plan runner: ") + e.what());
            }
            std::unique_lock<std::mutex> lock(wait_mutex_);
            wake_.wait_for(lock, std::chrono::seconds(failed ? 30 : 2), [this] {
                return stopping_.load();
            });
        }
    });
}

void AutomationService::stop() {
    stopping_ = true;
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void AutomationService::commit(Json next) {
    if (!store_)
        throw std::logic_error("automation requires persistent Server");
    store_->saveAutomation(next);
    state_.swap(next);
    progress_dirty_ = false;
}

Json AutomationService::viewLocked() const {
    Json result = {{"version", 1},
        {"revision", state_["revision"]},
        {"daily_enabled", state_["daily_enabled"]},
        {"plans", state_["plans"]},
        {"runs", Json::array()},
        {"total_runs", state_["runs"].size()}};
    // Never copy large pending testcase snapshots into a GUI response.
    std::set<std::string> seen, current;
    for (const auto& p : state_["plans"])
        current.insert(p["id"].get<std::string>());
    size_t count = 0;
    for (auto it = state_["runs"].rbegin(); it != state_["runs"].rend(); ++it, ++count) {
        const auto id = it->at("plan_id").get<std::string>();
        if (count >= 128 && (!current.count(id) || seen.count(id)))
            continue;
        Json receipt = Json::object();
        for (auto field = it->begin(); field != it->end(); ++field)
            if (field.key() != "snapshot")
                receipt[field.key()] = field.value();
        result["runs"].push_back(std::move(receipt));
        seen.insert(id);
    }
    std::reverse(result["runs"].begin(), result["runs"].end());
    result["persistent"] = store_ != nullptr;
    result["server_time_ms"] = int64_t(std::time(nullptr)) * 1000;
    result["case_list_supported"] = true;
    result["case_list_max"] = 20000;
    result["plans_max_bytes"] = 512 * 1024;
    result["runner_error"] = runner_error_;
    const auto now = local(std::time(nullptr));
    char zone[64];
    std::strftime(zone, sizeof(zone), "%Z %z", &now);
    result["server_timezone"] = zone;
    result["grace_minutes"] = 5;
    return result;
}

Json AutomationService::query() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return viewLocked();
}

Json AutomationService::update(const Json& input) {
    Api::validate(input, {"expected_revision", "daily_enabled", "plans"});
    if (!input.contains("expected_revision") || !input["expected_revision"].is_number_unsigned() ||
        input.size() < 2)
        throw std::invalid_argument("expected_revision and settings required");
    if (input.contains("daily_enabled") && !input["daily_enabled"].is_boolean())
        throw std::invalid_argument("daily_enabled must be boolean");
    if (input.contains("plans"))
        validatePlans(input["plans"]);
    std::lock_guard<std::mutex> lock(mutex_);
    if (input["expected_revision"] != state_["revision"])
        throw std::logic_error("settings changed by another user; reload before saving");
    auto next = state_;
    if (input.contains("plans")) {
        // An interrupted run owns an immutable configuration. Only pause/resume is allowed.
        for (const auto& run : state_["runs"])
            if (run["state"] == "SUBMITTING") {
                const auto id = run["plan_id"];
                auto old = std::find_if(
                    state_["plans"].begin(), state_["plans"].end(), [&](const Json& p) {
                        return p["id"] == id;
                    });
                auto replacement =
                    std::find_if(input["plans"].begin(), input["plans"].end(), [&](const Json& p) {
                        return p["id"] == id;
                    });
                if (old == state_["plans"].end() || replacement == input["plans"].end())
                    throw std::logic_error("pause rather than delete a submitting plan");
                auto a = *old, b = *replacement;
                a.erase("enabled");
                b.erase("enabled");
                if (a != b)
                    throw std::logic_error(
                        "cannot edit a submitting plan; pause or wait for submission to finish");
            }
        next["plans"] = input["plans"];
    }
    if (input.contains("daily_enabled"))
        next["daily_enabled"] = input["daily_enabled"];
    next["revision"] = state_["revision"].get<uint64_t>() + 1;
    commit(std::move(next));
    LOG_INFO(LogModule::SERVER, "automation settings saved revision=" + state_["revision"].dump());
    wake_.notify_all();
    return viewLocked();
}

void AutomationService::publishDaily(const Json& manifest, bool submission_complete) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Retry of an already accepted round remains legal after the switch is off.
    const auto previous = scheduler_.dailyPlan(manifest.at("date").get<std::string>());
    if (!state_["daily_enabled"].get<bool>() && previous != manifest)
        throw std::logic_error("daily regression is disabled");
    scheduler_.publishDailyPlan(manifest, submission_complete);
}

Json AutomationService::snapshot(const Json& p, const std::string& day) {
    auto page = catalog_.query({{"refresh", true}});
    for (const char* source : {"server.work_root", "server.templates_dir", "server.artifact_root"})
        if (page["sources"].value(source, std::string()) != "READY")
            throw std::runtime_error(std::string("catalog unavailable: ") + source);
    const auto name = p["template"].get<std::string>();
    if (!page["templates"].contains(name))
        throw std::runtime_error("template missing: " + name);
    const auto revisions = page["revisions"];
    if (revisions.empty())
        throw std::runtime_error("no ZIP versions available");
    auto revision = p["revision"].get<std::string>();
    if (revision == "latest")
        revision = revisions[0].get<std::string>();
    if (std::find(revisions.begin(), revisions.end(), Json(revision)) == revisions.end())
        throw std::runtime_error("selected ZIP is missing: " + revision);
    const auto scope = p["scope"].get<std::string>(), prefix = scope + "/",
               batch = "plan-" + p["id"].get<std::string>() + "-" + day;
    const auto flow = page["templates"][name]["flow_config"];
    const bool explicitList = p.contains("cases");
    std::set<std::string> available;
    auto entries = Json::array();
    const auto generation = page["snapshot_generation"];
    for (;;) {
        for (const auto& item : page["cases"]) {
            const auto path = item.get<std::string>();
            if (explicitList) {
                available.insert(path);
                continue;
            }
            if (scope != "." && path.compare(0, prefix.size(), prefix) != 0)
                continue;
            entries.push_back(path);
            if (entries.size() > 20000)
                throw std::runtime_error("plan exceeds 20000 cases; narrow scope");
        }
        if (page["next_offset"].is_null())
            break;
        page =
            catalog_.query({{"snapshot_generation", generation}, {"offset", page["next_offset"]}});
        if (stopping_)
            throw std::runtime_error("Server stopping");
    }
    if (explicitList) {
        // Resolve the complete list before enqueueing anything. Preserve the user's order.
        for (const auto& item : p["cases"]) {
            const auto path = item.get<std::string>();
            if (!available.count(path))
                throw std::runtime_error("listed testcase missing: " + path);
            entries.push_back(path);
        }
    }
    if (entries.empty())
        throw std::runtime_error("no testcase in scope: " + scope);
    return {{"cases", entries},
        {"flow_config", flow},
        {"revision", revision},
        {"batch_id", batch},
        {"template", name},
        {"name", p["name"]},
        {"priority", p["priority"]},
        {"timeout_seconds", p["timeout_seconds"]},
        {"timeout_mode", p.value("timeout_mode", std::string("fixed"))},
        {"date", day}};
}

void AutomationService::tick(std::time_t now) {
    if (!store_ || stopping_)
        return;
    Json candidate;
    std::string candidate_day;
    uint64_t config_revision = 0;
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (progress_dirty_) {
            store_->saveAutomation(state_);
            progress_dirty_ = false;
        }
        // Batch cancellation must settle a paused submit as well; otherwise a
        // paused-and-cancelled rule could never be deleted without re-enabling it.
        for (size_t i = 0; i < state_["runs"].size(); ++i) {
            const auto& run = state_["runs"][i];
            if (run["state"] != "SUBMITTING")
                continue;
            const auto batch = run["batch_id"].get<std::string>();
            if (scheduler_.isBatchStopped(batch) || scheduler_.isBatchDeleted(batch)) {
                auto next = state_;
                auto& progress = next["runs"][i];
                progress["state"] = "STOPPED";
                progress.erase("snapshot");
                commit(std::move(next));
            }
        }
        bool found = false;
        for (size_t i = 0; i < state_["runs"].size(); ++i) {
            const auto& run = state_["runs"][i];
            if (run["state"] != "SUBMITTING")
                continue;
            const auto p =
                std::find_if(state_["plans"].begin(), state_["plans"].end(), [&](const Json& p) {
                    return p["id"] == run["plan_id"];
                });
            if (p != state_["plans"].end() && (*p)["enabled"].get<bool>()) {
                pending = i;
                found = true;
                break;
            }
        }
        {
            // Claim new due schedules even while a large older plan is still
            // submitting; otherwise its work could make another plan miss grace.
            for (const auto& p : state_["plans"]) {
                const auto scheduled_day = dueDay(p, now);
                if (scheduled_day.empty())
                    continue;
                bool claimed = false;
                for (const auto& run : state_["runs"])
                    if (run["plan_id"] == p["id"] && run["date"] == scheduled_day) {
                        claimed = true;
                        break;
                    }
                if (!claimed) {
                    candidate = p;
                    candidate_day = scheduled_day;
                    config_revision = state_["revision"].get<uint64_t>();
                    break;
                }
            }
            if (candidate.is_null() && !found)
                return;
        }
    }
    if (!candidate.is_null()) {
        Json snap;
        std::string error;
        try {
            snap = snapshot(candidate, candidate_day);
        } catch (const std::exception& e) {
            error = std::string(e.what()).substr(0, 512);
        }
        if (stopping_)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_["revision"] != config_revision)
            return;
        auto next = state_;
        // Bounded audit history: retain pending snapshots and 31 days of receipts.
        const auto cutoff = dayOf(local(now - 31 * 24 * 60 * 60));
        auto& runs = next["runs"];
        runs.erase(std::remove_if(runs.begin(),
                       runs.end(),
                       [&](const Json& run) {
                           return run["state"] != "SUBMITTING" &&
                                  run["date"].get<std::string>() < cutoff;
                       }),
            runs.end());
        if (runs.size() >= 1100)
            throw std::runtime_error("plan history capacity reached");
        pending = runs.size();
        Json receipt = {{"plan_id", candidate["id"]},
            {"name", candidate["name"]},
            {"date", candidate_day},
            {"state", error.empty() ? "SUBMITTING" : "FAILED"},
            {"next_index", 0},
            {"error", error},
            {"created_at_ms", int64_t(now) * 1000}};
        if (error.empty()) {
            size_t bytes = snap.dump().size();
            for (const auto& run : runs)
                if (run.contains("snapshot"))
                    bytes += run["snapshot"].dump().size();
            if (bytes > 16 * 1024 * 1024)
                throw std::runtime_error("pending plan snapshot budget exceeded");
            receipt["snapshot"] = snap;
            receipt["planned"] = snap["cases"].size();
            receipt["revision"] = snap["revision"];
            receipt["batch_id"] = snap["batch_id"];
        } else
            receipt["planned"] = 0;
        if (error.empty())
            scheduler_.reserveArtifact(snap.at("revision").get<std::string>());
        runs.push_back(receipt);
        commit(std::move(next));
        if (!error.empty()) {
            LOG_WARN(LogModule::SERVER, "weekly plan failed: " + error);
            return;
        }
        LOG_INFO(LogModule::SERVER, "weekly plan started: " + snap["batch_id"].get<std::string>());
    }
    // Durable, immutable snapshot + stable submission keys make replay after a crash harmless.
    for (size_t count = 0; count < 100 && !stopping_; ++count) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& run = state_["runs"][pending];
        if (run["state"] != "SUBMITTING")
            break;
        const auto p =
            std::find_if(state_["plans"].begin(), state_["plans"].end(), [&](const Json& p) {
                return p["id"] == run["plan_id"];
            });
        if (p == state_["plans"].end() || !(*p)["enabled"].get<bool>())
            break;
        const auto& snap = run["snapshot"];
        const auto index = run["next_index"].get<size_t>();
        const auto batch = snap["batch_id"].get<std::string>();
        if (scheduler_.isBatchStopped(batch) || scheduler_.isBatchDeleted(batch)) {
            auto next = state_;
            auto& progress = next["runs"][pending];
            progress["state"] = "STOPPED";
            progress.erase("snapshot");
            commit(std::move(next));
            break;
        }
        CreateTaskRequest request;
        request.type = TaskType::REGRESSION;
        request.target = snap["name"].get<std::string>();
        request.revision = snap["revision"].get<std::string>();
        request.priority = snap["priority"].get<int>();
        request.timeout_mode = snap.value("timeout_mode", std::string("fixed"));
        request.submission_key = batch + ":" + std::to_string(index);
        request.payload = {{"spec_version", 2},
            {"case", snap["cases"][index]},
            {"flow", snap["template"]},
            {"flow_config", snap["flow_config"]},
            {"timeout_seconds", snap["timeout_seconds"]},
            {"context",
                {{"batch_id", batch},
                    {"template", snap["template"]},
                    {"suite", run["plan_id"]},
                    {"name", snap["name"]},
                    {"source", "plan"},
                    {"date", snap["date"]}}}};
        if (!tasks_.createTask(request))
            throw std::runtime_error("plan task submission rejected");
        // Tasks carry durable submission keys; the progress checkpoint may lag
        // by up to 24 cases without causing duplicate execution after a crash.
        auto& progress = state_["runs"][pending];
        progress["next_index"] = index + 1;
        if (index + 1 == snap["cases"].size()) {
            progress["state"] = "SUBMITTED";
            progress["finished_at_ms"] = int64_t(std::time(nullptr)) * 1000;
            progress.erase("snapshot");
        }
        progress_dirty_ = true;
        if ((count + 1) % 25 == 0 || state_["runs"][pending]["state"] == "SUBMITTED") {
            store_->saveAutomation(state_);
            progress_dirty_ = false;
        }
        if (state_["runs"][pending]["state"] == "SUBMITTED") {
            LOG_INFO(LogModule::SERVER, "weekly plan submitted: " + batch);
            break;
        }
    }
}
} // namespace GCRP
