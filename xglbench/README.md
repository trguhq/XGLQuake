# xglbench

`xglbench` is a standalone Sun XGL benchmark for choosing the rendering path
for XGLQuake. It uses procedural geometry and CPU-sampled material colors;
it needs no Quake executable, PAK files, OpenGL library, or texture-mapping
support. It lives beside `WinQuake` so either Quake client can use its findings.

The intended measurement machine is Solaris 2.6 on a SPARCstation 10 with a
Sun ZX/Leo board and the corresponding XGL runtime/device pipeline. The
benchmark does not assume that every XGL operation is hardware accelerated.
No ZX performance numbers are supplied with the source.

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
```

Options:

| Option | Meaning |
| --- | --- |
| `-winsize W H` | Run one requested drawable size instead of both defaults; width 128 through 4096, height 64 through 4096. |
| `-seconds N` | Measured duration per trial; default 1 second. |
| `-warmup N` | Unmeasured warm-up duration; default 0.15 second. |
| `-repeats N` | Trials per case; default 3, range 1 through 31. |
| `-triangles N` | Positive even base triangle count, at most 16384; default 512. |
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
* **Does strip reuse help?** Connected strip workloads are distinct from
  disconnected patch workloads, so savings from shared vertices are not
  confused with a change in visible triangle count.
* **Does retaining native arrays help?** A static-mesh case compares XGL's
  retained geometry-data lifetime contract with volatile arrays using the
  same geometry and batch size. It is separate from XGL display-list caches.
* **Is the bottleneck geometry or pixels?** Compare triangle sizes, overlap,
  Z buffering, near-plane clipping, and the two resolutions. Overlap tests
  include both near-first and far-first submission to expose depth rejection
  effects. A fourfold increase in pixel area
  with little elapsed-time change suggests a geometry/CPU limit; it does not
  identify the exact limiting device stage.
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

`total_ms` includes the reset clears before drawing; ordinary draw-case
`render_ms` excludes those clears. `frame/*` includes clear, draw and masked HUD in
`render_ms` and swaps in `present_ms`. Event polling is outside elapsed frame
totals. Process CPU time includes the trial's polling and loop overhead.
Reported mesh memory counts the portable arrays only, excluding native XGL
arrays, framebuffer storage and driver allocations. Geometry rebuilding also
includes allocation, both the portable independent-triangle and strip
representations, and native-array conversion. This is the preparation cost
of the benchmark harness, not an optimized engine mesh-generation or CPU
projection implementation. It is an intentionally costly alternative to
reusing caches.

CSV contains `trial` records and a `summary` record per successful case.
Skipped cases contain a summary with status `skipped` and the reason; zero
fields on those records are not measurements. The `frames_all_trials` field
is the total frame count for the case even on individual trial rows. Keep the
text report with the CSV to retain device/build metadata and interpretation.

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
linked with `tests/test_backend.c`. That backend is a deliberately empty test
double used only for argument handling, case scheduling, resize sequencing,
and report/CSV tests. It identifies its output as **NO GRAPHICS / NO VALID
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
performance specification.

The benchmark source is GPL-2.0-or-later. `COPYING` is duplicated from the
repository's `gnu.txt`. All benchmark files and generated build artifacts
reside in `xglbench/`.
