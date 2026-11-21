#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/nnue.hpp"
#include "koi_test_support.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path fixture_path;

int run_fixture(std::string_view arch, const std::filesystem::path* output = nullptr) {
#if defined(_WIN32)
    const auto quote_argument = [](std::wstring_view argument) {
        std::wstring quoted{L"\""};
        std::size_t backslashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++backslashes;
            } else if (character == L'"') {
                quoted.append(backslashes * 2U + 1U, L'\\');
                quoted.push_back(character);
                backslashes = 0;
            } else {
                quoted.append(backslashes, L'\\');
                backslashes = 0;
                quoted.push_back(character);
            }
        }
        quoted.append(backslashes * 2U, L'\\');
        quoted.push_back(L'"');
        return quoted;
    };
    std::wstring command = quote_argument(fixture_path.wstring());
    command += L" --arch " + quote_argument(std::wstring(arch.begin(), arch.end()));
    if (output != nullptr) {
        command += L" --output " + quote_argument(output->wstring());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(fixture_path.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return static_cast<int>(GetLastError());
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
#else
    std::string executable = fixture_path.string();
    std::string arch_flag = "--arch";
    std::string arch_value(arch);
    std::string output_flag = "--output";
    std::string output_value = output != nullptr ? output->string() : std::string{};
    std::vector<char*> argv{executable.data(), arch_flag.data(), arch_value.data()};
    if (output != nullptr) {
        argv.push_back(output_flag.data());
        argv.push_back(output_value.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return errno != 0 ? errno : 1;
    }
    if (pid == 0) {
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return errno != 0 ? errno : 1;
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("fixture generator did not create " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_architectures_generate_deterministic_loadable_distinct_containers() {
    koi::test::TempDirectory scratch;
    const auto v4_first = scratch.file("v4-first.nnue");
    const auto v4_second = scratch.file("v4-second.nnue");
    const auto v5 = scratch.file("v5.nnue");
    const auto v5_second = scratch.file("v5-second.nnue");

    koi::test::require(run_fixture("v4", &v4_first) == 0,
                       "v4 fixture generation should succeed");
    koi::test::require(run_fixture("v4", &v4_second) == 0,
                       "repeated v4 fixture generation should succeed");
    koi::test::require(run_fixture("v5", &v5) == 0,
                       "v5 fixture generation should succeed");
    koi::test::require(run_fixture("v5", &v5_second) == 0,
                       "repeated v5 fixture generation should succeed");

    const auto v4_first_bytes = read_bytes(v4_first);
    const auto v4_second_bytes = read_bytes(v4_second);
    const auto v5_bytes = read_bytes(v5);
    const auto v5_second_bytes = read_bytes(v5_second);
    koi::test::require(!v4_first_bytes.empty() && v4_first_bytes == v4_second_bytes,
                       "repeated v4 generation must produce identical bytes");
    koi::test::require(!v5_bytes.empty(), "v5 generation must produce bytes");
    koi::test::require(v5_bytes == v5_second_bytes,
                       "repeated v5 generation must produce identical bytes");
    koi::test::require(v4_first_bytes != v5_bytes,
                       "v4 and v5 fixture containers must have distinct identities");

    const auto loaded_v4 = koi::NnueLoader::load_file(v4_first);
    const auto loaded_v5 = koi::NnueLoader::load_file(v5);
    koi::test::require(loaded_v4.has_value() && loaded_v4->manifest.version == 4,
                       "generated v4 fixture must be accepted as NNUE v4");
    koi::test::require(loaded_v5.has_value() && loaded_v5->manifest.version == 5,
                       "generated v5 fixture must be accepted as NNUE v5");
    koi::test::require(loaded_v5->manifest.layer_sizes[1] == 1536 &&
                           loaded_v5->manifest.layer_sizes[3] == 32,
                       "generated v5 fixture must use the large hidden 1536 and L1 32 dimensions");
}

void test_invalid_architecture_and_missing_arguments_are_rejected() {
    koi::test::TempDirectory scratch;
    const auto invalid_output = scratch.file("invalid.nnue");
    koi::test::require(run_fixture("v6", &invalid_output) != 0,
                       "an unsupported architecture must return a nonzero status");
    koi::test::require(!std::filesystem::exists(invalid_output),
                       "an unsupported architecture must not create an output file");
    koi::test::require(run_fixture("v4") != 0,
                       "missing required output argument must return a nonzero status");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argv[1] == nullptr) {
        return 2;
    }
    fixture_path = std::filesystem::path(argv[1]);
    const std::vector<koi::test::TestCase> tests{
        {"deterministic loadable architecture fixtures",
         test_architectures_generate_deterministic_loadable_distinct_containers},
        {"invalid fixture arguments", test_invalid_architecture_and_missing_arguments_are_rejected},
    };
    return koi::test::run_tests(tests, argc, argv);
}
