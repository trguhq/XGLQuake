## Rendering and tuning

World, brush, alias and sprite materials remain CPU palette data. Geometry
is subdivided and sampled into vertex colors, then submitted to XGL; the
renderer does not upload textures or draw UI image rasters. BSP/PVS
visibility, brush/model transforms, animation and game collision use the
existing engine structures and rules. Sky and water use animated sampled
geometry. Menu, HUD, console and image-based labels use native stroke text
and geometric replacements, so their appearance differs from the original
pixel artwork; unknown pictures receive a labelled fallback.

Texture detail and sprite cutouts are geometric approximations. The XGL
renderer does not implement the OpenGL renderer's optional mirrors, projected
alias shadows or environment-map capture. Those features in the existing
OpenGL target are unchanged.

Several choices below follow from measurements of the target device; see
[Measured on the Sun ZX](hardware.md).

### Lighting

Baked BSP lighting and animated lightstyles are folded into the sampled
vertex colors, so the world needs one shading pass. A small light floor keeps
dark materials from going fully black, which lets native lights, which
multiply the vertex color, still show on them. This intentionally
approximates Quake's additive dynamic lighting.

Dynamic lights are XGL point lights: the `xgl_maxlights` lights nearest the
viewer, at most four. Their strength at distance d is 1/(1 + d/radius): half
at the light's radius and a fifth at four radii, never reaching zero as
Quake's does.

Walls and brush entities are submitted with native illumination only while it
can change the image, that is, while a dynamic light is active or
`xgl_ambient` is below 1. Otherwise they are submitted unlit with identical
colors, which the device draws considerably faster. Alias models are always
illuminated, by ambient light and a fixed directional fill plus any dynamic
lights; the two torch flame models are unlit. Sky and water are never
illuminated.

Translucent water and screen color effects use native blending; palette
effects appear as native overlay geometry.

### World detail

Every face has a coarse mesh: its polygon as a triangle fan, colored at the
corners. Faces near the viewer may also get a detailed mesh, subdivided up to
twice wherever a triangle spans more than `xgl_subdivide` texels. Colors are
filtered CPU samples of the texture; RGB mip averages for that share the
material memory budget and are never uploaded as XGL textures. Filtering
removes arbitrary single-texel colors; it cannot reproduce texture detail
smaller than the submitted triangles.

- Coarse meshes and their texture samples are built while the map loads, up
  to five eighths of the world cache; faces beyond that, on very large maps,
  are built when first seen.
- Detailed meshes are built on demand, at most `xgl_detailbuild` vertices per
  frame, so entering a new area fills in over a few frames instead of
  stalling one.
- Each frame, visible faces within `xgl_detaildistance` are ranked by
  projected size and the largest receive detail until `xgl_detailtris`
  triangles are used. A face detailed in the previous frame ranks slightly
  higher, so faces near the cut-off do not flicker between their two meshes.
- When the 8 MiB world cache is full, detailed meshes that have not been
  drawn recently are released to make room. Coarse meshes are kept.
- External brush models such as ammo and health boxes keep meshes of their
  own for the length of the map: up to 16 models of at most 64 faces each.
  Larger or further ones are streamed.

When a mesh cannot be built at all the face is still drawn, streamed as a
coarse fan each frame.

Light, lightstyle and gamma changes recolor a mesh from its stored texture
samples with integer arithmetic; the texture is not sampled again.

### Frame order

Changing between illuminated and unlit drawing is expensive on the Sun ZX,
so a frame is drawn in order of native state rather than kind:

1. walls, then brush entities;
2. sky and opaque water, sprites and particles (all unlit);
3. torch flames (unlit), then the illuminated models and the view weapon;
4. translucent water.

Depth testing makes the order of opaque geometry irrelevant to the image.
With no dynamic light active the first two groups share one state.

### View weapon

The weapon is drawn shrunk toward the eye to one eighth of its size. Every
vertex stays on its line of sight, so the image is unchanged, but the weapon
is nearer than any world geometry and needs no depth clear or extra
completion wait. The near clip plane is at 0.5 units instead of Quake's 4 so
that the shrunken weapon is clipped exactly where it used to be. As a side
effect, other models closer than four units to the eye are no longer cut
away; particles keep the four-unit limit.

### Animated models

Sampled skin colors are cached per model, skin, subdivision depth and player
colors as palette indices, so lighting, gamma and color changes take effect
at once. Subdivision topology is shared between poses, normals are cached
per pose, and positions are transformed from the live pose every frame. Each
loaded model carries an identity record, so a cache lookup compares a few
numbers instead of the model's command list. All of this is bounded: 1 MiB
for colors and 1 MiB for topology, normals and work space. A model whose
caches cannot be built is drawn by direct recursion instead.

