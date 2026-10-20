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
#include <cctype>
#include <exception>

// macOS has no MSG_NOSIGNAL; SIGPIPE is ignored globally there instead.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Handles one client connection from start to finish.
// One worker thread runs handle() for the whole life of the connection,
// serving up to maxKeepAlive requests over the same socket (HTTP Keep-Alive).
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
        clientIp_   = buf;
        remoteAddr_ = clientIp_ + ":" + std::to_string(ntohs(clientAddr.sin_port));
    }

    void handle() {
        // Idle timeout: if the client sends nothing for this long, recv() returns
        // and we close the socket, so a silent client cannot hold a thread forever.
        struct timeval tv { keepAliveTimeout_, 0 };
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int  reqCount  = 0;
        bool keepAlive = true;

        while (keepAlive && reqCount < maxKeepAlive_) {
            std::string rawRequest;
            ReadResult rr = readRequest(rawRequest);
            if (rr == ReadResult::Closed) break;
            if (rr == ReadResult::Bad) {
                sendAll(HttpResponse::badRequest().serialize(false));
                break;
            }
            if (rr == ReadResult::TooLarge) {
                HttpResponse res;
                res.statusCode = 413; res.statusText = "Payload Too Large";
                res.body = "<h1>413 Payload Too Large</h1>";
                sendAll(res.serialize(false));
                break;
            }

            HttpRequest req;
            req.remoteAddr = remoteAddr_;
            req.clientIp   = clientIp_;

            if (!HttpParser::parse(rawRequest, req)) {
                sendAll(HttpResponse::badRequest().serialize(false));
                break;
            }

            HttpResponse res;
            try {
                // Middleware pipeline -> router -> handler
                pipeline_.run(req, res, [&](HttpRequest& r, HttpResponse& resp) {
                    resp = router_.handle(r);
                });
            } catch (const std::exception& e) {
                // Last line of defence: a bug in one request must never take
                // down the whole server (an uncaught exception in a worker
                // thread calls std::terminate).
                LOG_ERROR("Unhandled exception for ", remoteAddr_, ": ", e.what());
                res = HttpResponse::internalError();
            }

            keepAlive = req.keepAlive() && (reqCount + 1 < maxKeepAlive_);
            if (!sendAll(res.serialize(keepAlive))) break;

            ++reqCount;
        }

        close(fd_);
    }

private:
    enum class ReadResult { Ok, Closed, Bad, TooLarge };

    int                 fd_;
    MiddlewarePipeline& pipeline_;
    const Router&       router_;
    int                 keepAliveTimeout_;
    int                 maxKeepAlive_;
    std::string         clientIp_;
    std::string         remoteAddr_;
    std::string         buffer_;   // bytes received but not yet used (supports pipelining)

    static constexpr size_t kMaxHeaderSize = 64 * 1024;

    // Reads exactly one request from the socket into `out`.
    // TCP is a byte stream, not messages: one recv() may hold half a request
    // or two requests, so we keep leftovers in buffer_ for the next call.
    ReadResult readRequest(std::string& out) {
        char buf[4096];

        while (true) {
            size_t hdrEnd = buffer_.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) {
                size_t bodyLen = 0;
                if (!findContentLength(buffer_.substr(0, hdrEnd), bodyLen)) {
                    return bodyLen > HttpParser::kMaxBodySize ? ReadResult::TooLarge
                                                              : ReadResult::Bad;
                }
                size_t total = hdrEnd + 4 + bodyLen;
                if (buffer_.size() >= total) {
                    out = buffer_.substr(0, total);
                    buffer_.erase(0, total);
                    return ReadResult::Ok;
                }
            } else if (buffer_.size() > kMaxHeaderSize) {
                return ReadResult::TooLarge;
            }

            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) return ReadResult::Closed;   // client closed, error, or idle timeout
            buffer_.append(buf, static_cast<size_t>(n));
        }
    }

    // Looks for a Content-Length header (case-insensitive) in the header block.
    // Returns false if it exists but is invalid or too large.
    static bool findContentLength(const std::string& headers, size_t& len) {
        len = 0;
        size_t pos = 0;
        while (pos < headers.size()) {
            size_t eol = headers.find("\r\n", pos);
            if (eol == std::string::npos) eol = headers.size();
            std::string line = headers.substr(pos, eol - pos);
            pos = eol + 2;

            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key != "content-length") continue;

            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();

            auto parsed = HttpParser::parseContentLength(val);
            if (!parsed) {
                // Tell the caller whether it was "too big" or "not a number"
                bool allDigits = !val.empty() &&
                    val.find_first_not_of("0123456789") == std::string::npos;
                len = allDigits ? HttpParser::kMaxBodySize + 1 : 0;
                return false;
            }
            len = *parsed;
        }
        return true;
    }

    // write() may send only part of the data, so keep going until all is sent.
    bool sendAll(const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }
};
