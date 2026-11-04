#pragma once
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace rss {
// Direct argv execution, no shell; nonzero exits and deadlines raise an error.
void run_bounded(const std::vector<std::string>& args, std::chrono::milliseconds deadline);
std::string capture_bounded(const std::vector<std::string>& args, std::chrono::milliseconds deadline, std::size_t max_bytes, bool include_stderr=false);
// Must run before acquiring device resources or starting threads.
void bind_to_launcher(const std::string& parent_pid);
}
