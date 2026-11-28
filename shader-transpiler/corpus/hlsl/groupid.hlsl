// SPDX-License-Identifier: GPL-3.0-or-later
// BetterXen shader corpus (#649): original kernel, written for translator tests.
// Shared layout: root constants b0 (4 x 32-bit), input SRV t0, output UAV u0.
#define BX_RS "RootConstants(num32BitConstants=4, b0), SRV(t0), UAV(u0)"
cbuffer Root : register(b0) { uint c0; uint c1; uint c2; uint c3; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);

// System values: group id, group thread id and flattened indices with a 2D group.
[RootSignature(BX_RS)] [numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint idx = (gid.y * c0 + gid.x) * 64u + gi;
    output[idx] = (gtid.x << 24) | (gtid.y << 16) | (tid.x << 8) | tid.y;
}
