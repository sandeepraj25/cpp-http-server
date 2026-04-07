#pragma once
#include <string>
#include <thread>

struct ServerConfig {
    std::string host             = "0.0.0.0";
    int         port             = 8080;
    size_t      threadPoolSize   = std::thread::hardware_concurrency();
    int         backlog          = 128;           // listen() queue depth
    int         keepAliveTimeout = 5;             // seconds
    int         maxKeepAliveReqs = 100;
    size_t      maxRequestSize   = 1024 * 1024;   // 1MB
    std::string staticRoot       = "./static";
    std::string logFile          = "./logs/access.log";
    bool        enableCors       = true;
    bool        enableRateLimit  = true;
    int         rateLimitMax     = 200;           // req per window
    int         rateLimitWindow  = 60;            // seconds
};