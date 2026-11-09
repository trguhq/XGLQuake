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
expect_fail -queue
expect_fail -queue 0
expect_fail -queue -1
expect_fail -queue 9
expect_fail -queue 2garbage
expect_fail -queue nan
expect_fail -test __no_such_case__

XB_TEST_LOG="$test_dir/help.log" "$bench" -help >"$test_dir/output" 2>&1
test ! -e "$test_dir/help.log" || fail "help opened a display"
XB_TEST_LOG="$test_dir/list.log" "$bench" -list >"$test_dir/output" 2>&1
test ! -e "$test_dir/list.log" || fail "list opened a display"
grep 'submit/multi_polygon/batch-128' "$test_dir/output" >/dev/null || fail "missing stable case"
for area in 4 16 64 100 256 1024; do
    grep "area/${area}px" "$test_dir/output" >/dev/null || fail "missing calibrated $area pixel case"
done
for shape in horizontal-16x1 vertical-1x16; do
    grep "shape/100px/$shape" "$test_dir/output" >/dev/null || fail "missing $shape case"
done
for mode in serialized streaming; do
    for colors in cached relight; do
        for depth in 0 1 2; do
            grep "pipeline/$mode/$colors/depth-$depth" "$test_dir/output" >/dev/null ||
                fail "missing $mode/$colors/depth-$depth case"
        done
    done
done

for path in cpu-flat cpu-smooth xgl-flat xgl-smooth; do
    for light in directional point-1; do
        for depth in 0 1 2; do
            grep "lighting/matched/$path/$light/depth-$depth" "$test_dir/output" >/dev/null ||
                fail "missing matched $path/$light/depth-$depth case"
        done
    done
    for light in 2 4; do
        grep "lighting/matched/$path/point-$light/depth-1" "$test_dir/output" >/dev/null ||
            fail "missing matched $path/point-$light case"
    done
    grep "lighting/static/$path/directional/depth-1" "$test_dir/output" >/dev/null ||
        fail "missing static $path case"
    grep "lighting/area-100/$path/point-1" "$test_dir/output" >/dev/null ||
        fail "missing controlled-area $path case"
done
for path in cpu-flat cpu-smooth; do
    for styles in cached styles-1 styles-4; do
        for depth in 0 1 2; do
            grep "lighting/baked/$path/$styles/depth-$depth" "$test_dir/output" >/dev/null ||
                fail "missing baked $path/$styles/depth-$depth case"
        done
    done
    for lights in 1 2 4; do
        grep "lighting/quake-like/$path/point-$lights/depth-1" "$test_dir/output" >/dev/null ||
            fail "missing Quake-like $path/point-$lights case"
    done
done

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
    fault_test=${2:-submit/multi_polygon/batch-128}
    fault_draw=${3:-5}
    case "$mode" in
        resize) set -- ;;
        *) set -- -winsize 320 240 ;;
    esac
    # A completion error in the final draw must not become a winning result.
    # The ordinary case uses preview + warm-up + three minimum timed frames.
    # Streaming uses preview + eight warm-up + eight timed work items.
    if XB_TEST_FAULT="$mode" XB_TEST_FAULT_DRAW="$fault_draw" \
        XB_TEST_LOG="$test_dir/$mode.log" "$bench" "$@" \
        -allow-fallback -test "$fault_test" -triangles 8 -queue 8 \
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

for mode in native-error resize expose interrupt; do
    fault_case "$mode" pipeline/streaming/relight/depth-0 17
done

# Fixed screen-space areas stay fixed across the default two resolutions,
# including the horizontal and vertical skinny-triangle variants.
"$bench" -allow-fallback -test 100px -triangles 8 -seconds .001 -warmup 0 \
    -repeats 1 -csv "$test_dir/areas.csv" >"$test_dir/output" 2>&1
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); column[$i]=i }
        mean=column["nominal_area_mean_px"]; lo=column["nominal_area_min_px"]
        hi=column["nominal_area_max_px"]; payload=column["input_payload_bytes"]
        width=column["width"]; height=column["height"]; status=column["status"]
        record=column["record"]
        next
    }
    {
        if ($status != "ok" || $mean < 99.99 || $mean > 100.01 ||
            $lo < 99.99 || $hi > 100.01 || $payload <= 0) bad++
        if ($record == "summary" && $width == 320 && $height == 240) small++
        if ($record == "summary" && $width == 640 && $height == 480) large++
    }
    END {
        if (!mean || !lo || !hi || !payload || !width || !height || !status ||
            !record || small != 3 || large != 3 || bad) exit 1
    }
