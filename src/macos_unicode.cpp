// SPDX-License-Identifier: GPL-3.0-or-later
#include "macos_unicode.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <limits>

namespace macha {
namespace {

#if defined(__APPLE__)
std::string normalize_utf8(std::string_view value, CFStringNormalizationForm form) {
    if (value.empty())
        return {};
    if (value.size() > static_cast<size_t>(std::numeric_limits<CFIndex>::max()))
        return std::string(value);

    auto source = CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
    if (!source)
        return std::string(value);

    auto normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, source);
    CFRelease(source);
    if (!normalized)
        return std::string(value);

    CFStringNormalize(normalized, form);
    const auto chars = CFStringGetLength(normalized);
    const auto maximum = CFStringGetMaximumSizeForEncoding(chars, kCFStringEncodingUTF8);
    if (maximum < 0) {
        CFRelease(normalized);
        return std::string(value);
    }

    std::string out(static_cast<size_t>(maximum) + 1, '\0');
    if (!CFStringGetCString(normalized, out.data(), static_cast<CFIndex>(out.size()),
                            kCFStringEncodingUTF8)) {
        CFRelease(normalized);
        return std::string(value);
    }
    CFRelease(normalized);
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}
#endif

} // namespace

std::string macos_fuse_decomposed_name(std::string_view name) {
#if defined(__APPLE__)
    return normalize_utf8(name, kCFStringNormalizationFormD);
#else
    return std::string(name);
#endif
}

std::string macos_fuse_composed_name(std::string_view name) {
#if defined(__APPLE__)
    return normalize_utf8(name, kCFStringNormalizationFormC);
#else
    return std::string(name);
#endif
}

} // namespace macha
