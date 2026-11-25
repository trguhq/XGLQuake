# XGLQuake

XGLQuake ports WinQuake 1.06 to Sun XGL on Solaris, targeting a SPARCstation
10 with a 60 MHz SM61 processor, 64 MB RAM, and Sun ZX (Leo) graphics. It
approximates textures with sampled-color polygons and draws the HUD and menus
with native XGL text and geometry. A similar approach can be seen at
[testglobe.dev](https://testglobe.dev/web). XGLQuake originated as a port
specifically for the ZX graphics card, so texture mapping is not supported yet 
but it will be added for Sun hardware that supports it.

The approximation done on the ZX with smooth-shaded polygons is very imperfect
compared to true texture mapping. This is inevitable to a degree but is being refined.
The intent here is provide software demonstrating 3D hardware that has very little
other software available. That it runs in a playable manner would be a bonus. It
does achieve approximately 17 FPS at 640x480 and 22 FPS at 320x240 on the test system.

## Quick start

### Build

Install GNU make (tested on 3.8.2), GCC (tested on 4.3.6, for example from [tgcware](https://jupiterrise.com/tgcware/sunos5.6_sparc/stable/)), the [Sun XGL SDK](https://archive.org/details/xgl-solaris2.6), and OpenWindows development files.
From the repository root:

```sh
cd WinQuake
make -f Makefile.XGL
```

Run the same make command after source updates; it rebuilds affected files
using generated `.d` dependencies. See [build details](docs/build.md) for
SDK paths and when a clean build is needed.

Makefile.XGL assumes a SuperSPARC or better processor with `-mcpu=supersparc`.

### Game data and launch

Place the resulting `xglquake` executable in your Quake directory, alongside
`id1/`. Shareware uses `id1/pak0.pak`; registered Quake 1.06 uses `id1/pak0.pak`
and `id1/pak1.pak`. Game data is not included.

Run from that Quake directory:

```sh
./xglquake -winsize 320 240 +_snd_mixahead 0.3 +map e1m1
```

Official Quake 1.00, 1.01, and 1.06 shareware PAK directories are recognized.
See [game data and display setup](docs/setup.md) for alternate
locations and shareware restrictions.

### Quality

`+xgl_quality` sets the renderer's detail settings together, as one of five
rough tiers. Without it the renderer runs at **medium**. Only **medium** has
been tested on real hardware (Sun ZX) and the others are pure guesses as to
what might work, provided here as a starting point for your convenience. See [quality settings](docs/render.md).

```sh
./xglquake -winsize 640 480 +_snd_mixahead 0.3 +xgl_quality high
```

| Tier | Also called | In short |
| --- | --- | --- |
| `ultralow` | `cg12`, `cg` | The coarsest walls the renderer makes, detail only close by, a twelfth of medium's detail allowance. For a Sun CG12. |
| `low` | `sx` | Coarse walls near the viewer only, original model triangles, a sixth of medium's detail allowance. For a Sun SX, or for the highest frame rate on a ZX. |
| `medium` | `zx` | The default settings. The intended settings for a Sun ZX and SM61 processor. |
| `high` | `creator` | Finer wall meshes to twice the range, four times the detail allowance, models subdivided twice. For a Creator3D. |
| `ultra` | `elite` | The finest meshes the renderer makes, to four times the range, with ten times the detail allowance. For an Elite3D. Note that this does not take advantage of hardware texture mapping at this time. |

A tier named after a board is a guess at settings for it, not a statement
that the board works. Which boards are expected to run XGLQuake at all, which
will not, and what each needs is listed under
[other Sun graphics hardware](docs/hardware.md#other-sun-graphics-hardware):
in short the ZX works, the Creator3D and Elite3D should, the SX may with
`-xglanydevice`, and the CG12 is unknown.

`+xgl_quality sx` is the same as `+xgl_quality low`, and so on. What each
tier sets:

Options take effect in the order given, and the last word wins. A setting
after the tier changes that one setting and keeps the rest of the tier; a
setting before it is replaced by the tier's value:

```sh
./xglquake +xgl_quality high +xgl_detailtris 4096
```

is `high` with a detail allowance of 4096. The same works in the console:
`xgl_quality low`, and `xgl_quality` alone names the tiers.

The tiers are first approximations and have not been timed on any machine.
Quake saves the last four settings of the table in `config.cfg`, so after a
run at another tier give `+xgl_quality` again, `medium` included, rather
than leaving it out.

### Common options

| Command-line option | Purpose |
| --- | --- |
| `-winsize 640 480` | Set the window size. |
| `+_snd_mixahead 0.3` | Buffer 0.3 seconds of audio; see [audio](#solaris-audio-and-hardware-interfaces). |
| `+xgl_alias_subdivide 0` | Keep original model triangles without extra subdivision. |
| `+xgl_alias_strips 0` | Send every model triangle separately. By default runs of four and more go out as triangle strips. |
| `+xgl_smp 0` | Select serial vertex preparation. The default detects multiple CPUs. Use 1 to request a helper. |
| `+xgl_detailtris 3072` | Set the detailed world triangle allowance; complete coarse faces remain. |
| `+xgl_detailerror 16` | Set how much a texture or shadow must differ before it is given more triangles; larger is coarser. |
| `+xgl_detaildensity 160` | Set how many triangles a face may have for its apparent size; 0 gives every face the same limit. |
| `+xgl_shapes 0` | Draw every wall as a smooth mesh. By default walls whose textures have clear shapes are cut along them; see [texture shapes](docs/render.md#texture-shapes). |
| `+xgl_keepbar 0` | Clear and redraw the status bar every frame. By default an unchanged bar is left in place in small windows. |
| `+xgl_profile 0` | Disable profiling for FPS measurements. Use 1 for summary timings or 2 for detail. |

More detail draws more triangles and costs frame rate: a larger
`xgl_detailtris` or `xgl_alias_subdivide` adds detail, and so does a
*smaller* `xgl_detailerror`, `xgl_detailedge` or `xgl_subdivide`. The detail
allowance is not a cap on all drawn triangles. See
[world detail](docs/render.md#world-detail) for how triangles are placed and
the [full settings table](docs/render.md#renderer-settings) for defaults and
other controls.

### Measure performance

Measure FPS without profiling:

```sh
./xglquake -winsize 640 480 +_snd_mixahead 0.3 +xgl_profile 0 +timedemo demo1 > xglquake-timedemo.txt 2>&1
```

Then collect a detailed profile with the same resolution and detail settings:

```sh
./xglquake -winsize 640 480 +_snd_mixahead 0.3 +xgl_profile 2 +timedemo demo1 > xglquake-profile.txt 2>&1
```

Keep the console closed during playback and quit after each timedemo finishes.
See [profiling details](docs/profile.md#profiling-details) for interpreting the
results. On the Sun ZX the frame rate moves in steps of whole retrace periods
(12.7, 15.2, 19.1 FPS), so small savings may not change the result.

## Hardware
### XGL benchmark

[xglbench](xglbench/README.md) tests basic graphics-card functionality and
performance and generates reports independently of the game. What it measured
on the Sun ZX, and how the renderer uses it, is in
[Measured on the Sun ZX](docs/hardware.md).

### Memory limits

The existing Solaris main allocates a 16 MiB engine heap. World geometry
cache storage is capped at another 8 MiB, CPU material pixels and optional
filter levels at another 8 MiB, and alias color caches at 1 MiB. The alias
mesh cache, its fixed entry table, the normal tables and the reusable work
array together are capped at another 1 MiB. The XGL audio ring is 32 KiB,
building world meshes uses about 0.35 MiB of fixed scratch space, and a
multiprocessor worker adds about 0.75 MiB. Cutting walls along
[texture shapes](docs/render.md#texture-shapes) uses another 0.25 MiB of
fixed scratch space and keeps about 3 KiB for each texture that has shapes;
examining a texture borrows memory while a map loads, 0.3 MiB for one of 64
texels square and about 4 MiB for the largest examined, 256 square.
Native contexts, window buffers, program data and
library allocations are additional. These limits are not a measurement of
total process memory.
A full world cache releases detailed meshes that have not been drawn recently
and otherwise falls back to complete coarse faces; exceeding the CPU material
limit reports an error rather than silently dropping assets. While a map
loads, the textures of the map before are still held within that limit, so
that those both maps have need not be prepared twice; they are released
first if the limit is reached.

### Solaris audio and hardware interfaces

Solaris audio uses `/dev/audio`, 11,025 Hz 16-bit stereo, and the original
mono fallback. The XGL-specific `xgl_snd_sun.c` retains those device interfaces
with a larger mixing buffer and corrected nonblocking submission. CD audio
remains the existing null driver. System services and BSD/UDP networking use
the original Solaris source files and libraries.

The original Solaris submission path writes at most 1,024 bytes per update,
only about 23 ms of stereo audio. The XGL path submits all available mixed
sound, retains unwritten data on partial writes/backpressure, and services
the mixer during rendering and cache preparation, at most every 25 ms. Its
default 0.3-second lookahead trades some sound latency for tolerance of long
frames. A single blocking graphics call longer than the queued audio can
still cause a gap; this is not an independent audio thread.

## More documentation

- [Rendering and tuning](docs/render.md): how the renderer works and every setting.
- [Measured on the Sun ZX](docs/hardware.md): device timings behind those choices.
- [Profiling details](docs/profile.md): reading `xgl_profile` reports.
- [Build details](docs/build.md) and [testing](docs/test.md).
- [Game data and display setup](docs/setup.md).
