#pragma once
#include "http_types.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <regex>
#include <optional>

using Handler = std::function<HttpResponse(const HttpRequest&)>;

struct Route {
    std::string method;      // "GET", "POST", "" = any
    std::string pattern;     // e.g. "/users/:id"
    std::regex  regex;
    std::vector<std::string> paramNames;
    Handler     handler;
};

class Router {
public:
    void get(const std::string& path, Handler h)    { addRoute("GET",    path, std::move(h)); }
    void post(const std::string& path, Handler h)   { addRoute("POST",   path, std::move(h)); }
    void put(const std::string& path, Handler h)    { addRoute("PUT",    path, std::move(h)); }
    void del(const std::string& path, Handler h)    { addRoute("DELETE", path, std::move(h)); }
    void any(const std::string& path, Handler h)    { addRoute("",       path, std::move(h)); }

    HttpResponse handle(HttpRequest& req) const {
        bool pathFound = false;

        for (const auto& route : routes_) {
            std::smatch match;
            if (!std::regex_match(req.path, match, route.regex)) continue;
            pathFound = true;

            if (!route.method.empty() && route.method != req.method) continue;

            // Inject path params into queryParams
            for (size_t i = 0; i < route.paramNames.size(); ++i) {
                if (i + 1 < match.size())
                    req.queryParams[route.paramNames[i]] = match[i + 1].str();
            }

            try {
                return route.handler(req);
            } catch (const std::exception& e) {
                return HttpResponse::internalError(
                    std::string("<h1>500</h1><p>") + e.what() + "</p>");
            }
        }

        if (pathFound)
            return HttpResponse::methodNotAllowed();
        return HttpResponse::notFound();
    }

private:
    std::vector<Route> routes_;

    void addRoute(const std::string& method, const std::string& path, Handler h) {
        Route r;
        r.method  = method;
        r.pattern = path;
        r.handler = std::move(h);

        // Convert "/users/:id/posts/:pid" → regex + param names
        std::string regexStr;
        size_t i = 0;
        while (i < path.size()) {
            if (path[i] == ':') {
                size_t start = i + 1;
                while (i < path.size() && path[i] != '/') ++i;
                r.paramNames.push_back(path.substr(start, i - start));
                regexStr += "([^/]+)";
            } else if (path[i] == '*') {
                regexStr += "(.*)";
                ++i;
            } else {
                regexStr += std::regex_replace(
                    std::string(1, path[i]), std::regex(R"([.+?^${}()|[\]\\])"), R"(\$&)");
                ++i;
            }
        }
        r.regex = std::regex("^" + regexStr + "$");
        routes_.push_back(std::move(r));
    }
};