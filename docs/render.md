## Rendering and tuning

World, brush, alias and sprite materials remain CPU palette data. Geometry
is subdivided and sampled into vertex colors, then submitted to XGL as
smooth-shaded triangles; the
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
viewer, at most four. XGL attenuates a light by 1/(C1 + C2·d) and offers
nothing steeper, so a light's strength at distance d is
1/(1 + `xgl_lightfalloff`·d/radius). With the default of 2 that is two thirds
at a quarter of the light's radius, a third at its radius and a ninth at four
radii. Quake's own lights end at their radius; these never quite do. A light
multiplies the vertex color, so it shows most on dim surfaces and cannot
brighten one that is already at full brightness.

Only geometry a light can reach is illuminated: walls that face a light and
lie within `xgl_lightrange` radii of it, and brush entities whose bounds do.
Every other wall, and every wall while no dynamic light is active, is
submitted unlit with identical colors, which the device draws considerably
faster. A light that reaches nothing visible is switched off. At the default
range of four radii the light that is cut off adds about 11% to a surface
facing it directly. A larger `xgl_lightfalloff` draws the light in toward its
source and makes that step smaller, at the price of a fainter light: 8 left a
muzzle flash barely visible. `xgl_lightfalloff 1` with `xgl_lightrange 0` is
the original behavior, a slow falloff applied to every wall facing a light.
`xgl_ambient` below 1 keeps every wall illuminated.

Animated models are shaded on the CPU and submitted unlit. Each vertex color
is multiplied by ambient light plus a fixed directional fill from above,
which is the sum the device used to compute. A dynamic light within reach
raises the whole model's light level by its radius less its distance, as
Quake does, instead of lighting the side that faces it. `xgl_alias_shade 0`
returns models to native illumination by the fill and every dynamic light.
The two torch flame models are drawn as sampled. Sky and water are never
illuminated.

Translucent water and screen color effects use native blending; palette
effects appear as native overlay geometry.

### World detail

Every face has a coarse mesh: its own polygon, cut into a zig-zag of
triangles and colored at its corners. Faces near the viewer may also get a
detailed mesh, which starts from the coarse one and adds triangles where they
would show something:

- An edge longer than `xgl_subdivide` texels is always split. That keeps
  baked light adequately sampled across large plain faces.
- Any other edge is split if a vertex at its middle would differ, in any
  color channel, by more than `xgl_detailerror` of 255 from what its two ends
  already interpolate. The comparison is of filtered texture times baked
  light, so texture shapes and shadow edges both attract triangles and even
  areas get none. An edge's quarter points and a triangle's middle are
  checked too, so that something lying between the vertices is not missed.
- Edges of `xgl_detailedge` texels or less are never split, and a face never
  gets more than `xgl_detailface` triangles. When that limit binds, the
  edges with the largest differences are split first.

Every triangle that has a split edge uses the same new vertex, so no seam can
open inside a face. Each distinct vertex is colored once and shared by the
triangles around it. Its texture color is the mean of a box of texels about
three quarters as wide as the spacing of the vertices around it, taken from
reduced RGB images. Those share the material memory budget and are never
uploaded as XGL textures.

A mesh therefore shows a texture's shapes at about the scale of its
triangles: with the defaults, features of roughly 16 texels and more, such as
arches, discs, panels and borders. A pattern finer than that, such as small
bricks, averages to one color and is given no triangles.

- Coarse meshes and their texture samples are built while the map loads, up
  to five eighths of the world cache; faces beyond that, on very large maps,
  are built when first seen.
- Detailed meshes are built on demand, at most `xgl_detailbuild` triangles
  per frame, so entering a new area fills in over a number of frames instead
  of stalling one. A face with nothing to show in more detail is examined
  once and keeps only its coarse mesh.
- Each frame, visible faces within `xgl_detaildistance` are ranked by
  projected size and the largest receive their detailed mesh until
  `xgl_detailtris` triangles are used. A face detailed in the previous frame
  ranks slightly higher, so faces near the cut-off do not flicker between
  their two meshes.
- When the 8 MiB world cache is full, detailed meshes that have not been
  drawn recently are released to make room. Coarse meshes are kept.
- External brush models such as ammo and health boxes keep meshes of their
  own for the length of the map: up to 16 models of at most 64 faces each.
  Larger or further ones are streamed.