' "$test_dir/areas.csv" || fail "calibrated triangle area or payload CSV is invalid"

# The mock retains independent per-slot color copies until completion. Its
# journal verifies bounded submission and lifetime, not a GPU speed model.
check_queue_log()
{
    awk -v expected="$2" '
        /^queue-begin / {
            if (active || $2 != expected) bad++
            active=1; limit=$2; outstanding=0; submitted=0; completed=0
            for (slot in pending) delete pending[slot]
            begins++
        }
        /^queue-prepare / {
            if (!active || $2 < 0 || $2 >= limit || pending[$2] || $3 != outstanding) bad++
        }
        /^queue-draw / {
            if (!active || $2 < 0 || $2 >= limit || pending[$2]) bad++
            pending[$2]=1; outstanding++; submitted++
            draws++; unposted++
            if ($3 != outstanding || outstanding > limit) bad++
            if (outstanding > peak) peak=outstanding
        }
        /^queue-clear / {
            if (!active || $2 != 1 || $3 != 1) bad++
            clears++
        }
        /^queue-post / {
            if (!active || $2 != outstanding || $3 != completed || unposted != 1) bad++
            unposted=0
            posts++
        }
        /^queue-drain / {
            if (!active || $2 != outstanding || $3 != completed+outstanding || unposted) bad++
            completed+=outstanding; outstanding=0
            for (slot in pending) pending[slot]=0
            drains++
        }
        /^queue-end / {
            if (!active || outstanding || $2 != submitted || $3 != completed || submitted != completed) bad++
            active=0; ends++
        }
        /^queue-error / { bad++ }
        END {
            if (active || bad || !begins || begins != ends || !posts || !drains ||
                peak != expected || draws != posts || draws != clears) exit 1
        }
    ' "$1" || fail "queue $2 exceeded its bound, reused retained data, or lost completions"
}

check_pipeline_csv()
{
    awk -F, -v expected="$2" '
        NR == 1 {
            for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); column[$i]=i }
            q=column["queue_limit"]; m=column["max_queued"]; d=column["drain_calls"]
            e=column["enqueue_ms"]; wait=column["drain_ms"]; render=column["render_ms"]
            total=column["total_ms"]; mode=column["timing_mode"]; status=column["status"]
            next
        }
        {
            if ($status != "ok" || $q != expected || $m < 1 || $m > expected ||
                $d < 1 || $e < 0 || $wait < 0 || $render != "" || $mode == "") bad++
            rows++
        }
        END {
            if (!q || !m || !d || !e || !wait || !render || !total || !mode || !status || !rows || bad) exit 1
        }
    ' "$1" || fail "pipeline CSV omitted completion diagnostics or mislabeled rendering time"
}

for queue in 1 2 8; do
    for colors in cached relight; do
        log="$test_dir/queue-$queue-$colors.log"
        csv="$test_dir/queue-$queue-$colors.csv"
        XB_TEST_LOG="$log" "$bench" -winsize 320 240 -allow-fallback \
            -test "pipeline/streaming/$colors/depth-1" -triangles 8 \
            -queue "$queue" -seconds .001 -warmup 0 -repeats 1 -csv "$csv" \
            >"$test_dir/output" 2>&1
        check_queue_log "$log" "$queue"
        check_pipeline_csv "$csv" "$queue"
        if test "$colors" = relight; then
            grep '^queue-prepare ' "$log" >/dev/null || fail "relight did not update slot colors"
        fi
    done
done

# Serialized controls use one retained buffer even when the streaming limit is
# higher. Both modes exercise the same primitive, clears and native storage.
XB_TEST_LOG="$test_dir/serialized.log" "$bench" -winsize 320 240 -allow-fallback \
    -test pipeline/serialized/relight/depth-1 -triangles 8 -queue 8 \
    -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/serialized.csv" \
    >"$test_dir/output" 2>&1
check_queue_log "$test_dir/serialized.log" 1
check_pipeline_csv "$test_dir/serialized.csv" 1

# The default queue is two. The queue option does not change serialized tests.
XB_TEST_LOG="$test_dir/default-queue.log" "$bench" -winsize 320 240 -allow-fallback \
    -test pipeline/streaming/cached/depth-0 -triangles 8 \
    -seconds .001 -warmup 0 -repeats 1 >"$test_dir/output" 2>&1
