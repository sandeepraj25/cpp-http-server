// Unit tests for the core components. No test framework needed:
//   make unit
// Each CHECK prints the failing line, and the program exits non-zero on failure.

#include "http_parser.h"
#include "router.h"
#include "middleware.h"
#include "thread_pool.h"

#include <atomic>
#include <iostream>
#include <string>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__            \
                      << "  " #cond "\n";                                    \
        }                                                                    \
    } while (0)

#define TEST(name) static void name()
#define RUN(name)  do { std::cout << "- " #name "\n"; name(); } while (0)

// ─── Parser ───────────────────────────────────────────────────────────────────

TEST(parses_request_line_headers_and_query) {
    HttpRequest req;
    std::string raw = "GET /echo?msg=hello%20world&x=1 HTTP/1.1\r\n"
                      "Host: localhost\r\nUser-Agent: test\r\n\r\n";
    CHECK(HttpParser::parse(raw, req));
    CHECK(req.method == "GET");
    CHECK(req.path == "/echo");
    CHECK(req.queryParams["msg"] == "hello world");
    CHECK(req.queryParams["x"] == "1");
    CHECK(req.headers["host"] == "localhost");        // keys are lowercased
    CHECK(req.headers["user-agent"] == "test");
}

TEST(parses_body_using_content_length) {
    HttpRequest req;
    std::string raw = "POST /users HTTP/1.1\r\ncontent-LENGTH: 5\r\n\r\nhello-extra";
    CHECK(HttpParser::parse(raw, req));
    CHECK(req.body == "hello");                        // trimmed to Content-Length
}

TEST(rejects_malformed_requests_without_throwing) {
    HttpRequest a, b, c, d;
    CHECK(!HttpParser::parse("GET / HTTP/1.1\r\nHost: x\r\n", a));            // no blank line
    CHECK(!HttpParser::parse("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n", b));
    CHECK(!HttpParser::parse("GET relative HTTP/1.1\r\n\r\n", c));            // path must start with /
    CHECK(!HttpParser::parse("GET / FOO/9\r\n\r\n", d));                      // unknown version
}

TEST(content_length_validation) {
    CHECK(HttpParser::parseContentLength("0").value() == 0);
    CHECK(HttpParser::parseContentLength("1024").value() == 1024);
    CHECK(!HttpParser::parseContentLength(""));
    CHECK(!HttpParser::parseContentLength("12a"));
    CHECK(!HttpParser::parseContentLength("-5"));
    CHECK(!HttpParser::parseContentLength("99999999999"));                   // over 1 MB limit
}

TEST(url_decode_handles_bad_escapes) {
    CHECK(HttpParser::urlDecode("a%20b") == "a b");
    CHECK(HttpParser::urlDecode("a+b") == "a b");
    CHECK(HttpParser::urlDecode("%zz") == "%zz");      // used to crash the server
    CHECK(HttpParser::urlDecode("100%") == "100%");
    CHECK(HttpParser::urlDecode("%4") == "%4");
}

TEST(json_escape_prevents_injection) {
    CHECK(jsonEscape("plain") == "plain");
    CHECK(jsonEscape("a\"b") == "a\\\"b");
    CHECK(jsonEscape("line\nbreak") == "line\\nbreak");
    CHECK(jsonEscape("back\\slash") == "back\\\\slash");
}

// ─── Router ───────────────────────────────────────────────────────────────────

static HttpRequest makeReq(const std::string& method, const std::string& path) {
    HttpRequest r;
    r.method = method;
    r.path   = path;
    return r;
}

TEST(router_matches_static_and_dynamic_routes) {
    Router router;
    router.get("/health", [](const HttpRequest&) { return HttpResponse::ok("up"); });
    router.get("/users/:id", [](const HttpRequest& r) {
        return HttpResponse::ok("user " + r.queryParams.at("id"));
    });
    router.get("/a/:x/b/:y", [](const HttpRequest& r) {
        return HttpResponse::ok(r.queryParams.at("x") + "-" + r.queryParams.at("y"));
    });

    auto r1 = makeReq("GET", "/health");
    CHECK(router.handle(r1).body == "up");

    auto r2 = makeReq("GET", "/users/42");
    CHECK(router.handle(r2).body == "user 42");

    auto r3 = makeReq("GET", "/a/1/b/2");
    CHECK(router.handle(r3).body == "1-2");
}

TEST(router_returns_404_and_405) {
    Router router;
    router.get("/users/:id", [](const HttpRequest&) { return HttpResponse::ok("x"); });

    auto missing = makeReq("GET", "/nope");
    CHECK(router.handle(missing).statusCode == 404);

    auto wrongMethod = makeReq("POST", "/users/1");
    CHECK(router.handle(wrongMethod).statusCode == 405);

    auto extraSegment = makeReq("GET", "/users/1/extra");
    CHECK(router.handle(extraSegment).statusCode == 404);
}

TEST(router_turns_handler_exceptions_into_500) {
    Router router;
    router.get("/boom", [](const HttpRequest&) -> HttpResponse {
        throw std::runtime_error("secret internal detail");
    });
    auto req = makeReq("GET", "/boom");
    auto res = router.handle(req);
    CHECK(res.statusCode == 500);
    CHECK(res.body.find("secret") == std::string::npos);   // no internal detail leaked
}

