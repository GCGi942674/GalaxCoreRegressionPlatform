#include "scheduler/ArtifactProtection.h"
#include "persistence/StateStore.h"
#include "Logging.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace GCRP {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

void validRevision(const std::string& revision) {
    if (revision.empty() || revision.size() > 32 ||
        revision.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("ZIP revision must contain 1..32 digits");
}

struct FileLock {
    int fd;

    explicit FileLock(const std::string& file) {
        fs::create_directories(fs::absolute(file).parent_path());
        fd = ::open((file + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0664);
        if (fd < 0)
            throw std::runtime_error(
                "cannot open ZIP protection lock: " + std::string(std::strerror(errno)));

        struct stat info {};

        if (::fstat(fd, &info) || (info.st_uid == ::geteuid() && ::fchmod(fd, 0664))) {
            ::close(fd);
            throw std::runtime_error("cannot set ZIP protection lock permissions");
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            throw std::runtime_error("ZIP protection is busy; retry after cleaner finishes");
        }
    }

    ~FileLock() {
        ::close(fd);
    }
};

std::string read(const std::string& path, bool optional = false) {
    if (optional && !fs::exists(path))
        return {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read ZIP protection file: " + path);
    std::string result;
    char buffer[4096];
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        result.append(buffer, static_cast<size_t>(input.gcount()));
        if (result.size() > 1024 * 1024)
            throw std::runtime_error("ZIP protection file exceeds 1 MiB");
    }
    if (input.bad())
        throw std::runtime_error("ZIP protection read failed: " + path);
    return result;
}

std::set<std::string> lines(const std::string& text) {
    std::set<std::string> result;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos)
            continue;
        line = line.substr(start, line.find_last_not_of(" \t\r") - start + 1);
        validRevision(line);
        result.insert(line);
    }
    return result;
}

void atomicWrite(const std::string& path, const std::string& body) {
    std::string name = path + ".tmp-XXXXXX";
    std::vector<char> temp(name.begin(), name.end());
    temp.push_back('\0');
    int fd = ::mkstemp(temp.data());
    if (fd < 0)
        throw std::runtime_error("cannot create ZIP protection temporary file");
    try {
        // The cleaner may run under a different account; keep the list readable.
        if (::fchmod(fd, 0644))
            throw std::runtime_error("cannot set ZIP protection permissions");
        size_t offset = 0;
        while (offset < body.size()) {
            const auto n = ::write(fd, body.data() + offset, body.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("cannot write ZIP protection file");
            offset += static_cast<size_t>(n);
        }
        if (::fsync(fd))
            throw std::runtime_error("cannot sync ZIP protection file");
        ::close(fd);
        fd = -1;
        if (::rename(temp.data(), path.c_str()))
            throw std::runtime_error("cannot replace ZIP protection file");
        const int dir =
            ::open(fs::absolute(path).parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0)
            throw std::runtime_error("cannot open ZIP protection directory");
        const int synced = ::fsync(dir);
        ::close(dir);
        if (synced)
            throw std::runtime_error("cannot sync ZIP protection directory");
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        ::unlink(temp.data());
        throw;
    }
}
} // namespace

ArtifactProtection::ArtifactProtection(StateStore& store,
    std::string file,
    std::string root,
    const std::string& clock_state,
    int grace_seconds)
    : store_(store)
    , file_(std::move(file))
    , root_(std::move(root))
    , grace_(grace_seconds) {
    FileLock lock(file_);
    state_ = store_.loadArtifactProtection();
    if (state_.is_null()) {
        state_ = {{"version", 1},
            {"daily", Json::array()},
            {"daily_date", ""},
            {"daily_suite", ""},
            {"manual", Json::array()},
            {"pending", Json::object()},
            {"leases", Json::object()}};
        auto legacy = lines(read(file_, true));
        // Preserve the old daily pin on upgrade; all other pre-existing entries
        // are imported as fixed pins, never silently discarded.
        if (!clock_state.empty() && fs::exists(clock_state)) {
            const auto clock = Json::parse(read(clock_state));
            const auto revision = clock.value("revision", std::string());
            if (clock.value("phase", std::string()) == "complete" && legacy.count(revision)) {
                validRevision(revision);
                state_["daily"].push_back(revision);
                legacy.erase(revision);
            }
        }
        if (state_["daily"].empty() && legacy.size() == 1) {
            state_["daily"].push_back(*legacy.begin());
            legacy.clear();
        }
        state_["manual"] = legacy;
        store_.saveArtifactProtection(state_);
    }
    if (state_.at("version") != 1 || !state_.at("leases").is_object() ||
        !state_.at("pending").is_object() || !state_.at("daily").is_array() ||
        !state_.at("manual").is_array())
        throw std::runtime_error("invalid persisted ZIP protection state");
    // Do not publish a reduced list here: Scheduler first restores ALL active tasks.
}

