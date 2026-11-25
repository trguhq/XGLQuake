# Game data and display setup

For basic launch commands, see [game data and launch](../README.md#game-data-and-launch).

Run game commands from the directory containing `id1/`. The shareware
episode is the initial compatibility baseline. PAK recognition identifies
known file directories; it does not establish complete hardware validation
of every version.

Official Quake 1.00, 1.01 and 1.06 shareware PAK directories are recognized.
For a data directory elsewhere, use `-basedir /path/to/quake`, where that
directory contains `id1/pak0.pak`. Do not use `-game id1` or `-path` to select
the standard shareware data: the engine treats those options as modified
game paths. Startup reports the file count and directory CRC of each PAK
to help identify incompatible, repacked or extra files. Recognition of these
older official PAK versions is specific to the XGL target; registration and
modified-game checks remain enabled.

Use a local display with the Sun ZX's 24-bit TrueColor visual and DGA access.
The renderer requires hardware RGB, Z buffering, shading and two buffers.
Where a board has more than one 24-bit TrueColor visual, as the Creator3D
and Elite3D have, the one the hardware does not gamma-correct is chosen:
Quake applies its own gamma, and in a corrected ("linear") visual the
picture would be corrected twice and look washed out. Solaris is asked
which is which; startup names the visual chosen, and `-visualid` overrides
it. Startup also prints the gamma each such visual reports: a corrected
visual is taken to report 1 and an uncorrected one about 2.2, and if a
board turns out to report the other way round the picture will look washed
out and `-visualid` with the other visual puts it right. On a board with one
such visual, such as the ZX, nothing changes. This has not been run on
those boards.
`-display`, `-visualid`, `-width`, `-height`, `-winsize` and `-nomouse` select
the display/window/input settings. The video menu offers window sizes.
Original Quake console, save/load, demo, mouse sensitivity and key bindings
remain in the shared engine. The software-only `lcd_x` interlaced framebuffer
mode is not applied to XGL rendering.

## Other Sun graphics hardware

XGLQuake was written and measured on a Sun ZX and has run on nothing else.
What follows is from Sun's documentation, not from trial.

| Hardware | `xgl_quality` | Expected |
| --- | --- | --- |
| ZX, TurboZX | `zx` (medium) | Works: the board the renderer was made on. |
| Creator3D | `creator` (high) | Should work as it is. 24-bit windows with hardware double buffering and Z through XGL. See the note on gamma above. |
| Elite3D | `elite` (ultra) | Should work as it is, by the same reasoning; its XGL support is assumed, not confirmed. |
| Creator (not 3D) | none | Will not work: no double buffering or Z for 24-bit windows. |
| SX (SPARCstation 10SX, 20) | `sx` (low) | May work with `-xglanydevice` and memory reserved; see below. |
| CG12 (GS) | `cg12` (ultralow) | Unknown. Sun's Solaris 2.6 frame buffer handbook does not list it, and nothing found says whether XGL on 2.6 supports it. |
| GX, TurboGX, S24, PGX | none | Will not work: no 24-bit Z-buffered 3D. |

Startup refuses a device unless XGL reports direct access, RGB color, a
Z-buffer and shading all in hardware, and two window buffers.
`-xglanydevice` goes on with less, as long as XGL can do each of these at
all; the status bar is then redrawn every frame, and with a single buffer
frames are drawn in view and flicker.

### SX

The SX draws pixels and leaves transforms and lighting to the processor,
which is how the renderer works anyway, and the calls its XGL driver is
tuned for are the ones used here: three-sided polygons, and opaque 24-bit
triangle strips with a less-than-or-equal depth test. But its Z-buffer is in
main memory and a 24-bit window is double-buffered by copying, so startup
will probably need `-xglanydevice`.

- Reserve memory for it, which XGL uses for the Z-buffer and the back
  buffer at 4 bytes a pixel each: 8 MB for both, set in the system
  configuration and taken at boot. Sun's "Platform Notes: SPARCstation 10SX
  and SPARCstation 20 System Configuration Guide" has the procedure.
- Use a resolution that has a 24-bit visual. With 4 MB of video memory the
  larger resolutions (1280x1024 and up) have 8 bits a pixel only.
- Sun's benchmark gives it about a quarter of the ZX's shaded triangles.

```sh
./xglquake -xglanydevice -winsize 320 240 +_snd_mixahead 0.3 +xgl_quality sx
```

## Troubleshooting

If a native XGL call fails, capture both output streams:

```sh
./xglquake -winsize 320 240 +map e1m1 > xglquake.log 2>&1
```

The diagnostic includes the rendering stage, XGL object and operands, error
type/category, and the source line where the error was checked. Some Solaris
installations return an empty error ID and only `Error number` as the message;
the remaining fields are retained to help identify the failing attribute.
Startup console drawing begins before sound initialization, so a graphics
failure at that point does not imply an audio failure.