check_queue_log "$test_dir/default-queue.log" 2

# A deliberately slow final mock completion is included in completed wall time
# and reported as drain time. It is not evidence about hardware performance.
XB_TEST_LOG="$test_dir/delayed.log" XB_TEST_DRAIN_DELAY_MS=20 \
    "$bench" -winsize 320 240 -allow-fallback \
    -test pipeline/streaming/relight/depth-0 -triangles 8 -queue 8 \
    -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/delayed.csv" \
    >"$test_dir/output" 2>&1
check_queue_log "$test_dir/delayed.log" 8
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); column[$i]=i }
        total=column["total_ms"]; wait=column["drain_ms"]; record=column["record"]
        next
    }
    $record == "trial" {
        if ($total < 1 || $wait < 1 || $total < $wait) bad++
        trials++
    }
    END { if (!total || !wait || !record || trials != 1 || bad) exit 1 }
' "$test_dir/delayed.csv" || fail "completed timing excluded the final drain"

if XB_TEST_LOG="$test_dir/drain-failure.log" XB_TEST_FAIL_DRAIN=2 \
    XB_TEST_DRAIN_DELAY_MS=20 "$bench" -winsize 320 240 -allow-fallback \
    -test pipeline/streaming/relight/depth-0 -triangles 8 -queue 8 \
    -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/drain-failure.csv" \
    >"$test_dir/output" 2>&1; then
    fail "failed final drain reported success"
fi
grep '^queue-drain-failed ' "$test_dir/drain-failure.log" >/dev/null || fail "final drain failure not injected"
grep 'injected stream completion failure' "$test_dir/output" >/dev/null || fail "completion failure reason lost"
if grep 'Lowest measured' "$test_dir/output" >/dev/null; then fail "failed completion produced a winner"; fi
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); column[$i]=i }
        record=column["record"]; status=column["status"]
        next
    }
    { if ($record != "summary" || $status != "skipped") bad++; rows++ }
    END { if (!record || !status || rows != 1 || bad) exit 1 }
' "$test_dir/drain-failure.csv" || fail "failed final drain produced a successful measurement"

# Lighting logs verify host scheduling and data ownership, not illumination
# pixels. Native readback gates and the portable math tests cover those parts.
check_lighting_log()
{
    awk -v hardware="$2" -v smooth="$3" -v updates="$4" '
        /^lighting-configure / { if (!$2 || $3 != smooth) bad++; cases++ }
        /^lighting-compute / { if ($2 != hardware) bad++ }
        /^lighting-colors / {
            if (hardware || $2 != smooth) bad++
            colors++; changed+=$3
        }
        /^lighting-update / { if (!hardware) bad++; lights++ }
        /^lighting-error / { bad++ }
        END {
            if (bad || cases != 1) exit 1
            if (!updates && (colors || lights)) exit 1
            if (updates && hardware && !lights) exit 1
            if (updates && !hardware && (!colors || !changed)) exit 1
        }
    ' "$1" || fail "lighting path used incorrect updates, payloads or cached data"
}

check_lighting_csv()
{
    awk -F, -v path="$2" -v count="$3" -v model="$4" -v motion="$5" -v styles="$6" '
        NR == 1 {
            for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); col[$i]=i }
            record=col["record"]; status=col["status"]; total=col["total_ms"]
            facets=col["triangles"]; rate=col["triangles_per_second"]
            shade=col["shading"]; equation=col["lighting_model"]; cpu=col["lighting_compute"]
            lights=col["light_count"]; moving=col["light_motion"]; layers=col["style_layers"]
            sample=col["lighting_sample"]; mode=col["timing_mode"]; payload=col["input_payload_bytes"]
            next
        }
        {
            smooth=path ~ /smooth/; hardware=path ~ /^xgl/
            sampling=smooth ? "vertices" : (hardware ? "device-vertex" : "centroid")
            expected_shade=smooth ? "smooth" : "flat"
            expected_compute=hardware ? "xgl" : "cpu"
            if ($status != "ok" || $shade != expected_shade || $equation != model ||
                $cpu != expected_compute || $lights != count || $moving != motion ||
                $layers != styles || $sample != sampling || $mode != "lighting-serialized" ||
                $payload <= 0 || $total <= 0) bad++
            expected=$facets*1000/$total
            if ($rate < expected*.999 || $rate > expected*1.001) bad++
            rows++
        }
        END {
            if (!record || !status || !total || !facets || !rate || !shade || !equation ||
                !cpu || !lights || !moving || !layers || !sample || !mode || !payload ||
                rows != 2 || bad) exit 1
        }
    ' "$1" || fail "lighting CSV mislabeled equation, shading, sample location or completed rate"
}

