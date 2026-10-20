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
    std::string remoteAddr;   // "ip:port", used for logging
    std::string clientIp;     // "ip" only, used for rate limiting

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

// ─── JSON helper ──────────────────────────────────────────────────────────────

// Escapes a value before putting it inside a JSON string, so user input
// like  a","admin":"true  cannot inject extra fields into the response.
inline std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

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

    static HttpResponse badRequest(const std::string& msg = "<h1>400 Bad Request</h1>") {
        HttpResponse r;
        r.statusCode = 400; r.statusText = "Bad Request";
        r.body = msg;
        return r;
    }

    static HttpResponse forbidden(const std::string& msg = "<h1>403 Forbidden</h1>") {
        HttpResponse r;
        r.statusCode = 403; r.statusText = "Forbidden";
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