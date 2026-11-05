#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace rss {
struct UpdateRelease {
    std::string version, notes, asset_url, checksums_url, asset_name;
    std::uint64_t size = 0;
    bool available = false;
};
std::string app_version();
UpdateRelease parse_update_release(const std::string& json, const std::string& current);
UpdateRelease parse_update_releases(const std::string& json, const std::string& current);
using UpdateProgress = std::function<void(const std::string&)>;
UpdateRelease check_update(const std::atomic<bool>& cancel, const UpdateProgress& progress = {});
void prepare_update(const UpdateRelease& release, const std::filesystem::path& root,
                    const std::atomic<bool>& cancel, const UpdateProgress& progress);
// Shared with offline fixture tests; never writes installed files.
void stage_update(const std::filesystem::path& archive, const std::filesystem::path& root,
                  const std::string& version, const std::string& sha256, const std::atomic<bool>& cancel);
void apply_update(const std::filesystem::path& root, const UpdateProgress& progress = {});
bool recover_update(const std::filesystem::path& root);
void commit_update(const std::filesystem::path& root);
void backup_update_database(const std::filesystem::path& root, const UpdateProgress& progress = {});
}
