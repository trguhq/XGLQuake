## Profiling details

Use the two [performance commands](../README.md#measure-performance) in the
README with the same resolution, demo, audio lookahead, and detail settings
for each comparison.

Keep the console closed during playback and quit after the timedemo result
appears. The console stays visible after playback and has a different
workload; its later reports must not be averaged into gameplay results.
Compare the timedemo FPS from both runs to measure profiling overhead.
Repeat the same procedure at another resolution only when testing resolution
cost.

On the Sun ZX a frame lasts a whole number of 13.12 ms retrace periods, so
the frame rate moves in steps (12.7, 15.2, 19.1 FPS); see
[Measured on the Sun ZX](hardware.md#the-buffer-switch-waits-for-retrace).
A saving shows in the timedemo result only when it carries frames across one
of those boundaries.

### Reading a report

`xgl_profile 1` prints two lines about once a second. These are from the
supplied demo1 run of 19 November 2026 (before the `state` and
`status bar kept` fields existed):

```
XGL profile: 21 frames; gfx 46.61 ms (max 102.92); world 13.51 entities 6.47 effects 0.92 weapon 9.31 UI 7.00 audio-overlap 2.35 ms
  native calls (overlap stages): submit 14.13 sync 3.79 swap 7.33 ms; per frame: triangles 1298.5 glyphs 18.3 polygon calls 10.5 waits 4.0
```

- 21 frames were drawn in the interval. Graphics took 46.61 ms per frame on
  average and 102.92 ms in the slowest frame.
- The five stages add up to 37.21 ms. The buffer switch (`swap`, 7.33 ms) is
  outside them; the remaining 2 ms is frame setup and the clear.
- `submit`, `sync` and `audio-overlap` happen inside the stages. They show
  where stage time went and must not be added to it: 14.13 of the 37.21 ms
  was spent in native submission calls.
- 1,298.5 triangles in 14.13 ms is about 11 µs per triangle, which also
  covers the text and rectangle calls.
- `gfx` excludes the server, client and final sound update, a few more
  milliseconds. Together they put these frames at four retrace periods.

### Timing scope

The first line gives graphics-frame mean and maximum time and the world,
entity, effects, weapon and UI stage times. The second gives native call
times and per-frame counts:

- `submit`: time inside triangle, rectangle and text calls.
- `sync`: time waiting for the device to finish queued work; `waits` counts
  those waits.
- `swap`: the buffer switch, which waits for retrace.
- `state`: time in the attribute calls that change between illuminated and
  unlit drawing, select lights or change blending; `state changes` counts
  them. The device
  may defer part of a change's cost to the next submission.
- `triangles` includes alias subdivision and particles; `glyphs` and
  `polygon calls` include the UI.
- `status bar kept`: the share of frames, 0 to 1, whose clear left the status
  bar in place. In the rest the bar may have been drawn; `glyphs` shows how
  often it was. Zero throughout means the bar is not being kept: see
  [the status bar](render.md#status-bar) for what rules it out.

Profiling is off by default and makes no timing queries when disabled. Output
goes to the terminal, so it adds no console glyphs to the workload measured.

`xgl_profile 2` adds more timing queries and the detail lines below. Detail
times overlap the stages and one another; do not add them to a frame total.

- **detail ms**: world visibility; detail selection (which includes mesh
  builds); mesh builds, where a detailed mesh also pays for the texture and
  light samples that decide its shape; ordinary color validation and
  refresh; animated sky/water colors; generic vertex packing; alias cache work plus cached-pose
  transforms. Packing excludes submission, so a drop in packing time alone is
  not an FPS gain.
- **entity scopes**: wall, submission and wait time for alias, brush and
  sprite drawing, and visits per frame. Native times refer to calls executed
  during the scope, which can include geometry queued earlier: batches span
  scope and stage boundaries. Visits count every model handed to its drawing
  routine, including ones then culled; the view weapon is separate.
- **weapon scope**: the same three times for drawing the view weapon.
- **effects scopes**: particles (including the shared particle simulation)
  and queued translucent water.
- **alias entity detail** and **view weapon detail**: the light lookup
  (`R_LightPoint` only; flames and fullbright skip it), cached output (final
  position and color assembly into the native batch, with a normal only
  under native illumination) and fallback
  drawing (uncached transform, subdivision, skin sampling and packing). These
  are nested inside the entity and weapon scopes.
- **SMP alias preparation**: jobs completed by the worker per frame; the main
  thread's time finding and queuing them (`main prepass`); the worker's time
  in them, which overlaps world drawing; the main thread's time at the join
  (`main wait`), both waiting and running jobs the worker had not started;
  and how many jobs per frame the main thread ran itself. Use the job counts
  to check whether a scene exercises the worker at all.

### Counters

Detailed counts are per-frame averages.

- **world per frame**: cached draws include meshes built in that frame;
  fallback draws are faces streamed without a cached mesh, so neither is a
  hit rate. Color reuse and updates count mesh checks; a streamed face is
  checked once per triangle. Vertices built and recolored are distinct
  vertices, each shared by several triangles; animated vertices are triangle
  corners, and `animated samples` in the reuse line the distinct ones.
  `detail meshes evicted` counts detailed meshes released because the world
  cache was full.
- **world detail per frame**: faces drawn from their detailed mesh and the
  triangles in them, water on its finer grid included; how many of those
  faces and triangles were walls cut along
  [texture shapes](render.md#texture-shapes); then the dynamic lights
  enabled for illuminated geometry and the faces drawn illuminated, both
  zero in a frame with no dynamic light.
- **alias cache per frame**: color and pose (normal/topology) cache hits,
  misses and evictions, and fallback draws, show cache pressure. Compare text
  calls with glyphs to see how well the installed font allows runs. Both are
  counted when the text is drawn, not when it is asked for.
- **reuse**: sky and water color time, distinct animated vertices colored
  against animated corners emitted, and vertices per frame by path: queued from
  persistent meshes (`cached native`), written straight into the batch
  (`direct native`) or converted (`packed`).
- **alias preparation**: cache work against live transforms, shared-topology
  hits and misses, and pose evictions by entry or byte limit.

Measure FPS with profiling disabled.
