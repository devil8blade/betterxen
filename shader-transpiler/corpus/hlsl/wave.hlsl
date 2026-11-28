// SPDX-License-Identifier: GPL-3.0-or-later
// BetterXen shader corpus (#649): original kernel, written for translator tests.
// Shared layout: root constants b0 (4 x 32-bit), input SRV t0, output UAV u0.
#define BX_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0)"
cbuffer Root : register(b0) { uint c0; uint c1; uint c2; uint c3; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);

// Cross-lane wave operations: the costly case when a 64-wide wave runs as two 32-wide halves.
[RootSignature(BX_RS)] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint x = input[tid.x];
    uint sum = WaveActiveSum(x);
    uint prefix = WavePrefixSum(x);
    uint first = WaveReadLaneFirst(x);
    uint4 ballot = WaveActiveBallot((x & 1u) != 0u);
    output[tid.x * 4u + 0u] = sum;
    output[tid.x * 4u + 1u] = prefix;
    output[tid.x * 4u + 2u] = first;
    output[tid.x * 4u + 3u] = countbits(ballot.x) + countbits(ballot.y);
}
