#pragma once
#include "http_types.h"
#include <functional>
#include <vector>
#include <chrono>
#include "logger.h"

// A middleware is a function: (req, res, next) -> void
// Call next() to pass control to the next middleware
using NextFn     = std::function<void()>;
using MiddlewareFn = std::function<void(HttpRequest&, HttpResponse&, NextFn)>;

class MiddlewarePipeline {
public:
    void use(MiddlewareFn fn) {
        middlewares_.push_back(std::move(fn));
    }

    // Run all middlewares in order, then call finalHandler
    void run(HttpRequest& req, HttpResponse& res,
             std::function<void(HttpRequest&, HttpResponse&)> finalHandler) {
        size_t idx = 0;

        std::function<void()> dispatch = [&]() {
            if (idx < middlewares_.size()) {
                auto& mw = middlewares_[idx++];
                mw(req, res, dispatch);
            } else {
                finalHandler(req, res);
            }
        };
        dispatch();
    }

private:
    std::vector<MiddlewareFn> middlewares_;
};

// ─── Built-in Middleware ───────────────────────────────────────────────────────

// Access logger: prints method, path, status, and duration
inline MiddlewareFn accessLoggerMiddleware() {
    return [](HttpRequest& req, HttpResponse& res, NextFn next) {
        auto start = std::chrono::steady_clock::now();
        next();
        auto end = std::chrono::steady_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        LOG_INFO(req.remoteAddr, " \"", req.method, " ", req.path, "\" ",
                 res.statusCode, " ", res.body.size(), "B ", ms, "ms");
    };
}

// CORS middleware: adds Access-Control headers
inline MiddlewareFn corsMiddleware(const std::string& origin = "*") {
    return [origin](HttpRequest&, HttpResponse& res, NextFn next) {
        next();
        res.headers["Access-Control-Allow-Origin"]  = origin;
        res.headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
        res.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
    };
}

// Security headers middleware
inline MiddlewareFn securityHeadersMiddleware() {
    return [](HttpRequest&, HttpResponse& res, NextFn next) {
        next();
        res.headers["X-Content-Type-Options"] = "nosniff";
        res.headers["X-Frame-Options"]        = "DENY";
        res.headers["X-XSS-Protection"]       = "1; mode=block";
    };
}

// Rate limiter middleware (per-IP, sliding window)
#include <unordered_map>
#include <mutex>
inline MiddlewareFn rateLimiterMiddleware(int maxReq = 100, int windowSec = 60) {
    struct Entry { int count; std::chrono::steady_clock::time_point windowStart; };
    auto store = std::make_shared<std::unordered_map<std::string, Entry>>();
    auto mu    = std::make_shared<std::mutex>();

    return [store, mu, maxReq, windowSec](HttpRequest& req, HttpResponse& res, NextFn next) {
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(*mu);
            auto& entry = (*store)[req.remoteAddr];
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               now - entry.windowStart).count();
            if (elapsed > windowSec) {
                entry.count       = 0;
                entry.windowStart = now;
            }
            if (entry.count >= maxReq) {
                res.statusCode = 429;
                res.statusText = "Too Many Requests";
                res.body       = "{\"error\":\"rate limit exceeded\"}";
                res.contentType= "application/json";
                res.headers["Retry-After"] = std::to_string(windowSec - elapsed);
                return; // don't call next
            }
            ++entry.count;
        }
        next();
    };
}