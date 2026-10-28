#ifndef GCRP_LOG_MACROS_H
#define GCRP_LOG_MACROS_H

#include "Logger.h"

#define LOG_DEBUG(module, message)                                                                 \
    GCRP::Logger::instance().log(GCRP::LogLevel::DEBUG, module, __FILE__, __LINE__, message)

#define LOG_INFO(module, message)                                                                  \
    GCRP::Logger::instance().log(GCRP::LogLevel::INFO, module, __FILE__, __LINE__, message)

#define LOG_WARN(module, message)                                                                  \
    GCRP::Logger::instance().log(GCRP::LogLevel::WARN, module, __FILE__, __LINE__, message)

#define LOG_ERROR(module, message)                                                                 \
    GCRP::Logger::instance().log(GCRP::LogLevel::ERROR, module, __FILE__, __LINE__, message)

#define LOG_FATAL(module, message)                                                                 \
    GCRP::Logger::instance().log(GCRP::LogLevel::FATAL, module, __FILE__, __LINE__, message)

#endif // GCRP_LOG_MACROS_H
