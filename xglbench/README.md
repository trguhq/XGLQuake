# xglbench

`xglbench` is a standalone Sun XGL benchmark for choosing the rendering path
for XGLQuake. It uses procedural geometry and CPU-sampled material colors;
it needs no Quake executable, PAK files, OpenGL library, or texture-mapping
support. It lives beside `WinQuake` so either Quake client can use its findings.

The intended measurement machine is Solaris 2.6 on a SPARCstation 10 with a
Sun ZX/Leo board and the corresponding XGL runtime/device pipeline. The
benchmark does not assume that every XGL operation is hardware accelerated.
No measured ZX results from this program are supplied with the source. The
historical rates discussed below are reference points from the Leo whitepaper.

## Build on Solaris

Use GNU make and an ANSI C compiler, with the XGL and OpenWindows development
headers and libraries installed:

```sh
cd xglbench
gmake
```

The default paths follow the XGL Programmer's Guide:

```sh
gmake XGLHOME=/opt/SUNWits/Graphics-sw/xgl OPENWINHOME=/usr/openwin
```

`CC`, `CFLAGS`, `CPPFLAGS`, `LANGFLAGS`, `WARNFLAGS`, `LDFLAGS`, `XGL_LIBS`,
and `XGL_RPATH` can be overridden. For example, a compiler that does not
accept GCC's language/warning options can use:

```sh
gmake CC=cc LANGFLAGS= WARNFLAGS= CFLAGS=-O
```

The build follows the native 32-bit SPARC toolchain. It does not request
UltraSPARC instructions, 64-bit code, fast-math, or architecture-specific
assembly. There are no dependencies on engine objects or changes to the
engine makefiles. Runtime library paths are embedded with Solaris `-R`;
the XGL device pipeline must also be installed for the selected framebuffer.
The default GCC options use `-ansi` and `-W` for older toolchain compatibility.
`__EXTENSIONS__` exposes Solaris interfaces such as `gethrtime()` in that mode.

`gmake sdk-check` checks the configured header locations. Compilation and
linking against the real installed SDK remain the authoritative API check.

## Run

```sh
./xglbench | tee zx-results.txt
```

With no size override, the program opens **320x240**, runs the selected suite,
resizes the same window to **640x480**, and repeats the suite. Resize and
warm-up work are outside the measured intervals. Results are kept separate
for each resolution, then compared.

An explicit `-winsize` runs only that resolution:

```sh
./xglbench -winsize 800 600
./xglbench -seconds 5 -repeats 5 -csv zx-results.csv | tee zx-results.txt
./xglbench -quick
./xglbench -list
./xglbench -test submit/multi_polygon/batch-128 -winsize 320 240
./xglbench -test pipeline/ -queue 2 -csv pipeline.csv | tee pipeline.txt
```

Options:

| Option | Meaning |
| --- | --- |
| `-winsize W H` | Run one requested drawable size instead of both defaults; width 128 through 4096, height 64 through 4096. |
| `-seconds N` | Measured duration per trial; default 1 second. |
| `-warmup N` | Unmeasured warm-up duration; default 0.15 second. |
| `-repeats N` | Trials per case; default 3, range 1 through 31. |
| `-triangles N` | Positive even base triangle count, at most 16384; default 512. |
| `-queue N` | Maximum outstanding work items in streaming cases, 1 through 8; default 2. Serialized controls use one slot. |
| `-test TEXT` | Run cases whose names contain `TEXT`. |
| `-csv PATH` | Save machine-readable results as well as the terminal report. |
| `-display DISPLAY` | Select an X display. Local DGA is the intended path. |
| `-allow-fallback` | Permit a software/non-DGA configuration for diagnosis. |
| `-quick` | Use short trials and one repeat for a smoke run. Explicit timing options override it. |
| `-list` | List case names without opening a display. |
| `-help` | Print usage without opening a display. |

The graphics window shows test geometry; the launching terminal receives the
report. Progress goes to standard error. CSV output is optional and its name
is supplied by the caller. Keep the window fully visible and unobscured, and
avoid moving or resizing it during a trial. Run on the workstation's local
OpenWindows display rather than through SSH X forwarding. Background work
and window-system activity can materially affect a 60 MHz machine.
The full suite's default repeated trials take several minutes. Use `-list`
to inspect the current case list, `-quick` for a smoke run, or `-test` to
investigate a particular family. There is no randomized test order, so rerun
close comparisons and control changes in background load.

