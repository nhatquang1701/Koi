#include "koi/detail/debug_log.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace koi::detail {
namespace {

constexpr std::uintmax_t kDebugRotationBytes = 8U * 1024U * 1024U;

[[nodiscard]] bool is_openable_regular_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return !error;
    }
    if (error) {
        return false;
    }
    return std::filesystem::is_regular_file(path, error) && !error;
}

[[nodiscard]] std::string quoted(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '\\':
        case '"':
            result.push_back('\\');
            result.push_back(character);
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                result += "\\u00";
                constexpr char kHex[] = "0123456789abcdef";
                result.push_back(kHex[(static_cast<unsigned char>(character) >> 4) & 0xF]);
                result.push_back(kHex[static_cast<unsigned char>(character) & 0xF]);
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

} // namespace

DebugLog::~DebugLog() {
    std::lock_guard lock(mutex_);
    if (file_.is_open()) {
        file_.close();
    }
}

void DebugLog::set_executable_directory(std::filesystem::path directory) {
    std::lock_guard lock(mutex_);
    executable_directory_ = std::move(directory);
}

void DebugLog::set_requested_path(std::filesystem::path path) {
    std::lock_guard lock(mutex_);
    requested_path_ = std::move(path);
}

bool DebugLog::enabled() const noexcept {
    std::lock_guard lock(mutex_);
    return enabled_;
}

void DebugLog::set_enabled(bool enabled) noexcept {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
}

std::filesystem::path DebugLog::path() const {
    const std::filesystem::path base = executable_directory_.empty() ?
        std::filesystem::current_path() : executable_directory_;
    const std::filesystem::path requested = requested_path_.empty() ?
        std::filesystem::path{"koi-debug.log"} : requested_path_;
    return requested.is_absolute() ? requested : base / requested;
}

void DebugLog::configure_locked() {
    if (file_.is_open()) {
        file_.close();
    }
    if (!enabled_) {
        return;
    }
    const std::filesystem::path target = path();
    std::error_code error;
    const bool oversized = std::filesystem::exists(target, error) && !error &&
        std::filesystem::file_size(target, error) >= kDebugRotationBytes;
    if (oversized && !error) {
        rotate_if_needed_locked(0);
    }
    if (!is_openable_regular_file(target)) {
        return;
    }
    file_.open(target, std::ios::binary | std::ios::app);
}

void DebugLog::configure() {
    std::lock_guard lock(mutex_);
    configure_locked();
}

void DebugLog::rotate_if_needed_locked(std::size_t incoming_bytes) {
    const std::filesystem::path target = path();
    std::error_code error;
    const std::uintmax_t current_size =
        std::filesystem::exists(target, error) && !error ? std::filesystem::file_size(target, error)
                                                       : 0;
    if (error || (current_size < kDebugRotationBytes &&
                  incoming_bytes <= kDebugRotationBytes - current_size)) {
        return;
    }
    if (file_.is_open()) {
        file_.close();
    }
    for (int backup = 2; backup >= 1; --backup) {
        const std::filesystem::path source = target.string() + "." + std::to_string(backup);
        const std::filesystem::path destination = target.string() + "." + std::to_string(backup + 1);
        std::filesystem::remove(destination, error);
        error.clear();
        if (std::filesystem::exists(source, error) && !error) {
            std::filesystem::rename(source, destination, error);
            error.clear();
        }
    }
    std::filesystem::rename(target, target.string() + ".1", error);
}

void DebugLog::write_event(std::string_view event, std::string_view fields) noexcept {
    try {
        std::lock_guard lock(mutex_);
        if (!enabled_) {
            return;
        }
        if (!file_.is_open()) {
            if (!is_openable_regular_file(path())) {
                return;
            }
            file_.open(path(), std::ios::binary | std::ios::app);
        }
        if (!file_.is_open()) {
            return;
        }
        std::string message = "{\"event\":" + quoted(event);
        if (!fields.empty()) {
            message += ',';
            message += fields;
        }
        message += "}\n";
        rotate_if_needed_locked(message.size());
        if (!file_.is_open()) {
            if (!is_openable_regular_file(path())) {
                return;
            }
            file_.open(path(), std::ios::binary | std::ios::app);
        }
        if (!file_.is_open()) {
            return;
        }
        file_.write(message.data(), static_cast<std::streamsize>(message.size()));
        file_.flush();
    } catch (...) {
        // Developer diagnostics are strictly best effort and never affect UCI.
    }
}

} // namespace koi::detail
