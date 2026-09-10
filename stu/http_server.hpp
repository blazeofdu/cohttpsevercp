#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "learning_http_server.hpp"

static void die(const char *msg) {
    std::cerr << msg << ": " << std::strerror(errno) << '\n';
    std::exit(1);
}

int main() {
    auto server = http_server::make();

    server->get_router().route("/", [](http_server::http_request &req) {
        req.write_response(200, "hello");
    });

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        die("socket");
    }

    int yes = 1;
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        die("setsockopt");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        die("inet_pton");
    }

    if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == -1) {
        die("bind");
    }

    if (::listen(listen_fd, 16) == -1) {
        die("listen");
    }

    std::cout << "listening on http://127.0.0.1:8080\n";

    while (true) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int conn_fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (conn_fd == -1) {
            die("accept");
        }

        http_server::http_request_parser parser;
        char buf[4096];

        while (true) {
            ssize_t n = ::read(conn_fd, buf, sizeof(buf));
            if (n == -1) {
                die("read");
            }
            if (n == 0) {
                break;
            }

            if (parser.feed(std::string_view(buf, static_cast<size_t>(n)))) {
                break;
            }
        }

        std::cout << "method: " << static_cast<int>(parser.request.method) << '\n';
        std::cout << "url: " << parser.request.url << '\n';
        std::cout << "body: " << parser.request.body << '\n';

        server->get_router().do_handle(parser.request);

        std::string response =
            "HTTP/1.1 " + std::to_string(parser.request.response_status) + " OK\r\n"
            "Content-Type: " + parser.request.response_content_type + "\r\n"
            "Content-Length: " + std::to_string(parser.request.response_content.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" +
            parser.request.response_content;

        if (::write(conn_fd, response.data(), response.size()) == -1) {
            die("write");
        }

        ::close(conn_fd);
    }
}
