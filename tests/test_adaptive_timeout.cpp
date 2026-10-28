#include "scheduler/TimeoutPolicy.h"
#include "persistence/StateStore.h"
#include "worker/WorkerManager.h"
#include "config/Config.h"
#include "task/ExecutionTiming.h"
#include "protocol/dto/TaskDTO.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace GCRP;
using Json = nlohmann::json;

static void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

static Json payload() {
    return {{"spec_version", 2},
        {"case", "family/part/case+name/run.tcl"},
        {"flow", "place_design"},
        {"flow_config", {{"place_design", 1}, {"checksum_cmp", 1}}},
        {"timeout_seconds", 5400}};
}

static Json summary(int elapsed, const std::string& machine = "bench") {
    return {{"status", "SUCCEEDED"},
        {"case_status", "PASS"},
        {"reason", "pass"},
        {"raw_exit_code", 0},
        {"timing",
            {{"version", 1},
                {"scope", "single_case_runner_wall_v1"},
                {"prepare_wall_ms", 9000},
                {"command_wall_ms", elapsed},
                {"reap_wall_ms", 0},
                {"finalize_wall_ms", 100},
                {"effective_timeout_seconds", 5400},
                {"script_fingerprint", std::string(64, 'a')},
                {"archive_sha256", std::string(64, 'b')},
                {"supervisor_reason", "command_exit"},
                {"fallback_reason", "observe"},
                {"machine_id", machine}}}};
}

static Task sample(StateStore& store, uint64_t id, int elapsed, bool pass = true) {
    Task t(id, TaskType::REGRESSION, "place_design", "19350");
    auto p = payload();
    p["context"] = {{"batch_id", "batch-" + std::to_string(id % 3)}};
    t.setPayload(p);
    t.setTimeoutMode("auto");
    t.transitionTo(TaskStatus::QUEUED);
    t.setWorkerId("bench-slot1");
    t.setRunId("sample-" + std::to_string(id));
    t.setTimeoutDecision({{"mode", "observe"},
        {"recommended_seconds", 600},
        {"effective_seconds", 5400},
        {"machine_id", "bench"},
        {"applied", false}});
    t.transitionTo(TaskStatus::ASSIGNED);
    store.save({t});
    t.transitionTo(TaskStatus::RUNNING);
    auto s = summary(elapsed);
    if (!pass) {
        s["case_status"] = "FAIL";
        s["reason"] = "business_failed";
    }
    t.setExecutionSummary(s);
    t.transitionTo(pass ? TaskStatus::SUCCEEDED : TaskStatus::FAILED);
    store.save({t});
    return t;
}

