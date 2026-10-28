#include "config/Config.h"
#include "logger/Logger.h"
#include "logger/LogLevel.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace GCRP {

Config& Config::instance() {
    static Config instance;
    return instance;
}

bool Config::loadDefault() {
    if (const char* path = std::getenv("GCRP_CONFIG"))
        return *path && load(path);
    std::error_code error;
    auto directory = std::filesystem::current_path(error);
    if (error)
        return false;
    while (true) {
        for (const char* name : {"server.conf", "gcrp.conf"}) {
            const auto candidate = directory / "config" / name;
            if (std::filesystem::is_regular_file(candidate, error))
                return load(candidate.string());
        }
        if (directory == directory.root_path())
            break;
        directory = directory.parent_path();
    }
    return false;
}

bool Config::load(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[CONFIG] Warning: Failed to open config file: " << filename << std::endl;
        std::cerr << "[CONFIG] Using default values" << std::endl;
        return false;
    }

    config_map_.clear();
    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        line_num++;
        std::string trimmed = trim(line);

        // Skip blank lines
        if (trimmed.empty()) {
            continue;
        }

        // Skip comments
        if (trimmed[0] == '#') {
            continue;
        }

        // Find key=value
        size_t pos = trimmed.find('=');
        if (pos == std::string::npos) {
            std::cerr << "[CONFIG] Warning: Malformed line " << line_num << " in " << filename
                      << ": " << line << std::endl;
            continue;
        }

        std::string key = trim(trimmed.substr(0, pos));
        std::string value = trim(trimmed.substr(pos + 1));

        if (key.empty()) {
            std::cerr << "[CONFIG] Warning: Empty key on line " << line_num << " in " << filename
                      << std::endl;
            continue;
        }

        // Check for duplicate keys (last value wins)
        auto it = config_map_.find(key);
        if (it != config_map_.end()) {
            std::cerr << "[CONFIG] Warning: Duplicate key '" << key << "' on line " << line_num
                      << " in " << filename << ". Overwriting previous value." << std::endl;
        }

        config_map_[key] = value;
    }

    file.close();
    return true;
}

std::string Config::getString(const std::string& key, const std::string& default_value) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        return it->second;
    }
    return default_value;
}

int Config::getInt(const std::string& key, int default_value) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        try {
            size_t consumed = 0;
            int value = std::stoi(it->second, &consumed);
            if (consumed != it->second.size())
                throw std::invalid_argument("trailing characters");
            return value;
        } catch (const std::exception& e) {
            std::cerr << "[CONFIG] Warning: Invalid integer value for key '" << key
                      << "': " << it->second << ". Using default: " << default_value << std::endl;
        }
    }
    return default_value;
}

bool Config::getBool(const std::string& key, bool default_value) const {
    auto it = config_map_.find(key);
    if (it != config_map_.end()) {
        std::string value = it->second;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (value == "true" || value == "yes" || value == "on" || value == "1") {
            return true;
        }
        if (value == "false" || value == "no" || value == "off" || value == "0") {
            return false;
        }
        std::cerr << "[CONFIG] Warning: Invalid boolean value for key '" << key
                  << "': " << it->second << ". Using default: " << default_value << std::endl;
    }
    return default_value;
}

bool Config::contains(const std::string& key) const {
    return config_map_.find(key) != config_map_.end();
}

std::string Config::trim(const std::string& str) const {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

} // namespace GCRP
