#include "TestSupport.h"
#include "log/RunLogArchive.h"
#include "protocol/ConnectionRole.h"
#include "Sha256.h"
#include <filesystem>
#include "logger/Logger.h"
#include <chrono>
#include <fstream>
#include <stdexcept>
using namespace GCRP;
using namespace GCRP::Protocol;

int main() { return testMain([] {
    Sha256 empty;CHECK(empty.hex()=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Sha256 abc;abc.update("a",1);abc.update("bc",2);
    CHECK(abc.hex()=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 million;std::string thousand(1000,'a');for(int i=0;i<1000;++i)million.update(thousand.data(),thousand.size());
    CHECK(million.hex()=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    char name[] = "/tmp/gcrp-logs-XXXXXX";
    CHECK(::mkdtemp(name) != nullptr);
    struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{name};
    CHECK(isAllowed(ConnectionRole::WORKER, MessageType::LOG_APPEND));
    CHECK(!isAllowed(ConnectionRole::CLIENT, MessageType::LOG_APPEND));
    CHECK(isAllowed(ConnectionRole::CLIENT, MessageType::QUERY_LOGS));
    CHECK(!isAllowed(ConnectionRole::WORKER, MessageType::QUERY_LOGS));
    CHECK(!RunLogArchive::validName("../secret"));
    CHECK(!RunLogArchive::validRunId("../other"));
    RunLogArchive archive(name);
    auto first = archive.append(10, "10-a1", "stdout.log", 0, "68656c6c6f", false);
    CHECK(first["next_offset"] == 5);
    CHECK(!first["complete"].get<bool>());
    // An ACK lost after fsync causes a replay. The server returns its durable offset.
    auto replay = archive.append(10, "10-a1", "stdout.log", 0, "68656c6c6f", false);
    CHECK(replay["next_offset"] == 5);
    auto done = archive.append(10, "10-a1", "stdout.log", 5, "00ff", true);
    CHECK(done["next_offset"] == 7);
    CHECK(done["complete"].get<bool>());
    CHECK(archive.append(10, "10-a1", "stdout.log", 5, "00ff", true)["next_offset"] == 7);
    auto page = archive.read(10, "10-a1", "stdout.log", 2, 4);
    CHECK(page["data_hex"] == "6c6c6f00");
    CHECK(page["next_offset"] == 6);
    CHECK(archive.list(10, "10-a1")["items"].size() == 1);
    CHECK(archive.list(11, "10-a1")["items"].empty());
    CHECK(archive.list(11,"10-a1")["archive_state"]=="NOT_REPORTED");
    archive.reportState(10,"10-a1",{{"phase","UPLOADING"},{"files",{"stdout.log"}}});
    CHECK(archive.list(10,"10-a1")["archive_state"]=="UPLOADING");
    bool unverified=false;
    try{archive.reportState(10,"10-a1",{{"phase","LOCAL_CLEANED"},{"files",{"stdout.log"}}});}
    catch(const std::invalid_argument&){unverified=true;}CHECK(unverified);
    Sha256 logDigest;const char logBytes[]={'h','e','l','l','o',0,static_cast<char>(0xff)};
    logDigest.update(logBytes,sizeof(logBytes));
    CHECK(archive.append(10,"10-a1","stdout.log",7,"",true,logDigest.hex())["verified_sha256"]==logDigest.hex());
    CHECK(archive.list(10,"10-a1")["archive_state"]=="SERVER_VERIFIED");
    archive.reportState(10,"10-a1",{{"phase","LOCAL_CLEANED"},{"files",{"stdout.log"}}});
    CHECK(archive.list(10,"10-a1")["archive_state"]=="LOCAL_CLEANED");
    CHECK(archive.read(10,"10-a1","stdout.log",0,131072)["sha256"]==logDigest.hex());
    bool mismatch=false;try{archive.append(10,"10-a1","stdout.log",7,"",true,std::string(64,'0'));}
    catch(const std::invalid_argument&){mismatch=true;}CHECK(mismatch);
    {
        RunLogArchive reopened(name);
        auto all = reopened.read(10, "10-a1", "stdout.log", 0, 16);
        CHECK(all["data_hex"] == "68656c6c6f00ff");
        CHECK(all["complete"].get<bool>());
        const auto long_offline=std::filesystem::file_time_type::clock::now()-std::chrono::hours(24*31);
        const auto old=std::filesystem::path(name)/"10"/"10-a1"/"stdout.log";
        std::filesystem::last_write_time(old,long_offline);
        std::filesystem::last_write_time(old.string()+".done",long_offline);
        CHECK(reopened.append(10,"10-a1","stdout.log",7,"",true,logDigest.hex())["complete"]==true);
    }
    bool rejected = false;
    try { archive.append(10, "10-a1", "../secret", 0, "00", true); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { archive.append(10, "10-a1", "stderr.log", 0, "gg", true); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { archive.read(10, "10-a1", "stdout.log", 0, 131073); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    auto aged = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 31);
    archive.append(12, "12-old", "stdout.log", 0, "00", true);
    auto old_path = std::filesystem::path(name) / "12" / "12-old" / "stdout.log";
    std::filesystem::last_write_time(old_path, aged);
    std::filesystem::last_write_time(old_path.string() + ".done", aged);
    archive.append(13, "13-new", "stdout.log", 0, "00", true);
    archive.append(14, "14-unknown", "stdout.log", 0, "00", true);
    auto unknown_path = std::filesystem::path(name) / "14" / "14-unknown";
    std::ofstream(unknown_path / "notes.txt") << "preserve";
    for (const auto& item : std::filesystem::directory_iterator(unknown_path))
        std::filesystem::last_write_time(item.path(), aged);
    CHECK(archive.pruneOlderThan(30) == 1);
    CHECK(!std::filesystem::exists(old_path));
    CHECK(archive.list(12,"12-old")["archive_state"]=="EXPIRED");
    CHECK(std::filesystem::exists(std::filesystem::path(name)/"10"/"10-a1"/"stdout.log"));
    CHECK(std::filesystem::exists(std::filesystem::path(name) / "13" / "13-new" / "stdout.log"));
    CHECK(std::filesystem::exists(unknown_path / "notes.txt"));
    auto log_root = std::filesystem::path(name) / "dated";
    Logger::instance().setLogDirectory(log_root.string());
    std::filesystem::create_directory(log_root / "20000101");
    std::ofstream(log_root / "20000101" / "server.log") << "old";
    CHECK(Logger::instance().pruneOldLogs(30) == 1);
    CHECK(!std::filesystem::exists(log_root / "20000101"));
}); }
