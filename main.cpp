#include <iostream>

#include "http_server.hpp"

int main() {
    // 创建服务器对象；真正的网络细节由 http_server 管理。
    auto server = http_server::make();

    // 注册路由：path 匹配成功后执行对应的回调函数。
    server->get_router().route("/", [](http_server::http_request &req) {
        req.write_response(200, "hello");
    });
    server->get_router().route("/echo", [](http_server::http_request &req) {
        req.write_response(200, "echo: " + req.body);
    });
    server->get_router().route("/json", [](http_server::http_request &req) {
        req.write_response(200, R"({"ok":true})", "application/json; charset=utf-8");
    });
    server->get_router().route("/inspect", [](http_server::http_request &req) {
        // 这个路由把解析出的请求信息全部返回，方便调试。
        std::string text;
        text += "method=" + std::to_string(static_cast<int>(req.method)) + "\n";
        text += "url=" + req.url + "\n";
        text += "path=" + req.path + "\n";
        text += "query=" + req.query + "\n";
        text += "query_params:\n";
        for (const auto &kv : req.query_params) {
            text += "  " + kv.first + ": " + kv.second + "\n";
        }
        text += "body=" + req.body + "\n";
        text += "headers:\n";
        for (const auto &kv : req.headers) {
            text += "  " + kv.first + ": " + kv.second + "\n";
        }
        req.write_response(200, text);
    });

    // 启动服务器：绑定本机 8080 端口并进入 accept 循环。
    // server->run("127.0.0.1", 8080);
    server->run_epoll("127.0.0.1", 8080);
}
