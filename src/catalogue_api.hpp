// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "catalogue.hpp"
#include "config.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>

namespace macha {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string, std::less<>> query;
    std::map<std::string, std::string, std::less<>> headers;
    Bytes body;
};

struct HttpResponse {
    int status{200};
    std::string content_type{"application/json; charset=utf-8"};
    std::map<std::string, std::string, std::less<>> headers;
    Bytes body;
};

class CatalogueApi {
    CatalogueManager& catalogue_;
  public:
    explicit CatalogueApi(CatalogueManager& catalogue) : catalogue_(catalogue) {}
    HttpResponse handle(const HttpRequest&);
};

class HttpServer {
    CatalogueApiConfig config_;
    std::function<HttpResponse(const HttpRequest&)> handler_;
    std::optional<std::string> bearer_token_;
    std::jthread thread_;
    std::atomic_bool running_{};
    int listen_fd_{-1};

    void run(std::stop_token);
    void handle_client(int);

  public:
    HttpServer(CatalogueApiConfig, std::function<HttpResponse(const HttpRequest&)>);
    ~HttpServer();
    void start();
    void stop();
    bool running() const { return running_.load(); }
};

} // namespace macha
