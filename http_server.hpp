#pragma once

#include <functional>
#include <cerrno>
#include <array>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>
#include <arpa/inet.h>

// 这是同步版 HTTP 服务器：
// 先完成 socket、请求解析、路由和响应，再学习异步和 epoll。

struct http_server : std::enable_shared_from_this<http_server> {
    using pointer = std::shared_ptr<http_server>;

    // 监听 socket 的文件描述符。-1 表示当前还没有创建。
    int listen_fd = -1;

    static pointer make() {
        // 用 shared_ptr 管理服务器对象的生命周期。
        return std::make_shared<http_server>();
    }

    static std::string status_text(int status) {
        switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "OK";
        }
    }

    enum class http_method {
        UNKNOWN = -1,
        GET,
        POST,
        PUT,
        DELETE,
        HEAD,
        OPTIONS,
        PATCH,
        TRACE,
        CONNECT,
    };

    struct http_request {
        // 下面这些字段保存客户端发来的 HTTP 请求。
        std::string url;
        std::string path;
        std::string query;
        std::string body;
        http_method method = http_method::UNKNOWN;
        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> query_params;

        // 下面这些字段保存路由处理后要返回给客户端的响应。
        int response_status = 0;
        std::string response_content;
        std::string response_content_type = "text/plain;charset=utf-8";
        bool response_ready = false;

        void write_response(
            int status,
            std::string_view content,
            std::string_view content_type = "text/plain;charset=utf-8") {
            // 路由回调通过这个函数设置响应。
            response_status = status;
            response_content.assign(content.begin(), content.end());
            response_content_type.assign(content_type.begin(), content_type.end());
            response_ready = true;
        }
    };

    static std::string url_decode(std::string_view s) {
        // query 参数中的 '+' 代表空格，%XX 代表一个十六进制字节。
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (c == '+') {
                out.push_back(' ');
            } else if (c == '%' && i + 2 < s.size()) {
                auto hex = [](char ch) -> int {
                    if ('0' <= ch && ch <= '9') return ch - '0';
                    if ('a' <= ch && ch <= 'f') return ch - 'a' + 10;
                    if ('A' <= ch && ch <= 'F') return ch - 'A' + 10;
                    return -1;
                };
                int hi = hex(s[i + 1]);
                int lo = hex(s[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                } else {
                    out.push_back(c);
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    static void parse_query_string(http_request &request) {
        // 把 name=Tom&age=18 拆成 query_params 中的键值对。
        request.query_params.clear();
        if (request.query.empty()) {
            return;
        }

        size_t pos = 0;
        while (pos <= request.query.size()) {
            size_t amp = request.query.find('&', pos);
            std::string_view part = std::string_view(request.query).substr(
                pos,
                amp == std::string::npos ? std::string_view::npos : amp - pos);
            size_t eq = part.find('=');
            std::string key;
            std::string value;
            if (eq == std::string_view::npos) {
                key = url_decode(part);
            } else {
                key = url_decode(part.substr(0, eq));
                value = url_decode(part.substr(eq + 1));
            }
            if (!key.empty()) {
                request.query_params[key] = value;
            }
            if (amp == std::string::npos) {
                break;
            }
            pos = amp + 1;
        }
    }

    struct   http_request_parser {
        // buffer 保存目前已经从 socket 读取到的全部请求数据。
        std::string buffer;
        http_request request;
        size_t content_length = 0;
        bool header_ready = false;
        bool body_ready = false;
        bool parse_failed = false;

        void reset() {
            buffer.clear();
            request = {};
            content_length = 0;
            header_ready = false;
            body_ready = false;
            parse_failed = false;
        }

        bool failed() const {
            return parse_failed;
        }

        bool finished() const {
            return body_ready;
        }

        bool feed(std::string_view chunk) {
            // 一次 read 不一定读完整个请求，所以每次都追加到 buffer。
            buffer.append(chunk.begin(), chunk.end());

            if (!header_ready) {
                // HTTP 头部以两个连续的 CRLF 结束。
                size_t header_end = buffer.find("\r\n\r\n");
                if (header_end == std::string::npos) {
                    return false;
                }

                std::string_view head(buffer.data(), header_end);
                size_t line_end = head.find("\r\n");
                if (line_end == std::string::npos) {
                    parse_failed = true;
                    return true;
                }
                // 请求行格式：方法 URL HTTP/1.1。
                std::string_view request_line = head.substr(0, line_end);

                size_t first_space = request_line.find(' ');
                size_t second_space = request_line.find(' ', first_space + 1);
                if (first_space == std::string_view::npos ||
                    second_space == std::string_view::npos ||
                    first_space == 0 ||
                    second_space == first_space + 1 ||
                    second_space + 1 >= request_line.size() ||
                    request_line.substr(second_space + 1) != "HTTP/1.1") {
                    parse_failed = true;
                    return true;
                }

                {
                    std::string method_text(request_line.substr(0, first_space));
                    if (method_text == "GET") {
                        request.method = http_method::GET;
                    } else if (method_text == "POST") {
                        request.method = http_method::POST;
                    } else if (method_text == "PUT") {
                        request.method = http_method::PUT;
                    } else if (method_text == "DELETE") {
                        request.method = http_method::DELETE;
                    } else {
                        request.method = http_method::UNKNOWN;
                    }

                    // 从请求行中取出 URL，再拆成 path 和 query。
                    request.url = std::string(
                        request_line.substr(first_space + 1,
                                            second_space - first_space - 1));
                    size_t query_mark = request.url.find('?');
                    if (query_mark == std::string::npos) {
                        request.path = request.url;
                        request.query.clear();
                    } else {
                        request.path = request.url.substr(0, query_mark);
                        request.query = request.url.substr(query_mark + 1);
                    }
                    parse_query_string(request);
                }

                // 从请求行之后开始逐行解析请求头。
                size_t pos = line_end;
                while (pos != std::string_view::npos) {
                    pos += 2;
                    size_t next = head.find("\r\n", pos);
                    std::string_view line = head.substr(
                        pos, next == std::string_view::npos ? std::string::npos : next - pos);
                    size_t colon = line.find(':');
                    if (colon != std::string_view::npos) {
                        std::string key(line.substr(0, colon));
                        std::string value(line.substr(colon + 1));
                        if (!value.empty() && value.front() == ' ') {
                            value.erase(value.begin());
                        }
                        for (char &c : key) {
                            if ('A' <= c && c <= 'Z') {
                                c += 'a' - 'A';
                            }
                        }
                        request.headers[key] = value;
                        if (key == "content-length") {
                            try {
                                size_t used = 0;
                                content_length = static_cast<size_t>(
                                    std::stoull(value, &used));
                                if (used != value.size()) {
                                    parse_failed = true;
                                }
                            } catch (...) {
                                parse_failed = true;
                            }
                        }
                    }

                    if (next == std::string::npos) {
                        break;
                    }
                    pos = next;
                }

                if (parse_failed) {
                    return true;
                }

                // header_end 后面的内容属于请求体，可能暂时还不完整。
                request.body = buffer.substr(header_end + 4);
                header_ready = true;
            }

            if (!header_ready) {
                return false;
            }

            if (request.body.size() < content_length) {
                return false;
            }

            // 已经收到 Content-Length 指定的字节数，请求解析完成。
            request.body.resize(content_length);
            body_ready = true;
            return true;
        }
    };

    struct connection_state {
        http_request_parser parser;
        std::string response;
        size_t sent = 0;
    };

    struct http_router {
        // 路由表：请求 path 对应一个处理函数。
        std::map<std::string, std::function<void(http_request &)>> m_routes;

        void route(std::string url, std::function<void(http_request &)> cb) {
            m_routes.insert_or_assign(std::move(url), std::move(cb));
        }

        void do_handle(http_request &request) {
            // 根据 path 查找路由，不使用 query 部分匹配。
            auto it = m_routes.find(request.path);
            if (it != m_routes.end()) {
                std::cout << "[route hit] " << request.path << '\n';
                it->second(request);
                return;
            }

            std::cout << "[route miss] " << request.path << '\n';
            request.write_response(404, "404 Not Found");
        }
    };

    static std::string make_response(const http_request &req) {
        // 把响应状态、响应头和响应体拼成 HTTP 字节流。
        std::cout << "[response status] " << req.response_status << '\n';
        std::cout << "[response body size] " << req.response_content.size() << '\n';
        std::cout << "[response body]\n" << req.response_content << '\n';
        std::string response =
            "HTTP/1.1 " + std::to_string(req.response_status) + " " + status_text(req.response_status) + "\r\n" +
            "Content-Type: " + req.response_content_type + "\r\n" +
            "Content-Length: " + std::to_string(req.response_content.size()) + "\r\n" +
            "Connection: close\r\n" +
            "\r\n" + req.response_content;
        return response;
    }

    static bool write_all(int fd, std::string_view data) {
        // write 可能只发送一部分，因此循环直到全部发送完成。
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
            if (n == -1) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void handle_connection(int conn_fd) {
        // 处理一个客户端连接的一次 HTTP 请求。
        http_request_parser parser;
        char buf[4096];

        while (true) {
            // 读取 TCP 字节流，并交给请求解析器。
            ssize_t n = ::read(conn_fd, buf, sizeof(buf));
            if (n == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::system_category(), "read");
            }
            if (n == 0) {
                break;
            }

            if (parser.feed(std::string_view(buf, static_cast<size_t>(n)))) {
                break;
            }
        }

        // 客户端提前断开或请求格式错误时，不进入路由。
        if (!parser.finished()) {
            parser.request.write_response(400, "400 Bad Request");
        } else {
            std::cout << "method: " << static_cast<int>(parser.request.method) << '\n';
            std::cout << "url: " << parser.request.url << '\n';
            std::cout << "path: " << parser.request.path << '\n';
            std::cout << "query: " << parser.request.query << '\n';
            std::cout << "body: " << parser.request.body << '\n';

            // 请求完整后，交给路由回调处理。
            m_router.do_handle(parser.request);
        }

        std::string response = make_response(parser.request);
        if (!write_all(conn_fd, response)) {
            throw std::system_error(errno, std::system_category(), "write");
        }
    }

    void run(const char *ip, uint16_t port) {
        // 创建并启动监听 socket，然后不断接收客户端连接。
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd == -1) {
            throw std::system_error(errno, std::system_category(), "socket");
        }

        // 允许程序重启后尽快重新绑定同一个端口。
        int yes = 1;
        if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                         &yes, sizeof(yes)) == -1) {
            throw std::system_error(errno, std::system_category(), "setsockopt");
        }

        // sockaddr_in 保存 IPv4 地址和端口。
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { //
            throw std::runtime_error("invalid IPv4 address");
        }

        if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr),
                   sizeof(addr)) == -1) {
            throw std::system_error(errno, std::system_category(), "bind");
        }

        if (::listen(listen_fd, 16) == -1) {
            throw std::system_error(errno, std::system_category(), "listen");
        }

        std::cout << "listening on http://" << ip << ':' << port << '\n';

        // accept 会等待下一个客户端连接。
        while (true) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            int conn_fd = ::accept(listen_fd,
                                   reinterpret_cast<sockaddr *>(&peer),
                                   &peer_len);
            if (conn_fd == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::system_category(), "accept");
            }

            // 当前版本按顺序处理连接，一次只处理一个客户端。
            try {
                handle_connection(conn_fd);
            } catch (...) {
                ::close(conn_fd);
                throw;
            }
            ::close(conn_fd);
        }
    }


    static void set_nonblocking(int fd) { // 非阻塞
        int flags = ::fcntl(fd, F_GETFL, 0); // 当前状态
        if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) { // 添加状态
            throw std::system_error(errno, std::system_category(), "fcntl");
        }
    }

      // 使用 epoll 同时管理多个客户端连接。
    void run_epoll(const char *ip, uint16_t port) {
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);// ipv4 +tcp
        if (listen_fd == -1) {
            throw std::system_error(errno, std::system_category(), "socket");
        }
        set_nonblocking(listen_fd);

        int yes = 1;
        if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                         &yes, sizeof(yes)) == -1) {
            throw std::system_error(errno, std::system_category(), "setsockopt");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
            throw std::runtime_error("invalid IPv4 address");
        }
        if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr),
                   sizeof(addr)) == -1) {
            throw std::system_error(errno, std::system_category(), "bind");
        }
        if (::listen(listen_fd, 16) == -1) {
            throw std::system_error(errno, std::system_category(), "listen");
        }

        int epfd = ::epoll_create1(0); // Linux 提供的 I/O 多路复用机制，可以让一个线程同时监视很多 socket，
        if (epfd == -1) {
            throw std::system_error(errno, std::system_category(), "epoll_create1");
        }

        epoll_event listen_event{};
        listen_event.events = EPOLLIN;//有新的连接
        listen_event.data.fd = listen_fd; // 这个事件打上标签
        // 把 listen_fd 加入 epfd，当它有新连接时通知我。
        if (::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &listen_event) == -1) {
            ::close(epfd);
            throw std::system_error(errno, std::system_category(), "epoll_ctl");
        }

        std::map<int, connection_state> connections;// epoll 服务器保存连接状态和事件结果的两个容器。
        std::array<epoll_event, 64> events{}; //
        std::cout << "epoll server listening on http://" << ip << ':'
                  << port << '\n';

        while (true) {
            int count = ::epoll_wait(epfd, events.data(),
                                     static_cast<int>(events.size()), -1); //将需要处理的数组传入events
            if (count == -1) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(epfd);
                throw std::system_error(errno, std::system_category(),
                                        "epoll_wait");
            }

            for (int i = 0; i < count; ++i) {
                int fd = events[i].data.fd;
                uint32_t event_flags = events[i].events;

                if (fd == listen_fd) {
                    // ET 模式下要循环 accept，直到暂时没有新连接。
                    while (true) {
                        sockaddr_in peer{};
                        socklen_t peer_len = sizeof(peer);
                        int conn_fd = ::accept(
                            listen_fd,
                            reinterpret_cast<sockaddr *>(&peer),
                            &peer_len);
                        if (conn_fd == -1) {
                            if (errno == EINTR) { // 信号中断
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) { //当前已经没有更多客户端连接可以 accept
                                break;
                            }
                            ::close(epfd);
                            throw std::system_error(
                                errno, std::system_category(), "accept");
                        }

                        set_nonblocking(conn_fd);
                        connections.emplace(conn_fd, connection_state{});
                        epoll_event conn_event{};
                        conn_event.events = EPOLLIN | EPOLLET; //监听可读事件使用和 边缘触发模式：只有状态从“没有数据”变成“有数据”时通知一次
                        conn_event.data.fd = conn_fd;
                        if (::epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd,
                        &conn_event) == -1) { // 把客户端 socket 加入 epoll 监控。
                            ::close(conn_fd);
                            connections.erase(conn_fd);
                            throw std::system_error(errno,std::system_category(),"epoll_ctl ADD");
                        }
                    }
                    continue;
                }

                auto it = connections.find(fd);
                if (it == connections.end()) {
                    continue;
                }

                if ((event_flags & (EPOLLERR | EPOLLHUP)) != 0) { // 是否连接关闭或者断开
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);//移除连接
                    ::close(fd);
                    connections.erase(it);
                    continue;
                }

                connection_state &connection = it->second;
                if ((event_flags & EPOLLIN) != 0) {//可读事件使用
                    bool request_ready = false;
                    while (true) {
                        char buffer[4096];
                        ssize_t n = ::read(fd, buffer, sizeof(buffer));
                        if (n == -1) {
                            if (errno == EINTR) { //read 正在等待或读取时，被信号中断了。这部分数据没有消失
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) { //当前已经没有更多数据可以读取了。
                                break;
                            }
                            //没读完的处理
                            n = 0;// 其他异常
                        }
                        if (n == 0) {//异常就删除这个事件
                            ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                            ::close(fd);
                            connections.erase(it);
                            request_ready = false;
                            break;
                        }
                        //读完了
                        if (connection.parser.feed(std::string_view(
                                buffer, static_cast<size_t>(n)))) {
                            request_ready = true;
                            break;
                        }
                    }

                    if (connections.find(fd) == connections.end()) {
                        continue;
                    }//事件已经不存在了

                    if (request_ready) {
                        if (!connection.parser.finished()) { // 整个连接是否解析完成
                            connection.parser.request.write_response(
                                400, "400 Bad Request"); // HTTP/1.1 400 Bad Request
                        } else {
                            m_router.do_handle(connection.parser.request); //处理头
                        }
                        connection.response = make_response(
                            connection.parser.request);
                        connection.sent = 0;

                        epoll_event write_event{};
                        write_event.events = EPOLLOUT | EPOLLET; //把当前客户端 socket 改成监听“可写事件”，并使用边缘触发模式，告诉监控内核的发送缓冲区还有空间，
                       // 现在调用 write() 不会因为暂时写不进去而阻塞。
                        write_event.data.fd = fd;
                        if (::epoll_ctl(epfd, EPOLL_CTL_MOD, fd,&write_event) == -1) {
                            ::close(fd);
                            connections.erase(fd);
                            continue;
                        }

                    }
                }

                if ((event_flags & EPOLLOUT) != 0 &&
                    connections.find(fd) != connections.end()) { // 当前事件是不是“客户端 socket 可以写”，并且这个客户端连接还存在。
                    connection_state &current = connections.at(fd);
                    while (current.sent < current.response.size()) { // 一直写直到完成发送
                        ssize_t n = ::write(
                            fd,
                            current.response.data() + current.sent,
                            current.response.size() - current.sent);
                        if (n == -1) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            n = 0;
                        }
                        if (n == 0) {
                            ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                            ::close(fd);
                            connections.erase(fd);
                            break;
                        }
                        current.sent += static_cast<size_t>(n);
                    }

                    if (connections.find(fd) != connections.end() && // 这个事件的任务完成关闭，并删除
                        connections.at(fd).sent ==
                            connections.at(fd).response.size()) {
                        ::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                        ::close(fd);
                        connections.erase(fd);
                    }
                }
            }
        }
    }

    ~http_server() {
        // 服务器对象销毁时关闭监听 socket。
        if (listen_fd != -1) {
            ::close(listen_fd);
        }
    }

    http_router m_router;

    http_router &get_router() {
        return m_router;
    }
};
