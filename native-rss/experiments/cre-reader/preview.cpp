// Shows one rendered probe page through the existing screen-session recovery path.
#include "device.hpp"
#include "process.hpp"
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "probe") {
            std::cout << rss::probe_device();
            return 0;
        }
        const auto guard = std::getenv("RSS_UI_GUARD");
        const auto owner = std::getenv("RSS_LAUNCHER_PID");
        const auto page = std::getenv("RSS_CRE_PREVIEW");
        if (!guard || std::string(guard) != "1" || !page)
            throw std::runtime_error("Use an isolated copy of the existing screen-session launcher");
        rss::bind_to_launcher(owner ? owner : "");
        std::ifstream input(page, std::ios::binary);
        std::string magic;
        int width = 0, height = 0, maximum = 0;
        input >> magic >> width >> height >> maximum;
        input.get();
        if (!input || magic != "P5" || width != 1072 || height != 1448 || maximum != 255)
            throw std::runtime_error("Expected a probe PGM with 1072x1448 8-bit pixels");
        std::vector<std::uint8_t> pixels(1072 * 1448);
        input.read(reinterpret_cast<char*>(pixels.data()), pixels.size());
        if (input.gcount() != static_cast<std::streamsize>(pixels.size()) || input.peek() != EOF)
            throw std::runtime_error("Truncated or oversized probe page");
        std::signal(SIGTERM, stop);
        std::signal(SIGINT, stop);
        std::signal(SIGHUP, stop);
        rss::KindleDevice hardware;
        hardware.present(pixels);
        while (!stopped && !hardware.touch(100)) {}
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "preview: " << error.what() << '\n';
        return 1;
    }
}
