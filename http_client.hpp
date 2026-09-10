#pragma once

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <map>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include "net.hpp"
struct http_client {
    struct http_response {
        int status = 0;
        std::string raw_headers;
        std::map<std::string, std::string> headers;
        std::string body;
    };




    // 持续读取服务端响应，直到服务端关闭连接。
    static std::string read_until_close(int fd) {
        std::string result;
        char buffer[4096];

        // 当前服务端使用 Connection: close，所以读到 EOF 就表示响应结束。
        while (true) {
            ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("read response failed");
            }
            if (n == 0) {
                break;
            }
            result.append(buffer, static_cast<size_t>(n));
        }
        return result;
    }

    // 解析响应头中的键值对，键统一转换成小写。
    static std::map<std::string, std::string> parse_headers(
        std::string_view header) {
        std::map<std::string, std::string> headers;
        size_t line_start = header.find("\r\n");

        while (line_start != std::string_view::npos) {
            line_start += 2;
            size_t line_end = header.find("\r\n", line_start);
            std::string_view line = header.substr(
                line_start,
                line_end == std::string_view::npos
                    ? std::string_view::npos
                    : line_end - line_start);
            if (!line.empty()) {
                size_t colon = line.find(':');
                if (colon != std::string_view::npos) {
                    std::string key(line.substr(0, colon));
                    std::string_view value = line.substr(colon + 1);

                    // 只移动视图的起点和终点，不反复搬移字符串内容。
                    while (!value.empty() &&
                           std::isspace(static_cast<unsigned char>(value.front()))) {
                        value.remove_prefix(1);
                    }
                    while (!value.empty() &&
                           std::isspace(static_cast<unsigned char>(value.back()))) {
                        value.remove_suffix(1);
                    }
                    for (char &c : key) {
                        c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
                    }
                    headers[std::move(key)] = std::string(value);
                }
            }
            if (line_end == std::string_view::npos) {
                break;
            }
            line_start = line_end;
        }
        return headers;
    }

    // 读取响应头并按照 Content-Length 接收完整响应体。
    static std::string read_response(int fd) {
        std::string result;
        char buffer[4096];
        size_t header_end = std::string::npos;

        // 先读取到响应头结束，不能在此之前判断响应体长度。
        while (header_end == std::string::npos) {
            ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("read response header failed");
            }
            if (n == 0) {
                throw std::runtime_error("response ended before headers");
            }
            result.append(buffer, static_cast<size_t>(n));
            header_end = result.find("\r\n\r\n");
        }

        std::string_view header(result.data(), header_end);
        auto headers = parse_headers(header);
        size_t content_length = 0;
        bool has_content_length = false;
        auto content_length_it = headers.find("content-length");
        if (content_length_it != headers.end()) {
            try {
                size_t used = 0;
                content_length = static_cast<size_t>(
                    std::stoull(content_length_it->second, &used));
                if (used != content_length_it->second.size()) {
                    throw std::runtime_error("invalid Content-Length");
                }
                has_content_length = true;
            } catch (const std::exception &) {
                throw std::runtime_error("invalid Content-Length");
            }
        }

        size_t body_start = header_end + 4;
        if (!has_content_length) {
            // 没有 Content-Length 时，退回到 Connection: close 的规则。
            result += read_until_close(fd);
            return result;
        }

        // 当前 result 可能已经包含一部分 body，继续读取剩余部分。
        while (result.size() - body_start < content_length) {
            ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("read response body failed");
            }
            if (n == 0) {
                throw std::runtime_error("response body is incomplete");
            }
            result.append(buffer, static_cast<size_t>(n));
        }

        // 只保留一个完整响应，后续多余字节属于下一条数据。
        result.resize(body_start + content_length);
        return result;
    }

    // 从原始 HTTP 响应中提取状态码、响应头和响应体。
    static http_response parse_response(std::string raw) {
        size_t header_end = raw.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            throw std::runtime_error("invalid HTTP response");
        }

        std::string_view header(raw.data(), header_end);
        size_t line_end = header.find("\r\n");
        if (line_end == std::string_view::npos) {
            throw std::runtime_error("invalid HTTP status line");
        }

        std::string_view status_line = header.substr(0, line_end);
        size_t first_space = status_line.find(' ');
        size_t second_space = status_line.find(' ', first_space + 1);
        if (first_space == std::string_view::npos ||
            second_space == std::string_view::npos) {
            throw std::runtime_error("invalid HTTP status line");
        }

        http_response response;
        response.status = std::stoi(std::string(
            status_line.substr(first_space + 1,
                               second_space - first_space - 1)));
        response.raw_headers = std::string(header);
        response.headers = parse_headers(header);
        response.body = raw.substr(header_end + 4);
        return response;
    }

    // 发送一个 HTTP 请求，并返回服务端的响应。
    static http_response request(const char *ip, uint16_t port,
                                 std::string_view method,
                                 std::string_view path,
                                 std::string_view body = {}) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            throw std::runtime_error("socket failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
            ::close(fd);
            throw std::runtime_error("invalid IPv4 address");
        }

        // 客户端主动连接服务端，而服务端使用 accept 等待客户端。
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr),
                      sizeof(addr)) == -1) {
            ::close(fd);
            throw std::runtime_error("connect failed");
        }

        std::string request_text = std::string(method) + " " +
            std::string(path) + " HTTP/1.1\r\n";
        request_text += "Host: " + std::string(ip) + "\r\n";
        request_text += "Connection: close\r\n";
        if (!body.empty() || method == "POST") {
            request_text += "Content-Type: text/plain; charset=utf-8\r\n";
            request_text += "Content-Length: " +
                std::to_string(body.size()) + "\r\n";
        }
        request_text += "\r\n";
        request_text += body;

        if (!net::write_all(fd, request_text)) {
            ::close(fd);
            throw std::runtime_error("write request failed");
        }

        std::string raw_response = read_response(fd);
        ::close(fd);
        return parse_response(std::move(raw_response));
    }

    // 发送 GET 请求。GET 通常只携带 URL，不携带请求体。
    static http_response get(const char *ip, uint16_t port,
                             std::string_view path) {
        return request(ip, port, "GET", path);
    }

    // 发送 POST 请求，并把 body 作为请求体发送给服务端。
    static http_response post(const char *ip, uint16_t port,
                             std::string_view path,
                             std::string_view body) {
        return request(ip, port, "POST", path, body);
    }
};
