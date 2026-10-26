// Minimal TOML-subset reader for per-game recompiler configs.
// Supports: comments (#), key = "string" | integer (dec/hex) | true/false | [int, ...]
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct ConfigValue {
    enum Type { String, Int, Bool, IntArray } type = String;
    std::string str;
    int64_t num = 0;
    std::vector<int64_t> arr;
};

class Config {
public:
    bool load(const std::string& path, std::string& error);

    std::string getString(const std::string& key, const std::string& def = "") const;
    int64_t getInt(const std::string& key, int64_t def = 0) const;
    bool getBool(const std::string& key, bool def = false) const;
    std::vector<int64_t> getIntArray(const std::string& key) const;

    // Directory containing the config file; relative paths resolve against it.
    std::string baseDir;

private:
    std::map<std::string, ConfigValue> values_;
};
