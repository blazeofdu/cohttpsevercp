#include <iostream>

#include "http_client.hpp"

int main() {
    try {
        // 先启动 learning_demo，再运行这个客户端。
        auto response = http_client::get("127.0.0.1", 8080, "/inspect?name=abc");

        std::cout << "status: " << response.status << '\n';
        std::cout << "headers:\n" << response.raw_headers << "\n\n";
        std::cout << "body:\n" << response.body << '\n';
        for (const auto &header : response.headers) {
            std::cout << header.first << " = " << header.second << '\n';
        }
        auto echo_response = http_client::post(
           "127.0.0.1", 8080, "/echo", "hello from client");
        std::cout << "POST status: " << echo_response.status << '\n';
        std::cout << "POST body: " << echo_response.body << '\n';
        
    } catch (const std::exception &e) {
        std::cerr << "client error: " << e.what() << '\n';
        return 1;
    }
}