void ArtifactProtection::write(Json next, std::time_t now) {
    std::set<std::string> versions;
    const auto manual = file_ + ".manual";
    if (fs::exists(manual))
        next["manual"] = lines(read(manual));
    for (const char* key : {"daily", "manual"})
        for (const auto& r : next.at(key))
            versions.insert(r.get<std::string>());
    for (const auto& p : next.at("pending"))
        for (const auto& r : p.at("revisions"))
            versions.insert(r.get<std::string>());
    auto& leases = next.at("leases");
    for (auto it = leases.begin(); it != leases.end();) {
        if (it.value().get<int64_t>() <= int64_t(now))
            it = leases.erase(it);
        else {
            versions.insert(it.key());
            ++it;
        }
    }
    std::string body;
    for (const auto& revision : versions) {
        validRevision(revision);
        body += revision + "\n";
    }
    if (body.size() > 1024 * 1024)
        throw std::runtime_error("ZIP protection list exceeds 1 MiB; refusing submission");
    // Commit additions before publishing. A crash may over-protect, never lose
    // an accepted lease. File failures propagate on submit; refresh retries later.
    if (next != state_) {
        store_.saveArtifactProtection(next);
        state_ = std::move(next);
    }
    if (!fs::exists(file_) || read(file_) != body) {
        atomicWrite(file_, body);
        LOG_INFO(LogModule::SERVER,
            "ZIP protection updated, versions=" + std::to_string(versions.size()) +
                ", file=" + file_);
    }
}

void ArtifactProtection::checkArchive(const std::string& revision) const {
    size_t found = 0;
    for (const auto& name : {"GalaxCore_" + revision + ".zip",
             "Galaxcore_" + revision + ".zip",
             "GalaxCore_r" + revision + ".zip",
             "GalaxCore-" + revision + ".zip"}) {
        const auto path = fs::path(root_) / name;
        const auto status = fs::symlink_status(path);
        if (fs::is_symlink(status))
            throw std::invalid_argument("unsafe ZIP symlink for revision " + revision);
        if (fs::exists(status)) {
            if (!fs::is_regular_file(status))
                throw std::invalid_argument("ZIP is not a regular file: " + path.string());
            ++found;
        }
    }
    if (found != 1)
        throw std::invalid_argument(found ? "ambiguous ZIP for revision " + revision
                                          : "ZIP not found for revision " + revision);
}

void ArtifactProtection::reserve(const std::string& revision) {
    validRevision(revision);
    FileLock lock(file_);
    auto prior = state_, next = state_;
    const auto now = std::time(nullptr);
    next["leases"][revision] = int64_t(now) + grace_ + 30;
    write(std::move(next), now);
    try {
        checkArchive(revision);
    } catch (...) {
        try {
            write(std::move(prior), now);
        } catch (const std::exception& error) {
            LOG_WARN(LogModule::SERVER,
                std::string("failed ZIP reservation retained conservatively: ") + error.what());
        }
        throw;
    }
}