// ─── Rate limiter ─────────────────────────────────────────────────────────────

TEST(rate_limiter_is_a_sliding_window) {
    using namespace std::chrono;
    SlidingWindowRateLimiter limiter(3, 10);   // 3 requests per 10 seconds
    auto t0 = SlidingWindowRateLimiter::Clock::now();
    int retry = 0;

    CHECK(limiter.allow("1.1.1.1", t0, retry));
    CHECK(limiter.allow("1.1.1.1", t0 + seconds(1), retry));
    CHECK(limiter.allow("1.1.1.1", t0 + seconds(2), retry));
    CHECK(!limiter.allow("1.1.1.1", t0 + seconds(3), retry));   // 4th within window
    CHECK(retry >= 1 && retry <= 7);

    // At t0+10s only the first request has left the window -> exactly one slot
    CHECK(limiter.allow("1.1.1.1", t0 + seconds(10), retry));
    CHECK(!limiter.allow("1.1.1.1", t0 + seconds(10), retry));

    // A fixed window would have reset at t0+10s and allowed 3 more here.
}

TEST(rate_limiter_is_per_ip) {
    SlidingWindowRateLimiter limiter(1, 60);
    auto t0 = SlidingWindowRateLimiter::Clock::now();
    int retry = 0;
    CHECK(limiter.allow("10.0.0.1", t0, retry));
    CHECK(!limiter.allow("10.0.0.1", t0, retry));
    CHECK(limiter.allow("10.0.0.2", t0, retry));               // different client, own budget
}

TEST(rate_limiter_forgets_idle_clients) {
    using namespace std::chrono;
    SlidingWindowRateLimiter limiter(5, 1);
    auto t0 = SlidingWindowRateLimiter::Clock::now();
    int retry = 0;
    for (int i = 0; i < 2000; ++i)
        limiter.allow("ip-" + std::to_string(i), t0, retry);
    // Later traffic triggers a sweep that removes the 2000 idle IPs
    for (int i = 0; i < 1100; ++i)
        limiter.allow("active", t0 + seconds(5) + milliseconds(i), retry);
    CHECK(limiter.trackedClients() < 10);
}

TEST(rate_limiter_is_thread_safe) {
    SlidingWindowRateLimiter limiter(1000, 60);
    std::atomic<int> allowed{0};
    {
        ThreadPool pool(8);
        for (int i = 0; i < 2000; ++i) {
            pool.enqueue([&] {
                int retry = 0;
                if (limiter.allow("same-ip", SlidingWindowRateLimiter::Clock::now(), retry))
                    ++allowed;
            });
        }
        pool.waitAll();
    }
    CHECK(allowed == 1000);   // exactly the limit, never more, even under contention
}

// ─── Thread pool ──────────────────────────────────────────────────────────────

TEST(thread_pool_runs_every_task) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < 10000; ++i) pool.enqueue([&] { ++counter; });
        pool.waitAll();
    }
    CHECK(counter == 10000);
}

TEST(thread_pool_survives_throwing_tasks) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        pool.enqueue([] { throw std::runtime_error("bad task"); });
        for (int i = 0; i < 100; ++i) pool.enqueue([&] { ++counter; });
        pool.waitAll();
    }
    CHECK(counter == 100);   // workers kept running after the exception
}

// ─── Middleware ───────────────────────────────────────────────────────────────

TEST(middleware_runs_in_order_and_can_stop_the_chain) {
    MiddlewarePipeline pipeline;
    std::string trace;
    pipeline.use([&](HttpRequest&, HttpResponse&, NextFn next) { trace += "A"; next(); trace += "a"; });
    pipeline.use([&](HttpRequest& req, HttpResponse& res, NextFn next) {
        trace += "B";
        if (req.path == "/blocked") { res.statusCode = 403; return; }
        next();
    });

    HttpRequest ok; ok.path = "/ok"; HttpResponse r1;
    pipeline.run(ok, r1, [&](HttpRequest&, HttpResponse&) { trace += "H"; });
    CHECK(trace == "ABHa");

    trace.clear();
    HttpRequest blocked; blocked.path = "/blocked"; HttpResponse r2;
    pipeline.run(blocked, r2, [&](HttpRequest&, HttpResponse&) { trace += "H"; });
    CHECK(trace == "ABa");          // handler skipped
    CHECK(r2.statusCode == 403);
}

int main() {
    std::cout << "Running unit tests\n";
    RUN(parses_request_line_headers_and_query);
    RUN(parses_body_using_content_length);
    RUN(rejects_malformed_requests_without_throwing);
    RUN(content_length_validation);
    RUN(url_decode_handles_bad_escapes);
    RUN(json_escape_prevents_injection);
    RUN(router_matches_static_and_dynamic_routes);
    RUN(router_returns_404_and_405);
    RUN(router_turns_handler_exceptions_into_500);
    RUN(rate_limiter_is_a_sliding_window);
    RUN(rate_limiter_is_per_ip);
    RUN(rate_limiter_forgets_idle_clients);
    RUN(rate_limiter_is_thread_safe);
    RUN(thread_pool_runs_every_task);
    RUN(thread_pool_survives_throwing_tasks);
    RUN(middleware_runs_in_order_and_can_stop_the_chain);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
