// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

#ifndef LANES
#define LANES 320
#endif

#ifndef GROUP_LANES
#define GROUP_LANES 16
#endif

#ifndef DSP_ADD_GROUPS
#define DSP_ADD_GROUPS 8
#endif

#ifndef KERNEL_CLOCK_MHZ
#define KERNEL_CLOCK_MHZ 300
#endif

namespace {

struct Options {
    std::string xclbin;
    uint32_t device = 0;
    uint32_t iterations = 100000000;
    uint32_t repeats = 3;
    uint32_t verify_iterations = 1024;
    float alpha = 0.5f;
    float beta = 0.25f;
    uint32_t seed = 0x005a17u;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " -x <xclbin> [options]\n"
              << "  -x, --xclbin <file>       FPGA binary (required)\n"
              << "  -d, --device <id>         XRT device index (default: 0)\n"
              << "  -i, --iterations <n>      timed compute iterations (default: 100000000)\n"
              << "  -r, --repeats <n>         timed kernel launches (default: 3)\n"
              << "      --verify-iterations   short validation/warm-up run (default: 1024)\n"
              << "      --alpha <value>       multiply operand (default: 0.5)\n"
              << "      --beta <value>        add operand (default: 0.25)\n";
}

uint32_t parse_u32(const std::string& text, const char* name, bool allow_zero = false) {
    size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed, 0);
    if (consumed != text.size() || value > std::numeric_limits<uint32_t>::max() ||
        (!allow_zero && value == 0)) {
        throw std::invalid_argument(std::string("invalid ") + name + ": " + text);
    }
    return static_cast<uint32_t>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    if (std::getenv("XCL_EMULATION_MODE") != nullptr) {
        options.iterations = 4096;
        options.repeats = 1;
        options.verify_iterations = 64;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* name) -> std::string {
            if (++i >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return argv[i];
        };

        if (arg == "-x" || arg == "--xclbin") {
            options.xclbin = require_value(arg.c_str());
        } else if (arg == "-d" || arg == "--device") {
            options.device = parse_u32(require_value(arg.c_str()), "device", true);
        } else if (arg == "-i" || arg == "--iterations") {
            options.iterations = parse_u32(require_value(arg.c_str()), "iterations");
        } else if (arg == "-r" || arg == "--repeats") {
            options.repeats = parse_u32(require_value(arg.c_str()), "repeats");
        } else if (arg == "--verify-iterations") {
            options.verify_iterations =
                parse_u32(require_value(arg.c_str()), "verify iterations");
        } else if (arg == "--alpha") {
            options.alpha = std::stof(require_value(arg.c_str()));
        } else if (arg == "--beta") {
            options.beta = std::stof(require_value(arg.c_str()));
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }

    if (options.xclbin.empty()) {
        throw std::invalid_argument("-x/--xclbin is required");
    }
    if (!std::isfinite(options.alpha) || !std::isfinite(options.beta)) {
        throw std::invalid_argument("alpha and beta must be finite");
    }
    return options;
}

union FloatBits {
    float value;
    uint32_t bits;
};

uint32_t float_to_bits(float value) {
    FloatBits converted;
    converted.value = value;
    return converted.bits;
}

float bits_to_float(uint32_t bits) {
    FloatBits converted;
    converted.bits = bits;
    return converted.value;
}

uint32_t cpu_reference(uint32_t seed, uint32_t iterations, float alpha, float beta) {
    std::vector<uint32_t> lane_checksums(LANES);
    for (int lane = 0; lane < LANES; ++lane) {
        const uint32_t lane_seed = seed + static_cast<uint32_t>(lane) * 0x001f123bu;
        lane_checksums[lane] = lane_seed ^ 0x9e3779b9u;
    }

    for (uint32_t iter = 0; iter < iterations; ++iter) {
        for (int lane = 0; lane < LANES; ++lane) {
            const uint32_t lane_seed =
                seed + static_cast<uint32_t>(lane) * 0x001f123bu;
            const uint32_t input_bits =
                0x3f000000u | ((iter ^ (lane_seed & 0x007fffffu)) & 0x007fffffu);
            // Force the same two roundings as the separate HLS fmul/fadd
            // operators.  The default 0.5 and 0.25 are exactly representable.
            volatile float product = bits_to_float(input_bits) * alpha;
            const float result = product + beta;
            lane_checksums[lane] ^= float_to_bits(result);
        }
    }

    uint32_t reduced = 0;
    for (uint32_t value : lane_checksums) {
        reduced ^= value;
    }
    return reduced;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::cout << "SmartSSD FPGA peak-compute benchmark\n"
                  << "  device             : " << options.device << '\n'
                  << "  LANES              : " << LANES << '\n'
                  << "  group lanes        : " << GROUP_LANES << '\n'
                  << "  DSP-add lanes      : " << DSP_ADD_GROUPS * GROUP_LANES
                  << '\n'
                  << "  target clock       : " << KERNEL_CLOCK_MHZ << " MHz\n"
                  << "  timed iterations   : " << options.iterations << '\n'
                  << "  repeats            : " << options.repeats << '\n';

        auto device = xrt::device(options.device);
        auto uuid = device.load_xclbin(options.xclbin);
        auto kernel = xrt::kernel(device, uuid, "peak_compute",
                                  xrt::kernel::cu_access_mode::exclusive);
        const uint32_t checksum_offset = kernel.offset(4);

        auto run = xrt::run(kernel);
        run.set_arg(1, options.alpha);
        run.set_arg(2, options.beta);
        run.set_arg(3, options.seed);

        std::cout << "Validating and warming up ... " << std::flush;
        run.set_arg(0, options.verify_iterations);
        run.start();
        run.wait();
        const uint32_t actual = kernel.read_register(checksum_offset);
        const uint32_t expected = cpu_reference(options.seed, options.verify_iterations,
                                                options.alpha, options.beta);
        if (actual != expected) {
            throw std::runtime_error("validation failed: FPGA checksum=" +
                                     std::to_string(actual) + ", CPU checksum=" +
                                     std::to_string(expected));
        }
        std::cout << "passed\n";

        run.set_arg(0, options.iterations);
        double total_seconds = 0.0;
        double best_seconds = std::numeric_limits<double>::max();
        uint32_t first_timed_checksum = 0;
        for (uint32_t repeat = 0; repeat < options.repeats; ++repeat) {
            const auto begin = std::chrono::steady_clock::now();
            run.start();
            run.wait();
            const auto end = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(end - begin).count();
            total_seconds += seconds;
            best_seconds = std::min(best_seconds, seconds);

            const uint32_t timed_checksum = kernel.read_register(checksum_offset);
            if (repeat == 0) {
                first_timed_checksum = timed_checksum;
            } else if (timed_checksum != first_timed_checksum) {
                throw std::runtime_error("timed runs produced different checksums");
            }

            const double flops = 2.0 * LANES * static_cast<double>(options.iterations);
            std::cout << "  run " << (repeat + 1) << ": " << std::fixed
                      << std::setprecision(6) << seconds << " s, " << std::setprecision(2)
                      << flops / seconds / 1.0e9 << " GFLOP/s\n";
        }

        const double flops_per_run =
            2.0 * LANES * static_cast<double>(options.iterations);
        const double average_gflops =
            flops_per_run * options.repeats / total_seconds / 1.0e9;
        const double best_gflops = flops_per_run / best_seconds / 1.0e9;
        const double ii1_ceiling_gflops =
            2.0 * LANES * static_cast<double>(KERNEL_CLOCK_MHZ) / 1000.0;
        const double inferred_average_clock_mhz =
            static_cast<double>(options.iterations) * options.repeats /
            total_seconds / 1.0e6;
        const double inferred_best_clock_mhz =
            static_cast<double>(options.iterations) / best_seconds / 1.0e6;

        std::cout << "\nResult\n"
                  << "  average             : " << std::fixed << std::setprecision(2)
                  << average_gflops << " GFLOP/s\n"
                  << "  best                 : " << best_gflops << " GFLOP/s\n"
                  << "  inferred clock       : " << inferred_average_clock_mhz
                  << " MHz (best " << inferred_best_clock_mhz << " MHz)\n"
                  << "  II=1 requested roof : " << ii1_ceiling_gflops << " GFLOP/s\n"
                  << "  vs requested roof   : "
                  << 100.0 * average_gflops / ii1_ceiling_gflops << " %\n"
                  << "TEST PASSED\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
}
