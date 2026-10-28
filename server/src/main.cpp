#include "EchoServer.h"
#include "WorkerServer.h"
#include "Logging.h"
#include "config/Config.h"
#include "logger/Logger.h"
#include "logger/LogLevel.h"
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <iostream>
#include <unordered_map>
#include <memory>

static int g_signal_fd = -1;

void handle_sigint(int) {
    uint64_t one = 1;
    ssize_t n = ::write(g_signal_fd, &one, sizeof(one));
    (void)n;
}

GCRP::LogLevel parseLogLevel(const std::string& level_str) {
    static const std::unordered_map<std::string, GCRP::LogLevel> level_map = {
        {"DEBUG", GCRP::LogLevel::DEBUG},
        {"INFO", GCRP::LogLevel::INFO},
        {"WARN", GCRP::LogLevel::WARN},
        {"ERROR", GCRP::LogLevel::ERROR},
        {"FATAL", GCRP::LogLevel::FATAL}};

    auto it = level_map.find(level_str);
    if (it != level_map.end()) {
        return it->second;
    }
    std::cerr << "[CONFIG] Warning: Unknown log level '" << level_str << "'. Defaulting to INFO."
              << std::endl;
    return GCRP::LogLevel::INFO;
}

int main(int argc, char** argv) {
    // Step 1: Load Config
    if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--help")) {
        std::cerr << "Usage: gcrp_server [--help]\n";
        return 1;
    }
    if (argc == 2) {
        std::cout << "Set GCRP_CONFIG to the shared config file path.\n";
        return 0;
    }
    bool config_loaded = GCRP::Config::instance().loadDefault();
    if (!config_loaded) {
        std::cerr << "[SERVER] Missing config; set GCRP_CONFIG" << std::endl;
        return 1;
    }

    // Step 2: Configure Logger
    std::string log_level_str = GCRP::Config::instance().getString("log.level", "INFO");
    std::string log_dir = GCRP::Config::instance().getString("log.dir", "");

    GCRP::LogLevel log_level = parseLogLevel(log_level_str);
    GCRP::Logger::instance().setLogDirectory(log_dir);
    GCRP::Logger::instance().setLevel(log_level);

    // Step 3: Read server settings
    int port = GCRP::Config::instance().getInt("server.port", 0);
    int worker_threads = GCRP::Config::instance().getInt("server.worker_threads", 8);
    int io_threads = GCRP::Config::instance().getInt("server.io_threads", 2);
    const std::string bind_ip = GCRP::Config::instance().getString("server.bind_ip", "");
    if (port < 1 || port > 65535 || worker_threads < 1 || worker_threads > 256 || io_threads < 1 ||
        io_threads > 256 || log_dir.empty() || bind_ip.empty()) {
        std::cerr << "[SERVER] Invalid port or thread count" << std::endl;
        return 1;
    }

    std::cerr << "[SERVER] Configuration: port=" << port << ", workers=" << worker_threads
              << ", log_level=" << log_level_str << ", log_dir=" << log_dir << std::endl;

    LOG_INFO(GCRP::LogModule::SERVER, "server starting...");

    g_signal_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_signal_fd < 0)
        return 1;

    struct sigaction sa {};

    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Step 4: Start server with configured settings
    EchoHandler handler;
    std::unique_ptr<GCRP::WorkerServer> worker_server;
    try {
        worker_server = std::make_unique<GCRP::WorkerServer>(true);
    } catch (const std::exception& error) {
        std::cerr << "[SERVER] State database unavailable: " << error.what() << std::endl;
        ::close(g_signal_fd);
        g_signal_fd = -1;
        return 1;
    }
    EchoServer server(port, handler, g_signal_fd, io_threads, worker_threads, bind_ip);
    worker_server->attach(server);

    bool ran = server.run();

    if (g_signal_fd != -1) {
        ::close(g_signal_fd);
        g_signal_fd = -1;
    }

    LOG_INFO(GCRP::LogModule::SERVER, "server stopped!");

    return ran ? 0 : 1;
}
