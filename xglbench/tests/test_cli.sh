#!/bin/sh
# Portable CLI/scheduling regression tests. Never measures XGL performance.
# GPL-2.0-or-later; see COPYING.
set -eu

bench=${1:-./build/xglbench-test}
test_dir=build/cli-test-$$
mkdir -p "$test_dir"
trap 'rm -rf "$test_dir"' 0
trap 'exit 1' 1 2 3 15

fail()
{
    echo "CLI test failed: $*" >&2
    if test -f "$test_dir/output"; then cat "$test_dir/output" >&2; fi
    exit 1
}

expect_fail()
{
    if "$bench" "$@" >"$test_dir/output" 2>&1; then
        fail "unexpected success: $*"
    fi
}

expect_fail -unknown
expect_fail -winsize
expect_fail -winsize 320
expect_fail -winsize 0 240
expect_fail -winsize -320 240
expect_fail -winsize 320x 240
expect_fail -seconds 0
expect_fail -seconds nan
expect_fail -seconds inf
expect_fail -seconds 1garbage
expect_fail -warmup -1
expect_fail -repeats 0
expect_fail -repeats 32
expect_fail -triangles 3
expect_fail -triangles 0
expect_fail -triangles 16386
expect_fail -test __no_such_case__

XB_TEST_LOG="$test_dir/help.log" "$bench" -help >"$test_dir/output" 2>&1
test ! -e "$test_dir/help.log" || fail "help opened a display"
XB_TEST_LOG="$test_dir/list.log" "$bench" -list >"$test_dir/output" 2>&1
test ! -e "$test_dir/list.log" || fail "list opened a display"
grep 'submit/multi_polygon/batch-128' "$test_dir/output" >/dev/null || fail "missing stable case"

XB_TEST_LOG="$test_dir/default.log" "$bench" -quick -allow-fallback \
    -test submit/multi_polygon/batch-128 -triangles 8 -seconds .002 -warmup 0 \
    -repeats 1 -csv "$test_dir/default.csv" >"$test_dir/output" 2>&1
grep '^open 320 240$' "$test_dir/default.log" >/dev/null || fail "default initial size"
grep '^resize 640 480$' "$test_dir/default.log" >/dev/null || fail "default second size"
test "$(grep -c '^resize ' "$test_dir/default.log")" = 1 || fail "unexpected resize sequence"
grep 'NO VALID HARDWARE PERFORMANCE DATA' "$test_dir/output" >/dev/null || fail "missing mock warning"

# Discover columns by name so harmless CSV column reordering is permitted.
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) {
            gsub(/"/, "", $i)
            if ($i == "width") w=i
            if ($i == "height") h=i
        }
        next
    }
    w && h {
        if ($w == 320 && $h == 240) small++
        if ($w == 640 && $h == 480) large++
    }
    END { if (!w || !h || !small || !large) exit 1 }
' "$test_dir/default.csv" || fail "CSV did not distinguish both resolutions"

XB_TEST_LOG="$test_dir/forced.log" "$bench" -winsize 800 600 -quick \
    -allow-fallback -test submit/multi_polygon/batch-128 -triangles 8 \
    -seconds .002 -warmup 0 -repeats 1 -csv "$test_dir/forced.csv" \
    >"$test_dir/output" 2>&1
grep '^open 800 600$' "$test_dir/forced.log" >/dev/null || fail "forced size not used"
if grep '^resize ' "$test_dir/forced.log" >/dev/null; then fail "forced size resized"; fi
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) {
            gsub(/"/, "", $i)
            if ($i == "width") w=i
            if ($i == "height") h=i
        }
        next
    }
    w && h { if ($w != 800 || $h != 600) bad++; rows++ }
    END { if (!w || !h || !rows || bad) exit 1 }
' "$test_dir/forced.csv" || fail "forced size CSV contained other dimensions"

if XB_TEST_FAIL_METHOD=multi_polygon "$bench" -winsize 320 240 \
    -quick -allow-fallback -test submit/multi_polygon/batch-128 \
    -csv "$test_dir/skipped.csv" >"$test_dir/output" 2>&1; then
    fail "all-invalid run reported success"
fi
grep 'injected test-only validation failure' "$test_dir/output" >/dev/null || fail "missing validation failure"
if grep 'Lowest measured' "$test_dir/output" >/dev/null; then fail "failed method was recommended"; fi
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) {
            gsub(/"/, "", $i)
            if ($i == "record") r=i
            if ($i == "status") s=i
        }
        next
    }
    r && s { if ($r != "summary" || $s != "skipped") bad++; rows++ }
    END { if (!r || !s || !rows || bad) exit 1 }
' "$test_dir/skipped.csv" || fail "invalid method had a successful/timed CSV row"

# Explicit timing settings override -quick even when -quick comes last.
"$bench" -winsize 320 240 -test submit/multi_polygon/batch-128 \
    -triangles 8 -allow-fallback -seconds .002 -warmup 0 -repeats 2 -quick \
    >"$test_dir/output" 2>&1
grep '0.002 seconds/trial; 2 repeats; 0.000 seconds warm-up' \
    "$test_dir/output" >/dev/null || fail "quick overrode explicit settings"

expect_fail -quick -allow-fallback -test submit/multi_polygon/batch-128 \
    -csv "$test_dir/nonexistent/results.csv"

fault_case()
{
    mode=$1
    case "$mode" in
        resize) set -- ;;
        *) set -- -winsize 320 240 ;;
    esac
    # Preview + one warm-up draw precede the three minimum timed frames.
    # Latching the fault on draw five exercises the post-trial event check:
    # a completion error in the final frame must never become a winning result.
    if XB_TEST_FAULT="$mode" XB_TEST_FAULT_DRAW=5 \
        XB_TEST_LOG="$test_dir/$mode.log" "$bench" "$@" \
        -allow-fallback -test submit/multi_polygon/batch-128 -triangles 8 \
        -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/$mode.csv" \
        >"$test_dir/output" 2>&1; then
        fail "faulted $mode run reported success"
    fi
    grep "^$mode 320 240" "$test_dir/$mode.log" >/dev/null || fail "$mode was not injected"
    if grep 'Lowest measured' "$test_dir/output" >/dev/null; then
        fail "$mode fault produced a winner"
    fi
    awk -F, '
        NR == 1 {
            for (i=1; i<=NF; ++i) {
                gsub(/"/, "", $i)
                if ($i == "record") r=i
                if ($i == "status") s=i
            }
            next
        }
        r && s { if ($r != "summary" || $s != "skipped") bad++; rows++ }
        END { if (!r || !s || rows != 1 || bad) exit 1 }
    ' "$test_dir/$mode.csv" || fail "$mode produced a successful or mislabeled measurement"
    case "$mode" in
        native-error)
            grep 'injected native completion failure' "$test_dir/output" >/dev/null || fail "native error lost"
            ;;
        resize)
            egrep 'size.*stopped|stopped.*size' "$test_dir/output" >/dev/null || fail "resize stop not explained"
            if grep '^resize 640 480$' "$test_dir/$mode.log" >/dev/null; then
                fail "unexpected resize did not stop the sequence"
            fi
            ;;
        expose)
            egrep 'exposed|obscured' "$test_dir/output" >/dev/null || fail "exposure not explained"
            ;;
        interrupt)
            grep 'interrupted by user' "$test_dir/output" >/dev/null || fail "interrupt not explained"
            ;;
    esac
}

fault_case native-error
fault_case resize
fault_case expose
fault_case interrupt

echo 'CLI tests: arguments, resize sequencing, CSV, validation, overrides and final-frame fault rejection passed'
