#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs test_d3d12_presentx once normally and once per negative control, and checks that each
# control fails in its own stage with its own evidence. Needs build-shims/ and bx-cli built.
# Usage (from the repository root): tests/conformance/test_d3d12_presentx/run_controls.sh <bottle>
set -u

bottle=${1:?usage: run_controls.sh <d3dmetal_bottle>}
dir=$(mktemp -d) || exit 1
bad=0

run() { # name injection
    BX_PRESENTX_INJECT=$2 target/debug/bx-cli run --inherit-env --bottle "$bottle" --backend d3dmetal \
        --shims build-shims/bin --log "$dir/$1.jsonl" --timeout 90 \
        --exe build-shims/bin/test_d3d12_presentx.exe >"$dir/$1.out" 2>&1
    status=$?
}
expect() { # name file pattern
    grep -aq -- "$3" "$dir/$1.$2" || { echo "$1: missing in $2: $3"; bad=1; }
}
must_fail() { # name stage
    [ "$status" -ne 0 ] || { echo "$1: exited 0"; bad=1; }
    expect "$1" out "first failing stage: $2)"
}

run normal ""
[ "$status" -eq 0 ] || { echo "normal: exit $status"; bad=1; }
expect normal out "^PASS: test_d3d12_presentx (.* 0 skipped)"

run bad-window bad-window
must_fail bad-window create
expect bad-window out "FAIL \[create\]: CreateSwapChain hr=0x887a0001"
# The contract stage logs seven argument rejections; the injected window must add the eighth.
n=$(grep -ac '"event":"present_create_rejected".*invalid window or description' "$dir/bad-window.jsonl")
[ "$n" -eq 8 ] || { echo "bad-window: $n argument rejections logged, expected 8"; bad=1; }

run skip-draw skip-draw
must_fail skip-draw pixels
expect skip-draw out "frame 5: 2432 of 6000 pixels differ"
expect skip-draw out "pixels_wrong=2432 "

run wrong-expectation wrong-expectation
must_fail wrong-expectation pixels
expect wrong-expectation out "pixels_wrong=29184 "

run skip-wait-fault skip-wait-fault
must_fail skip-wait-fault sync
expect skip-wait-fault out "SetEventOnCompletion fault was not injected"

run wrong-removed-hr wrong-removed-hr
must_fail wrong-removed-hr sync
expect wrong-removed-hr out "post-present device reset must return DXGI_ERROR_DEVICE_RESET: hr=0x80040205"

run zero-timeout-overrun zero-timeout-overrun
must_fail zero-timeout-overrun sync
expect zero-timeout-overrun out "WaitForIdle(0) must reject zero without signaling:"

if [ "$bad" -ne 0 ]; then
    echo "FAIL: run_controls (output kept in $dir)"
    exit 1
fi
rm -rf "$dir"
echo "PASS: run_controls (1 normal run, 6 negative controls)"
