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
        res.headers["X-Content-Type-Options"]  = "nosniff";      // no MIME sniffing
        res.headers["X-Frame-Options"]         = "DENY";         // no clickjacking via iframes
        res.headers["Content-Security-Policy"] = "default-src 'self'; style-src 'self' 'unsafe-inline'";
        res.headers["Referrer-Policy"]         = "no-referrer";
        // X-XSS-Protection was removed: it is deprecated and ignored by modern
        // browsers. Content-Security-Policy is the modern replacement.
    };
}

// ─── Rate limiting ────────────────────────────────────────────────────────────
#include <unordered_map>
#include <deque>
#include <mutex>
#include <memory>

// Per-IP sliding-window rate limiter ("sliding log" algorithm).
//
// For each client IP we keep the timestamps of its recent requests in a deque.
// On every request we drop timestamps older than the window from the front,
// then allow the request only if fewer than maxRequests remain.
//
// Why not a fixed window? A fixed window resets the count at fixed times, so a
// client can send maxRequests at 0:59 and maxRequests again at 1:01 (double the
// limit in 2 seconds). A sliding window always looks at the last N seconds.
//
// Time:  O(1) amortized per request (each timestamp is pushed and popped once)
// Space: O(maxRequests) per active IP
class SlidingWindowRateLimiter {
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    SlidingWindowRateLimiter(int maxRequests, int windowSec)
        : maxRequests_(maxRequests), window_(std::chrono::seconds(windowSec)) {}

    // Returns true if the request is allowed. If not, retryAfterSec tells the
    // client how long until the oldest request leaves the window.
    bool allow(const std::string& clientIp, TimePoint now, int& retryAfterSec) {
        std::lock_guard<std::mutex> lock(mu_);

        if (++callsSinceSweep_ >= kSweepEvery) sweep(now);

        auto& log = clients_[clientIp];
        while (!log.empty() && now - log.front() >= window_) log.pop_front();

        if (static_cast<int>(log.size()) >= maxRequests_) {
            auto wait = std::chrono::duration_cast<std::chrono::seconds>(
                            window_ - (now - log.front())).count();
            retryAfterSec = static_cast<int>(wait < 1 ? 1 : wait);
            return false;
        }
        log.push_back(now);
        retryAfterSec = 0;
        return true;
    }

    size_t trackedClients() const {
        std::lock_guard<std::mutex> lock(mu_);
        return clients_.size();
    }

private:
    static constexpr int kSweepEvery = 1024;

    // Forget IPs that have been quiet for a whole window, so the map does not
    // grow forever as new clients come and go.
    void sweep(TimePoint now) {
        callsSinceSweep_ = 0;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->second.empty() || now - it->second.back() >= window_)
                it = clients_.erase(it);
            else
                ++it;
        }
    }

    int maxRequests_;
    Clock::duration window_;
    int callsSinceSweep_ = 0;
    std::unordered_map<std::string, std::deque<TimePoint>> clients_;
    mutable std::mutex mu_;
};

inline MiddlewareFn rateLimiterMiddleware(int maxReq = 100, int windowSec = 60) {
    auto limiter = std::make_shared<SlidingWindowRateLimiter>(maxReq, windowSec);

    return [limiter](HttpRequest& req, HttpResponse& res, NextFn next) {
        int retryAfter = 0;
        // Keyed by IP only (not ip:port). Every new TCP connection gets a new
        // port, so keying by port would let a client bypass the limit by reconnecting.
        if (!limiter->allow(req.clientIp, SlidingWindowRateLimiter::Clock::now(), retryAfter)) {
            res.statusCode  = 429;
            res.statusText  = "Too Many Requests";
            res.body        = "{\"error\":\"rate limit exceeded\"}";
            res.contentType = "application/json";
            res.headers["Retry-After"] = std::to_string(retryAfter);
            return;   // stop the pipeline: handler is not called
        }
        next();
    };
}
