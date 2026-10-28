#include "logger/Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <cctype>
#include <ctime>
#include <sstream>
#include <thread>

namespace GCRP {

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::Logger()
    : log_dir_("./logs") {
    try {
        ensureDirectoryExists(log_dir_);
        current_date_ = getDateDir();
    } catch (const std::exception& e) {
        std::cerr << "[FATAL] Logger initialization failed: " << e.what() << std::endl;
    }
}

Logger::~Logger() {
    shutdown();
}

void Logger::log(LogLevel level,
    LogModule module,
    const char* file,
    int line,
    const std::string& message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (level < current_level_) {
            return;
        }

        checkDateChange();

        std::string formatted = formatMessage(level, module, file, line, message);
        std::string logfile = getLogFile(module, level);
        bool is_error = (level == LogLevel::ERROR || level == LogLevel::FATAL);

        writeToFile(logfile, formatted);

        // Also write to error.log for ERROR and FATAL
        if (is_error) {
            writeToFile("error.log", formatted);
        }
    } catch (...) {
        // Logging must not change a completed business/transport operation.
    }
}

std::string Logger::formatMessage(
    LogLevel level, LogModule module, const char* file, int line, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    oss << " [" << toString(level) << "]";
    oss << " [" << toString(module) << "]";
    oss << " [tid:" << std::this_thread::get_id() << "]";
    oss << " [" << getBaseName(file) << ":" << line << "]";
    oss << " " << message;

    return oss.str();
}

std::string Logger::getDateDir() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y%m%d");
    return oss.str();
}

std::string Logger::getLogFile(LogModule module, LogLevel) {
    std::string filename;

    if (module == LogModule::SERVER || module == LogModule::NETWORK) {
        filename = "server.log";
    } else {
        filename = "task.log";
    }

    return filename;
}

void Logger::writeToFile(const std::string& filename, const std::string& message) {
    try {
        std::filesystem::path filepath = log_dir_ / current_date_ / filename;
        ensureDirectoryExists(filepath.parent_path());

        std::ofstream file(filepath, std::ios::app);
        if (file.is_open()) {
            file << message << std::endl;
            file.flush();
        } else {
            std::cerr << "[ERROR] Failed to open log file: " << filepath << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to write log: " << e.what() << std::endl;
        std::cerr << message << std::endl;
    }
}

void Logger::checkDateChange() {
    std::string new_date = getDateDir();
    if (new_date != current_date_) {
        current_date_ = new_date;
    }
}

void Logger::ensureDirectoryExists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        std::cerr << "[WARN] Failed to create directory " << path << ": " << ec.message()
                  << std::endl;
    }
}

void Logger::shutdown() {
    // Synchronous logging - nothing to drain
}

void Logger::setLogDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_dir_ = dir;
    ensureDirectoryExists(log_dir_);
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = level;
}

size_t Logger::pruneOldLogs(int days) {
    if (days < 1 || days > 3650)
        throw std::invalid_argument("invalid log retention days");
    std::lock_guard<std::mutex> lock(mutex_);
    if (log_dir_.empty() || std::filesystem::is_symlink(log_dir_) ||
        !std::filesystem::is_directory(log_dir_))
        throw std::runtime_error("unsafe log directory");
    const auto cutoff_time = std::chrono::system_clock::now() - std::chrono::hours(24 * days);
    const auto cutoff_t = std::chrono::system_clock::to_time_t(cutoff_time);
    std::ostringstream cutoff_stream;
    cutoff_stream << std::put_time(std::localtime(&cutoff_t), "%Y%m%d");
    const auto cutoff = cutoff_stream.str();
    size_t removed = 0;
    for (const auto& day : std::filesystem::directory_iterator(log_dir_)) {
        const auto date = day.path().filename().string();
        if (day.is_symlink() || !day.is_directory() || date.size() != 8 || date >= cutoff ||
            !std::all_of(date.begin(), date.end(), [](unsigned char c) {
                return std::isdigit(c);
            }))
            continue;
        for (const char* name : {"server.log", "task.log", "error.log"}) {
            const auto path = day.path() / name;
            if (std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(path)) {
                std::filesystem::remove(path);
                ++removed;
            }
        }
        if (std::filesystem::is_empty(day.path()))
            std::filesystem::remove(day.path());
    }
    return removed;
}

} // namespace GCRP
