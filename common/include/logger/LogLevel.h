#ifndef GCRP_LOG_LEVEL_H
#define GCRP_LOG_LEVEL_H

#include <string>

namespace GCRP {

enum class LogLevel { DEBUG, INFO, WARN, ERROR, FATAL };

inline std::string toString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:
            return "DEBUG";
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
        case LogLevel::FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}

} // namespace GCRP

#endif // GCRP_LOG_LEVEL_H