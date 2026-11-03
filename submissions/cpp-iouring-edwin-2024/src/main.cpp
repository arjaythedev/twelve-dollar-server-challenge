#include "server.hpp"
#include <iostream>
#include <cstdlib>
#include <csignal>

static net::Server* g_server = nullptr;

void signal_handler(int sig) {
    if (g_server) {
        g_server->stop();
    }
}

int main(int argc, char** argv) {
    net::ServerConfig config;

    const char* env_db = std::getenv("SQLITE_PATH");
    if (env_db && *env_db) config.sqlite_path = env_db;

    const char* env_secret = std::getenv("JWT_SECRET");
    if (env_secret && *env_secret) config.jwt_secret = env_secret;

    const char* env_host = std::getenv("HOST");
    if (env_host && *env_host) config.host = env_host;

    const char* env_port = std::getenv("PORT");
    if (env_port && *env_port) config.port = std::atoi(env_port);

    std::cout << "Starting 12-Dollar Server Challenge C++23 Server...\n"
              << "Host: " << config.host << ":" << config.port << "\n"
              << "DB:   " << config.sqlite_path << "\n";

    net::Server server(config);
    g_server = &server;

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!server.init()) {
        std::cerr << "Initialization failed!\n";
        return 1;
    }

    server.run();
    return 0;
}
