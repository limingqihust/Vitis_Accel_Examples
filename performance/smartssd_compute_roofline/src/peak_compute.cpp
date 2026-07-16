// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#ifndef LANES
#define LANES 320
#endif

static_assert(LANES > 0, "LANES must be positive");

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

    uint32_t lane_seed[LANES];
    uint32_t checksum[LANES];
#pragma HLS ARRAY_PARTITION variable = lane_seed complete dim = 1
#pragma HLS ARRAY_PARTITION variable = checksum complete dim = 1

    uint32_t current_seed = seed;
load_seeds:
    for (int lane = 0; lane < LANES; ++lane) {
#pragma HLS PIPELINE II = 1
        // Runtime, per-lane seeds prevent common-subexpression elimination
        // from merging nominally parallel floating-point datapaths.
        lane_seed[lane] = current_seed & 0x007fffffu;
        checksum[lane] = current_seed ^ 0x9e3779b9u;
        current_seed += 0x001f123bu;
    }

compute:
    for (uint32_t iter = 0; iter < iterations; ++iter) {
#pragma HLS PIPELINE II = 1
    compute_lanes:
        for (int lane = 0; lane < LANES; ++lane) {
#pragma HLS UNROLL
            // Construct a normal float in [0.5, 1.0) without an integer-to-
            // float conversion unit.  XOR is deliberately cheap auxiliary
            // logic; only the following multiply and add count as FLOPs.
            const uint32_t input_bits =
                0x3f000000u | ((iter ^ lane_seed[lane]) & 0x007fffffu);
            const float input = bits_to_float(input_bits);
            // The SmartSSD dynamic region exposes 1344 DSPs.  Vitis otherwise
            // chooses 3 DSPs for fmul plus 2 DSPs for fadd (5 per lane), so
            // LANES=320 cannot be placed.  Keep fmul in DSPs and implement
            // fadd in LUT fabric: throughput remains one mul-add pair/cycle.
            const float product = input * alpha;
#pragma HLS BIND_OP variable = product op = fmul impl = maxdsp
            const float result = product + beta;
#pragma HLS BIND_OP variable = result op = fadd impl = fabric
            checksum[lane] ^= float_to_bits(result);
        }
    }

    // A balanced XOR tree makes every lane observable without creating an
    // external-memory port.  This removes the AXI memory interconnect that was
    // the worst 300 MHz timing path in the SmartSSD dynamic region.
    checksum_out = xor_reduce<LANES>(checksum);
}

} // extern "C"
