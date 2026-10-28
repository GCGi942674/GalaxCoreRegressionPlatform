#include "scheduler/TimeoutPolicy.h"
#include "task/ExecutionTiming.h"
#include "config/Config.h"
#include "Logging.h"
#include <cmath>
#include <fstream>
#include <filesystem>
#include <map>
#include <algorithm>

namespace GCRP {
namespace {
using Json = nlohmann::json;

double p95(std::vector<double> values) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>(std::ceil(values.size() * 0.95)) - 1];
}

std::string stageKey(const Json& p) {
    const auto data = p.value("flow_config", Json::object()).dump();
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.hex();
}
} // namespace

TimeoutPolicy::TimeoutPolicy(StateStore& store)
    : store_(store) {
    auto& c = Config::instance();
    options_.mode = c.getString("timeout.mode", "observe");
    options_.history_days = c.getInt("timeout.history_days", 30);
    options_.min_success = c.getInt("timeout.min_success_samples", 10);
    options_.min_seconds = c.getInt("timeout.min_seconds", 300);
    options_.max_seconds = c.getInt("timeout.max_seconds", 5400);
    options_.margin_seconds = c.getInt("timeout.margin_seconds", 60);
    options_.factor = std::stod(c.getString("timeout.safety_factor", "2.0"));
    options_.baseline_file =
        c.getString("timeout.baseline_index_file", "./database/vivado-runtime-index.json");
    if ((options_.mode != "off" && options_.mode != "observe" && options_.mode != "enforce") ||
        options_.history_days < 1 || options_.history_days > 365 || options_.min_success < 3 ||
        options_.min_success > 100 || options_.min_seconds < 1 ||
        options_.max_seconds < options_.min_seconds || options_.max_seconds > 86400 ||
        options_.margin_seconds < 0 || options_.margin_seconds > 3600 ||
        !std::isfinite(options_.factor) || options_.factor < 1 || options_.factor > 10)
        throw std::runtime_error("invalid adaptive timeout settings");
    refresh();
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!cv_.wait_for(lock, std::chrono::seconds(30), [this] {
            return stopping_;
        })) {
            lock.unlock();
            try {
                refresh();
            } catch (const std::exception& e) {
                LOG_WARN(LogModule::TASK, std::string("timeout snapshot retained: ") + e.what());
            }
            lock.lock();
        }
    });
}

TimeoutPolicy::~TimeoutPolicy() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void TimeoutPolicy::refresh() {
    if (std::chrono::steady_clock::now() >= next_prune_) {
        store_.pruneRuntimeSamples(options_.history_days);
        next_prune_ = std::chrono::steady_clock::now() + std::chrono::hours(1);
    }
    if (options_.mode == "off")
        return;
    auto snapshot = store_.runtimeSnapshot(options_.history_days);
    std::vector<Json> rows;
    rows.reserve(snapshot["samples"].size());
    std::map<std::string, int> counts;
    for (auto& row : snapshot["samples"]) {
        const auto key = row.value("workload", std::string()) + ":" +
                         row.value("script_fingerprint", std::string()) + ":" +
                         row.value("machine", std::string());
        if (++counts[key] <= 100)
            rows.push_back(std::move(row));
    }
    Json baseline = Json::object();
    std::string baseline_error;
    bool baseline_valid = false;
    try {
        if (options_.baseline_file.empty() || !std::filesystem::exists(options_.baseline_file)) {
            baseline_error = "baseline_missing";
        } else {
            if (std::filesystem::file_size(options_.baseline_file) > 16 * 1024 * 1024)
                throw std::runtime_error("baseline index too large");
            std::ifstream in(options_.baseline_file);
            const auto source = Json::parse(in);
            if (!source.is_object() || source.value("version", Json()) != 1 ||
                !source.contains("entries") || !source["entries"].is_array() ||
                source["entries"].size() > 50000)
                throw std::runtime_error("invalid baseline index");
            for (const auto& entry : source["entries"]) {
                if (!entry.is_object() || !timingDigest(entry, "workload_key") ||
                    !timingDigest(entry, "sha256") ||
                    !timingInteger(entry, "elapsed_ms", 1, 604800000) ||
                    entry.value("scope", Json()) != "vivado_stage_elapsed_v1")
                    throw std::runtime_error("invalid baseline entry");
                const auto key = entry["workload_key"].get<std::string>();
                if (baseline.contains(key))
                    throw std::runtime_error("duplicate baseline workload");
                baseline[key] = entry;
            }
            baseline_valid = true;
        }
    } catch (const std::exception& e) {
        baseline_error = std::string("baseline_invalid: ") + e.what();
    }
    std::map<std::string, std::vector<Json>> profiles;
    std::map<std::string, std::vector<double>> ratio_values;
    std::map<std::string, std::set<std::string>> ratio_cases;
    for (auto& row : rows) {
        if (baseline_valid && row.value("trainable", false)) {
            const auto workload = row.value("workload", std::string());
            if (baseline.contains(workload)) {
                const auto key = row.value("stage_key", std::string()) + ":" +
                                 row.value("machine", std::string());
                ratio_values[key].push_back(row["command_wall_ms"].get<double>() /
                                            baseline[workload]["elapsed_ms"].get<double>());
                ratio_cases[key].insert(workload);
            }
        }
        profiles[row.value("workload", std::string())].push_back(std::move(row));
    }
    std::map<std::string, std::pair<double, size_t>> stage_ratios;
    for (const auto& entry : ratio_values)
        if (entry.second.size() >= 30 && ratio_cases[entry.first].size() >= 10)
            stage_ratios[entry.first] = {p95(entry.second), entry.second.size()};
    std::lock_guard<std::mutex> lock(mutex_);
    profiles_.swap(profiles);
    stage_ratios_.swap(stage_ratios);
    for (const auto& key : snapshot["guards"])
        guards_.insert(key.get<std::string>());
    if (baseline_valid)
        baselines_.swap(baseline);
    baseline_error_ = baseline_error;
    ++generation_;
}

