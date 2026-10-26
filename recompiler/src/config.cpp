#include "config.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

bool parseInt(const std::string& s, int64_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    try {
        size_t used = 0;
        out = std::stoll(t, &used, 0);
        return used == t.size();
    } catch (...) {
        return false;
    }
}

// Removes a trailing comment that is not inside a string.
std::string stripComment(const std::string& line) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '"') inStr = !inStr;
        else if (line[i] == '#' && !inStr) return line.substr(0, i);
    }
    return line;
}

} // namespace

bool Config::load(const std::string& path, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open config " + path;
        return false;
    }
    baseDir = std::filesystem::absolute(path).parent_path().string();

    std::string line, pending;
    int lineNo = 0;
    while (std::getline(f, line)) {
        lineNo++;
        line = trim(stripComment(line));
        if (line.empty()) continue;
        // Arrays may span multiple lines.
        if (!pending.empty()) {
            pending += " " + line;
            if (line.find(']') == std::string::npos) continue;
            line = pending;
            pending.clear();
        } else if (line.find('[') != std::string::npos && line.find(']') == std::string::npos) {
            pending = line;
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = path + ":" + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        ConfigValue v;
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            v.type = ConfigValue::String;
            v.str = val.substr(1, val.size() - 2);
        } else if (val == "true" || val == "false") {
            v.type = ConfigValue::Bool;
            v.num = val == "true";
        } else if (!val.empty() && val.front() == '[' && val.back() == ']') {
            v.type = ConfigValue::IntArray;
            std::stringstream ss(val.substr(1, val.size() - 2));
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (trim(item).empty()) continue;
                int64_t n;
                if (!parseInt(item, n)) {
                    error = path + ":" + std::to_string(lineNo) + ": bad integer '" + trim(item) + "'";
                    return false;
                }
                v.arr.push_back(n);
            }
        } else if (parseInt(val, v.num)) {
            v.type = ConfigValue::Int;
        } else {
            error = path + ":" + std::to_string(lineNo) + ": cannot parse value '" + val + "'";
            return false;
        }
        values_[key] = v;
    }
    return true;
}

std::string Config::getString(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it != values_.end() && it->second.type == ConfigValue::String ? it->second.str : def;
}

int64_t Config::getInt(const std::string& key, int64_t def) const {
    auto it = values_.find(key);
    return it != values_.end() && it->second.type == ConfigValue::Int ? it->second.num : def;
}

bool Config::getBool(const std::string& key, bool def) const {
    auto it = values_.find(key);
    return it != values_.end() && it->second.type == ConfigValue::Bool ? it->second.num != 0 : def;
}

std::vector<int64_t> Config::getIntArray(const std::string& key) const {
    auto it = values_.find(key);
    return it != values_.end() && it->second.type == ConfigValue::IntArray ? it->second.arr : std::vector<int64_t>{};
}
