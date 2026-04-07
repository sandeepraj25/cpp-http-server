#pragma once
#include "http_types.h"
#include <string>
#include <sstream>
#include <algorithm>

class HttpParser {
public:
    // Returns false if request is incomplete / malformed
    static bool parse(const std::string& raw, HttpRequest& req) {
        // Find end of headers
        size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;

        std::istringstream stream(raw.substr(0, headerEnd));

        // Request line
        std::string requestLine;
        if (!std::getline(stream, requestLine)) return false;
        if (!requestLine.empty() && requestLine.back() == '\r')
            requestLine.pop_back();

        std::istringstream rl(requestLine);
        rl >> req.method >> req.path >> req.version;
        if (req.method.empty() || req.path.empty()) return false;

        // Split path and query string
        size_t qpos = req.path.find('?');
        if (qpos != std::string::npos) {
            req.query = req.path.substr(qpos + 1);
            req.path  = req.path.substr(0, qpos);
            parseQueryParams(req.query, req.queryParams);
        }

        // Headers
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim and lowercase the key
            val.erase(0, val.find_first_not_of(" \t"));
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }

        // Body (if Content-Length present)
        req.body = raw.substr(headerEnd + 4);
        auto it = req.headers.find("content-length");
        if (it != req.headers.end()) {
            size_t len = std::stoul(it->second);
            if (req.body.size() > len) req.body.resize(len);
        }

        return true;
    }

private:
    static void parseQueryParams(const std::string& query,
                                 std::unordered_map<std::string, std::string>& out) {
        std::istringstream stream(query);
        std::string pair;
        while (std::getline(stream, pair, '&')) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) {
                out[urlDecode(pair)] = "";
            } else {
                out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
        }
    }

    static std::string urlDecode(const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size()) {
                int val = std::stoi(s.substr(i + 1, 2), nullptr, 16);
                result += static_cast<char>(val);
                i += 2;
            } else if (s[i] == '+') {
                result += ' ';
            } else {
                result += s[i];
            }
        }
        return result;
    }
};