int main() {
    try {
        char dir[] = "/tmp/gcrp-timeout-unit-XXXXXX";
        require(mkdtemp(dir) != nullptr, "temp directory");
        const std::string base = dir, cfg = base + "/server.conf", db = base + "/state.db";
        auto configure = [&](const std::string& mode) {
            std::ofstream out(cfg);
            out << "timeout.mode=" << mode
                << "\npolicy.enabled=false\nserver.resource_policy_file=\n"
                << "timeout.baseline_index_file=" << base << "/missing-index.json\n";
            out.close();
            require(Config::instance().load(cfg), "config load");
        };
        configure("observe");
        {
            StateStore store(db);
            WorkerManager workers(&store);
            require(
                workers.registerWorker("bench-slot1", "bench", 1, "bench", "slot1", 1), "register");
            workers.setTimeoutSupported("bench-slot1", true);
            Json r = {{"version", 1},
                {"machine_id", "bench"},
                {"slot_count", 1},
                {"agent_id", std::string(32, 'a')},
                {"sequence", uint64_t(1)},
                {"valid", true},
                {"age_ms", 0},
                {"cpu_percent", 99.0},
                {"mem_total_bytes", uint64_t(100000000000ULL)},
                {"mem_available_bytes", uint64_t(70000000000ULL)},
                {"swap_in_bytes_per_sec", 0.0},
                {"swap_out_bytes_per_sec", 0.0},
                {"disk_total_bytes", uint64_t(100000000000ULL)},
                {"disk_free_bytes", uint64_t(70000000000ULL)},
                {"psi_memory_some_avg60", nullptr},
                {"psi_io_some_avg60", nullptr}};
            workers.reportResources("bench-slot1", r);
            auto ws = workers.getAvailableWorkers();
            require(ws.size() == 1, "available Worker");
            Task request(100, TaskType::REGRESSION, "place_design", "19350");
            request.setPayload(payload());
            request.setTimeoutMode("auto");
            request.setRunId("request-100");
            TimeoutPolicy observe(store);
            auto cold = observe.decide(request, ws[0]);
            require(cold["recommended_seconds"].is_null() && cold["effective_seconds"] == 5400,
                "cold fallback");
            auto first = sample(store, 1, 240000);
            for (int i = 2; i <= 12; ++i)
                sample(store, i, 240000);
            sample(store, 13, 1000000, false);
            observe.refresh();
            auto decision = observe.decide(request, ws[0]);
            require(decision["sample_count"] == 12, "FAIL excluded from model");
            require(decision["recommended_seconds"] == 540, "P95 recommendation");
            require(decision["effective_seconds"] == 5400 && decision["applied"] == false,
                "observe keeps budget");
            store.save({first}); // repeated result is one run sample.
            require(store.runtimeSnapshot(30)["samples"].size() == 13, "idempotent samples");
            auto restored = store.loadTasks(1);
            require(restored.size() == 1, "load task");
            require(restored[0].task.getTimeoutMode() == "auto", "mode persistence");
            require(restored[0].task.getTimeoutDecision()["recommended_seconds"] == 600,
                "decision persistence");
            auto other = payload();
            other["flow_config"]["checksum_cmp"] = 0;
            request.setPayload(other);
            require(observe.decide(request, ws[0])["recommended_seconds"].is_null(),
                "flow config isolation");
            request.setPayload(payload());
            request.setRunId("request-100");
            configure("enforce");
            TimeoutPolicy enforce(store);
            auto automatic = enforce.decide(request, ws[0]);
            require(automatic["applied"] == true && automatic["effective_seconds"] == 540,
                "qualified auto");
            auto changed = request;
            changed.setTimeoutMode("fixed");
            require(
                enforce.decide(changed, ws[0])["effective_seconds"] == 5400, "manual precedence");
            workers.setTimeoutSupported("bench-slot1", false);
            require(enforce.decide(request, workers.getAvailableWorkers()[0])["applied"] == false,
                "old Worker fallback");
            workers.setTimeoutSupported("bench-slot1", true);
            auto shortBudget = payload();
            shortBudget["timeout_seconds"] = 100;
            request.setPayload(shortBudget);
            require(enforce.decide(request, ws[0])["effective_seconds"] == 100,
                "never increase requested cap");
            request.setPayload(payload());
            Task timeout(20, TaskType::REGRESSION, "place_design", "19350");
            timeout.setPayload(payload());
            timeout.setTimeoutMode("auto");
            timeout.transitionTo(TaskStatus::QUEUED);
            timeout.setWorkerId("bench-slot1");
            timeout.setRunId("timeout-20");
            automatic["decision_id"] = "timeout-20";
            timeout.setTimeoutDecision(automatic);
            timeout.transitionTo(TaskStatus::ASSIGNED);
            store.save({timeout});
            timeout.transitionTo(TaskStatus::RUNNING);
            auto s = summary(540000);
            s["case_status"] = "TIMEOUT";
            s["reason"] = "timeout";
            s["timing"]["fallback_reason"] = "auto_applied";
            s["timing"]["supervisor_reason"] = "timeout";
            s["timing"]["effective_timeout_seconds"] = 540;
            timeout.setExecutionSummary(s);
            timeout.transitionTo(TaskStatus::TIMEOUT);
            store.save({timeout});
            enforce.completed(timeout);
            require(enforce.decide(request, ws[0])["fallback_reason"] == "auto_timeout_guard",
                "immediate guard");
            TimeoutPolicy restarted(store);
            require(restarted.decide(request, ws[0])["fallback_reason"] == "auto_timeout_guard",
                "durable guard");
            auto invalid = summary(1)["timing"];
            invalid["command_wall_ms"] = -1;
            require(cleanExecutionTiming(invalid).is_null(), "invalid timing rejected");
            auto wire = Json{{"task_type", "REGRESSION"},
                {"target", "t"},
                {"revision", "19350"},
                {"payload", payload()}};
            Protocol::DTO::SubmitTaskRequest dto;
            require(Protocol::DTO::fromJson(Json::parse(wire.dump()), dto) &&
                        dto.timeout_mode == "fixed",
                "legacy submit");
            wire["timeout_mode"] = "auto";
            require(Protocol::DTO::fromJson(Json::parse(wire.dump()), dto) &&
                        dto.timeout_mode == "auto",
                "auto submit");
            wire["timeout_mode"] = "anything";
            require(
                !Protocol::DTO::fromJson(Json::parse(wire.dump()), dto), "invalid mode rejected");
            std::cout
                << "PASS: timing validation, observed P95, config isolation, persistence, auto gates, guards, DTO compatibility\n";
        }
        std::filesystem::remove_all(base);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
