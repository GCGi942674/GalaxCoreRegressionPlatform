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

namespace GCRP {
namespace {
constexpr uint64_t kFileLimit = 1024ULL * 1024 * 1024;
constexpr size_t kChunkLimit = 16384;
constexpr std::array<const char*, 9> names{{"stdout.log", "stderr.log", "result.env",
    "result.json", "flow_config", "evidence.run", "runner.log", "case-run.log", "evidence.json"}};
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string decode(const std::string& value) {
    if (value.size() > 2 * kChunkLimit || value.size() % 2)
        throw std::invalid_argument("invalid log chunk length");
    std::string out(value.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i) {
        const int a = nibble(value[2 * i]), b = nibble(value[2 * i + 1]);
        if (a < 0 || b < 0) throw std::invalid_argument("invalid log hex");
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
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("log write failed");
        offset += static_cast<size_t>(count);
    }
    if (::fsync(fd) != 0) throw std::runtime_error("log sync failed");
}
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
};
void syncDirectory(const std::filesystem::path& path) {
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0 || ::fsync(directory.value) != 0)
        throw std::runtime_error("cannot sync log directory");
}
nlohmann::json metadata(const std::filesystem::path& path) {
    if(std::filesystem::is_symlink(path))throw std::runtime_error("unsafe archive metadata");
    if(!std::filesystem::exists(path))return nullptr;
    if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>8192)
        throw std::runtime_error("invalid archive metadata");
    std::ifstream stream(path);return nlohmann::json::parse(stream);
}
void saveMetadata(const std::filesystem::path& path,const nlohmann::json& value) {
    const auto temporary=path.string()+".tmp";
    Fd fd(::open(temporary.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600));
    if(fd.value<0)throw std::runtime_error("cannot save archive state");
    writeAll(fd.value,value.dump());std::filesystem::rename(temporary,path);syncDirectory(path.parent_path());
}
}
RunLogArchive::RunLogArchive(std::filesystem::path root) : root_(std::move(root)) {
    if (root_.empty()) throw std::invalid_argument("log archive root is empty");
    std::filesystem::create_directories(root_);
    if (std::filesystem::is_symlink(root_) || !std::filesystem::is_directory(root_))
        throw std::runtime_error("unsafe log archive root");
}
bool RunLogArchive::validRunId(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
bool RunLogArchive::validName(const std::string& value) {
    for (const char* name : names) if (value == name) return true;
    return false;
}
std::filesystem::path RunLogArchive::file(uint64_t task_id, const std::string& run_id,
                                           const std::string& name) const {
    if (!task_id || !validRunId(run_id) || !validName(name))
        throw std::invalid_argument("invalid log identity");
    auto parent = root_ / std::to_string(task_id);
    auto directory = parent / run_id;
    if (std::filesystem::is_symlink(parent) || std::filesystem::is_symlink(directory))
        throw std::runtime_error("unsafe log directory");
    return directory / name;
}
nlohmann::json RunLogArchive::append(uint64_t task_id, const std::string& run_id,
    const std::string& name, uint64_t offset, const std::string& hex, bool complete,
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
    if (new_task_dir) syncDirectory(root_);
    if (new_run_dir) syncDirectory(task_dir);
    if (std::filesystem::is_symlink(path) || std::filesystem::is_symlink(path.string() + ".done"))
        throw std::runtime_error("unsafe log file");
    const bool new_file = !std::filesystem::exists(path);
    Fd fd(::open(path.c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.value < 0) throw std::runtime_error("cannot open log file");
    const uint64_t size = std::filesystem::file_size(path);
    const bool done = std::filesystem::exists(path.string() + ".done");
    if (size > kFileLimit) throw std::runtime_error("log file limit exceeded");
    if (!expected_sha256.empty()) {
        if (expected_sha256.size()!=64 || expected_sha256.find_first_not_of("0123456789abcdef")!=std::string::npos ||
            !complete || !bytes.empty() || !done || offset!=size)
            throw std::invalid_argument("invalid archive verification request");
        Sha256 digest;std::array<char,65536> block{};uint64_t at=0;
        while(at<size){auto count=::pread(fd.value,block.data(),std::min<uint64_t>(block.size(),size-at),static_cast<off_t>(at));
            if(count<0&&errno==EINTR)continue;
            if(count<=0)throw std::runtime_error("cannot verify archived file");
            digest.update(block.data(),static_cast<size_t>(count));at+=static_cast<uint64_t>(count);}
        if(digest.hex()!=expected_sha256)throw std::invalid_argument("archive SHA-256 mismatch; preserve Worker source");
        // Persist the digest, not just the transient ACK, for later GUI/download checks.
        saveMetadata(path.string()+".verified",{{"sha256",expected_sha256},{"size",size}});
        Fd marker(::open((path.string()+".done").c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
        if(marker.value<0)throw std::runtime_error("verified archive marker missing");
        // A long-offline Worker may verify an older upload. Start its Server
        // retention window again before authorizing removal of the local copy.
        std::filesystem::last_write_time(path.string()+".done",std::filesystem::file_time_type::clock::now());
        if(::fsync(fd.value)!=0||::fsync(marker.value)!=0)
            throw std::runtime_error("cannot sync verified archive");
        syncDirectory(path.parent_path());
        return {{"next_offset",size},{"complete",true},{"verified_sha256",expected_sha256}};
    }
    if (offset != size || done) return {{"next_offset", size}, {"complete", done}};
    writeAll(fd.value, bytes);
    if (new_file) syncDirectory(path.parent_path());
    const uint64_t next = size + bytes.size();
    if (complete) {
        Fd marker(::open((path.string() + ".done").c_str(),
                         O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (marker.value < 0) throw std::runtime_error("cannot complete log file");
        if (::fsync(marker.value) != 0) throw std::runtime_error("cannot sync log marker");
        syncDirectory(path.parent_path());
    }
    return {{"next_offset", next}, {"complete", complete}};
}
nlohmann::json RunLogArchive::list(uint64_t task_id, const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json items = nlohmann::json::array();
    for (const char* name : names) {
        const auto path = file(task_id, run_id, name);
        if (std::filesystem::is_symlink(path)) continue;
        if (std::filesystem::is_regular_file(path)){
            const auto receipt=metadata(path.string()+".verified");
            items.push_back({{"name", name}, {"size", std::filesystem::file_size(path)},
                {"sha256",receipt.is_object()?receipt.value("sha256",std::string{}):std::string{}},
                {"verified",receipt.is_object()&&receipt.value("size",uint64_t(-1))==std::filesystem::file_size(path)},
                {"complete", std::filesystem::is_regular_file(path.string() + ".done")}});
        }
    }
    const auto receipt=metadata(file(task_id,run_id,"stdout.log").parent_path()/".archive.json");
    std::string state=items.empty()?"NOT_REPORTED":"UPLOADED_UNVERIFIED";
    bool all=true,complete=true;
    for(const auto& item:items){all=all&&item["verified"].get<bool>();complete=complete&&item["complete"].get<bool>();}
    if(!complete)state="UPLOADING";
    if(receipt.is_object()){
        if(receipt.value("expired",false))state="EXPIRED";
        else{
            const auto& expected=receipt.at("files");
            all=all&&expected.size()==items.size()&&!expected.empty();
            for(const auto& item:items)
                all=all&&std::find(expected.begin(),expected.end(),item["name"])!=expected.end();
            if(all)state=receipt.value("local_cleaned",false)?"LOCAL_CLEANED":"SERVER_VERIFIED";
            else state="UPLOADING";
        }
    }
    return {{"task_id", task_id}, {"run_id", run_id}, {"items", items},{"archive_state",state},
        {"local_cleanup_confirmed",state=="LOCAL_CLEANED"}};
}
nlohmann::json RunLogArchive::reportState(uint64_t task_id,const std::string& run_id,const nlohmann::json& state) {
    if(!state.is_object()||state.size()!=2||!state.contains("phase")||!state["phase"].is_string()||
        !state.contains("files")||!state["files"].is_array()||state["files"].empty()||state["files"].size()>names.size())
        throw std::invalid_argument("invalid archive state");
    const auto phase=state["phase"].get<std::string>();
    if(phase!="UPLOADING"&&phase!="LOCAL_CLEANED")throw std::invalid_argument("invalid archive phase");
    std::set<std::string> expected;
    for(const auto& name:state["files"]){
        if(!name.is_string()||!validName(name.get<std::string>())||!expected.insert(name.get<std::string>()).second)
            throw std::invalid_argument("invalid archive file set");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto directory=file(task_id,run_id,"stdout.log").parent_path();
    const auto task_directory=directory.parent_path();
    const bool new_task=!std::filesystem::exists(task_directory),new_run=!std::filesystem::exists(directory);
    std::filesystem::create_directories(directory);
    if(new_task)syncDirectory(root_);
    if(new_run)syncDirectory(task_directory);
    const auto path=directory/".archive.json";
    auto receipt=metadata(path);
    const auto files=nlohmann::json(expected);
    if(receipt.is_object()&&receipt.at("files")!=files)throw std::invalid_argument("archive file set changed");
    if(phase=="LOCAL_CLEANED"){
        for(const auto& name:expected){
            const auto source=file(task_id,run_id,name);const auto verified=metadata(source.string()+".verified");
            if(!std::filesystem::is_regular_file(source)||std::filesystem::is_symlink(source)||
                !std::filesystem::is_regular_file(source.string()+".done")||!verified.is_object()||
                verified.value("size",uint64_t(-1))!=std::filesystem::file_size(source))
                throw std::invalid_argument("archive is not fully verified; preserve Worker logs");
        }
    }
    saveMetadata(path,{{"files",files},{"local_cleaned",phase=="LOCAL_CLEANED"||
        (receipt.is_object()&&receipt.value("local_cleaned",false))},{"expired",false}});
    return {{"recorded",true}};
}
nlohmann::json RunLogArchive::read(uint64_t task_id, const std::string& run_id,
    const std::string& name, uint64_t offset, size_t limit) {
    if (!limit || limit > 131072) throw std::invalid_argument("invalid log read limit");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = file(task_id, run_id, name);
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.value < 0) throw std::invalid_argument("log file not found");
    const auto size = std::filesystem::file_size(path);
    if (offset > size) throw std::invalid_argument("log offset beyond end");
    std::string bytes(std::min<uint64_t>(limit, size - offset), '\0');
    const auto count = ::pread(fd.value, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    if (count < 0) throw std::runtime_error("log read failed");
    bytes.resize(static_cast<size_t>(count));
    const auto receipt=metadata(path.string()+".verified");
    return {{"task_id", task_id}, {"run_id", run_id}, {"name", name},
            {"offset", offset}, {"next_offset", offset + bytes.size()},
            {"size", size}, {"complete", std::filesystem::exists(path.string() + ".done")},
            {"sha256",receipt.is_object()?receipt.value("sha256",std::string{}):std::string{}},
            {"data_hex", encode(bytes)}};
}
size_t RunLogArchive::pruneOlderThan(int days) {
    if (days < 1 || days > 3650) throw std::invalid_argument("invalid log retention days");
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * days);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (const auto& task : std::filesystem::directory_iterator(root_)) {
        if (task.is_symlink() || !task.is_directory()) continue;
        const auto id = task.path().filename().string();
        if (id.empty() || id[0] == '0' || id.find_first_not_of("0123456789") != std::string::npos) continue;
        for (const auto& run : std::filesystem::directory_iterator(task.path())) {
            if (run.is_symlink() || !run.is_directory() || !validRunId(run.path().filename().string())) continue;
            bool safe = true, has_file = false;
            auto newest = std::filesystem::file_time_type::min();
            for (const auto& entry : std::filesystem::directory_iterator(run.path())) {
                const auto filename = entry.path().filename().string();
                if(filename==".archive.json")continue;
                const auto base = filename.size() > 9 && filename.substr(filename.size()-9)==".verified"
                    ?filename.substr(0,filename.size()-9):filename.size()>5&&filename.substr(filename.size()-5)==".done"
                    ? filename.substr(0, filename.size() - 5) : filename;
                if (entry.is_symlink() || !entry.is_regular_file() || !validName(base)) { safe = false; break; }
                has_file = true;
                newest = std::max(newest, entry.last_write_time());
            }
            if (!safe || !has_file || newest >= cutoff) continue;
            auto receipt=metadata(run.path()/".archive.json");
            if(!receipt.is_object())receipt={{"files",nlohmann::json::array()},{"local_cleaned",false}};
            receipt["expired"]=true;saveMetadata(run.path()/".archive.json",receipt);
            for (const auto& entry : std::filesystem::directory_iterator(run.path()))
                if(entry.path().filename()!=".archive.json")std::filesystem::remove(entry.path());
            ++removed;
        }
        if (std::filesystem::is_empty(task.path())) std::filesystem::remove(task.path());
    }
    return removed;
}
} // namespace GCRP
