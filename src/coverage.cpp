// SPDX-License-Identifier: GPL-3.0-or-later
#include "coverage.hpp"

#include <cstdlib>
#include <unistd.h>

namespace macha::coverage {

std::string child_profile_name(std::string_view pattern, long pid, std::string_view tag) {
    std::string name(pattern);
    const auto replacement = std::to_string(pid) + std::string(tag);
    for (auto at = name.find("%p"); at != std::string::npos; at = name.find("%p", at + replacement.size()))
        name.replace(at, 2, replacement);
    return name;
}

#if defined(MACHA_COVERAGE) && defined(__clang__)
extern "C" void __llvm_profile_reset_counters(void);
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);

void begin_child() {
    if (const char* pattern = std::getenv("LLVM_PROFILE_FILE"))
        __llvm_profile_set_filename(child_profile_name(pattern, ::getpid(), "-core").c_str());
    __llvm_profile_reset_counters();
}

void dump() {
    (void)__llvm_profile_write_file();
}
#else
void begin_child() {}
void dump() {}
#endif

} // namespace macha::coverage
