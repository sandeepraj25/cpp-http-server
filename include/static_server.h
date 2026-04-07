#pragma once
#include "http_types.h"
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

class StaticFileServer {
public:
    explicit StaticFileServer(const std::string& root) : root_(root) {}

    HttpResponse serve(const HttpRequest& req) {
        std::string relPath = req.path;

        // Remove /static prefix from URL path
        const std::string prefix = "/static";
        if (relPath.rfind(prefix, 0) == 0) {
            relPath = relPath.substr(prefix.size());
        }

        // Handle /static and /static/
        if (relPath.empty() || relPath == "/") {
            relPath = "index.html";
        } else {
            // Remove leading slash before joining with root_
            if (!relPath.empty() && relPath[0] == '/') {
                relPath.erase(0, 1);
            }
        }

        // Sanitize path - prevent directory traversal
        fs::path rootPath = fs::path(root_).lexically_normal();
        fs::path reqPath  = (rootPath / fs::path(relPath)).lexically_normal();

        // Ensure resolved path stays inside root
        auto rel = reqPath.lexically_relative(rootPath);
        std::string relStr = rel.string();
        if (relStr.empty() || relStr == "." || relStr.rfind("..", 0) == 0) {
            return HttpResponse::notFound("<h1>403 Forbidden</h1>");
        }

        // If directory, serve index.html
        if (fs::exists(reqPath) && fs::is_directory(reqPath)) {
            reqPath /= "index.html";
        }

        if (!fs::exists(reqPath) || !fs::is_regular_file(reqPath)) {
            return HttpResponse::notFound();
        }

        // Check cache
        auto lastWrite = fs::last_write_time(reqPath);
        {
            std::lock_guard<std::mutex> lock(cacheMu_);
            auto it = cache_.find(reqPath.string());
            if (it != cache_.end() && it->second.lastWrite == lastWrite) {
                return HttpResponse::ok(it->second.content, it->second.mimeType);
            }
        }

        // Read file
        std::ifstream file(reqPath, std::ios::binary);
        if (!file) return HttpResponse::internalError();

        std::ostringstream ss;
        ss << file.rdbuf();

        std::string content = ss.str();
        std::string mime    = mimeType(reqPath.extension().string());

        // Cache file
        {
            std::lock_guard<std::mutex> lock(cacheMu_);
            cache_[reqPath.string()] = {content, mime, lastWrite};
        }

        return HttpResponse::ok(content, mime);
    }

private:
    struct CacheEntry {
        std::string content;
        std::string mimeType;
        fs::file_time_type lastWrite;
    };

    std::string root_;
    std::unordered_map<std::string, CacheEntry> cache_;
    std::mutex cacheMu_;

    static std::string mimeType(const std::string& ext) {
        static const std::unordered_map<std::string, std::string> types = {
            {".html",  "text/html; charset=utf-8"},
            {".htm",   "text/html; charset=utf-8"},
            {".css",   "text/css"},
            {".js",    "application/javascript"},
            {".json",  "application/json"},
            {".png",   "image/png"},
            {".jpg",   "image/jpeg"},
            {".jpeg",  "image/jpeg"},
            {".gif",   "image/gif"},
            {".svg",   "image/svg+xml"},
            {".ico",   "image/x-icon"},
            {".txt",   "text/plain"},
            {".pdf",   "application/pdf"},
            {".xml",   "application/xml"},
            {".woff",  "font/woff"},
            {".woff2", "font/woff2"},
        };

        auto it = types.find(ext);
        return (it != types.end()) ? it->second : "application/octet-stream";
    }
};