## Questions the suite addresses

* **Which primitive submission API is best?** Compare individual polygons,
  batched simple polygons, restart triangle lists, and triangle strips using
  matching positions and facet colors. Batch-size sweeps expose call overhead.
* **Does strip encoding help?** Connected strip workloads are distinct from
  disconnected patch workloads. LeoCommand expands strips into independent
  triangles before LeoFloat processing, so compact input can save bandwidth
  and submission work without implying downstream shared-vertex transform
  reuse. The compared methods still describe matching triangles and colors.
* **Does retaining native arrays help?** A static-mesh case compares XGL's
  retained geometry-data lifetime contract with volatile arrays using the
  same geometry and batch size. It is separate from XGL display-list caches.
* **Is the bottleneck geometry or pixels?** Compare triangle sizes, overlap,
  Z buffering, near-plane clipping, and the two resolutions. Overlap tests
  include both near-first and far-first submission to expose depth rejection
  effects. A fourfold increase in pixel area
  with little elapsed-time change suggests a geometry/CPU limit; it does not
  identify the exact limiting device stage.
* **Does triangle shape matter at a controlled area?** Calibrated cases use
  projected areas of 4, 16, 64, 100, 256, and 1024 square pixels. Equal-area
  100-pixel cases also compare width/height ratios of 1, 16, and 1/16. Leo's
  five horizontally interleaved drawing banks make thin triangles worth
  checking separately from total triangle count.
* **Does posting work without an immediate completion wait help?** Paired
  serialized and streaming cases use the same cached or relit meshes at
  subdivision depths 0, 1, and 2. A bounded queue allows the single CPU thread
  to prepare the next item while previously posted XGL work can execute.
* **How expensive is subdivision?** Depths 0, 1, and 2 produce 1, 4, and 16
  triangles per base triangle. Cached and regenerated geometry are separate
  cases. Subdivision colors come from a procedural material sampled on the
  CPU, not from an XGL texture object.
* **What changes with animation?** Static meshes, changing model transforms,
  and changing facet colors exercise different preparation and submission
  costs.
* **How much frame overhead remains?** Color clears, depth clears, swaps,
  and full-frame workloads separate fixed work from primitive throughput.
  Full-frame cases include a masked HUD copy as well as the scene and swap.
* **Can a practical HUD be cheap?** Opaque and one-bit-masked raster-copy
  cases exercise 2D images without creating a triangle per image pixel.

The generated scenes deliberately include conditions that are expensive or
unrepresentative of an ordinary frame. A large-overlap test is a stress test,
not an estimate of average Quake overdraw. No test result is a prediction of
whole-engine frame rate.

## Calibrated geometry and the whitepaper

The fixed-area cases specify geometric area in screen pixels, before edge
coverage and rasterization rules. Their requested area is held constant at
both window sizes; it does not grow fourfold at 640x480. The report includes
the area range computed from the generated coordinates under the reference
orthographic mapping. This mapping is the one used by calibrated cases, so
their reported area matches the requested area within floating-point
precision. Triangles remain inside the viewport. If a requested shape cannot
fit a forced window size, the case is skipped rather than silently shrinking
the triangles.
The square-aspect cases are named `area/{4,16,64,100,256,1024}px`;
`shape/100px/horizontal-16x1` and `shape/100px/vertical-1x16` compare the
elongated shapes with `area/100px`.
The generator retains the requested triangle count, so controlled overlap
may be needed when the triangles cannot all fit without overlap. Calibrated
triangles use a constant depth and the less-than-or-equal Z comparison, so
later triangles at that depth can pass even where they overlap. These are
geometry/raster stress tests, not a promise of scene-like spatial coverage.

The original patch scenes retain their normalized dimensions instead. Their
nominal reference area grows approximately fourfold when the window changes
from 320x240 to 640x480. For the default 512-triangle normal-size scene, nominal
triangle areas are about 88, 22, and 5.5 pixels at subdivision depths 0, 1,
and 2 in the smaller window. The larger window gives approximately 351, 88,
and 22 pixels. The fixed 128x64 HUD raster also keeps its pixel dimensions
at both window sizes. Compare resolution effects within the appropriate
workload family.

Michael F. Deering and Scott R. Nelson's *Leo: A System for Cost Effective
3D Shaded Graphics* provides the architectural context:

