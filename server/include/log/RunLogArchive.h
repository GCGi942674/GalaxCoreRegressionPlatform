#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include "nlohmann/json.hpp"

namespace GCRP {
// A bounded, per-run archive. All path components except the configured root
// are validated identities or names from a fixed allowlist.
class RunLogArchive {
public:
    using ExportInfo = std::function<nlohmann::json(uint64_t, const std::string&)>;
    explicit RunLogArchive(std::filesystem::path root,
        std::filesystem::path public_root = {},
        std::string copy_time = "08:00",
        ExportInfo export_info = {});
    ~RunLogArchive();
    static bool validRunId(const std::string& value);
    static bool validName(const std::string& value);
    nlohmann::json append(uint64_t task_id,
        const std::string& run_id,
        const std::string& name,
        uint64_t offset,
        const std::string& hex,
        bool complete,
        const std::string& expected_sha256 = "");
    nlohmann::json list(uint64_t task_id, const std::string& run_id);
    nlohmann::json reportState(
        uint64_t task_id, const std::string& run_id, const nlohmann::json& state);
    nlohmann::json read(uint64_t task_id,
        const std::string& run_id,
        const std::string& name,
        uint64_t offset,
        size_t limit);
    size_t pruneOlderThan(int days);

private:
    std::filesystem::path file(
        uint64_t task_id, const std::string& run_id, const std::string& name) const;
    std::filesystem::path root_;
    std::filesystem::path public_root_;
    ExportInfo export_info_;
    int copy_minute_{480};
    nlohmann::json publicIdentity(const std::filesystem::path& run) const;
    std::filesystem::path publicReceipt(const nlohmann::json& identity) const;
    std::thread public_thread_;
    std::atomic<bool> public_stop_{false};
    std::mutex public_mutex_;
    std::condition_variable public_cv_;
    void copyPublicLogs();
    bool publicComplete(const std::filesystem::path& run) const;
    std::mutex mutex_;
};
} // namespace GCRP
