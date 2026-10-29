#include "home_bridge.hpp"
#include "home_patch.hpp"

#include <exception>
#include <string>

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "patch") {
        try {
            rss::apply_home_patch(argv[2], argv[3], argv[4]);
            return 0;
        } catch (const std::exception&) {
            return 1;
        }
    }
    try {
        return home_bridge::run_cli(argc, argv);
    } catch (const std::exception&) {
        return 1;
    }
}
