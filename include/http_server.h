#pragma once
#include "config.h"
#include "thread_pool.h"
#include "middleware.h"
#include "router.h"
#include "connection_handler.h"
#include "static_server.h"
#include "logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <atomic>
#include <memory>
#include <stdexcept>

class HttpServer {
public:
    explicit HttpServer(ServerConfig cfg = {})
        : cfg_(std::move(cfg))
        , pool_(cfg_.threadPoolSize)
        , staticServer_(cfg_.staticRoot)
        , serverFd_(-1)
    {
        Logger::instance().setFile(cfg_.logFile);

        pipeline_.use(accessLoggerMiddleware());
        if (cfg_.enableCors)
            pipeline_.use(corsMiddleware());
        if (cfg_.enableRateLimit)
            pipeline_.use(rateLimiterMiddleware(
                cfg_.rateLimitMax, cfg_.rateLimitWindow));
        pipeline_.use(securityHeadersMiddleware());

        // Serve static files
        router_.get("/static", [this](const HttpRequest& req) {
            return staticServer_.serve(req);
        });

        router_.get("/static/", [this](const HttpRequest& req) {
            return staticServer_.serve(req);
        });

        // Any file under /static, including nested folders like /static/css/app.css
        router_.get("/static/*", [this](const HttpRequest& req) {
            return staticServer_.serve(req);
        });
    }

    Router& router() { return router_; }

    void use(MiddlewareFn fn) { pipeline_.use(std::move(fn)); }

    void run() {
        setupSocket();
        registerSignals();

        LOG_INFO("CppHttpd/2.0 started on ", cfg_.host, ":", cfg_.port,
                 "  threads=", cfg_.threadPoolSize);

        while (running_) {
            // Wait for a new connection, but wake up every 250 ms to check
            // running_. Relying on shutdown() to interrupt a blocked accept()
            // works on Linux but not on macOS, where Ctrl+C would hang forever.
            pollfd pfd{serverFd_, POLLIN, 0};
            int ready = poll(&pfd, 1, 250);
            if (ready <= 0) continue;          // timeout or EINTR: re-check running_

            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);

            int clientFd = accept(serverFd_,
                                  reinterpret_cast<sockaddr*>(&clientAddr),
                                  &clientLen);
            if (clientFd < 0) {
                if (!running_) break;
                continue;
            }

            pool_.enqueue([this, clientFd, clientAddr]() {
                ConnectionHandler handler(clientFd, clientAddr,
                                          pipeline_, router_,
                                          cfg_.keepAliveTimeout,
                                          cfg_.maxKeepAliveReqs);
                handler.handle();
            });
        }

        if (lastSignal_ != 0)
            LOG_INFO("Signal ", lastSignal_.load(), " received — shutting down gracefully");
        LOG_INFO("Shutting down — draining thread pool...");
        pool_.waitAll();
        pool_.shutdown();

        listenFd_ = -1;
        if (serverFd_ >= 0)
            close(serverFd_);

        LOG_INFO("Goodbye.");
    }

    void stop() {
        running_ = false;
        int fd = listenFd_.load();
        if (fd >= 0)
            ::shutdown(fd, SHUT_RDWR);   // wakes up the blocked accept()
    }

private:
    ServerConfig       cfg_;
    ThreadPool         pool_;
    MiddlewarePipeline pipeline_;
    Router             router_;
    StaticFileServer   staticServer_;
    int                serverFd_;

    // Shared with the signal handler. Lock-free atomics are safe to touch
    // from a signal handler; mutexes, malloc and logging are not.
    inline static std::atomic<bool> running_{true};
    inline static std::atomic<int>  listenFd_{-1};
    inline static std::atomic<int>  lastSignal_{0};

    void setupSocket() {
        serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd_ < 0)
            throw std::runtime_error("socket() failed");

        int opt = 1;
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(serverFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr);
        addr.sin_port = htons(cfg_.port);

        if (bind(serverFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed — port already in use?");

        if (listen(serverFd_, cfg_.backlog) < 0)
            throw std::runtime_error("listen() failed");

        listenFd_ = serverFd_;
    }

    void registerSignals() {
        // The handler only does async-signal-safe work: set flags and call
        // shutdown(). Logging here could deadlock if the signal arrives while
        // a worker thread holds the logger mutex (found with ThreadSanitizer).
        auto handler = [](int sig) {
            lastSignal_ = sig;
            running_    = false;
            int fd = listenFd_.load();
            if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        };

        std::signal(SIGINT, handler);
        std::signal(SIGTERM, handler);
        std::signal(SIGPIPE, SIG_IGN);
    }
};