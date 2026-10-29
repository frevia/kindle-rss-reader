#pragma once
#include <string>

namespace rss {
// Verify both digests and all bounds before atomically creating a patched copy.
// The input firmware is never modified. Throws on invalid recipes or output paths.
void apply_home_patch(const std::string& original, const std::string& recipe,
                      const std::string& output);
}
