cmake_minimum_required(VERSION 3.12)
project(co_http LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

add_executable(main main.cpp
        http_server.hpp
        client.cpp
        http_client.hpp
        net.hpp)