#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

#include "koi/nnue.hpp"
#include "../../tests/support/nnue_synthetic.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "1.2.0"
#endif
#ifndef KOI_ENGINE_BUILD_COMMIT
#define KOI_ENGINE_BUILD_COMMIT "unknown"
#endif

namespace {

constexpr std::string_view kUsage =
    "usage: koi-nnue-fixture --arch v4|v5|v9 --output <path>\n";
constexpr std::uint32_t kV5HiddenUnits = 1'536;
constexpr std::uint32_t kV5L1Units = 32;

struct Arguments {
    std::string_view arch;
    std::filesystem::path output;
};

koi::NnueNetwork large_v5_fixture() {
    koi::NnueNetwork network = koi::test::synthetic_network_v5();
    // Keep the seed weights while value-initializing the added dimensions.
    // A large v5 network shape keeps the v5 fixture exercising its stress path.
    network.manifest.layer_sizes[1] = kV5HiddenUnits;
    network.manifest.layer_sizes[3] = kV5L1Units;

    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.l1_weights.resize(l1_units * hidden_units);
    network.l1_bias.resize(l1_units);
    network.bottleneck_weights.resize(output_buckets * l1_units);
    return network;
}

std::optional<Arguments> parse_arguments(int argc, char** argv) {
    std::optional<std::string_view> arch;
    std::optional<std::filesystem::path> output;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--arch") {
            if (arch.has_value() || index + 1 >= argc || argv[index + 1] == nullptr) {
                return std::nullopt;
            }
            arch = argv[++index];
        } else if (argument == "--output") {
            if (output.has_value() || index + 1 >= argc || argv[index + 1] == nullptr) {
                return std::nullopt;
            }
            output = std::filesystem::path(argv[++index]);
        } else {
            return std::nullopt;
        }
    }

    if (!arch.has_value() || !output.has_value() || output->empty() ||
        (*arch != "v4" && *arch != "v5" && *arch != "v9")) {
        return std::nullopt;
    }
    return Arguments{*arch, std::move(*output)};
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr) {
            const std::string_view argument(argv[index]);
            if (argument == "-h" || argument == "--help") {
                std::cout << kUsage;
                return 0;
            }
            if (argument == "-v" || argument == "--version") {
                std::cout << "koi-nnue-fixture " << KOI_ENGINE_BUILD_VERSION << " (" << KOI_ENGINE_BUILD_COMMIT << ")\n";
                return 0;
            }
        }
    }
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments.has_value()) {
        std::cerr << kUsage;
        return 2;
    }

    koi::NnueNetwork network;
    if (arguments->arch == "v4") {
        network = koi::test::synthetic_network_v4();
    } else if (arguments->arch == "v5") {
        network = large_v5_fixture();
    } else {
        network = koi::test::synthetic_network_v9();
    }
    const auto encoded = koi::NnueLoader::serialize(network);
    if (!encoded.has_value()) {
        std::cerr << "unable to serialize NNUE fixture: " << encoded.error().message << '\n';
        return 1;
    }

    std::ofstream output(arguments->output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "unable to open NNUE fixture output: " << arguments->output.string() << '\n';
        return 1;
    }
    output.write(reinterpret_cast<const char*>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
    output.close();
    if (!output) {
        std::cerr << "unable to write NNUE fixture output: " << arguments->output.string() << '\n';
        return 1;
    }
    return 0;
}
