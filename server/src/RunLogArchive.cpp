#include "log/RunLogArchive.h"
#include "Sha256.h"
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <chrono>
#include <stdexcept>
#include <set>
#include "logger/Logger.h"
#include "logger/LogModule.h"
#include "Logging.h"

namespace GCRP {
namespace {
constexpr uint64_t kFileLimit = 1024ULL * 1024 * 1024;
constexpr size_t kChunkLimit = 16384;
constexpr std::array<const char*, 9> names{{"stdout.log",
    "stderr.log",
    "result.env",
    "result.json",
    "flow_config",
    "evidence.run",
    "runner.log",
    "case-run.log",
    "evidence.json"}};

int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string decode(const std::string& value) {
    if (value.size() > 2 * kChunkLimit || value.size() % 2)
        throw std::invalid_argument("invalid log chunk length");
    std::string out(value.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i) {
        const int a = nibble(value[2 * i]), b = nibble(value[2 * i + 1]);
        if (a < 0 || b < 0)
            throw std::invalid_argument("invalid log hex");
        out[i] = static_cast<char>((a << 4) | b);
    }
    return out;
}

std::string encode(const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(value.size() * 2, '\0');
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        out[2 * i] = digits[c >> 4];
        out[2 * i + 1] = digits[c & 15];
    }
    return out;
}

void writeAll(int fd, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("log write failed");
        offset += static_cast<size_t>(count);
    }
    if (::fsync(fd) != 0)
        throw std::runtime_error("log sync failed");
}

struct Fd {
    int value;

    explicit Fd(int fd)
        : value(fd) {
    }

    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
};

void syncDirectory(const std::filesystem::path& path) {
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0 || ::fsync(directory.value) != 0)
        throw std::runtime_error("cannot sync log directory");
}

nlohmann::json metadata(const std::filesystem::path& path) {
    if (std::filesystem::is_symlink(path))
        throw std::runtime_error("unsafe archive metadata");
    if (!std::filesystem::exists(path))
        return nullptr;
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8192)
        throw std::runtime_error("invalid archive metadata");
    std::ifstream stream(path);
    return nlohmann::json::parse(stream);
}

void saveMetadata(const std::filesystem::path& path, const nlohmann::json& value) {
    const auto temporary = path.string() + ".tmp";
    Fd fd(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.value < 0)
        throw std::runtime_error("cannot save archive state");
    writeAll(fd.value, value.dump());
    std::filesystem::rename(temporary, path);
    syncDirectory(path.parent_path());
}
} // namespace

