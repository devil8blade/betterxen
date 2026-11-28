// SPDX-License-Identifier: GPL-3.0-or-later
// BetterXen shader corpus (#649): original kernel, written for translator tests.
// Shared layout: root constants b0 (4 x 32-bit), input SRV t0, output UAV u0.
#define BX_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0)"
cbuffer Root : register(b0) { uint c0; uint c1; uint c2; uint c3; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);

// Float ALU and conversions: fma, sqrt, rcp, float<->uint.
[RootSignature(BX_RS)] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float f = (float)input[tid.x] * asfloat(c0);
    float g = mad(f, 0.5f, 1.0f);
    float h = sqrt(abs(g)) + rcp(g + 2.0f);
    output[tid.x] = asuint(h) ^ (uint)floor(h * 256.0f);
}
