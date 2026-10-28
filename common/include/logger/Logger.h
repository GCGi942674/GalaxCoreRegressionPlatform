#ifndef GCRP_LOGGER_H
#define GCRP_LOGGER_H

#include "LogLevel.h"
#include "LogModule.h"
#include <string>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace GCRP {

class Logger {
public:
    static Logger& instance();
    void log(LogLevel level,
        LogModule module,
        const char* file,
        int line,
        const std::string& message) noexcept;
    void shutdown();
    void setLogDirectory(const std::string& dir);
    void setLevel(LogLevel level);
    size_t pruneOldLogs(int days);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static const char* getBaseName(const char* path) {
        if (path == nullptr) {
            return "";
        }

        const char* slash1 = std::strrchr(path, '/');
        const char* slash2 = std::strrchr(path, '\\');
        const char* pos =
            !slash1 ? slash2 : (!slash2 ? slash1 : (slash1 > slash2 ? slash1 : slash2));
        return pos ? pos + 1 : path;
    }

private:
    Logger();
    ~Logger();

    std::string formatMessage(
        LogLevel level, LogModule module, const char* file, int line, const std::string& message);
    std::string getDateDir();
    std::string getLogFile(LogModule module, LogLevel level);
    void writeToFile(const std::string& filename, const std::string& message);
    void checkDateChange();
    void ensureDirectoryExists(const std::filesystem::path& path);

    std::mutex mutex_;
    std::string current_date_;
    std::filesystem::path log_dir_;
    LogLevel current_level_{LogLevel::INFO};
};

} // namespace GCRP

#endif // GCRP_LOGGER_H
