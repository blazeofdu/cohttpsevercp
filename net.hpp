#pragma once

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>

// 是我们新建的网络基础工具文件，专门放服务器和客户端都会用到的底层网络函数。

namespace net {

    // 循环调用 write，直到 data 中的全部字节都发送完成。
    inline bool write_all(int fd, std::string_view data) {
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

    // 将 socket 设置为非阻塞，供 epoll 事件循环使用。
    inline void set_nonblocking(int fd) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            throw std::system_error(errno, std::system_category(), "fcntl F_GETFL");
        }
        if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::system_error(errno, std::system_category(), "fcntl F_SETFL");
        }
    }

} // namespace  net
