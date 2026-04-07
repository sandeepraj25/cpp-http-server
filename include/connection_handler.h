#pragma once
#include "http_types.h"
#include "http_parser.h"
#include "middleware.h"
#include "router.h"
#include "logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <chrono>

class ConnectionHandler {
public:
    ConnectionHandler(int fd,
                      sockaddr_in clientAddr,
                      MiddlewarePipeline& pipeline,
                      const Router& router,
                      int keepAliveTimeoutSec = 5,
                      int maxKeepAliveReqs    = 100)
        : fd_(fd)
        , pipeline_(pipeline)
        , router_(router)
        , keepAliveTimeout_(keepAliveTimeoutSec)
        , maxKeepAlive_(maxKeepAliveReqs)
    {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, buf, sizeof(buf));
        remoteAddr_ = std::string(buf) + ":" +
                      std::to_string(ntohs(clientAddr.sin_port));
    }

    void handle() {
        int  reqCount = 0;
        bool keepAlive = true;

        while (keepAlive && reqCount < maxKeepAlive_) {
            // Set read timeout
            struct timeval tv { keepAliveTimeout_, 0 };
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            std::string rawRequest;
            if (!readRequest(rawRequest)) break;

            HttpRequest req;
            req.remoteAddr = remoteAddr_;

            if (!HttpParser::parse(rawRequest, req)) {
                // Bad request
                HttpResponse res;
                res.statusCode = 400;
                res.statusText = "Bad Request";
                res.body       = "<h1>400 Bad Request</h1>";
                std::string out = res.serialize(false);
                auto _ = write(fd_, out.c_str(), out.size()); (void)_;
                break;
            }

            HttpResponse res;

            // Run middleware pipeline → router
            pipeline_.run(req, res, [&](HttpRequest& r, HttpResponse& resp) {
                resp = router_.handle(r);
            });

            keepAlive = req.keepAlive();
            std::string out = res.serialize(keepAlive);
            if (write(fd_, out.c_str(), out.size()) < 0) break;

            ++reqCount;
        }

        close(fd_);
    }

private:
    int                 fd_;
    MiddlewarePipeline& pipeline_;
    const Router&       router_;
    int                 keepAliveTimeout_;
    int                 maxKeepAlive_;
    std::string         remoteAddr_;

    bool readRequest(std::string& out) {
        char buf[4096];
        out.clear();

        while (true) {
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) return false;   // timeout or closed
            out.append(buf, n);

            // Check if we have full headers
            if (out.find("\r\n\r\n") != std::string::npos) {
                // Check if body is complete
                size_t hdrEnd = out.find("\r\n\r\n") + 4;
                size_t cl = 0;
                size_t clPos = out.find("Content-Length:");
                if (clPos != std::string::npos && clPos < hdrEnd) {
                    cl = std::stoul(out.substr(clPos + 15));
                }
                if (out.size() >= hdrEnd + cl) return true;
            }

            if (out.size() > 1024 * 1024) return false; // 1MB limit
        }
    }
};