When a mesh cannot be built at all the face is still drawn, streamed as a fan
each frame.

Light, lightstyle and gamma changes recolor a mesh's distinct vertices from
their stored texture samples with integer arithmetic. The texture is not
sampled again and the mesh keeps its shape, which was decided with every
light style at its nominal level.

### Frame order

Changing between illuminated and unlit drawing is expensive on the Sun ZX,
so a frame is drawn in order of native state rather than kind:

1. the walls and brush entities a dynamic light reaches, illuminated;
2. everything else, unlit: the other walls and brush entities, sky and
   opaque water, sprites, particles, models and the view weapon;
3. translucent water.

Depth testing makes the order of opaque geometry irrelevant to the image.
Most frames have no dynamic light and so no first group and no change of
illumination at all.

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
positions, evaluates each vertex's shade and prepares gamma-adjusted colors. When the
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

Sky and water faces are cut on regular grids aligned to the world axes, so
that neighboring faces share every vertex along their common edges: 128 units
for sky and for distant water, 64 units for water within
`xgl_detaildistance`. Each distinct vertex is colored once per frame.

A grid of that size cannot show a liquid texture's own 64-texel pattern, and
a single texel taken at each vertex only flickers. Each water, slime, lava or
teleporter vertex instead moves between the texture's two tones, the averages
of its darker and its lighter half, on a long slow wave with a slight swell
in brightness. On the finer grid a shorter crossing wave is added; it fades
out with distance, so the change to the coarser grid cannot be seen. The
texture is sampled once, when first used. Water in range always uses its
finer grid and is not charged to `xgl_detailtris`.

For sky, each of the two layers is reduced to 16 by 16 cells, the cloud layer
with its coverage. A vertex takes the scrolling cloud layer laid over the
scrolling back layer, interpolated between cells, through GLQuake's sky
projection.

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
| `xgl_subdivide` | 128 | Edge length, in texels, above which a detailed mesh always splits an edge, clamped to 8–256; smaller is finer. |
| `xgl_detailerror` | 16 | Color difference, of 255, that a new vertex must make to be worth adding, clamped to 1–255; smaller is finer. |
| `xgl_detailedge` | 16 | Edges of this many texels or less are never split, clamped to 4–64; smaller is finer. |
| `xgl_detailface` | 192 | Most triangles in one face's detailed mesh, clamped to 4–1024. |
| `xgl_detailtris` | 2048 | Detailed wall and brush-entity triangles per frame; faces beyond the allowance draw their coarse mesh. |
| `xgl_detaildistance` | 512 | Distance within which faces are considered for detail and water uses its finer grid. |
| `xgl_detailbuild` | 128 | Detailed-mesh triangles built per frame; one mesh is always allowed, 0 removes the limit. |
| `xgl_alias_subdivide` | 1 | Alias subdivision depth, 0–2; distant models use less. |
| `xgl_entity_triangles` | 512 | Additional alias subdivision triangles per frame; original model triangles remain. |
| `xgl_sprite_cells` | 8 | Sample grid per sprite dimension, clamped to 2–16. |
| `xgl_playercolors` | 1 | Apply original player shirt/pants palette translations to sampled skins. |
| `xgl_ambient` | 1 | Ambient contribution, clamped to 0–1; below 1 keeps the world illuminated. |
| `xgl_maxlights` | 4 | Maximum native point lights, clamped to 0–4. |
| `xgl_lightfalloff` | 2 | How fast a dynamic light fades with distance, clamped to 1–64; 1 is the slowest and brightest. |
| `xgl_lightrange` | 4 | Distance, in light radii, beyond which geometry is not illuminated by a dynamic light; 0 removes the limit. |
| `xgl_alias_shade` | 1 | Shade models on the CPU and submit them unlit; 0 uses native illumination. |
| `xgl_smp` | -1 | Worker thread for model preparation: -1 automatic, 0 off, 1 on. |
| `xgl_profile` | 0 | 1 reports graphics timings and native call counts; 2 adds preparation and cache detail. Output goes to the terminal once per second. |
| `_snd_mixahead` | 0.3 | Audio lookahead in seconds for XGL; an existing archived value overrides this default. |
| `r_wateralpha` | 1 | Water opacity; values below 1 request native blending. |

Changing `xgl_subdivide`, `xgl_detailerror`, `xgl_detailedge` or
`xgl_detailface` rebuilds the mesh cache.

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
