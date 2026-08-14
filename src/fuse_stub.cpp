// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_adapter.hpp"
#include <stdexcept>
namespace macha {
int run_fuse(FileSystem&, const std::filesystem::path&, bool) {
    throw std::runtime_error("FUSE support was not available when this binary was built");
}
} // namespace macha
