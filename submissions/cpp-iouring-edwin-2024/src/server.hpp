#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include "db.hpp"
#include "jwt.hpp"

namespace net {

struct ServerConfig {
    std::string host{"0.0.0.0"};
    int port{80};
    std::string sqlite_path{"feed.db"};
    std::string jwt_secret{"twelve-dollar-challenge"};
    uint32_t max_conns{65536};
};

class Server {
public:
    Server(ServerConfig config);
    ~Server();

    bool init();
    void run();
    void stop();

private:
    ServerConfig config_;
    db::Database db_;
    sec::JwtVerifier jwt_;
    int listen_fd_{-1};
    bool running_{false};
    int64_t start_time_s_{0};

    bool setup_listener();
};

} // namespace net
