# Measured on the Sun ZX

These figures come from [xglbench](../xglbench/README.md) runs in November
2026 on the project's test machine: a SPARCstation 10 with one 60 MHz SM61,
Solaris 2.6, XGL 3.3 and a Sun ZX (reported by XGL as `Sun:LEO`), in a 320x240
window unless stated. They are measurements of that one machine, not
specifications. The renderer's design follows from them, as noted under each.

## The buffer switch waits for retrace

A buffer switch does not return until the next vertical retrace. The interval
between consecutive switches, with nothing drawn, was 13.12 ms (median of
200; the refresh rate is measured this way, never assumed). Work done before
the switch only uses up part of that wait; nothing can overlap the rest.

A frame therefore lasts a whole number of retrace periods, and the frame rate
moves in steps:

| Periods per frame | Frame time | Frame rate |
| ---: | ---: | ---: |
| 4 | 52.5 ms | 19.1 FPS |
| 5 | 65.6 ms | 15.2 FPS |
| 6 | 78.7 ms | 12.7 FPS |
| 7 | 91.8 ms | 10.9 FPS |

A saving that does not carry a frame across one of these boundaries changes
its profile but not its duration. An average such as a timedemo result mixes
frames of different lengths.

## Triangles: illumination and lights

2,048 smooth-shaded triangles of about 22 pixels each, complete draw:

| Submission | Time | Per triangle |
| --- | ---: | ---: |
| Unlit, color-only points | 8.69 ms | 4.2 µs |
| Unlit, points that also carry normals | 9.64 ms | 4.7 µs |
| Illuminated, ambient light only | 13.3–13.6 ms | 6.6 µs |
| Illuminated, one directional light | 12.6 ms | 6.2 µs |
| Illuminated, one point light | 15.7 ms | 7.7 µs |
| Illuminated, four point lights | 27.0 ms | 13.2 µs |

Illumination costs about 40% more than unlit submission even when no light
can change the result, and each point light adds about 1.8 µs to every lit
triangle. The number of lights merely allocated and switched off does not
matter. The renderer therefore illuminates only the walls and brush entities
a dynamic light reaches, enables only the lights that reach something, and
shades models on the CPU. Unlit geometry is submitted as color-only points;
only the walls a light reaches carry normals.

At this size the device is limited by triangle count, not pixels: the same
triangles at 640x480, four times the area, take the same time. Triangles of
87 pixels take about 2.6 times as long at 640x480.

## State changes

