# Synthetic PE32+ Conformance Tests (`tests/conformance/`)

Every directory matching `test_*/CMakeLists.txt` is **auto-discovered** by `tests/conformance/CMakeLists.txt` when running `scripts/build-shims.sh`.

## Adding a New Test (Zero Shared-File Edits)

1. Create `tests/conformance/test_<shim>/CMakeLists.txt` and `main.cpp`.
2. Build via `scripts/build-shims.sh` (outputs `build-shims/bin/test_<shim>.exe`).
3. Run live under CrossOver + Rosetta 2:
   ```sh
   target/debug/bx-cli run --prefix /tmp/bx-test --shims build-shims/bin --log /tmp/bx-test.jsonl --exe build-shims/bin/test_<shim>.exe
   ```

## Exit Status: Pass, Fail, and Unavailable

`bx-cli run` normally returns the guest's exit status. A timeout returns 124, a signal
returns 128 plus its number, and a cleanup failure raises an otherwise successful status
to at least 1. Fixtures that cannot reach a conclusion on the host report **exit 77**
and print `SKIP: <reason>` on stdout; for example
`test_d3dmetal_gpu_limits` skips when `d3d12.dll`, device creation, or shader model 6.6 is
unavailable.

A skip is **not** a pass and **not** a failure. AGENTS.md §5 forbids counting a skipped test
as a pass, so record the three outcomes separately:

```sh
target/debug/bx-cli run --prefix /tmp/bx-test --shims build-shims/bin \
    --log /tmp/bx-test.jsonl --exe build-shims/bin/test_<shim>.exe
status=$?
case $status in
    0)   echo "PASS: test_<shim>" ;;
    77)  echo "UNAVAILABLE (skip): test_<shim> — see the SKIP line above" ;;
    *)   echo "FAIL: test_<shim> (exit $status)" ;;
esac
```

`bx-cli` does not itself reinterpret 77 (it is the guest's own status, and other fixtures use
their own conventions), so always read the status before reporting a run. For a fixture that
adopts this protocol, printing a `SKIP:` line without exit 77, or exiting 77 without one, is a
defect. Older per-check skip conventions, such as `test_d3d12_xs`, are separate: its `SKIP:`
lines are informational and the fixture exits according to `g_failures`.

## M2 First-Frame Fixture

The M2 fixture requires both the placed-resource and PresentX headers. Its CMake target fails
configuration if either provider contract is missing; the fixture must not be silently skipped.
Run it with a D3DMetal bottle:

```sh
target/debug/bx-cli run --bottle <d3dmetal_bottle> --backend d3dmetal \
  --shims build-shims/bin --log /tmp/bx-m2-first-frame.jsonl \
  --exe build-shims/bin/test_m2_first_frame.exe
```

Replace `<d3dmetal_bottle>` with the installed CrossOver bottle name.

To verify that the pixel comparison detects a mismatch, set the injection variable in the
launcher environment and opt into environment inheritance. `env -i` keeps this test-only
variable and the listed runtime essentials from exposing unrelated host environment values to
the guest:

```sh
env -i HOME="$HOME" USER="$USER" LOGNAME="$LOGNAME" TMPDIR=/tmp LANG=C \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  BX_M2_FIRST_FRAME_INJECT_PIXEL_MISMATCH=1 \
  target/debug/bx-cli run --inherit-env --bottle <d3dmetal_bottle> --backend d3dmetal \
  --shims build-shims/bin --log /tmp/bx-m2-first-frame-negative.jsonl \
  --exe build-shims/bin/test_m2_first_frame.exe
```

Replace `<d3dmetal_bottle>` with the installed CrossOver bottle name.

The negative control should print `NOTE: pixel mismatch control armed` and exit 1.
The `pixel_readback` checkpoint should report `pixel (0,0) expected 01234567 got 9f598b46`.
