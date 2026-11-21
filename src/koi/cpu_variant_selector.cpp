#include "koi/cpu_variant.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "koi/cpu_features.hpp"
#include "koi/executable_path.hpp"

namespace koi {
namespace {

#ifdef _WIN32

// Launches `variant` with the parent's standard handles and waits for it. The
// child is placed in a job object that dies with the parent so a GUI that kills
// `koi-engine.exe` cannot leave an orphaned search process behind. Returns the
// child's exit code, or -1 when the child could not be started at all.
int launch_variant(const std::filesystem::path& directory, CpuVariant variant,
                   bool explicit_choice) noexcept {
    const std::filesystem::path child_path =
        directory / std::filesystem::path(cpu_variant_executable_name(variant));
    std::wstring command_line = L"\"" + child_path.wstring() + L"\"";

    // Duplicate the standard handles so the child keeps working when the parent
    // inherited non-inheritable pipe handles from a GUI. When duplication is not
    // possible (no console at all), fall back to plain handle inheritance.
    const HANDLE sources[3] = {::GetStdHandle(STD_INPUT_HANDLE),
                               ::GetStdHandle(STD_OUTPUT_HANDLE),
                               ::GetStdHandle(STD_ERROR_HANDLE)};
    HANDLE duplicates[3] = {nullptr, nullptr, nullptr};
    bool any_duplicated = false;
    for (int index = 0; index < 3; ++index) {
        if (sources[index] == nullptr || sources[index] == INVALID_HANDLE_VALUE) {
            continue;
        }
        if (::DuplicateHandle(::GetCurrentProcess(), sources[index], ::GetCurrentProcess(),
                              &duplicates[index], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            any_duplicated = true;
        } else {
            duplicates[index] = nullptr;
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (any_duplicated) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = duplicates[0];
        startup.hStdOutput = duplicates[1];
        startup.hStdError = duplicates[2];
    }

    // While the selector waits, the child should own the console's Ctrl+C.
    const BOOL ignored_ctrl_c = ::SetConsoleCtrlHandler(nullptr, TRUE);

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        (void)::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits));
    }

    PROCESS_INFORMATION process{};
    const BOOL created =
        ::CreateProcessW(child_path.c_str(), command_line.data(), nullptr, nullptr, TRUE, 0,
                         nullptr, directory.c_str(), &startup, &process);

    for (HANDLE& handle : duplicates) {
        if (handle != nullptr) {
            ::CloseHandle(handle);
            handle = nullptr;
        }
    }

    if (!created) {
        const DWORD error = ::GetLastError();
        if (job != nullptr) {
            ::CloseHandle(job);
        }
        if (ignored_ctrl_c) {
            (void)::SetConsoleCtrlHandler(nullptr, FALSE);
        }
        if (explicit_choice) {
            std::fprintf(stderr, "koi-engine: could not start %s (error %lu).\n",
                         child_path.string().c_str(), static_cast<unsigned long>(error));
        }
        return -1;
    }

    if (job != nullptr) {
        (void)::AssignProcessToJobObject(job, process.hProcess);
    }

    (void)::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    (void)::GetExitCodeProcess(process.hProcess, &exit_code);

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (job != nullptr) {
        ::CloseHandle(job);
    }
    if (ignored_ctrl_c) {
        (void)::SetConsoleCtrlHandler(nullptr, FALSE);
    }
    return static_cast<int>(exit_code);
}

#elif defined(__linux__) || defined(__APPLE__)

// Replaces this process with `variant` through execv: the child keeps the
// standard streams, the process id, and the controlling terminal, so a GUI or
// tournament sees exactly one engine process. execv returns only when the child
// could not be started; the caller then keeps running the baseline build unless
// the variant was requested explicitly.
int launch_variant(const std::filesystem::path& directory, CpuVariant variant,
                   bool explicit_choice) noexcept {
    const std::filesystem::path child_path =
        directory / std::filesystem::path(cpu_variant_executable_name(variant));
    std::string path_text = child_path.string();
    char* arguments[] = {path_text.data(), nullptr};
    ::execv(path_text.c_str(), arguments);
    if (explicit_choice) {
        std::fprintf(stderr, "koi-engine: could not start %s.\n", path_text.c_str());
    }
    return -1;
}

#else

int launch_variant(const std::filesystem::path&, CpuVariant, bool) noexcept {
    return -1;
}

#endif // platform launch

} // namespace

int run_cpu_selector() noexcept {
    const std::filesystem::path executable = current_executable_path();
    if (executable.empty()) {
        return -1;
    }
    const std::filesystem::path directory = executable.parent_path();

    std::optional<CpuVariant> forced;
    if (const char* text = std::getenv("KOI_CPU_VARIANT"); text != nullptr && *text != '\0') {
        forced = parse_cpu_variant(text);
    }

    std::error_code exists_error;
    const bool avx512_available = std::filesystem::exists(
        directory / std::filesystem::path(cpu_variant_executable_name(CpuVariant::avx512)),
        exists_error);
    exists_error.clear();
    const bool avx2_available = std::filesystem::exists(
        directory / std::filesystem::path(cpu_variant_executable_name(CpuVariant::avx2)),
        exists_error);

    const CpuVariant chosen = choose_cpu_variant(cpu_supports_avx512(), cpu_supports_avx2(),
                                                 avx512_available, avx2_available, forced,
                                                 cpu_supports_fast_pext());
    if (chosen == CpuVariant::generic) {
        return -1;
    }
    return launch_variant(directory, chosen, forced.has_value());
}

} // namespace koi
