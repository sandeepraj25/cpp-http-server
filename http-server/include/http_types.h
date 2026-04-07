#pragma once
#include <string>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <cctype>

// ─── Request ──────────────────────────────────────────────────────────────────

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;      // everything after '?'
    std::string version;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> queryParams;
    std::string remoteAddr;

    bool keepAlive() const {
        auto it = headers.find("connection");
        if (it != headers.end()) {
            std::string v = it->second;
            std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            return v == "keep-alive";
        }
        return version == "HTTP/1.1"; // keep-alive is default in 1.1
    }
};

// ─── Response ─────────────────────────────────────────────────────────────────

struct HttpResponse {
    int         statusCode   = 200;
    std::string statusText   = "OK";
    std::string body;
    std::string contentType  = "text/html; charset=utf-8";
    std::unordered_map<std::string, std::string> headers;

    void setJson(const std::string& json) {
        body        = json;
        contentType = "application/json";
    }

    void setHtml(const std::string& html) {
        body        = html;
        contentType = "text/html; charset=utf-8";
    }

    std::string serialize(bool keepAlive = false) const {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
        oss << "Content-Type: "   << contentType           << "\r\n";
        oss << "Content-Length: " << body.size()           << "\r\n";
        oss << "Connection: "     << (keepAlive ? "keep-alive" : "close") << "\r\n";
        oss << "Server: CppHttpd/2.0\r\n";
        for (auto& [k, v] : headers)
            oss << k << ": " << v << "\r\n";
        oss << "\r\n" << body;
        return oss.str();
    }

    // Convenience factory methods
    static HttpResponse ok(const std::string& body,
                           const std::string& ct = "text/html; charset=utf-8") {
        HttpResponse r;
        r.statusCode = 200; r.statusText = "OK";
        r.body = body; r.contentType = ct;
        return r;
    }

    static HttpResponse notFound(const std::string& msg = "<h1>404 Not Found</h1>") {
        HttpResponse r;
        r.statusCode = 404; r.statusText = "Not Found";
        r.body = msg;
        return r;
    }

    static HttpResponse methodNotAllowed() {
        HttpResponse r;
        r.statusCode = 405; r.statusText = "Method Not Allowed";
        r.body = "<h1>405 Method Not Allowed</h1>";
        return r;
    }

    static HttpResponse internalError(const std::string& msg = "<h1>500 Internal Server Error</h1>") {
        HttpResponse r;
        r.statusCode = 500; r.statusText = "Internal Server Error";
        r.body = msg;
        return r;
    }

    static HttpResponse redirect(const std::string& location, int code = 301) {
        HttpResponse r;
        r.statusCode = code;
        r.statusText = (code == 301) ? "Moved Permanently" : "Found";
        r.headers["Location"] = location;
        r.body = "";
        return r;
    }
};