Changing between illuminated and unlit drawing (three attribute calls, as the
renderer issues them) costs about 0.54 ms per change. The renderer orders each
frame by native state for this reason; see
[rendering](render.md#frame-order).

Replacing the model transform costs about 0.5 ms per change, so the CPU keeps
transforming entity vertices and the transform is left alone.

## Clears

| Clear | 320x240 |
| --- | ---: |
| Color and depth together | 0.42 ms |
| Depth only, or color only | 2.8 ms |
| Color and depth within a viewport of 80% of the window | 2.3 ms |

The combined full-window clear is a fast path that costs the same at 640x480.
The others fill at about 27 million pixels per second and so grow with the
window. The view weapon therefore no longer gets a depth clear of its own.

A region that is neither cleared nor redrawn keeps its contents on both
buffers across buffer switches, and geometry and a clear both stay inside a
device viewport set shorter than the window. The renderer uses this to
[leave an unchanged status bar in place](render.md#status-bar) in small
windows, where the slower clear costs about 2 ms and drawing the bar cost
several times that. The probe checked the middle of the view and of the
band, not the row where they meet; the renderer's use of it has not yet been
run on the device.

## Submission calls

| 512 flat triangles | Time |
| --- | ---: |
| `xgl_multi_simple_polygon`, 128 per call | 3.10 ms |
| `xgl_triangle_list`, 128 per call | 2.80 ms |
| `xgl_triangle_strip`, two triangles per strip | 4.20 ms |
| `xgl_polygon`, one per call | 19.8 ms |

Each call costs about 33 µs on top of its triangles, so batching matters far
more than the choice of batched primitive.

Those are flat triangles. Whether strips or triangle lists pay for the
smooth-shaded triangles the renderer draws has not been measured; the
`probe/mesh` cases of [xglbench](../xglbench/README.md#hardware-probes) are
there to find out.

## Native stroke text

Text costs about 23 µs per glyph and about 0.2 ms per call. A status bar of
four lines took 3.2 ms and a twelve-line console 8.0 ms, each including one
polygon call for its rectangles and one completion wait. The renderer keeps
the number of text calls down: one per run of characters, spaces included,
and one per status-bar number. It also queues text so that a status bar's
rectangles go out in one polygon call, and draws the bar only when it has
changed where the window is small enough.

## Native texture mapping

XGL's texture mapping runs in software on this device: one frame of 512
point-sampled triangles took 1.84 seconds. The renderer does not use it.

## Other Sun graphics hardware

XGLQuake was written and measured on a Sun ZX. It has also run on an SX,
as described below. For the other boards what follows is from Sun's
documentation, not from trial.

| Hardware | `xgl_quality` | Expected |
| --- | --- | --- |
| ZX, TurboZX | `zx` (medium) | Works: the board the renderer was made on. |
| Creator3D | `creator` (high) | Should work as it is. 24-bit windows with hardware double buffering and Z through XGL. See the note on gamma in [setup](setup.md). |
| Elite3D | `elite` (ultra) | Should work as it is, by the same reasoning; its XGL support is assumed, not confirmed. |
| Creator (not 3D) | none | Will not work: no double buffering or Z for 24-bit windows. |
| SX (SPARCstation 10SX, 20) | `sx` (low) | Works as it is, at about a quarter of the ZX's speed for each triangle; see below. |
| CG12 (GS) | `cg12` (ultralow) | Unknown. Sun's Solaris 2.6 frame buffer handbook does not list it, and nothing found says whether XGL on 2.6 supports it. |
| GX, TurboGX, S24, PGX | none | Will not work: no 24-bit Z-buffered 3D. |

Startup refuses a device unless XGL reports direct access, RGB color, a
Z-buffer and shading all in hardware, and two window buffers. The ZX and
the SX pass. `-xglanydevice` goes on with less, as long as XGL can do each of these at
all; the status bar is then redrawn every frame, and with a single buffer
frames are drawn in view and flicker.

### SX

XGLQuake 0.1.13 has run on an SX in a machine with two 125 MHz HyperSPARC
processors, at 320x240, with no special options. XGL reports the device as
`Sun:SX` with RGB color, the Z-buffer and shading all in hardware, and
gives the window two buffers, so startup accepts it without
`-xglanydevice`. The status bar is kept from frame to frame as on the ZX:
the shortened clear works there too.

Playing the three demos in a loop at the default settings, which are the
ZX's (`medium`), with `xgl_profile 2`, over 960 frames:

| Per frame | SX, two 125 MHz HyperSPARC | ZX, one 60 MHz SM61 |
| --- | ---: | ---: |
| Frames a second | about 11 | 21.9 |
| Graphics time | 91.4 ms | 43.4 ms |
| of it inside XGL drawing calls | 68.0 ms | |
| World | 36.7 ms | 13.3 ms |
| Entities | 20.8 ms | 10.6 ms |
| Triangles | 2,106 | 1,868 |
| Buffer switch | 3.3 ms | 7.6 ms |
| Choosing and building meshes | 2.6 ms | |

The ZX column is the `demo1` timedemo of an earlier version, so the two are
to be compared loosely; the SX run was ordinary playback, not a timedemo.

- The board is what limits it, not the processors: three quarters of a
  frame is spent inside XGL's drawing calls, and under 3 ms choosing and
  building meshes.
- A triangle costs about 20 microseconds more, where the ZX takes 4.5. Sun's
  own benchmark has the SX at about a quarter of the ZX's shaded triangles
  (30,500 a second against 125,000), and this agrees.
- About 27 ms of a frame does not depend on the number of triangles. That
  may be the cost of each call: a frame has some 57 triangle strips of nine
  triangles on average. It has not been looked into; `xglbench` on the SX,
  or a run with `xgl_alias_strips 0`, would tell.

The `sx` tier draws well under half the triangles and has not been timed
there yet:

```sh
./xglquake -winsize 320 240 +_snd_mixahead 0.3 +xgl_quality sx +xgl_profile 2 +timedemo demo1 > xglquake-sx.txt 2>&1
```

What Sun's documentation says of the board:

- It draws pixels and leaves transforms and lighting to the processor,
  which is how the renderer works anyway, and the calls its XGL driver is
  tuned for are the ones used here: three-sided polygons, and opaque 24-bit
  triangle strips with a less-than-or-equal depth test.
- Its Z-buffer and the back buffer of a 24-bit window are kept in main
  memory, 4 bytes a pixel each, and faster if memory is reserved for them:
  8 MB for both, set in the system configuration and taken at boot. Sun's
  "Platform Notes: SPARCstation 10SX and SPARCstation 20 System
  Configuration Guide" has the procedure. Whether the machine above had
  memory reserved is not known.
- With 4 MB of video memory the larger resolutions (1280x1024 and up) have
  8 bits a pixel only, and so no 24-bit visual to run in.
