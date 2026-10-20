#pragma once
#include "http_types.h"
#include <string>
#include <sstream>
#include <algorithm>
#include <optional>
#include <cctype>

class HttpParser {
public:
    // Largest body we accept. Anything bigger is rejected before we read it.
    static constexpr size_t kMaxBodySize = 1024 * 1024;   // 1 MB

    // Parses a complete raw request into `req`.
    // Returns false if the request is malformed. It never throws, so a bad
    // client can only get a 400, never crash the server.
    static bool parse(const std::string& raw, HttpRequest& req) {
        size_t headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;

        std::istringstream stream(raw.substr(0, headerEnd));

        // Request line: METHOD PATH VERSION
        std::string requestLine;
        if (!std::getline(stream, requestLine)) return false;
        if (!requestLine.empty() && requestLine.back() == '\r')
            requestLine.pop_back();

        std::istringstream rl(requestLine);
        rl >> req.method >> req.path >> req.version;
        if (req.method.empty() || req.path.empty() || req.path[0] != '/') return false;
        if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") return false;

        // Split path and query string
        size_t qpos = req.path.find('?');
        if (qpos != std::string::npos) {
            req.query = req.path.substr(qpos + 1);
            req.path  = req.path.substr(0, qpos);
            parseQueryParams(req.query, req.queryParams);
        }

        // Headers (keys are stored lowercase, so lookups are case-insensitive)
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            req.headers[key] = val;
        }

        // Body: trust Content-Length only if it is a valid number
        req.body = raw.substr(headerEnd + 4);
        auto it = req.headers.find("content-length");
        if (it != req.headers.end()) {
            auto len = parseContentLength(it->second);
            if (!len) return false;
            if (req.body.size() > *len) req.body.resize(*len);
        }

        return true;
    }

    // "123" -> 123. Rejects empty, non-digit, or too-large values
    // instead of throwing like std::stoul would.
    static std::optional<size_t> parseContentLength(const std::string& s) {
        if (s.empty() || s.size() > 10) return std::nullopt;
        size_t value = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<size_t>(c - '0');
        }
        if (value > kMaxBodySize) return std::nullopt;
        return value;
    }

    // Decodes %XX and '+'. Invalid escapes like "%zz" are kept as-is
    // instead of throwing (the old std::stoi version crashed the server).
    static std::string urlDecode(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '%' && i + 2 < s.size() &&
                hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
                result += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
                i += 2;
            } else if (s[i] == '+') {
                result += ' ';
            } else {
                result += s[i];
            }
        }
        return result;
    }

private:
    static int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

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
};
