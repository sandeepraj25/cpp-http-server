#include "http_server.h"
#include <sstream>
#include <chrono>

// ─── Simple in-memory "database" for demo ─────────────────────────────────────
#include <unordered_map>
#include <mutex>
struct User { std::string id, name, email; };
std::unordered_map<std::string, User> userDB;
std::mutex userMu;
int nextId = 1;

std::string userToJson(const User& u) {
    return "{\"id\":\"" + u.id + "\",\"name\":\"" + u.name +
           "\",\"email\":\"" + u.email + "\"}";
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
std::string extractJsonField(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = json.find('"', pos);
    return (end == std::string::npos) ? "" : json.substr(pos, end - pos);
}

int main() {
    ServerConfig cfg;
    cfg.port           = 8080;
    cfg.threadPoolSize = 8;
    cfg.enableCors     = true;
    cfg.enableRateLimit= true;
    cfg.rateLimitMax   = 200;

    HttpServer server(cfg);
    auto& router = server.router();

    // ── GET / ─────────────────────────────────────────────────────────────────
    router.get("/", [](const HttpRequest&) {
        return HttpResponse::ok(R"html(
<!DOCTYPE html>
<html>
<head><title>CppHttpd/2.0</title>
<style>body{font-family:sans-serif;max-width:700px;margin:60px auto;color:#222}
h1{color:#1a6faf}code{background:#f4f4f4;padding:2px 6px;border-radius:3px}
a{color:#1a6faf}ul{line-height:2}</style></head>
<body>
<h1>CppHttpd/2.0</h1>
<p>Advanced C++ HTTP/1.1 server with thread pool, middleware pipeline, keep-alive.</p>
<h2>API Endpoints</h2>
<ul>
  <li><code>GET  /</code>          — this page</li>
  <li><code>GET  /health</code>    — health check (JSON)</li>
  <li><code>GET  /users</code>     — list all users (JSON)</li>
  <li><code>POST /users</code>     — create user (JSON body)</li>
  <li><code>GET  /users/:id</code> — get user by id</li>
  <li><code>DELETE /users/:id</code> — delete user</li>
  <li><code>GET  /echo?msg=hello</code> — echo query param</li>
  <li><code>GET  /static/index.html</code> — static files</li>
</ul>
</body></html>)html");
    });

    // ── GET /health ───────────────────────────────────────────────────────────
    router.get("/health", [](const HttpRequest&) {
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::string ts = std::ctime(&t);
        if (!ts.empty() && ts.back() == '\n') ts.pop_back();
        HttpResponse res;
        res.setJson("{\"status\":\"ok\",\"server\":\"CppHttpd/2.0\",\"time\":\"" + ts + "\"}");
        return res;
    });

    // ── GET /users ────────────────────────────────────────────────────────────
    router.get("/users", [](const HttpRequest&) {
        std::lock_guard<std::mutex> lock(userMu);
        std::string json = "[";
        bool first = true;
        for (auto& [id, u] : userDB) {
            if (!first) json += ",";
            json += userToJson(u);
            first = false;
        }
        json += "]";
        HttpResponse res;
        res.setJson(json);
        return res;
    });

    // ── POST /users ───────────────────────────────────────────────────────────
    router.post("/users", [](const HttpRequest& req) {
        std::string name  = extractJsonField(req.body, "name");
        std::string email = extractJsonField(req.body, "email");

        if (name.empty() || email.empty()) {
            HttpResponse r;
            r.statusCode = 400; r.statusText = "Bad Request";
            r.setJson("{\"error\":\"name and email required\"}");
            return r;
        }

        User u;
        {
            std::lock_guard<std::mutex> lock(userMu);
            u.id    = std::to_string(nextId++);
            u.name  = name;
            u.email = email;
            userDB[u.id] = u;
        }

        HttpResponse res;
        res.statusCode = 201; res.statusText = "Created";
        res.setJson(userToJson(u));
        return res;
    });

    // ── GET /users/:id ────────────────────────────────────────────────────────
    router.get("/users/:id", [](const HttpRequest& req) {
        auto it2 = req.queryParams.find("id");
        if (it2 == req.queryParams.end())
            return HttpResponse::notFound("{\"error\":\"not found\"}");

        std::lock_guard<std::mutex> lock(userMu);
        auto it = userDB.find(it2->second);
        if (it == userDB.end()) {
            HttpResponse r = HttpResponse::notFound("{\"error\":\"user not found\"}");
            r.contentType = "application/json";
            return r;
        }
        HttpResponse res; res.setJson(userToJson(it->second));
        return res;
    });

    // ── DELETE /users/:id ─────────────────────────────────────────────────────
    router.del("/users/:id", [](const HttpRequest& req) {
        auto it2 = req.queryParams.find("id");
        if (it2 == req.queryParams.end())
            return HttpResponse::notFound();

        std::lock_guard<std::mutex> lock(userMu);
        auto erased = userDB.erase(it2->second);
        if (!erased) {
            HttpResponse r = HttpResponse::notFound("{\"error\":\"not found\"}");
            r.contentType = "application/json";
            return r;
        }
        HttpResponse res;
        res.statusCode = 204; res.statusText = "No Content";
        return res;
    });

    // ── GET /echo ─────────────────────────────────────────────────────────────
    router.get("/echo", [](const HttpRequest& req) {
        auto it = req.queryParams.find("msg");
        std::string msg = (it != req.queryParams.end()) ? it->second : "(empty)";
        HttpResponse res;
        res.setJson("{\"echo\":\"" + msg + "\",\"method\":\"" + req.method + "\"}");
        return res;
    });

    server.run();
    return 0;
}