Json TimeoutPolicy::decide(const Task& task, const Worker& worker) const {
    const auto& payload = task.getPayload();
    if (!payload.is_object() || !payload.contains("timeout_seconds") ||
        !payload["timeout_seconds"].is_number_integer())
        return nullptr;
    const int requested = payload["timeout_seconds"].get<int>();
    const auto workload = timeoutWorkloadKey(payload);
    std::lock_guard<std::mutex> lock(mutex_);
    Json result = {{"version", 1},
        {"decision_id", task.getRunId()},
        {"mode", options_.mode},
        {"requested_seconds", requested},
        {"effective_seconds", requested},
        {"recommended_seconds", nullptr},
        {"source", "fixed"},
        {"confidence", "insufficient"},
        {"sample_count", 0},
        {"machine_sample_count", 0},
        {"profile_generation", generation_},
        {"machine_id", worker.getMachineId()},
        {"applied", false},
        {"fallback_reason", "insufficient_data"},
        {"baseline_state", baseline_error_}};
    if (options_.mode == "off" || workload.empty()) {
        result["fallback_reason"] = options_.mode == "off" ? "disabled" : "unsupported_payload";
        return result;
    }
    static const std::vector<Json> empty;
    const auto found = profiles_.find(workload);
    const auto& samples = found == profiles_.end() ? empty : found->second;
    std::string fingerprint;
    for (const auto& row : samples) {
        if (row.value("workload", std::string()) == workload &&
            row.value("machine", std::string()) == worker.getMachineId()) {
            fingerprint = row.value("script_fingerprint", std::string());
            break;
        }
    }
    if (fingerprint.empty()) {
        for (const auto& row : samples)
            if (row.value("workload", std::string()) == workload) {
                fingerprint = row.value("script_fingerprint", std::string());
                break;
            }
    }
    std::vector<double> all, machine;
    std::set<std::string> batches;
    int revision_count = 0, observe_ok = 0;
    bool observe_exceeded = false, ambiguous_archive = false;
    std::string archive;
    double maximum = 0;
    for (const auto& row : samples) {
        if (row.value("workload", std::string()) != workload ||
            row.value("script_fingerprint", std::string()) != fingerprint)
            continue;
        if (row.value("observe_exceeded", false))
            observe_exceeded = true;
        if (!row.value("trainable", false))
            continue;
        const double elapsed = row["command_wall_ms"].get<double>() / 1000.0;
        all.push_back(elapsed);
        maximum = std::max(maximum, elapsed);
        if (row.value("machine", std::string()) == worker.getMachineId()) {
            machine.push_back(elapsed);
            if (row.value("observe_ok", false))
                ++observe_ok;
            const auto b = row.value("batch", std::string());
            if (!b.empty() && row.value("observe_ok", false))
                batches.insert(b);
        }
        if (row.value("revision", std::string()) == task.getRevision()) {
            ++revision_count;
            const auto a = row.value("archive_sha256", std::string());
            if (!archive.empty() && archive != a)
                ambiguous_archive = true;
            archive = a;
        }
    }
    double prediction = std::max(p95(all), p95(machine));
    result["sample_count"] = all.size();
    result["machine_sample_count"] = machine.size();
    result["version_sample_count"] = revision_count;
    result["observe_pass_count"] = observe_ok;
    result["observe_batch_count"] = batches.size();
    result["expected_fingerprint"] = fingerprint;
    result["expected_archive_sha256"] = archive;
    // Stage ratios are paired with each case's own validated baseline.
    const auto ratio = stage_ratios_.find(stageKey(payload) + ":" + worker.getMachineId());
    if (baselines_.contains(workload) && baseline_error_.empty() && ratio != stage_ratios_.end()) {
        const auto& own = baselines_[workload];
        const double estimate = own["elapsed_ms"].get<double>() / 1000.0 * ratio->second.first;
        prediction = std::max(prediction, estimate);
        result["baseline_sha256"] = own["sha256"];
        result["baseline_elapsed_ms"] = own["elapsed_ms"];
        result["stage_ratio"] = ratio->second.first;
        result["source"] = all.empty() ? "vivado_stage_ratio" : "history_and_vivado";
    }
    if (prediction <= 0)
        return result;
    if (result["source"] == "fixed")
        result["source"] = "history";
    const int proposed =
        static_cast<int>(std::ceil(std::max(prediction * options_.factor + options_.margin_seconds,
            maximum * 1.25 + options_.margin_seconds)));
    const int recommendation = std::max(options_.min_seconds, proposed);
    result["recommended_seconds"] = recommendation;
    result["p95_seconds"] = p95(all);
    result["max_success_seconds"] = maximum;
    result["safety_factor"] = options_.factor;
    result["margin_seconds"] = options_.margin_seconds;
    result["confidence"] = "low";
    const bool sufficient = all.size() >= static_cast<size_t>(options_.min_success) &&
                            machine.size() >= 3 && revision_count >= 3 && !ambiguous_archive;
    if (sufficient)
        result["confidence"] = "history_ready";
    std::string fallback;
    if (recommendation > std::min(requested, options_.max_seconds))
        fallback = "estimate_exceeds_cap";
    else if (!sufficient)
        fallback = "insufficient_samples";
    else if (degraded_ ||
             guards_.count(timeoutGuardKey(workload, task.getRevision(), worker.getMachineId())))
        fallback = "auto_timeout_guard";
    else if (observe_exceeded || observe_ok < 10 || batches.size() < 3)
        fallback = "observation_not_qualified";
    else if (!worker.supportsTimeout())
        fallback = "worker_not_supported";
    else if (worker.getRunPolicy().value("resource_status", std::string()) != "fresh" ||
             !worker.getRunPolicy().contains("resources"))
        fallback = "resource_not_verified";
    else {
        const auto& resources = worker.getRunPolicy()["resources"];
        const double total = resources.value("mem_total_bytes", 0.0);
        const double available = resources.value("mem_available_bytes", 0.0);
        if (total <= 0 || available / total < 0.20 ||
            resources.value("swap_in_bytes_per_sec", 0.0) +
                    resources.value("swap_out_bytes_per_sec", 0.0) >
                1048576.0)
            fallback = "resource_pressure";
    }
    if (options_.mode == "enforce" && task.getTimeoutMode() == "auto" && fallback.empty()) {
        result["effective_seconds"] = recommendation;
        result["applied"] = recommendation < requested;
        result["confidence"] = "qualified";
        result["fallback_reason"] =
            result["applied"] == true ? "auto_applied" : "fixed_not_shorter";
    } else {
        result["fallback_reason"] = !fallback.empty()            ? fallback
                                    : options_.mode == "observe" ? "observe_only"
                                                                 : "fixed_requested";
    }
    return result;
}

void TimeoutPolicy::completed(const Task& task) noexcept {
    try {
        if (task.getStatus() != TaskStatus::TIMEOUT || !task.getTimeoutDecision().is_object() ||
            !task.getTimeoutDecision().value("applied", false))
            return;
        const auto& summary = task.getExecutionSummary();
        if (!summary.is_object() || !summary.contains("timing") ||
            summary["timing"].value("fallback_reason", std::string()) != "auto_applied")
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        guards_.insert(timeoutGuardKey(timeoutWorkloadKey(task.getPayload()),
            task.getRevision(),
            task.getTimeoutDecision().value("machine_id", std::string())));
    } catch (...) {
        degraded_ = true;
    }
}
} // namespace GCRP
