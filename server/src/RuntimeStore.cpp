#include "persistence/StateStore.h"
#include <sqlite3.h>
#include <chrono>
#include <stdexcept>

namespace GCRP {
nlohmann::json StateStore::runtimeSnapshot(int days) const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    nlohmann::json result = {
        {"samples", nlohmann::json::array()}, {"guards", nlohmann::json::array()}};
    sqlite3_stmt* statement = nullptr;
    const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() -
                        int64_t(days) * 86400000;
    const char* sql =
        "SELECT r.details_json FROM runtime_samples r WHERE r.received_at_ms>=? AND r.task_id NOT IN (SELECT task_id FROM deleted_tasks) ORDER BY r.received_at_ms DESC,r.run_id DESC LIMIT 50000";
    if (sqlite3_prepare_v2(history_db_, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot read runtime samples");
    sqlite3_bind_int64(statement, 1, cutoff);
    int step;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        const auto* data = sqlite3_column_text(statement, 0);
        auto row = nlohmann::json::parse(reinterpret_cast<const char*>(data), nullptr, false);
        if (row.is_object())
            result["samples"].push_back(std::move(row));
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE)
        throw std::runtime_error("incomplete runtime snapshot");
    if (sqlite3_prepare_v2(
            history_db_, "SELECT guard_key FROM timeout_guards", -1, &statement, nullptr) !=
        SQLITE_OK)
        throw std::runtime_error("cannot read timeout guards");
    while ((step = sqlite3_step(statement)) == SQLITE_ROW)
        result["guards"].push_back(
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE)
        throw std::runtime_error("incomplete timeout guards");
    return result;
}

void StateStore::pruneRuntimeSamples(int days) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM runtime_samples WHERE run_id IN (SELECT run_id FROM runtime_samples WHERE received_at_ms<? ORDER BY received_at_ms LIMIT 1000)",
            -1,
            &statement,
            nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot prepare runtime sample retention");
    const auto cutoff = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() -
                        int64_t(days) * 86400000;
    sqlite3_bind_int64(statement, 1, cutoff);
    const int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE)
        throw std::runtime_error("cannot prune runtime samples");
}

void StateStore::clearTimeoutGuard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM timeout_guards WHERE guard_key=?", -1, &s, nullptr) !=
        SQLITE_OK)
        throw std::runtime_error("cannot prepare timeout guard reset");
    sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    const int code = sqlite3_step(s);
    sqlite3_finalize(s);
    if (code != SQLITE_DONE)
        throw std::runtime_error("cannot reset timeout guard");
}
} // namespace GCRP
