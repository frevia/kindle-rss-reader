#pragma once

#include <cstdint>
#include <string>

namespace home_bridge {

struct Options {
    std::string database = "/mnt/us/native-rss/data/rss.sqlite";
    std::uint16_t port = 12681;
    std::string session_lock = "/var/local/native-rss/session.lock";
    std::string state;
    std::string open_dir;
};

int run_summary(const Options& options);
int run_server(const Options& options);
int run_cli(int argc, char** argv);

} // namespace home_bridge