for path in cpu-flat cpu-smooth xgl-flat xgl-smooth; do
    case "$path" in xgl-*) hardware=1 ;; *) hardware=0 ;; esac
    case "$path" in *smooth) smooth=1 ;; *) smooth=0 ;; esac
    for light in directional point-1 point-2 point-4; do
        case "$light" in point-2) lights=2 ;; point-4) lights=4 ;; *) lights=1 ;; esac
        log="$test_dir/lighting-$path-$light.log"
        csv="$test_dir/lighting-$path-$light.csv"
        XB_TEST_LOG="$log" "$bench" -winsize 320 240 -allow-fallback \
            -test "lighting/matched/$path/$light/depth-1" -triangles 8 \
            -seconds .005 -warmup 0 -repeats 1 -csv "$csv" >"$test_dir/output" 2>&1
        check_lighting_log "$log" "$hardware" "$smooth" 1
        check_lighting_csv "$csv" "$path" "$lights" lambert 1 0
    done
    log="$test_dir/static-$path.log"
    csv="$test_dir/static-$path.csv"
    XB_TEST_LOG="$log" "$bench" -winsize 320 240 -allow-fallback \
        -test "lighting/static/$path/directional/depth-1" -triangles 8 \
        -seconds .001 -warmup 0 -repeats 1 -csv "$csv" >"$test_dir/output" 2>&1
    check_lighting_log "$log" "$hardware" "$smooth" 0
    check_lighting_csv "$csv" "$path" 1 lambert 0 0
done

# New lit geometry preserves the requested pixel area while its Z surface and
# normals vary. Both resolutions must still report the same geometric area.
"$bench" -allow-fallback -test lighting/area-100/ -triangles 8 \
    -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/lit-area.csv" \
    >"$test_dir/output" 2>&1
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); col[$i]=i }
        status=col["status"]; record=col["record"]; width=col["width"]
        average=col["nominal_area_mean_px"]; low=col["nominal_area_min_px"]; high=col["nominal_area_max_px"]
        next
    }
    $record == "summary" {
        if ($status != "ok" || $average < 99.99 || $average > 100.01 ||
            $low < 99.99 || $high > 100.01) bad++
        if ($width == 320) small++
        else if ($width == 640) large++
        else bad++
    }
    END { if (!status || !record || !width || !average || !low || !high || small != 4 || large != 4 || bad) exit 1 }
' "$test_dir/lit-area.csv" || fail "lighting geometry changed calibrated area or resolution labels"


# Payloads include smooth per-vertex colors and hardware normals. Equal
# triangle counts must not imply identical referenced native input sizes.
awk -F, '
    FNR == 1 {
        for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); col[$i]=i }
        record=col["record"]; payload=col["input_payload_bytes"]
        shade=col["shading"]; cpu=col["lighting_compute"]
        next
    }
    $record == "summary" { bytes[$cpu "/" $shade]=$payload }
    END {
        if (!(bytes["cpu/smooth"] > bytes["cpu/flat"] &&
              bytes["xgl/flat"] > bytes["cpu/flat"] &&
              bytes["xgl/smooth"] > bytes["cpu/smooth"])) exit 1
    }
' "$test_dir"/static-*.csv || fail "lighting input estimates omitted vertex colors or normals"

for path in cpu-flat cpu-smooth; do
    case "$path" in *smooth) smooth=1 ;; *) smooth=0 ;; esac
    for styles in cached styles-1 styles-4; do
        case "$styles" in cached) updates=0; layers=1 ;; styles-1) updates=1; layers=1 ;; *) updates=1; layers=4 ;; esac
        log="$test_dir/baked-$path-$styles.log"
        csv="$test_dir/baked-$path-$styles.csv"
        XB_TEST_LOG="$log" "$bench" -winsize 320 240 -allow-fallback \
            -test "lighting/baked/$path/$styles/depth-1" -triangles 8 \
            -seconds .05 -warmup 0 -repeats 1 -csv "$csv" >"$test_dir/output" 2>&1
        check_lighting_log "$log" 0 "$smooth" "$updates"
        check_lighting_csv "$csv" "$path" 0 baked 0 "$layers"
    done
    log="$test_dir/quake-$path.log"
    csv="$test_dir/quake-$path.csv"
    XB_TEST_LOG="$log" "$bench" -winsize 320 240 -allow-fallback \
        -test "lighting/quake-like/$path/point-2/depth-1" -triangles 8 \
        -seconds .005 -warmup 0 -repeats 1 -csv "$csv" >"$test_dir/output" 2>&1
    check_lighting_log "$log" 0 "$smooth" 1
    check_lighting_csv "$csv" "$path" 2 quake-like 1 1