void ArtifactProtection::daily(const Json& manifest, bool complete) {
    FileLock lock(file_);
    auto prior = state_, next = state_;
    const auto now = std::time(nullptr);
    const auto suite = manifest.at("suite_id").get<std::string>();
    const auto day = manifest.at("date").get<std::string>();
    std::set<std::string> versions;
    Json groups = Json::array();
    for (const auto& b : manifest.at("batches")) {
        const auto r = b.at("revision").get<std::string>();
        validRevision(r);
        versions.insert(r);
        groups.push_back(b.at("batch_id"));
    }
    if (complete) {
        // Keep a completed receipt idempotent; an old replay cannot replace a newer daily pin.
        const auto pending = next["pending"].find(suite);
        if (pending == next["pending"].end()) {
            if (next["daily_suite"] == suite) {
                write(std::move(next), now);
                return;
            }
            throw std::logic_error("daily submission was not reserved");
        }
        if (pending->at("manifest") != manifest)
            throw std::invalid_argument("daily completion manifest mismatch");
        if (day >= next["daily_date"].get<std::string>()) {
            for (const auto& r : next["daily"])
                next["leases"][r.get<std::string>()] = int64_t(now) + grace_;
            next["daily"] = versions;
            next["daily_date"] = day;
            next["daily_suite"] = suite;
        }
        next["pending"].erase(suite);
    } else if (next["daily_suite"] != suite) {
        if (next["pending"].contains(suite) && next["pending"][suite]["manifest"] != manifest)
            throw std::invalid_argument("daily reservation manifest mismatch");
        next["pending"][suite] = {{"revisions", versions},
            {"groups", groups},
            {"manifest", manifest},
            {"reserved_at", int64_t(now)}};
    }
    write(std::move(next), now);
    if (!complete)
        try {
            for (const auto& r : versions)
                checkArchive(r);
        } catch (...) {
            try {
                write(std::move(prior), now);
            } catch (const std::exception& error) {
                LOG_WARN(LogModule::SERVER,
                    std::string("failed daily ZIP reservation retained conservatively: ") +
                        error.what());
            }
            throw;
        }
}

void ArtifactProtection::refresh(
    const std::set<std::string>& active, const std::set<std::string>& stopped, std::time_t now) {
    FileLock lock(file_);
    auto next = state_;
    // Include one refresh interval so a task completing between polls still
    // receives at least the configured grace under normal polling.
    for (const auto& r : active)
        next["leases"][r] = int64_t(now) + grace_ + 30;
    // Pending scheduled snapshots remain protected even when disabled/paused.
    const auto automation = store_.loadAutomation();
    if (!automation.is_null())
        for (const auto& run : automation.at("runs"))
            if (run.at("state") == "SUBMITTING" &&
                !stopped.count(run.at("batch_id").get<std::string>()))
                next["leases"][run.at("revision").get<std::string>()] = int64_t(now) + grace_;
    for (auto it = next["pending"].begin(); it != next["pending"].end();) {
        // A protection write can succeed before the daily-plan DB transaction
        // fails. Unaccepted reservations must not leak a permanent pin.
        const auto& manifest = it.value().at("manifest");
        const bool accepted = store_.dailyPlan(manifest.at("date").get<std::string>()) == manifest;
        if (!accepted && it.value().value("reserved_at", int64_t(now)) + grace_ <= int64_t(now)) {
            it = next["pending"].erase(it);
            continue;
        }
        bool cancelled = !it.value().at("groups").empty();
        for (const auto& group : it.value().at("groups"))
            if (!stopped.count(group.get<std::string>()))
                cancelled = false;
        const bool submitted = accepted && !cancelled && store_.dailySubmissionComplete(manifest);
        if (cancelled || submitted) {
            for (const auto& r : it.value().at("revisions"))
                next["leases"][r.get<std::string>()] = int64_t(now) + grace_;
            if (submitted &&
                manifest.at("date").get<std::string>() >= next["daily_date"].get<std::string>()) {
                for (const auto& r : next["daily"])
                    next["leases"][r.get<std::string>()] = int64_t(now) + grace_;
                next["daily"] = it.value().at("revisions");
                next["daily_date"] = manifest.at("date");
                next["daily_suite"] = it.key();
            }
            it = next["pending"].erase(it);
        } else
            ++it;
    }
    write(std::move(next), now);
}
} // namespace GCRP
