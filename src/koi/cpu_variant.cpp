#include "koi/cpu_variant.hpp"

#include <cstddef>

namespace koi {
namespace {

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

std::optional<CpuVariant> parse_cpu_variant(std::string_view text) noexcept {
    text = trim(text);
    if (equals_ignore_case(text, "avx512")) {
        return CpuVariant::avx512;
    }
    if (equals_ignore_case(text, "avx2")) {
        return CpuVariant::avx2;
    }
    if (equals_ignore_case(text, "generic")) {
        return CpuVariant::generic;
    }
    return std::nullopt;
}

std::string_view cpu_variant_executable_name(CpuVariant variant) noexcept {
#ifdef _WIN32
    switch (variant) {
    case CpuVariant::generic:
        return "koi-engine.exe";
    case CpuVariant::avx2:
        return "koi-engine-avx2.exe";
    case CpuVariant::avx512:
        return "koi-engine-avx512.exe";
    }
    return "koi-engine.exe";
#else
    switch (variant) {
    case CpuVariant::generic:
        return "koi-engine";
    case CpuVariant::avx2:
        return "koi-engine-avx2";
    case CpuVariant::avx512:
        return "koi-engine-avx512";
    }
    return "koi-engine";
#endif
}

CpuVariant choose_cpu_variant(bool avx512_supported, bool avx2_supported,
                              bool avx512_available, bool avx2_available,
                              std::optional<CpuVariant> forced,
                              bool fast_pext) noexcept {
    if (forced.has_value()) {
        switch (*forced) {
        case CpuVariant::generic:
            return CpuVariant::generic;
        case CpuVariant::avx512:
            if (avx512_available) {
                return CpuVariant::avx512;
            }
            break;
        case CpuVariant::avx2:
            if (avx2_available) {
                return CpuVariant::avx2;
            }
            break;
        }
    }
    // Both optimized siblings use _pext_u64 in the attack-table build.  On a
    // host where PEXT is microcoded (pre-Zen3 AMD) the baseline build is
    // faster, so require fast PEXT before auto-selecting an optimized sibling.
    if (avx512_supported && avx512_available && fast_pext) {
        return CpuVariant::avx512;
    }
    if (avx2_supported && avx2_available && fast_pext) {
        return CpuVariant::avx2;
    }
    return CpuVariant::generic;
}

} // namespace koi
