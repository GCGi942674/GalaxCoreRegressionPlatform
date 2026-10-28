#include "protocol/GuiCatalog.h"
#include "protocol/ApiQueries.h"
#include "config/Config.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace GCRP {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

std::string readBounded(const fs::path& path, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.filename().string());
    std::string value(limit + 1, '\0');
    in.read(&value[0], value.size());
    value.resize(static_cast<size_t>(in.gcount()));
    if (value.size() > limit)
        throw std::runtime_error("file exceeds size limit: " + path.filename().string());
    return value;
}

bool newer(const std::string& a, const std::string& b) {
    auto normalize = [](const std::string& s) {
        auto p = s.find_first_not_of('0');
        return p == std::string::npos ? std::string("0") : s.substr(p);
    };
    auto x = normalize(a), y = normalize(b);
    return x.size() != y.size() ? x.size() > y.size() : x != y ? x > y : a > b;
}
} // namespace

nlohmann::json GuiCatalog::scan() const {
    Json out = {{"version", 1},
        {"cases", Json::array()},
        {"templates", Json::object()},
        {"revisions", Json::array()},
        {"protected_revisions", Json::array()},
        {"sources", Json::object()},
        {"warnings", Json::array()}};
    const auto& config = Config::instance();
    auto source = [&](const char* key, auto collect) {
        const auto path = config.getString(key, "");
        try {
            if (path.empty())
                throw std::runtime_error(std::string("configure ") + key);
            if (!fs::is_directory(path))
                throw std::runtime_error(std::string(key) + " is not a readable directory");
            collect(fs::canonical(path));
            out["sources"][key] = "READY";
        } catch (const std::exception& e) {
            out["sources"][key] = "ERROR";
            out["warnings"].push_back(std::string(key) + ": " + e.what());
        }
    };
    source("server.work_root", [&](const fs::path& root) {
        std::vector<std::string> paths;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        size_t visited = 0, bytes = 0;
        for (fs::recursive_directory_iterator i(root), end; i != end; ++i) {
            if (++visited > 200000 || std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("scan limit exceeded; narrow server.work_root");
            const auto name = i->path().filename().string();
            if (i->is_symlink()) {
                i.disable_recursion_pending();
                continue;
            }
            if (i->is_directory()) {
                if (i.depth() > 32 || (!name.empty() && name[0] == '.') ||
                    name == "vivado_runner" || name == "runtime")
                    i.disable_recursion_pending();
                continue;
            }
            if (name != "run.tcl" || !i->is_regular_file() || i->path().parent_path() == root)
                continue;
            auto relative = i->path().lexically_relative(root).generic_string();
            static const std::regex safe("[A-Za-z0-9_.+/-]+");
            // Same safe-path contract as RegressionPayload; spaces are not accepted.
            if (relative.size() > 512 || relative.find(' ') != std::string::npos ||
                !std::regex_match(relative, safe))
                continue;
            bytes += relative.size();
            paths.push_back(relative);
            if (paths.size() > 50000 || bytes > 16 * 1024 * 1024)
                throw std::runtime_error("too many cases; narrow server.work_root");
        }
        std::sort(paths.begin(), paths.end());
        out["cases"] = paths;
        if (paths.empty())
            out["warnings"].push_back("server.work_root: no testcase run.tcl found");
    });
    source("server.templates_dir", [&](const fs::path& root) {
        Json templates = Json::object();
        size_t visited = 0, templateBytes = 0;
        for (const auto& file : fs::directory_iterator(root)) {
            if (++visited > 10000)
                throw std::runtime_error("template directory entry limit exceeded");
            if (file.is_symlink() || !file.is_regular_file() || file.path().extension() != ".json")
                continue;
            const auto name = file.path().stem().string();
            try {
                if (!std::regex_match(name, std::regex("[A-Za-z0-9][A-Za-z0-9_-]{0,63}")))
                    throw std::runtime_error("invalid template name");
                auto data = Json::parse(readBounded(file.path(), 65536));
                const auto& flow = data.at("flow_config");
                if (!flow.is_object() || flow.empty() || flow.size() > 64)
                    throw std::runtime_error("invalid flow_config");
                for (auto it = flow.begin(); it != flow.end(); ++it) {
                    if (!std::regex_match(it.key(), std::regex("[A-Za-z][A-Za-z0-9_]{0,63}")) ||
                        !it.value().is_number_integer() || it.value() < 0 || it.value() > 1000000)
                        throw std::runtime_error("invalid flow_config option");
                }
                const auto bytes = flow.dump().size() + name.size() + 32;
                if (templates.size() >= 128 || templateBytes + bytes > 131072)
                    throw std::runtime_error("template snapshot limit exceeded");
                templates[name] = {{"flow_config", flow}};
                templateBytes += bytes;
            } catch (const std::exception& e) {
                out["warnings"].push_back("template " + name + ": " + e.what());
            }
            if (out["warnings"].size() > 128)
                throw std::runtime_error("too many invalid templates");
        }
        out["templates"] = templates;
        if (templates.empty())
            out["warnings"].push_back("server.templates_dir: no valid templates found");
    });
    source("server.artifact_root", [&](const fs::path& root) {
        std::set<std::string, decltype(&newer)> versions(&newer);
        size_t visited = 0;
        static const std::regex archiveName("Galax[Cc]ore_([0-9]{1,32})\\.zip");
        for (const auto& file : fs::directory_iterator(root)) {
            if (++visited > 200000)
                throw std::runtime_error("artifact directory entry limit exceeded");
            std::smatch match;
            const auto name = file.path().filename().string();
            if (!file.is_symlink() && file.is_regular_file() &&
                std::regex_match(name, match, archiveName))
                versions.insert(match[1]);
        }
        std::set<std::string> protectedVersions;
        const auto protection = config.getString("server.protect_versions_file", "");
        if (!protection.empty()) {
            try {
                std::istringstream lines(readBounded(protection, 1024 * 1024));
                std::string line;
                while (lines >> line)
                    if (std::regex_match(line, std::regex("[0-9]{1,32}")) && versions.count(line))
                        protectedVersions.insert(line);
            } catch (const std::exception& e) {
                out["warnings"].push_back(std::string("server.protect_versions_file: ") + e.what());
            }
        }
        size_t index = 0;
        for (const auto& version : versions) {
            if (index++ < 50 || protectedVersions.count(version))
                out["revisions"].push_back(version);
            if (protectedVersions.count(version))
                out["protected_revisions"].push_back(version);
        }
        if (versions.empty())
            out["warnings"].push_back("server.artifact_root: no GalaxCore_N.zip found");
    });
    return out;
}

nlohmann::json GuiCatalog::query(const nlohmann::json& request) {
    Api::validate(request, {"offset", "snapshot_generation", "refresh"});
    uint64_t offset = 0;
    if (request.contains("offset")) {
        const auto& n = request["offset"];
        if (!n.is_number_integer() || n < 0 || n > 50000)
            throw std::invalid_argument("invalid catalog offset");
        offset = n.get<uint64_t>();
    }
    if (request.contains("refresh") && !request["refresh"].is_boolean())
        throw std::invalid_argument("refresh must be boolean");
    const bool continuation = request.contains("snapshot_generation");
    const auto generation = continuation ? Api::positive(request, "snapshot_generation", 0) : 0;
    if (offset && !continuation)
        throw std::invalid_argument("catalog offset requires snapshot_generation");
    if (continuation && request.value("refresh", false))
        throw std::invalid_argument("cannot refresh a continuation");
    std::lock_guard<std::mutex> lock(mutex_);
    if (continuation) {
        if (snapshot_.is_null() || (generation != generation_ && !recent_.count(generation)))
            throw std::logic_error("catalog snapshot expired; refresh catalog");
    } else if (snapshot_.is_null() || request.value("refresh", false) ||
               std::chrono::steady_clock::now() - created_ > std::chrono::seconds(60)) {
        auto next = scan();
        if (!snapshot_.is_null()) {
            recent_[generation_] = std::move(snapshot_);
            while (recent_.size() > 3)
                recent_.erase(recent_.begin());
        }
        snapshot_.swap(next);
        ++generation_;
        created_ = std::chrono::steady_clock::now();
    }
    const auto& selected =
        continuation && generation != generation_ ? recent_.at(generation) : snapshot_;
    const auto total = selected["cases"].size();
    if (offset > total)
        throw std::invalid_argument("catalog offset exceeds total");
    Json result = {{"version", 1},
        {"snapshot_generation", continuation ? generation : generation_},
        {"total_cases", total},
        {"cases", Json::array()}};
    const auto end = std::min<size_t>(total, offset + 200);
    for (size_t i = offset; i < end; ++i)
        result["cases"].push_back(selected["cases"][i]);
    result["next_offset"] = end < total ? Json(end) : Json(nullptr);
    if (!continuation)
        for (const char* key :
            {"templates", "revisions", "protected_revisions", "sources", "warnings"})
            result[key] = selected[key];
    return result;
}
} // namespace GCRP
