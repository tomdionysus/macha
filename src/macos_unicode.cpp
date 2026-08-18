// SPDX-License-Identifier: GPL-3.0-or-later
#include "macos_unicode.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <limits>

namespace macha {

std::string macos_fuse_decomposed_name(std::string_view name) {
#if defined(__APPLE__)
    if (name.empty())
        return {};
    if (name.size() > static_cast<size_t>(std::numeric_limits<CFIndex>::max()))
        return std::string(name);

    auto source = CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(name.data()),
        static_cast<CFIndex>(name.size()), kCFStringEncodingUTF8, false);
    if (!source)
        return std::string(name);

    auto normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
    CFRelease(source);
    if (!normalized)
        return std::string(name);

    CFStringNormalize(normalized, kCFStringNormalizationFormD);
    const auto chars = CFStringGetLength(normalized);
    const auto maximum = CFStringGetMaximumSizeForEncoding(chars, kCFStringEncodingUTF8);
    if (maximum < 0) {
        CFRelease(normalized);
        return std::string(name);
    }

    std::string out(static_cast<size_t>(maximum) + 1, '\0');
    if (!CFStringGetCString(normalized, out.data(), static_cast<CFIndex>(out.size()),
                            kCFStringEncodingUTF8)) {
        CFRelease(normalized);
        return std::string(name);
    }
    CFRelease(normalized);
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
#else
    return std::string(name);
#endif
}

} // namespace macha
