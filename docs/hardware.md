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
shades models on the CPU.

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
buffers across buffer switches.

## Submission calls

| 512 flat triangles | Time |
| --- | ---: |
| `xgl_multi_simple_polygon`, 128 per call | 3.10 ms |
| `xgl_triangle_list`, 128 per call | 2.80 ms |
| `xgl_triangle_strip`, two triangles per strip | 4.20 ms |
| `xgl_polygon`, one per call | 19.8 ms |

Each call costs about 33 µs on top of its triangles, so batching matters far
more than the choice of batched primitive.

## Native stroke text

Text costs about 23 µs per glyph and about 0.2 ms per call. A status bar of
four lines took 3.2 ms and a twelve-line console 8.0 ms, each including one
polygon call for its rectangles and one completion wait. The renderer keeps
the number of text calls down: one per run of characters, spaces included,
and one per status-bar number.

## Native texture mapping

XGL's texture mapping runs in software on this device: one frame of 512
point-sampled triangles took 1.84 seconds. The renderer does not use it.