`xgl_alias_subdivide` and `xgl_entity_triangles` bound the extra triangles.
The allowance is spent in visible-list order whatever the drawing order.

### Multiprocessor rendering

`xgl_smp` controls CPU preparation of vertices for animated models. The
default (`-1`) uses a worker thread when more than one CPU is online, which
means none on a single-processor machine. Use `0` to select the serial path
or `1` to request the worker explicitly.

While the main thread draws the world, the worker transforms cached model
positions and normals and prepares shaded, gamma-adjusted colors. When the
main thread reaches the models it runs any jobs the worker has not started,
waits for the one in progress, and submits the results to XGL in drawing
order. Models with cold caches and the view weapon use the serial path. If
the worker fails to start, the renderer reports the failure and continues
serially.

Up to 32 models and 16,380 output vertices per frame are prepared this way,
using about 0.75 MiB of scratch space allocated on first use plus the
worker's stack. Models beyond these limits draw serially at the detail level
already chosen for them.

Measure the benefit on the target with the
[Sun ZX timedemo comparison](test.md#compare-single-cpu-and-smp-rendering-on-the-sun-zx).

### Sky and water

Animated meshes sample each distinct vertex once per frame and reuse its
color at repeated triangle corners. Animation is updated every frame,
including sky scrolling, camera-relative sky projection and turbulent UVs.

### Native UI

Consecutive opaque rectangles share one polygon call. A run of characters is
one text call when the font's measured advances keep every glyph in its
fixed cell; otherwise glyphs are placed individually. Spaces stay inside a
run when the font's space is measured to advance exactly one cell. The digits
of a status-bar number are drawn as one centered text call rather than one
per digit, so they sit closer together than the original 24-pixel spacing.

### Renderer settings

| Setting | Default | Effect |
| --- | ---: | --- |
| `xgl_subdivide` | 128 | Texel span above which a detailed world triangle is split, clamped to 8–256; smaller is finer. |
| `xgl_detailtris` | 1024 | Detailed world/brush triangles per frame; faces beyond the allowance draw their coarse mesh. |
| `xgl_detaildistance` | 512 | Distance within which world faces are considered for detail. |
| `xgl_detailbuild` | 256 | Detailed-mesh vertices built per frame; one mesh is always allowed, 0 removes the limit. |
| `xgl_alias_subdivide` | 1 | Alias subdivision depth, 0–2; distant models use less. |
| `xgl_entity_triangles` | 512 | Additional alias subdivision triangles per frame; original model triangles remain. |
| `xgl_sprite_cells` | 8 | Sample grid per sprite dimension, clamped to 2–16. |
| `xgl_playercolors` | 1 | Apply original player shirt/pants palette translations to sampled skins. |
| `xgl_ambient` | 1 | Native ambient contribution, clamped to 0–1; below 1 keeps the world illuminated. |
| `xgl_maxlights` | 4 | Maximum native point lights, clamped to 0–4. |
| `xgl_smp` | -1 | Worker thread for model preparation: -1 automatic, 0 off, 1 on. |
| `xgl_profile` | 0 | 1 reports graphics timings and native call counts; 2 adds preparation and cache detail. Output goes to the terminal once per second. |
| `_snd_mixahead` | 0.3 | Audio lookahead in seconds for XGL; an existing archived value overrides this default. |
| `r_wateralpha` | 1 | Water opacity; values below 1 request native blending. |

Settings in configuration files or command-line options override these
defaults. In particular, `xgl_entity_triangles` is saved in `config.cfg`, so
an older saved value remains in effect until changed.

The original `r_dynamic`, `r_fullbright`, `r_novis`, `r_drawentities`,
`r_drawviewmodel`, `viewsize`, `fov`, `gamma` and `r_speeds` controls are also
available.

### Display mapping

The 3D context uses the benchmark's automatic full-raster VDC mapping.
Quake's view rectangle is selected through the VDC window and view clipping,
without explicitly setting `XGL_CTX_DC_VIEWPORT`. This preserves `viewsize`
and the space reserved for the status bar. Native UI uses device coordinates.

While the opaque console slides over the view, the 3D clip boundary follows
its lower edge without changing the camera or scaling the exposed scene.
One device pixel overlaps beneath the panel to avoid seams. Fully covered
views submit no 3D geometry; simulation continues normally.