done

# Every animated-style trial must reach real 10 Hz style changes, even when a
# draw itself exceeds the requested duration. Reuse the mock's existing
# pre-fault delay with an unreachable fault index; no error is injected.
XB_TEST_LOG="$test_dir/style-minimum.log" XB_TEST_FAULT=native-error \
    XB_TEST_FAULT_DRAW=1000000 "$bench" -winsize 320 240 -allow-fallback \
    -test lighting/baked/cpu-flat/styles-1/depth-0 -triangles 8 \
    -seconds .001 -warmup 0 -repeats 2 -csv "$test_dir/style-minimum.csv" \
    >"$test_dir/output" 2>&1
check_lighting_log "$test_dir/style-minimum.log" 0 0 1
awk -F, '
    NR == 1 {
        for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); col[$i]=i }
        record=col["record"]; status=col["status"]; frames=col["frames_all_trials"]
        next
    }
    {
        if ($status != "ok" || $frames < 48 || $frames % 6 != 0) bad++
        if ($record == "trial") trials++
        else if ($record == "summary") summaries++
        else bad++
    }
    END { if (!record || !status || !frames || trials != 2 || summaries != 1 || bad) exit 1 }
' "$test_dir/style-minimum.csv" || fail "short animated-style trials omitted a complete changing tick"
awk '
    /^lighting-colors / { updates++; changed+=$3 }
    /^native-error / { bad++ }
    END { if (updates < 4 || changed < 4 || bad) exit 1 }
' "$test_dir/style-minimum.log" || fail "short repeated style trials exercised only cached colors"

XB_TEST_LOG="$test_dir/style-cached-control.log" XB_TEST_FAULT=native-error \
    XB_TEST_FAULT_DRAW=1000000 "$bench" -winsize 320 240 -allow-fallback \
    -test lighting/baked/cpu-flat/cached/depth-0 -triangles 8 \
    -seconds .001 -warmup 0 -repeats 2 >"$test_dir/output" 2>&1
check_lighting_log "$test_dir/style-cached-control.log" 0 0 0


for mode in native-error resize expose interrupt; do
    fault_case "$mode" lighting/matched/xgl-smooth/point-1/depth-1 5
done

check_lighting_rejected()
{
    awk -F, '
        NR == 1 {
            for (i=1; i<=NF; ++i) { gsub(/"/, "", $i); col[$i]=i }
            record=col["record"]; status=col["status"]
            next
        }
        { if ($record != "summary" || $status != "skipped") bad++; rows++ }
        END { if (!record || !status || rows != 1 || bad) exit 1 }
    ' "$1" || fail "failed lighting operation produced a successful measurement"
}

if XB_TEST_FAIL_LIGHT_UPDATE=2 "$bench" -winsize 320 240 -allow-fallback \
    -test lighting/matched/xgl-flat/point-1/depth-1 -triangles 8 \
    -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/light-update-failure.csv" \
    >"$test_dir/output" 2>&1; then
    fail "failed light-state update reported success"
fi
grep 'injected light-state update failure' "$test_dir/output" >/dev/null || fail "light-state failure reason lost"
check_lighting_rejected "$test_dir/light-update-failure.csv"

if XB_TEST_FAIL_LIGHTING=1 "$bench" -winsize 320 240 -allow-fallback \
    -test lighting/matched/xgl-smooth/point-1/depth-1 -triangles 8 \
    -seconds .001 -warmup 0 -repeats 1 -csv "$test_dir/light-validation-failure.csv" \
    >"$test_dir/output" 2>&1; then
    fail "failed lighting readback reported success"
fi
grep 'injected lighting readback validation failure' "$test_dir/output" >/dev/null || fail "lighting validation failure reason lost"
check_lighting_rejected "$test_dir/light-validation-failure.csv"

echo 'CLI tests: arguments, resize, calibrated cases, queues, lighting/shading reports, update paths and fault rejection passed'
