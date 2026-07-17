// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#ifndef LANES
#define LANES 320
#endif

#ifndef GROUP_LANES
#define GROUP_LANES 16
#endif

#ifndef DSP_ADD_GROUPS
#define DSP_ADD_GROUPS 8
#endif

static_assert(LANES > 0, "LANES must be positive");
static_assert(GROUP_LANES > 0, "GROUP_LANES must be positive");
static_assert(LANES % GROUP_LANES == 0,
              "LANES must be an integer multiple of GROUP_LANES");

constexpr int GROUP_COUNT = LANES / GROUP_LANES;
static_assert(DSP_ADD_GROUPS >= 0 && DSP_ADD_GROUPS <= GROUP_COUNT,
              "DSP_ADD_GROUPS must be between zero and GROUP_COUNT");

namespace {

union FloatBits {
    float value;
    uint32_t bits;
};

float bits_to_float(uint32_t bits) {
#pragma HLS INLINE
    FloatBits converted;
    converted.bits = bits;
    return converted.value;
}

uint32_t float_to_bits(float value) {
#pragma HLS INLINE
    FloatBits converted;
    converted.value = value;
    return converted.bits;
}

template <bool USE_DSP>
float add_float(float lhs, float rhs);

template <>
float add_float<true>(float lhs, float rhs) {
#pragma HLS INLINE
    const float result = lhs + rhs;
#pragma HLS BIND_OP variable = result op = fadd impl = fulldsp
    return result;
}

template <>
float add_float<false>(float lhs, float rhs) {
#pragma HLS INLINE
    const float result = lhs + rhs;
#pragma HLS BIND_OP variable = result op = fadd impl = fabric
    return result;
}

// Each iteration performs one single-precision multiply and one
// single-precision add in every lane.  The floating-point input changes on
// every iteration, and the result is folded into a one-cycle integer checksum.
// Consequently there is no floating-point recurrence to limit the loop II,
// while every arithmetic result remains observable to the compiler.
template <int N>
uint32_t xor_reduce(const uint32_t* values) {
#pragma HLS INLINE
    return xor_reduce<N / 2>(values) ^ xor_reduce<N - N / 2>(values + N / 2);
}

template <>
uint32_t xor_reduce<1>(const uint32_t* values) {
#pragma HLS INLINE
    return values[0];
}

// Give each small group its own loop counter and pipeline control.  A single
// 320-lane pipeline makes its loop-valid/clock-enable signal drive thousands
// of registers across the device; independent groups keep those nets local.
template <int GROUP>
void compute_group(uint32_t iterations,
                   float alpha,
                   float beta,
                   uint32_t seed,
                   uint32_t& group_checksum) {
#pragma HLS INLINE off
    uint32_t lane_seed[GROUP_LANES];
    uint32_t checksum[GROUP_LANES];
#pragma HLS ARRAY_PARTITION variable = lane_seed complete dim = 1
#pragma HLS ARRAY_PARTITION variable = checksum complete dim = 1

    uint32_t current_seed =
        seed + static_cast<uint32_t>(GROUP * GROUP_LANES) * 0x001f123bu;
load_group_seeds:
    for (int local_lane = 0; local_lane < GROUP_LANES; ++local_lane) {
#pragma HLS PIPELINE II = 1
        lane_seed[local_lane] = current_seed & 0x007fffffu;
        checksum[local_lane] = current_seed ^ 0x9e3779b9u;
        current_seed += 0x001f123bu;
    }

compute_group_iterations:
    for (uint32_t iter = 0; iter < iterations; ++iter) {
#pragma HLS PIPELINE II = 1
    compute_group_lanes:
        for (int local_lane = 0; local_lane < GROUP_LANES; ++local_lane) {
#pragma HLS UNROLL
            const uint32_t input_bits =
                0x3f000000u |
                ((iter ^ lane_seed[local_lane]) & 0x007fffffu);
            const float input = bits_to_float(input_bits);
            const float product = input * alpha;
#pragma HLS BIND_OP variable = product op = fmul impl = maxdsp
            // Use otherwise-idle DSPs for part of the add array.  The remaining
            // groups stay in fabric so the 1344-DSP dynamic-region limit is not
            // exceeded.  This trades spare DSP capacity for lower LUT routing
            // pressure without changing the FLOP count or II.
            const float result =
                add_float<(GROUP < DSP_ADD_GROUPS)>(product, beta);
            checksum[local_lane] ^= float_to_bits(result);
        }
    }

    group_checksum = xor_reduce<GROUP_LANES>(checksum);
}

template <int GROUP, int COUNT>
struct GroupLauncher {
    static void run(uint32_t iterations,
                    float alpha,
                    float beta,
                    uint32_t seed,
                    uint32_t* group_checksum) {
#pragma HLS INLINE
        compute_group<GROUP>(iterations, alpha, beta, seed,
                             group_checksum[GROUP]);
        GroupLauncher<GROUP + 1, COUNT>::run(iterations, alpha, beta, seed,
                                             group_checksum);
    }
};

template <int COUNT>
struct GroupLauncher<COUNT, COUNT> {
    static void run(uint32_t, float, float, uint32_t, uint32_t*) {
#pragma HLS INLINE
    }
};

} // namespace

extern "C" {

void peak_compute(uint32_t iterations,
                  float alpha,
                  float beta,
                  uint32_t seed,
                  uint32_t& checksum_out) {
#pragma HLS INTERFACE s_axilite port = iterations bundle = control
#pragma HLS INTERFACE s_axilite port = alpha bundle = control
#pragma HLS INTERFACE s_axilite port = beta bundle = control
#pragma HLS INTERFACE s_axilite port = seed bundle = control
#pragma HLS INTERFACE s_axilite port = checksum_out bundle = control
#pragma HLS INTERFACE s_axilite port = return bundle = control

    uint32_t group_checksum[GROUP_COUNT];
#pragma HLS ARRAY_PARTITION variable = group_checksum complete dim = 1
#pragma HLS DATAFLOW

    GroupLauncher<0, GROUP_COUNT>::run(iterations, alpha, beta, seed,
                                       group_checksum);

    // A balanced XOR tree makes every lane observable without creating an
    // external-memory port.  This removes the AXI memory interconnect that was
    // the worst 300 MHz timing path in the SmartSSD dynamic region.
    checksum_out = xor_reduce<GROUP_COUNT>(group_checksum);
}

} // extern "C"
