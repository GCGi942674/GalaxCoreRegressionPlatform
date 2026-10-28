#ifndef GCRP_CONFIG_H
#define GCRP_CONFIG_H

#include <string>
#include <unordered_map>

namespace GCRP {

class Config {
public:
    static Config& instance();

    bool load(const std::string& filename);
    bool loadDefault();

    std::string getString(const std::string& key, const std::string& default_value) const;
    int getInt(const std::string& key, int default_value) const;
    bool getBool(const std::string& key, bool default_value) const;
    bool contains(const std::string& key) const;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

private:
    Config() = default;
    ~Config() = default;

    std::string trim(const std::string& str) const;

    std::unordered_map<std::string, std::string> config_map_;
};

} // namespace GCRP

#endif // GCRP_CONFIG_H