RunLogArchive::RunLogArchive(std::filesystem::path root,
    std::filesystem::path public_root,
    std::string copy_time,
    ExportInfo export_info)
    : root_(std::move(root))
    , public_root_(std::move(public_root))
    , export_info_(std::move(export_info)) {
    if (root_.empty())
        throw std::invalid_argument("log archive root is empty");
    std::filesystem::create_directories(root_);
    if (std::filesystem::is_symlink(root_) || !std::filesystem::is_directory(root_))
        throw std::runtime_error("unsafe log archive root");
    if (!public_root_.empty()) {
        if (copy_time.size() != 5 || copy_time[2] != ':' ||
            copy_time.find_first_not_of("0123456789:") != std::string::npos)
            throw std::invalid_argument("public copy time must be HH:MM");
        for (int i : {0, 1, 3, 4})
            if (copy_time[i] < '0' || copy_time[i] > '9')
                throw std::invalid_argument("public copy time must be HH:MM");
        const int hour = std::stoi(copy_time.substr(0, 2)), minute = std::stoi(copy_time.substr(3));
        if (hour > 23 || minute > 59 || !export_info_)
            throw std::invalid_argument("invalid public copy configuration");
        copy_minute_ = hour * 60 + minute;
        root_ = std::filesystem::weakly_canonical(root_);
        public_root_ = std::filesystem::absolute(public_root_).lexically_normal();
        const auto destination = std::filesystem::weakly_canonical(public_root_);
        auto within = [](const auto& a, const auto& b) {
            auto relative = a.lexically_relative(b);
            return !relative.empty() && *relative.begin() != "..";
        };
        if (within(destination, root_) || within(root_, destination))
            throw std::invalid_argument("public logs must not overlap the original archive");
        public_thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(public_mutex_);
            std::string last_error;
            while (!public_stop_) {
                lock.unlock();
                try {
                    const auto now = std::time(nullptr);
                    std::tm local{};
                    localtime_r(&now, &local);
                    if (local.tm_hour * 60 + local.tm_min < copy_minute_) {
                        --local.tm_mday;
                        std::mktime(&local);
                    }
                    char day[16]{};
                    std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
                    const auto schedule = publicReceipt(
                        {{"relative", "@daily-schedule/" + std::to_string(copy_minute_)}});
                    const auto previous = metadata(schedule);
                    if (!previous.is_object() || previous.value("day", std::string()) != day) {
                        copyPublicLogs();
                        if (!public_stop_) {
                            std::filesystem::create_directories(schedule.parent_path());
                            saveMetadata(schedule, {{"day", day}});
                        }
                    }
                    last_error.clear();
                } catch (const std::exception& error) {
                    if (last_error != error.what())
                        LOG_WARN(LogModule::SERVER,
                            std::string("public log copy pending; original archive preserved: ") +
                                error.what());
                    last_error = error.what();
                }
                lock.lock();
                public_cv_.wait_for(lock, std::chrono::seconds(30), [this] {
                    return public_stop_.load();
                });
            }
        });
    }
}

RunLogArchive::~RunLogArchive() {
    public_stop_ = true;
    public_cv_.notify_all();
    if (public_thread_.joinable())
        public_thread_.join();
}

namespace {
nlohmann::json verifiedSet(const std::filesystem::path& run) {
    auto state = metadata(run / ".archive.json");
    if (!state.is_object() || state.value("expired", false) || !state.contains("files") ||
        !state["files"].is_array() || state["files"].empty())
        return nullptr;
    auto files = nlohmann::json::object();
    for (const auto& item : state["files"]) {
        if (!item.is_string() || !RunLogArchive::validName(item.get<std::string>()))
            throw std::runtime_error("invalid archive file list");
        const auto name = item.get<std::string>();
        const auto path = run / name;
        if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
            std::filesystem::is_symlink(path.string() + ".done") ||
            !std::filesystem::is_regular_file(path.string() + ".done"))
            return nullptr;
        auto receipt = metadata(path.string() + ".verified");
        if (!receipt.is_object() ||
            receipt.value("size", uint64_t(-1)) != std::filesystem::file_size(path))
            return nullptr;
        const auto sha = receipt.value("sha256", std::string());
        if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos)
            return nullptr;
        files[name] = {{"size", receipt["size"]}, {"sha256", sha}};
    }
    return files;
}

void safePublicDirectory(const std::filesystem::path& path) {
    for (auto p = path; !p.empty();) {
        if (std::filesystem::is_symlink(p))
            throw std::runtime_error("symlink in public log directory");
        const auto parent = p.parent_path();
        if (parent == p)
            break;
        p = parent;
    }
    std::filesystem::create_directories(path);
}

