// SPDX-License-Identifier: GPL-3.0-or-later
// BetterXen shader corpus (#649): original kernel, written for translator tests.
// Shared layout: root constants b0 (4 x 32-bit), input SRV t0, output UAV u0.
#define BX_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0)"
cbuffer Root : register(b0) { uint c0; uint c1; uint c2; uint c3; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);

// Global atomics: add and max into shared counters, with returned old values.
[RootSignature(BX_RS)] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = input[tid.x];
    uint before;
    InterlockedAdd(output[0], x, before);
    InterlockedMax(output[1], x);
    output[2u + tid.x] = before;
}
