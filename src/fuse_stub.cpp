// SPDX-License-Identifier: GPL-3.0-or-later
#include "fuse_adapter.hpp"
#include "log.hpp"
namespace macha {
int run_fuse(FileSystem&, CacheHydrator&, const std::filesystem::path&, const FuseConfig&,
             std::function<void()>, std::function<void(std::weak_ptr<FuseFrontend>)>) {
    Log::error("FUSE mount requested but FUSE3 support was not available at build time");
    return 2;
}
} // namespace macha