* Printed pages 103-104 describe DMA from application memory, hardware input
  conversion, and expansion of strip input into isolated triangles before
  distribution to the four LeoFloat processors. A retained-array contract
  or a buffer-post operation does not itself prove that a particular XGL
  driver selected DMA.
* Pages 105-106 describe five horizontally interleaved LeoDraw units, each
  with edge walking and span interpolation. The size and shape tests examine
  behavior related to that organization without claiming to measure an
  individual internal unit.
* Page 107 reports 210,000 isolated 100-pixel triangles per second with
  lighting, Gouraud shading, Z buffering and depth cueing enabled. The paper
  says that workload is VRAM limited and that smaller triangles render
  faster. It also reports a special window/color/Z clear under 200
  microseconds and 24-bit image uploads at 10 million pixels per second.

Those are historical system results under the paper's conditions, not
pass/fail thresholds or expected Quake frame rates. Our flat-color cases do
not reproduce its lighted Gouraud workload. Subdivision increases setup and
input work while approximately preserving the covered area; dividing the
published triangle rate by a new triangle count is not a reliable FPS model.
This first extension keeps flat colors and CPU material sampling. It adds
neither texture mapping nor a Gouraud renderer, and it creates no additional
application threads.

## Bounded single-thread streaming

`pipeline/{serialized,streaming}/{cached,relight}/depth-{0,1,2}` are matched
throughput experiments using batched simple polygons. Each work item clears
color and depth, then draws the mesh. There is no HUD copy or buffer swap in
these cases, so presentation synchronization does not obscure the comparison.
They are separate from the existing `frame/*` tests, which include both HUD
and presentation.

The serialized control completes each item before preparing another. The
streaming version posts each item with `XGL_FLUSH_BUFFERS` and drains after
at most `-queue` items. Both use preallocated native slots and the same
geometry/color workload. Preparation may overlap already posted device work;
all XGL/Xlib calls still occur on the one owner thread. Thread count or SMP
performance is not being tested.

Slot contents and descriptors remain unchanged while XGL can reference them.
A slot becomes reusable only after a successful `XGL_FLUSH_SYNCHRONIZE`
completion drain. Queue storage is distinct from the two framebuffer buffers.
Allocation, validation and warm-up happen before measurement. Trial timing
includes pipeline fill and the final completion drain, and no result is
accepted with unfinished work. An unbounded queue would measure enqueue
capacity rather than completed throughput and is intentionally not used.

The total is elapsed wall time per completed work item, including loop and
event-polling overhead. `prepare_ms`, `enqueue_ms`, and `drain_ms` describe
time spent by the host thread; the pipeline's `render_ms` field is empty.
Enqueue time is not rendering duration, and drain time is the remaining wait
after work that may already have overlapped preparation. Do not add presumed
CPU and GPU stage durations together to reconstruct frame latency. Pipeline
facets/second uses completed-item total time. These no-swap items are not
displayed frames or Quake FPS. A queue of one is a useful control; larger
queues should be interpreted together with their outstanding-work bound,
not as free throughput with no latency tradeoff.

## Reading the report

Read the configuration and validation results before comparing rates. Device
name, actual drawable size, color depth, DGA availability, acceleration
reports, and allocated buffers describe the path that was actually opened.
The presence of an XGL function in the SDK does not prove it uses Leo
hardware. Fallback results must not be presented as ZX hardware results.

Rendering timings include waiting for XGL completion at measurement
boundaries. They are elapsed host times, not hardware GPU-counter readings.
CPU preparation, completed rendering, and presentation are reported
separately where the case performs that work. Swap-inclusive time can be
limited by display synchronization; it must not be used alone to rank
primitive throughput.

For the original serialized cases, `total_ms` includes the reset clears
before drawing; ordinary draw-case `render_ms` excludes those clears.
`frame/*` includes clear, draw and masked HUD in `render_ms` and swaps in
`present_ms`. Event polling is outside these elapsed frame totals. Process
CPU time includes the trial's polling and loop overhead.
Reported mesh memory counts the portable arrays only, excluding native XGL
arrays, framebuffer storage and driver allocations. Geometry rebuilding also
includes allocation, both the portable independent-triangle and strip
representations, and native-array conversion. This is the preparation cost
of the benchmark harness, not an optimized engine mesh-generation or CPU
projection implementation. It is an intentionally costly alternative to
reusing caches.

