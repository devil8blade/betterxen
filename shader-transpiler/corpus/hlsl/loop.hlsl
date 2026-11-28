// SPDX-License-Identifier: GPL-3.0-or-later
// BetterXen shader corpus (#649): original kernel, written for translator tests.
// Shared layout: root constants b0 (4 x 32-bit), input SRV t0, output UAV u0.
#define BX_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0)"
cbuffer Root : register(b0) { uint c0; uint c1; uint c2; uint c3; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);

// Loop with a per-lane, data-dependent trip count and an early exit.
[RootSignature(BX_RS)] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint n = input[tid.x] % (c0 + 1u);
    uint acc = 0u;
    for (uint i = 0u; i < n; ++i) {
        acc += i * c1;
        if (acc > c2) { break; }
    }
    output[tid.x] = acc;
}
