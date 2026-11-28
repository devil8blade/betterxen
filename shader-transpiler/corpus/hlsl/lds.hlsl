// SPDX-License-Identifier: GPL-3.0-or-later
// BetterXen shader corpus (#649): original kernel, written for translator tests.
// Shared layout: root constants b0 (4 x 32-bit), input SRV t0, output UAV u0.
#define BX_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0)"
cbuffer Root : register(b0) { uint c0; uint c1; uint c2; uint c3; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);

// Groupshared memory and barriers: a per-group tree reduction.
groupshared uint scratch[64];
[RootSignature(BX_RS)] [numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    scratch[gtid.x] = input[tid.x];
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 32u; s > 0u; s >>= 1) {
        if (gtid.x < s) { scratch[gtid.x] += scratch[gtid.x + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gtid.x == 0u) { output[gid.x] = scratch[0]; }
}