Estimated native input bytes count the vertex/facet payload referenced by
drawing calls, including strip-boundary repetition and triangle-list facet
padding where applicable. They are not measured SBus traffic or DMA byte
counts and do not include an unknown driver's internal representation. Use
them to understand why equal triangle counts can have different input costs,
not to calculate actual bus utilization.

CSV contains `trial` records and a `summary` record per successful case.
Skipped cases contain a summary with status `skipped` and the reason; zero
fields on those records are not measurements. The `frames_all_trials` field
is the total completed-item count for the case even on individual trial
rows; for pipeline cases those items are not presented frames. The
`timing_mode` field distinguishes the timing contracts.
The `retained` flag is one for pipeline cases because timed draws retain
their native slot arrays, including when slot colors change between drains.
`queue_limit`, `max_queued`, and `drain_calls` describe the bounded streaming run.
`max_queued` is the maximum number of items submitted since a successful
drain across all measured trials; it is a software bound, not a measured
hardware queue depth. `drain_calls` is the total number of measured drains
across those trials. Like `frames_all_trials`, both fields repeat the case's
aggregate metadata on individual trial rows.

The `nominal_area_*` columns report geometric pixel area under the reference
orthographic mapping, before model transforms or clipping. They do not
measure the actual projection in perspective or animated-transform cases.
`input_payload_bytes` reports the estimated native payload per item. Keep
the text report with the CSV to retain device/build metadata and
interpretation.

Repeated trials report a median and range. These describe the repeated-trial
measurements, not a per-frame 95th/99th-percentile latency distribution.
Run longer trials and more repeats when methods are close. Record compiler
options and the workstation configuration alongside reports when comparing
builds or machines.

Readback checks precede timing and can disqualify submission paths that
produce incorrect colors or geometry. A failed or unsupported case should
remain visible as a skipped result rather than becoming a zero-time winner.
These checks are focused regression probes, not a full XGL conformance test.

The report's recommendations are starting points for an engine experiment.
Recheck the winner using representative Quake visibility, animation,
lighting, and HUD workloads. In particular, a fixed subdivision level can
look acceptable on small faces while still losing detail on large walls.
Any renderer triangle budget should reduce refinement rather than discard
visible base geometry.

The application is single-threaded. Two installed processors do not imply
twice the rendering throughput. The benchmark does not make concurrent calls
into XGL or claim to measure a multithreaded renderer.

## Portable checks

On a development host without Solaris/XGL:

```sh
make check
```

This compiles geometry tests and a separate `build/xglbench-test` executable
linked with `tests/test_backend.c`. That backend is a test double used for
argument handling, case scheduling, resize sequencing, queue ownership and
report/CSV checks. Its deferred-queue bookkeeping does not emulate a ZX. It
identifies its output as **NO GRAPHICS / NO VALID
HARDWARE PERFORMANCE DATA**. Its timing numbers have no graphics meaning.
It is never linked into the native `xglbench` executable.
Fault injection also checks that a native completion error, exposure, user
interruption, or unexpected resize cannot turn a partial trial into a valid
measurement. An unexpected size change stops the sequence so that subsequent
results cannot be labeled with the old dimensions.

On a modern development host, `make syntax-check` checks C syntax and selected
API usage against the hand-written declarations in `tests/api_contract/`.
Those declarations are documentation-derived test scaffolding, **not the Sun
SDK**. This check cannot validate binary ABI compatibility, the arguments
passed through XGL's variadic attribute interfaces, native behavior, or
performance. It is optional and separate from `make check`.

Passing portable checks does not validate compilation against the proprietary
XGL headers, correct native rendering, or ZX performance. Those require the
Solaris build and native run. `make clean` removes only this folder's generated
binary and `build/` directory.

## References and license

The native implementation uses the supplied XGL Reference Manual and XGL
Programmer's Guide, notably facet colors/illumination, multi-simple polygons,
triangle lists/strips, raster masks, device inquiry, synchronization, and
window buffering. See source comments for API-specific details. The OpenGL
1.1 specification is conceptual background, not a dependency or a Leo
performance specification. The calibrated and streaming tests are informed
by the supplied *Leo: A System for Cost Effective 3D Shaded Graphics*
architectural whitepaper, especially printed pages 103-107.

The benchmark source is GPL-2.0-or-later. `COPYING` is duplicated from the
repository's `gnu.txt`. All benchmark files and generated build artifacts
reside in `xglbench/`.