std::string publicDigest(const std::filesystem::path& path, const std::atomic<bool>& stop) {
    Fd in(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (in.value < 0)
        throw std::runtime_error("cannot read public log");
    Sha256 digest;
    std::array<char, 65536> bytes{};
    uint64_t total = 0;
    while (!stop) {
        const auto n = ::read(in.value, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            throw std::runtime_error("public log read failed");
        if (!n)
            return digest.hex();
        total += n;
        if (total > kFileLimit)
            throw std::runtime_error("public log exceeds limit");
        digest.update(bytes.data(), n);
    }
    throw std::runtime_error("public log copy interrupted");
}
} // namespace

nlohmann::json RunLogArchive::publicIdentity(const std::filesystem::path& run) const {
    if (!export_info_)
        throw std::runtime_error("public export identity resolver missing");
    auto info =
        export_info_(std::stoull(run.parent_path().filename().string()), run.filename().string());
    if (!info.is_object())
        throw std::runtime_error("completed task identity unavailable: " + run.string());
    const auto revision = info.at("revision").get<std::string>();
    const auto& payload = info.at("payload");
    const auto stage = payload.contains("context") && payload["context"].contains("template")
                           ? payload["context"]["template"].get<std::string>()
                           : payload.at("flow").get<std::string>();
    auto component = [](const std::string& value) {
        return !value.empty() && value != "." && value != ".." &&
               value.find_first_of("/\\:\0", 0, 4) == std::string::npos;
    };
    if (!component(revision) || !component(stage))
        throw std::runtime_error("unsafe export version/template");
    const auto testcase = payload.at("case").get<std::string>();
    const std::filesystem::path path(testcase);
    if (path.is_absolute() || path.filename() != "run.tcl" ||
        testcase.find('\\') != std::string::npos)
        throw std::runtime_error("unsafe export testcase");
    size_t depth = 0;
    for (const auto& part : path) {
        if (!component(part.string()))
            throw std::runtime_error("unsafe testcase component");
        ++depth;
    }
    if (depth < 4)
        throw std::runtime_error("export testcase requires family/part/testcase/run.tcl");
    info["relative"] =
        (std::filesystem::path(revision) / stage / path.parent_path() / "run").generic_string();
    return info;
}

std::filesystem::path RunLogArchive::publicReceipt(const nlohmann::json& identity) const {
    if (std::filesystem::is_symlink(root_ / ".public-exports"))
        throw std::runtime_error("unsafe export receipt directory");
    const auto key = public_root_.string() + "\n" + identity.at("relative").get<std::string>();
    Sha256 hash;
    hash.update(key.data(), key.size());
    return root_ / ".public-exports" / (hash.hex() + ".json");
}

bool RunLogArchive::publicComplete(const std::filesystem::path& run) const {
    if (public_root_.empty())
        return true;
    const auto expected = verifiedSet(run);
    if (expected.is_null())
        return false;
    if (!expected.contains("evidence.run"))
        return true;
    const auto identity = publicIdentity(run), receipt = metadata(publicReceipt(identity));
    const auto path = public_root_ / identity["relative"].get<std::string>();
    if (!receipt.is_object() || !receipt.contains("order") ||
        receipt["order"] < identity["order"] || std::filesystem::is_symlink(path) ||
        !std::filesystem::is_regular_file(path))
        return false;
    if (receipt["order"] == identity["order"] &&
        receipt.value("sha256", std::string()) != expected["evidence.run"]["sha256"])
        return false;
    return std::filesystem::file_size(path) == receipt.at("size").get<uint64_t>();
}

void RunLogArchive::copyPublicLogs() {
    safePublicDirectory(public_root_);
    std::string first_error;
    for (const auto& task : std::filesystem::directory_iterator(root_)) {
        if (public_stop_)
            return;
        const auto id = task.path().filename().string();
        if (task.is_symlink() || !task.is_directory() || id.empty() ||
            id.find_first_not_of("0123456789") != std::string::npos)
            continue;
        for (const auto& run : std::filesystem::directory_iterator(task.path())) {
            if (public_stop_)
                return;
            try {
                if (run.is_symlink() || !run.is_directory() ||
                    !validRunId(run.path().filename().string()))
                    continue;
                const auto files = verifiedSet(run.path());
                if (files.is_null())
                    continue;
                if (!files.contains("evidence.run")) {
                    const auto missing = publicReceipt(
                        {{"relative", "@missing/" + id + "/" + run.path().filename().string()}});
                    if (!std::filesystem::exists(missing)) {
                        LOG_WARN(LogModule::SERVER,
                            "public export skipped: original run was not captured, task=" + id +
                                " run=" + run.path().filename().string());
                        std::filesystem::create_directories(missing.parent_path());
                        saveMetadata(missing, {{"reason", "original_run_not_captured"}});
                    }
                    continue;
                }
                if (publicComplete(run.path()))
                    continue;
                const auto identity = publicIdentity(run.path());
                const auto destination =
                    (public_root_ / identity["relative"].get<std::string>()).parent_path();
                safePublicDirectory(destination);
                for (auto it = files.begin(); it != files.end(); ++it) {
                    if (it.key() != "evidence.run")
                        continue;
                    const auto output = destination / "run";
                    const auto sha = it.value()["sha256"].get<std::string>();
                    if (std::filesystem::exists(output) || std::filesystem::is_symlink(output)) {
                        if (std::filesystem::is_symlink(output) ||
                            !std::filesystem::is_regular_file(output))
                            throw std::runtime_error("unsafe public run: " + output.string());
                        const auto prior = metadata(publicReceipt(identity));
                        if (prior.is_object() && prior.at("order") > identity["order"])
                            continue;
                        if (!prior.is_object() && publicDigest(output, public_stop_) != sha)
                            throw std::runtime_error(
                                "unmanaged public run conflict: " + output.string());
                    }
                    const auto temporary = output.string() + ".copy-" + std::to_string(::getpid());
                    Fd out(::open(temporary.c_str(),
                        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                        0644));
                    if (out.value < 0)
                        throw std::runtime_error(
                            "cannot create public log temporary file: " + temporary);
                    try {
                        Fd in(::open(
                            (run.path() / it.key()).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
                        if (in.value < 0)
                            throw std::runtime_error("archive source unavailable");
                        Sha256 digest;
                        std::array<char, 65536> buffer{};
                        uint64_t total = 0;
                        for (;;) {
                            if (public_stop_)
                                throw std::runtime_error("public copy interrupted");
                            auto n = ::read(in.value, buffer.data(), buffer.size());
                            if (n < 0 && errno == EINTR)
                                continue;
                            if (n < 0)
                                throw std::runtime_error("archive read failed");
                            if (!n)
                                break;
                            total += n;
                            if (total > kFileLimit)
                                throw std::runtime_error("archive exceeds copy limit");
                            digest.update(buffer.data(), n);
                            size_t offset = 0;
                            while (offset < size_t(n)) {
                                auto written =
                                    ::write(out.value, buffer.data() + offset, n - offset);
                                if (written < 0 && errno == EINTR)
                                    continue;
                                if (written <= 0)
                                    throw std::runtime_error("public log write failed");
                                offset += written;
                            }
                        }
                        if (total != it.value()["size"].get<uint64_t>() || digest.hex() != sha)
                            throw std::runtime_error("public log checksum mismatch");
                        if (::fsync(out.value) != 0)
                            throw std::runtime_error("public log sync failed");
                        if (::rename(temporary.c_str(), output.c_str()) != 0)
                            throw std::runtime_error("public log publish failed");
                        ::unlink(temporary.c_str());
                        syncDirectory(destination);
                    } catch (...) {
                        ::unlink(temporary.c_str());
                        throw;
                    }
                }
                const auto receipt = publicReceipt(identity);
                std::filesystem::create_directories(receipt.parent_path());
                const auto prior = metadata(receipt);
                if (!prior.is_object() || prior.at("order") <= identity["order"])
                    saveMetadata(receipt,
                        {{"order", identity["order"]},
                            {"sha256", files["evidence.run"]["sha256"]},
                            {"size", files["evidence.run"]["size"]},
                            {"relative", identity["relative"]}});
            } catch (const std::exception& error) {
                if (first_error.empty())
                    first_error = error.what();
            }
        }
    }
    if (!first_error.empty())
        throw std::runtime_error(first_error);
}

bool RunLogArchive::validRunId(const std::string& value) {
    if (value.empty() || value.size() > 128)
        return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_'))
            return false;
    return true;
}

bool RunLogArchive::validName(const std::string& value) {
    for (const char* name : names)
        if (value == name)
            return true;
    return false;
}

std::filesystem::path RunLogArchive::file(
    uint64_t task_id, const std::string& run_id, const std::string& name) const {
    if (!task_id || !validRunId(run_id) || !validName(name))
        throw std::invalid_argument("invalid log identity");
    auto parent = root_ / std::to_string(task_id);
    auto directory = parent / run_id;
    if (std::filesystem::is_symlink(parent) || std::filesystem::is_symlink(directory))
        throw std::runtime_error("unsafe log directory");
    return directory / name;
}

nlohmann::json RunLogArchive::append(uint64_t task_id,
    const std::string& run_id,
    const std::string& name,
    uint64_t offset,
    const std::string& hex,
    bool complete,
    const std::string& expected_sha256) {
    const auto bytes = decode(hex);
    if (offset > kFileLimit || bytes.size() > kFileLimit - offset)
        throw std::invalid_argument("log file limit exceeded");
    std::lock_guard<std::mutex> lock(mutex_);
    auto path = file(task_id, run_id, name);
    const auto task_dir = path.parent_path().parent_path();
    const bool new_task_dir = !std::filesystem::exists(task_dir);
    const bool new_run_dir = !std::filesystem::exists(path.parent_path());
    std::filesystem::create_directories(path.parent_path());
    if (new_task_dir)
        syncDirectory(root_);
    if (new_run_dir)
        syncDirectory(task_dir);
    if (std::filesystem::is_symlink(path) || std::filesystem::is_symlink(path.string() + ".done"))
        throw std::runtime_error("unsafe log file");
    const bool new_file = !std::filesystem::exists(path);
    Fd fd(::open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.value < 0)
        throw std::runtime_error("cannot open log file");
    const uint64_t size = std::filesystem::file_size(path);
    const bool done = std::filesystem::exists(path.string() + ".done");
    if (size > kFileLimit)
        throw std::runtime_error("log file limit exceeded");
    if (!expected_sha256.empty()) {
        if (expected_sha256.size() != 64 ||
            expected_sha256.find_first_not_of("0123456789abcdef") != std::string::npos ||
            !complete || !bytes.empty() || !done || offset != size)
            throw std::invalid_argument("invalid archive verification request");
        Sha256 digest;
        std::array<char, 65536> block{};
        uint64_t at = 0;
        while (at < size) {
            auto count = ::pread(fd.value,
                block.data(),
                std::min<uint64_t>(block.size(), size - at),
                static_cast<off_t>(at));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("cannot verify archived file");
            digest.update(block.data(), static_cast<size_t>(count));
            at += static_cast<uint64_t>(count);
        }
        if (digest.hex() != expected_sha256)
            throw std::invalid_argument("archive SHA-256 mismatch; preserve Worker source");
        // Persist the digest, not just the transient ACK, for later GUI/download checks.
        saveMetadata(path.string() + ".verified", {{"sha256", expected_sha256}, {"size", size}});
        Fd marker(::open((path.string() + ".done").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (marker.value < 0)
            throw std::runtime_error("verified archive marker missing");
        // A long-offline Worker may verify an older upload. Start its Server
        // retention window again before authorizing removal of the local copy.
        std::filesystem::last_write_time(
            path.string() + ".done", std::filesystem::file_time_type::clock::now());
        if (::fsync(fd.value) != 0 || ::fsync(marker.value) != 0)
            throw std::runtime_error("cannot sync verified archive");
        syncDirectory(path.parent_path());
        return {{"next_offset", size}, {"complete", true}, {"verified_sha256", expected_sha256}};
    }
    if (offset != size || done)
        return {{"next_offset", size}, {"complete", done}};
    writeAll(fd.value, bytes);
    if (new_file)
        syncDirectory(path.parent_path());
    const uint64_t next = size + bytes.size();
    if (complete) {
        Fd marker(::open((path.string() + ".done").c_str(),
            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
            0600));
        if (marker.value < 0)
            throw std::runtime_error("cannot complete log file");
        if (::fsync(marker.value) != 0)
            throw std::runtime_error("cannot sync log marker");
        syncDirectory(path.parent_path());
    }
    return {{"next_offset", next}, {"complete", complete}};
}

nlohmann::json RunLogArchive::list(uint64_t task_id, const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json items = nlohmann::json::array();
    for (const char* name : names) {
        const auto path = file(task_id, run_id, name);
        if (std::filesystem::is_symlink(path))
            continue;
        if (std::filesystem::is_regular_file(path)) {
            const auto receipt = metadata(path.string() + ".verified");
            items.push_back({{"name", name},
                {"size", std::filesystem::file_size(path)},
                {"sha256",
                    receipt.is_object() ? receipt.value("sha256", std::string{}) : std::string{}},
                {"verified",
                    receipt.is_object() &&
                        receipt.value("size", uint64_t(-1)) == std::filesystem::file_size(path)},
                {"complete", std::filesystem::is_regular_file(path.string() + ".done")}});
        }
    }
    const auto receipt =
        metadata(file(task_id, run_id, "stdout.log").parent_path() / ".archive.json");
    std::string state = items.empty() ? "NOT_REPORTED" : "UPLOADED_UNVERIFIED";
    bool all = true, complete = true;
    for (const auto& item : items) {
        all = all && item["verified"].get<bool>();
        complete = complete && item["complete"].get<bool>();
    }
    if (!complete)
        state = "UPLOADING";
    if (receipt.is_object()) {
        if (receipt.value("expired", false))
            state = "EXPIRED";
        else {
            const auto& expected = receipt.at("files");
            all = all && expected.size() == items.size() && !expected.empty();
            for (const auto& item : items)
                all = all &&
                      std::find(expected.begin(), expected.end(), item["name"]) != expected.end();
            if (all)
                state = receipt.value("local_cleaned", false) ? "LOCAL_CLEANED" : "SERVER_VERIFIED";
            else
                state = "UPLOADING";
        }
    }
    return {{"task_id", task_id},
        {"run_id", run_id},
        {"items", items},
        {"archive_state", state},
        {"local_cleanup_confirmed", state == "LOCAL_CLEANED"}};
}

nlohmann::json RunLogArchive::reportState(
    uint64_t task_id, const std::string& run_id, const nlohmann::json& state) {
    if (!state.is_object() || state.size() != 2 || !state.contains("phase") ||
        !state["phase"].is_string() || !state.contains("files") || !state["files"].is_array() ||
        state["files"].empty() || state["files"].size() > names.size())
        throw std::invalid_argument("invalid archive state");
    const auto phase = state["phase"].get<std::string>();
    if (phase != "UPLOADING" && phase != "LOCAL_CLEANED")
        throw std::invalid_argument("invalid archive phase");
    std::set<std::string> expected;
    for (const auto& name : state["files"]) {
        if (!name.is_string() || !validName(name.get<std::string>()) ||
            !expected.insert(name.get<std::string>()).second)
            throw std::invalid_argument("invalid archive file set");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto directory = file(task_id, run_id, "stdout.log").parent_path();
    const auto task_directory = directory.parent_path();
    const bool new_task = !std::filesystem::exists(task_directory),
               new_run = !std::filesystem::exists(directory);
    std::filesystem::create_directories(directory);
    if (new_task)
        syncDirectory(root_);
    if (new_run)
        syncDirectory(task_directory);
    const auto path = directory / ".archive.json";
    auto receipt = metadata(path);
    const auto files = nlohmann::json(expected);
    if (receipt.is_object() && receipt.at("files") != files)
        throw std::invalid_argument("archive file set changed");
    if (phase == "LOCAL_CLEANED") {
        for (const auto& name : expected) {
            const auto source = file(task_id, run_id, name);
            const auto verified = metadata(source.string() + ".verified");
            if (!std::filesystem::is_regular_file(source) || std::filesystem::is_symlink(source) ||
                !std::filesystem::is_regular_file(source.string() + ".done") ||
                !verified.is_object() ||
                verified.value("size", uint64_t(-1)) != std::filesystem::file_size(source))
                throw std::invalid_argument("archive is not fully verified; preserve Worker logs");
        }
    }
    saveMetadata(path,
        {{"files", files},
            {"local_cleaned",
                phase == "LOCAL_CLEANED" ||
                    (receipt.is_object() && receipt.value("local_cleaned", false))},
            {"expired", false}});
    return {{"recorded", true}};
}

nlohmann::json RunLogArchive::read(uint64_t task_id,
    const std::string& run_id,
    const std::string& name,
    uint64_t offset,
    size_t limit) {
    if (!limit || limit > 131072)
        throw std::invalid_argument("invalid log read limit");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = file(task_id, run_id, name);
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.value < 0)
        throw std::invalid_argument("log file not found");
    const auto size = std::filesystem::file_size(path);
    if (offset > size)
        throw std::invalid_argument("log offset beyond end");
    std::string bytes(std::min<uint64_t>(limit, size - offset), '\0');
    const auto count = ::pread(fd.value, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    if (count < 0)
        throw std::runtime_error("log read failed");
    bytes.resize(static_cast<size_t>(count));
    const auto receipt = metadata(path.string() + ".verified");
    return {{"task_id", task_id},
        {"run_id", run_id},
        {"name", name},
        {"offset", offset},
        {"next_offset", offset + bytes.size()},
        {"size", size},
        {"complete", std::filesystem::exists(path.string() + ".done")},
        {"sha256", receipt.is_object() ? receipt.value("sha256", std::string{}) : std::string{}},
        {"data_hex", encode(bytes)}};
}

size_t RunLogArchive::pruneOlderThan(int days) {
    if (days < 1 || days > 3650)
        throw std::invalid_argument("invalid log retention days");
    const auto cutoff =
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * days);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (const auto& task : std::filesystem::directory_iterator(root_)) {
        if (task.is_symlink() || !task.is_directory())
            continue;
        const auto id = task.path().filename().string();
        if (id.empty() || id[0] == '0' || id.find_first_not_of("0123456789") != std::string::npos)
            continue;
        for (const auto& run : std::filesystem::directory_iterator(task.path())) {
            if (run.is_symlink() || !run.is_directory() ||
                !validRunId(run.path().filename().string()))
                continue;
            bool safe = true, has_file = false;
            auto newest = std::filesystem::file_time_type::min();
            for (const auto& entry : std::filesystem::directory_iterator(run.path())) {
                const auto filename = entry.path().filename().string();
                if (filename == ".archive.json")
                    continue;
                const auto base =
                    filename.size() > 9 && filename.substr(filename.size() - 9) == ".verified"
                        ? filename.substr(0, filename.size() - 9)
                    : filename.size() > 5 && filename.substr(filename.size() - 5) == ".done"
                        ? filename.substr(0, filename.size() - 5)
                        : filename;
                if (entry.is_symlink() || !entry.is_regular_file() || !validName(base)) {
                    safe = false;
                    break;
                }
                has_file = true;
                newest = std::max(newest, entry.last_write_time());
            }
            if (!safe || !has_file || newest >= cutoff)
                continue;
            if (!publicComplete(run.path()))
                continue; // Configured public backup must succeed before local retention expires it.
            auto receipt = metadata(run.path() / ".archive.json");
            if (!receipt.is_object())
                receipt = {{"files", nlohmann::json::array()}, {"local_cleaned", false}};
            receipt["expired"] = true;
            saveMetadata(run.path() / ".archive.json", receipt);
            for (const auto& entry : std::filesystem::directory_iterator(run.path()))
                if (entry.path().filename() != ".archive.json")
                    std::filesystem::remove(entry.path());
            ++removed;
        }
        if (std::filesystem::is_empty(task.path()))
            std::filesystem::remove(task.path());
    }
    return removed;
}
} // namespace GCRP
