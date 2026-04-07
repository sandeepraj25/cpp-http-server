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

        router_.get("/static/:file", [this](const HttpRequest& req) {
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

        LOG_INFO("Shutting down — draining thread pool...");
        pool_.waitAll();
        pool_.shutdown();

        if (serverFd_ >= 0)
            close(serverFd_);

        LOG_INFO("Goodbye.");
    }

    void stop() {
        running_ = false;
        if (serverFd_ >= 0)
            shutdown(serverFd_, SHUT_RDWR);
    }

private:
    ServerConfig       cfg_;
    ThreadPool         pool_;
    MiddlewarePipeline pipeline_;
    Router             router_;
    StaticFileServer   staticServer_;
    int                serverFd_;

    inline static std::atomic<bool> running_{true};
    inline static HttpServer* instance_{nullptr};

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
    }

    void registerSignals() {
        instance_ = this;

        auto handler = [](int sig) {
            LOG_INFO("Signal ", sig, " received — shutting down gracefully");
            if (instance_)
                instance_->stop();
        };

        std::signal(SIGINT, handler);
        std::signal(SIGTERM, handler);
        std::signal(SIGPIPE, SIG_IGN);
    }
};