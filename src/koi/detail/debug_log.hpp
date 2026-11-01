#pragma once

// Developer debug log (best effort, never on UCI stdout/stderr).
//
// This lives in detail/ and is held by UciController behind a forward
// declaration + unique_ptr, so <fstream> and the rotation/JSON helpers do not
// leak into the public controller header.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace koi::detail {

class DebugLog {
public:
    DebugLog() = default;
    ~DebugLog();

    DebugLog(const DebugLog&) = delete;
    DebugLog& operator=(const DebugLog&) = delete;

    void set_executable_directory(std::filesystem::path directory);
    void set_requested_path(std::filesystem::path path);

    [[nodiscard]] bool enabled() const noexcept;
    void set_enabled(bool enabled) noexcept;

    // (Re)open the log file according to the current enabled/path state.
    void configure();

    // Write one JSON line. `fields` must already be JSON-formatted members
    // (without the outer braces); `event` is quoted here.
    void write_event(std::string_view event, std::string_view fields) noexcept;

private:
    [[nodiscard]] std::filesystem::path path() const;
    void configure_locked();
    void rotate_if_needed_locked(std::size_t incoming_bytes);

    mutable std::mutex mutex_;
    std::ofstream file_;
    std::filesystem::path executable_directory_;
    std::filesystem::path requested_path_;
    bool enabled_ = false;
};

} // namespace koi::detail
