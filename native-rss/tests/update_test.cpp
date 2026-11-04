#include "update.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
std::string release(const std::string& tag, bool draft = false, bool prerelease = false,
                    const std::string& repo = "frevia/kindle-rss-reader", std::uint64_t size = 123) {
    const auto asset = "RSS-Reader-" + tag + "-Kindle.zip";
    return "{\"tag_name\":\"" + tag + "\",\"draft\":" + (draft ? "true" : "false") +
        ",\"prerelease\":" + (prerelease ? "true" : "false") +
        ",\"assets\":[{\"name\":\"" + asset + "\",\"browser_download_url\":\"https://github.com/" + repo +
        "/releases/download/" + tag + "/" + asset + "\",\"size\":" + std::to_string(size) +
        "},{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://github.com/" + repo +
        "/releases/download/" + tag + "/SHA256SUMS.txt\"}]}";
}
void rejects(const std::string& json, const std::string& current) {
    bool failed = false;
    try { (void)rss::parse_update_release(json, current); }
    catch (const std::exception&) { failed = true; }
    assert(failed);
}
void parse_tests() {
    assert(rss::parse_update_release(release("v0.10.0"), "v0.9.0").available);
    assert(!rss::parse_update_release(release("v0.9.0"), "v0.9.0").available);
    assert(!rss::parse_update_release(release("v0.8.9"), "v0.9.0").available);
    const auto mixed = std::string("{\"tag_name\":\"v0.10.0\",\"draft\":false,\"prerelease\":false,\"assets\":[") +
        "{\"name\":\"RSS-Reader-v0.10.0-KPW3.zip\",\"browser_download_url\":\"https://github.com/frevia/kindle-rss-reader/releases/download/v0.10.0/RSS-Reader-v0.10.0-KPW3.zip\",\"size\":123}," +
        "{\"name\":\"RSS-Reader-v0.10.0-Kindle.zip\",\"browser_download_url\":\"https://github.com/frevia/kindle-rss-reader/releases/download/v0.10.0/RSS-Reader-v0.10.0-Kindle.zip\",\"size\":124}," +
        "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":\"https://github.com/frevia/kindle-rss-reader/releases/download/v0.10.0/SHA256SUMS.txt\"}]}";
    const auto selected = rss::parse_update_release(mixed, "v0.9.0");
    assert(selected.asset_name == "RSS-Reader-v0.10.0-Kindle.zip" && selected.size == 124);
    auto legacy_only=release("v0.10.0");
    for(std::size_t at=0;(at=legacy_only.find("Kindle.zip",at))!=std::string::npos;at+=8)legacy_only.replace(at,10,"KPW3.zip");
    rejects(legacy_only, "v0.9.0");
    rejects(release("v0.10.0", true), "v0.9.0");
    rejects(release("v0.10.0", false, true), "v0.9.0");
    rejects("{bad", "v0.9.0");
    rejects(release("v0.10.0", false, false, "attacker/kindle-rss-reader"), "v0.9.0");
    rejects(release("v0.10.0", false, false, "frevia/kindle-rss-reader", 128ULL * 1024 * 1024 + 1), "v0.9.0");
    rejects("{\"tag_name\":\"v1.0.0-rc1\",\"draft\":false,\"prerelease\":false,\"assets\":[]}", "v0.9.0");
    rejects("{\"tag_name\":\"v0.10.0\",\"draft\":false,\"prerelease\":false,\"assets\":[]}", "v0.9.0");
    rejects("{\"tag_name\":\"v0.10.0\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"RSS-Reader-v0.10.0-KPW3.zip\",\"browser_download_url\":\"https://github.com/frevia/kindle-rss-reader/releases/download/v0.10.0/RSS-Reader-v0.10.0-KPW3.zip\",\"size\":123}]}", "v0.9.0");
}
std::string arg(char** argv, int index) { return argv[index]; }
}

int main(int argc, char** argv) {
    try {
        if (argc == 1) { parse_tests(); return 0; }
        if ((argc == 6 || argc == 7) && arg(argv, 1) == "stage") {
            if (argc == 7 && arg(argv, 6) != "cancel") throw std::runtime_error("invalid update_test arguments");
            std::atomic<bool> cancel{argc == 7};
            rss::stage_update(arg(argv, 3), arg(argv, 2), arg(argv, 4), arg(argv, 5), cancel);
        } else if (argc == 4 && arg(argv, 1) == "prepare") {
            std::ifstream input(arg(argv,3));std::ostringstream json;json<<input.rdbuf();
            const std::atomic<bool> cancel{false};
            rss::prepare_update(rss::parse_update_release(json.str(),rss::app_version()),arg(argv,2),cancel,[](const std::string&){});
        } else if (argc == 3 && arg(argv, 1) == "apply") rss::apply_update(arg(argv, 2));
        else if (argc == 3 && arg(argv, 1) == "recover") std::cout << (rss::recover_update(arg(argv, 2)) ? "1\n" : "0\n");
        else if (argc == 3 && arg(argv, 1) == "commit") rss::commit_update(arg(argv, 2));
        else if (argc == 3 && arg(argv, 1) == "backup") rss::backup_update_database(arg(argv, 2));
        else throw std::runtime_error("invalid update_test arguments");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
