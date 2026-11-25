## Rendering and tuning

World, brush, alias and sprite materials remain CPU palette data. Geometry
is subdivided and sampled into vertex colors, then submitted to XGL as
smooth-shaded triangles; the
renderer does not upload textures or draw UI image rasters. BSP/PVS
visibility, brush/model transforms, animation and game collision use the
existing engine structures and rules. Sky and water use animated sampled
geometry. Menu, console and image-based labels use native stroke text
and geometric replacements, so their appearance differs from the original
pixel artwork; unknown pictures receive a labelled fallback. The status
bar's pictures are drawn from their own pixels, as rectangles of color.

Texture detail and sprite cutouts are geometric approximations. The XGL
renderer does not implement the OpenGL renderer's optional mirrors, projected
alias shadows or environment-map capture. Those features in the existing
OpenGL target are unchanged.

Several choices below follow from measurements of the target device; see
[Measured on the Sun ZX](hardware.md).

### Renderer settings

| Setting | Default | Effect |
| --- | ---: | --- |
| `xgl_quality` | medium | A command, not a setting: `low`, `medium`, `high` or `ultra`, or `cg12`, `sx`, `zx`, `creator` or `elite` for the same four by hardware, sets the detail settings below together. Settings given after it override it one at a time. See the [README](../README.md#quality) for what each tier sets. |
| `xgl_subdivide` | 128 | Edge length, in texels, above which a detailed mesh always splits an edge, clamped to 8–256; smaller is finer. |
| `xgl_detailerror` | 16 | Color difference, of 255, that a new vertex must make to be worth adding, clamped to 1–255; smaller is finer. |
| `xgl_detailedge` | 16 | Edges of this many texels or less are never split, clamped to 4–64; smaller is finer. |
| `xgl_detailface` | 192 | Most triangles in one face's detailed mesh, clamped to 4–1024. |
| `xgl_detaildensity` | 160 | Triangles a face may have for its apparent size: this times its area over its squared distance, up to `xgl_detailface`. Larger gives near faces their limit sooner; 0 gives every face `xgl_detailface`. |
| `xgl_detailtris` | 3072 | Detailed wall and brush-entity triangles per frame; faces beyond the allowance draw their coarse mesh. |
| `xgl_detaildistance` | 512 | Distance within which faces are considered for detail and water uses its finer grid. |
| `xgl_detailbuild` | 128 | Detailed-mesh triangles built per frame; one mesh is always allowed, 0 removes the limit. |
| `xgl_shapes` | 1 | Cut walls along the shapes of their textures where a texture has them; 0 draws every wall as a smooth mesh. See [texture shapes](#texture-shapes). |
| `xgl_alias_subdivide` | 1 | Alias subdivision depth, 0–2; distant models use less. |
| `xgl_alias_strips` | 4 | Fewest triangles a run of a model's triangles must have to be sent as a triangle strip; 0 sends every triangle separately. |
| `xgl_entity_triangles` | 512 | Additional alias subdivision triangles per frame; original model triangles remain. |
| `xgl_sprite_cells` | 8 | Sample grid per sprite dimension, clamped to 2–16. |
| `xgl_playercolors` | 1 | Apply original player shirt/pants palette translations to sampled skins. |
| `xgl_ambient` | 1 | Ambient contribution, clamped to 0–1; below 1 keeps the world illuminated. |
| `xgl_maxlights` | 4 | Maximum native point lights, clamped to 0–4. |
| `xgl_lightfalloff` | 2 | How fast a dynamic light fades with distance, clamped to 1–64; 1 is the slowest and brightest. |
| `xgl_lightrange` | 4 | Distance, in light radii, beyond which geometry is not illuminated by a dynamic light; 0 removes the limit. |
| `xgl_alias_shade` | 1 | Shade models on the CPU and submit them unlit; 0 uses native illumination. |
| `xgl_hudpieces` | 16 | Rectangles of color a status-bar picture is drawn in, besides those its outline takes; 0 draws labelled stand-ins instead. |
| `xgl_keepbar` | -1 | Leave an unchanged status bar in place instead of clearing and redrawing it: -1 in small windows only, 0 never, 1 whenever possible. |
| `xgl_smp` | -1 | Worker thread for model preparation: -1 automatic, 0 off, 1 on. |
| `xgl_profile` | 0 | 1 reports graphics timings and native call counts; 2 adds preparation and cache detail. Output goes to the terminal once per second. |
| `_snd_mixahead` | 0.3 | Audio lookahead in seconds for XGL; an existing archived value overrides this default. |
| `r_wateralpha` | 1 | Water opacity; values below 1 request native blending. |

Changing `xgl_subdivide`, `xgl_detailerror`, `xgl_detailedge`,
`xgl_detailface`, `xgl_detaildensity` or `xgl_shapes` rebuilds the mesh
cache. Textures are not examined again for it.

Settings in configuration files or command-line options override these
defaults. In particular, `xgl_entity_triangles` is saved in `config.cfg`, so
an older saved value remains in effect until changed.

The original `r_dynamic`, `r_fullbright`, `r_novis`, `r_drawentities`,
`r_drawviewmodel`, `viewsize`, `fov`, `gamma` and `r_speeds` controls are also
available.

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
which is the sum the device used to compute. That light is worked out at the
model's own vertices and runs evenly between them, whether or not the model
is [subdivided](#animated-models). A dynamic light within reach
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
  areas get none.
- The difference is judged relative to the light there: divided by the
  brighter of the two baked light levels, as a fraction of full light and
  never less than a quarter. A texture in shadow then earns the triangles
  the same texture earns in the open, where an absolute difference would
  leave dim walls coarse beside bright ones.
- An edge whose middle agrees is not let pass on that alone. It is walked at
  the finest spacing a mesh may reach, up to 16 points, and each triangle is
  looked at in its middle and toward its corners; whatever that finds,
  further splits close in on. Otherwise the same texture is caught on one
  wall and missed on the next, according to where their corners happen to
  fall. These second looks stop once the edges already marked would use all
  the triangles the face may have.
- Edges of `xgl_detailedge` texels or less are never split, and a face never
  gets more triangles than its limit. When that limit binds, the edges with
  the largest differences are split first.
- The limit follows how large the face appears: `xgl_detaildensity` times
  its area over its squared distance, at most `xgl_detailface`. A face is
  built with half as many again in hand, and built again, larger, once the
  viewer has come close enough to double what it may have. Walking away
  keeps the finer mesh. One whose limit would be under 8 triangles keeps its
  coarse mesh. With a density of 0 every face may have `xgl_detailface`
  triangles at any distance, as before; the frame's allowance then goes to
  the first few large faces and the rest of the room stays coarse.

Every triangle that has a split edge uses the same new vertex, so no seam can
open inside a face. Each distinct vertex is colored once and shared by the
triangles around it. Its texture color is the mean of a box of texels about
three quarters as wide as the spacing of the vertices around it, taken from
reduced RGB images. Those share the material memory budget and are never
uploaded as XGL textures.

A mesh therefore shows a texture's shapes at about the scale of its
triangles: with the defaults, features of roughly 16 texels and more, such as
arches, discs, panels and borders. A pattern finer than that, such as small
bricks, averages to one color and is given no triangles. Colors that run
smoothly from vertex to vertex also soften every edge they cross; textures
with clear shapes are therefore drawn another way, described under
[texture shapes](#texture-shapes).

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
- A face that lies wholly outside the view is left out of the frame. The
  engine tests leaves against the view by their boxes and marks every face
  of a leaf that reaches in, so about half the triangles found that way
  belong to faces the view cannot show: 56%, 49% and 46% in replays of the
  three shareware demos. Each face's own box is therefore tested against the
  planes of the view that its node may cross. A face outside is not drawn,
  colored, lit or animated and takes nothing from `xgl_detailtris`. It is
  still built, after the faces in view and as far as the same allowance
  would reach, so that turning toward it finds its detailed mesh ready.
- When the 8 MiB world cache is full, detailed meshes that have not been
  drawn recently are released to make room. Coarse meshes are kept.
- External brush models such as ammo and health boxes keep meshes of their
  own for the length of the map: up to 16 models of at most 64 faces each.
  Larger or further ones are streamed.

When a mesh cannot be built at all the face is still drawn, streamed as a fan
each frame.

Meshes are stored as XGL color points, a position and a color, without a
normal: illumination is the only thing that reads one, and nearly every wall
is drawn unlit. The device draws such points about a tenth faster and the
meshes are a third smaller. A wall that a dynamic light reaches is drawn
from a second copy that has the face's normal, made the first time it is
needed, kept in step with the wall's colors, and given up when the cache is
short of room and no light has reached the face for a while. Without room
for a copy the wall is illuminated all the same, copied out with its normal
each frame. Sky, water, sprites, particles and CPU-shaded models are color
points too; a native call holds points of one kind.

Light, lightstyle and gamma changes recolor a mesh's distinct vertices from
their stored texture samples with integer arithmetic. The texture is not
sampled again and the mesh keeps its shape, which was decided with every
light style at its nominal level.

### Texture shapes

Some textures are made of a few shapes of nearly one color each: a panel, a
light strip, a door's grooves, a border. A smooth mesh blurs such shapes,
because every triangle fades from one vertex color to the next. A wall of
such a texture is instead cut along the shapes, and each piece is drawn in
one color under the baked light at its own corners. `xgl_shapes 0` turns
this off and draws every wall as a smooth mesh.

Each wall texture is examined once, while the map loads:

- The repeat of the texture is divided by the straight cut that most reduces
  its color error, then either part by its best cut, and so on, up to 48
  pieces. Cuts along the texture's axes are tried at every texel, singly and
  in pairs that take out a thin band of 2 to 12 texels, such as a groove or
  a strip.
- Where the edge a cut found lies differently in the two halves of a part,
  the cut is tilted to follow it, and kept only if the color really steps
  across the tilted line along its whole length. A part with a tilted side
  is also tried with a cut parallel to that side, since an outline is often
  a thin line of its own color.
- A round or chamfered shape, once upright and level cuts have boxed it
  in, has corners of another color. Each corner of a part's box is tried
  cut off at 45 degrees, 3 to 32 texels a leg, in place of the part's
  upright cut: a round lamp becomes an octagon and not a square or a
  staircase. Such a cut is made only where the color steps clearly across
  it along its whole length and the corner it cuts off is of one color; the
  corner of a square, or a vein across grain, is left alone.
- Tilted cuts must not lose what plain ones had. If, with as many pieces as
  the verdict below looks at, the division with tilts and corners describes
  the texture worse than upright and level cuts alone, the plain division
  is used.
- A piece is drawn in the color of what mostly fills it: the mean of its
  texels without the darkest and the brightest fifth, so that a thin line or
  a few specks not yet cut out do not tint it.
- The repeat is taken to begin where the picture has an edge, so that the
  border between repeats is not one more line through a shape.
- The texture is drawn this way only if the pieces describe it distinctly
  better than a smooth mesh with as many triangles would: they must leave at
  most 85% of its color error. Rock, brick, wood grain and rough metal do
  not pass and are drawn as before.
- A small texture is examined at its own scale. A sign 32 texels square has
  as much to show as a panel of 64, but a mesh with a vertex every 16 texels
  has four vertices for it and pays for two pieces, and on two pieces no
  sign passes. Along a side shorter than 64 texels a texture is therefore
  compared with a mesh of four cells, as the same picture drawn 64 texels
  long would be, on the pieces that mesh pays for: nine for a square one.
  A cut is asked for as much less as the texture has fewer texels than a
  square of 64. A picture and the same picture at twice the size are then
  divided alike, cut for cut. Grain, a smooth swell or a glow without an
  edge still has no shapes at any size.

In the shareware episode this selects between none and nearly half of a
map's wall area: about 46% in `e1m1`, 36% in `e1m5`, 17% in `start`, 3% in
`e1m2`, `e1m6` and `e1m8` and under 1% elsewhere, and four fifths of the
faces of the ammunition and health boxes. Of the 97 wall textures with a
side under 64 texels, 58 have shapes: arrows, the exit sign, keys, runes,
buttons, small lights, trim strips and the box faces.

A face is cut by the lines of its texture's division, repeated as the
texture repeats. Pieces share their corners along their sides, so no seam
can open between them, and a piece longer than `xgl_subdivide` texels is
halved so that baked light is still sampled along it. How many of the cuts
are made depends on a step of detail that follows the distance:

- A repeat of the texture gets as many pieces as `xgl_detaildensity` pays
  for at that distance, in steps of 1, 2, 3, 4, 6, 8 and so on.
- A step shows like details all or none. The bars of one light, or four
  lamps in a row, are found one after another and are worth about the same;
  a step that made just the cuts it pays for would stop among them and show
  one bar of five, then two. A step therefore ends only where every cut it
  makes is worth half as much again as any it leaves out, at the nearest
  such place to the pieces it pays for and at most twice as many. Over the
  shareware textures that shows a set of like pieces in part at 34 of the
  steps, where the count alone did at 215, for about the same triangles.
- A wall is cut alike all over. Faces in one plane, facing one way, with
  one texture laid one way, that are joined by their edges are one wall,
  found when the map loads. The whole wall is cut at the step its nearest
  face asks for, so that the pattern runs unbroken across it. Left to their
  own distances, more than half of the neighbouring faces of such walls
  were cut differently. The far end of a long wall is in consequence cut
  finer than its distance alone would have it.
- The nearest face is the nearest from where the viewer stands, in view or
  not. Turning the head, which takes the near end of a wall out of view and
  brings it back, therefore changes nothing: only walking does. Counting
  only the faces in view, the rest of a wall went coarser or plain when its
  near end left the view, and was built finer again when it came back. In
  replays of walks through `e1m1`, `e1m5` and `start` this costs 2 to 4%
  more wall triangles and builds up to a fifth fewer meshes.
- A repeat of a small texture gets the pieces that one of 64 texels square
  would. By its own texels a sign 32 texels square would have fewer than
  two pieces from 200 units and stay plain; it is cut out to about 385, as
  a panel of 64 is.
- A cut must also be worth its triangles, by the same measure as a split in
  a smooth mesh (`xgl_detailerror` and `xgl_detailedge`); nearer than about
  160 units, where a texel is wider than a pixel, a cut needs to be worth
  less. A small texture's cut is weighed as the same cut at full size would
  be. A part that would be narrower than a pixel and a half is not cut off,
  whatever the size of the texture: it would only flicker.
- A wall is made finer as soon as the frame's building allows, but coarser
  only when it asks for half the pieces or fewer, and then one wall every
  four frames, all its faces following one another so that it is not long
  cut two ways. A step that would make the same cuts as the mesh a wall has
  does not build it again.
- A face that should be cut and has no cut mesh at all is built before any
  other mesh in the frame. A wall that is plain where it should be cut
  shows more than one a step too coarse, and walking into a room of such
  walls a third of them used to wait their turn, plain, for some frames.
- A wall that is cut stays cut until its nearest face is about 1.4 times
  as far off as where it would first be cut, and a cut face stays in range
  of detail as far beyond `xgl_detaildistance`. Stepping back and forth
  across either distance then changes nothing. Beyond that the wall goes
  plain all at once; the cut meshes it had wait in the cache, and serve
  again if it is cut again before their memory is wanted.
- One face has at most 256 triangles. A large face of a busy texture is cut
  at the finest step that fits; that is found out the first time and
  remembered.
- These walls take part in the frame's detail allowance like others, with
  two differences. Their faces rank by nearness alone, so that when the
  allowance runs out it is the far walls that go plain and not the small
  faces of a near one; and they may run a quarter over `xgl_detailtris`
  before any is left plain.

An animated texture keeps one division for all its frames, that of the
frame with the most pieces, and each piece takes the color it has in the
frame showing. The mesh is not built again as the texture changes.

Examining the textures adds to the time a map takes to load, and the engine
reports how long it took. On the SM61 the 72 textures of `e1m3` and its
item boxes, the map of `demo1`, took 5.3 seconds. The examination was then
made to do 43% less work for that map, and 35% less over the whole
shareware episode, with the same result for every texture, and took 3.0
seconds on the machine. Examining small textures at their own scale divides
more of them to the end: about 11% more work for that map and 7% more over
the episode, counted on a development host and not yet timed on the
machine.

A texture is examined once for as long as it stays loaded, not once a map.
While a map loads, the textures of the one before are kept; one the new map
asks for again, the same texels under the same name, is the same material
as before, with its reduced images and its division. The others are
released when the map has loaded, or sooner if their memory is wanted. The
same map again, as after a death, therefore examines nothing, and `e1m4`
after `e1m3` examines 49 textures where it would examine 71 from nothing.

Replayed off the machine on the same maps, walls cut this way cost about
what they cost before: in views of `e1m1`, 1725 wall triangles a frame
against 1785, with the mean color error against the full textures down from
9.9 to 9.4 of 255. Those figures are from before faces outside the view
were left out. Small textures were brought in later: in the same views of
`e1m1` the wall triangles stayed at 1017 a frame, and in replays of the
three demos the world's triangles rose by 1 to 3%, mostly on the boxes;
detailed meshes were built 9 to 18% more often.

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
at once. The subdivided mesh is kept for each model and depth and shared by
all its poses and skins; nothing is kept for a single pose. Each loaded
model carries an identity record, so a cache lookup compares a few numbers
instead of the model's command list. All of this is bounded: 1 MiB for
colors and 1 MiB for meshes, normal tables and work space. A model whose
caches cannot be built is drawn by direct recursion instead.

`xgl_alias_subdivide` and `xgl_entity_triangles` bound the extra triangles.
The allowance is spent in visible-list order whatever the drawing order.

A model's drawing commands name most of its vertices several times: once
for each strip or fan that passes through, and again where the skin has a
seam. Subdivision names the middle of an edge once for each triangle on it.
The loader records which of the commands' vertices are one vertex of the
model, and the mesh counts every place once. Its points are the model's
vertices and the middles of the edges between them.

- Every frame each point is placed once from the live pose: a vertex by the
  model's transform, a middle half way between its two ends. For the models
  of the shareware episode the points are 35% of the vertices the commands
  name, and 45% of those named after one subdivision.
- Light is worked out for the same points. A vertex has the light of its
  normal in that pose, worked out once for each normal the pose uses. The
  model's normals are used as they are stored: they are those of its true
  shape, and are not adjusted for the scale its vertices are stored at,
  which bent them badly on the unevenly scaled view weapons. The middle of an
  edge has the mean of its two ends' light. Light therefore runs evenly
  along every edge of the model's own triangles, as it does on a model that
  is not subdivided: subdivision adds detail from the skin and does not
  round the shading.
- Some models have a face made twice over, once for each side: two
  triangles on the same three vertices, wound opposite ways and colored
  from different places on the skin. The nailgun has six such pairs, as
  view weapon and as item, and five monsters one each. Quake hides the side
  turned away; the device here hides nothing, and the two sides fought for
  the same depth. Such pairs are found when a model's mesh is made, and each
  frame only the side facing the viewer is drawn. A model that has any goes
  out as separate triangles, not strips.
- Under native illumination (`xgl_alias_shade 0`) the device needs a normal
  for every point instead. A middle then has the mean of its ends' normals
  brought back to unit length, which is a square root for each middle every
  frame. Where two opposite normals leave a middle no direction of its own,
  it takes the normal of the end that is the earlier vertex of the model.

A model's triangles are sent as triangle strips where they join up. On the
Sun ZX, smooth-shaded strips of eight triangles and more took a third less
time for each triangle than separate ones, and a strip needs one new vertex
for each triangle instead of three, so the CPU also writes fewer than half
as many.

- The loader also records which vertices of a model's command list are at
  the same place on the skin as well as in space. When a model's subdivided
  mesh is first built, its triangles are joined across those vertices, and
  across the middles of the edges between them, into strips of up to 126
  triangles. That is done once for each model and depth; scaled from a
  development host it should take the SM61 a few milliseconds, and some
  tens for the largest model.
- Every corner that a strip shares has the same position, light and skin
  color, so strips draw exactly the triangles that separate triangles
  would. A seam of the skin is one point in space with two colors, and no
  strip crosses it.
- `xgl_alias_strips` is the fewest triangles a run must have to be sent as a
  strip. Shorter runs go out as separate triangles in the ordinary batch.
  The default of 4 is cautious: how short a strip still pays on the Sun ZX
  has not been measured. 0 sends every triangle separately.
- Strips are points without normals. Models the device illuminates
  (`xgl_alias_shade 0`) are therefore still sent as separate triangles, as
  is a model whose skin has holes.

With the default settings the models of the shareware episode come to about
1.3 vertices a triangle near the viewer, where they are subdivided, and 1.6
far off, against 3 before.

### Multiprocessor rendering

`xgl_smp` controls CPU preparation of vertices for animated models. The
default (`-1`) uses a worker thread when more than one CPU is online, which
means none on a single-processor machine. Use `0` to select the serial path
or `1` to request the worker explicitly.

While the main thread draws the world, the worker places each model's points
from its pose, works out their light and prepares gamma-adjusted colors. When the
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

A run of characters is one text call when the font's measured advances keep
every glyph in its fixed cell; otherwise glyphs are placed individually.
Spaces stay inside a run when the font's space is measured to advance exactly
one cell. The digits of a status-bar number are drawn as one centered text
call rather than one per digit, so they sit closer together than the original
24-pixel spacing.

Text is not drawn when it is asked for. It waits until the UI is complete,
or until a rectangle is asked for that would cover part of it, and the
opaque rectangles asked for in the meantime go out in one polygon call
instead of one before every piece of text. The picture is the one the
requests would have drawn in order. Runs of one size and color are then
drawn together, which leaves the fewest attribute changes. A status bar is
one polygon call followed by its dozen text calls.

Each picture Quake draws is told apart by its name once, when first asked
for, and found by its handle after that. A box with an edge is two
rectangles, the edge color under the fill, not a fill and four strips.

### Status bar pictures

The pictures of the status bar, and the other small pictures the game keeps
with them (in Quake, those of `gfx.wad`), are drawn from their own pixels.
Each is divided once, when first drawn, into rectangles of one color: the
most telling upright or level cut first, then the next. Where a picture has
transparent pixels its outline is followed first, up to 64 rectangles, so a
digit has exactly its own shape; after that its colors are told apart until
`xgl_hudpieces` rectangles show. Each rectangle has the mean color of the
pixels it covers. Nothing depends on what a picture is called or shows, so
another game's status bar is drawn from its own art in the same way.

With the shareware pictures a digit takes 8 to 35 rectangles, and at the
default of 16 a face, a weapon or an ammunition box 16 each, as does either
bar: recognisable, not detailed. A full bar comes to some 400 to 500
rectangles, sent in the polygon calls the bar already made, where it had
about 40 and a dozen pieces of text. Their cost has not been timed on the
Sun ZX. In a small window the bar is drawn only when it changes; in a large
one it is drawn every frame, and a lower setting then saves time. 0 draws
the earlier stand-ins, which the menus and the larger pictures still use:
those are told apart by name.

### Status bar

Quake's status bar code already knows when the bar has changed and draws it
only then, once for each of the two buffers; that is how it worked on
page-flipped video modes. The renderer used to clear the whole window every
frame and so had to draw the bar every frame, the largest fixed cost in one.

A frame now asks its clear to stop above the bar and leaves the bar to
Quake. The clear is cut short the way the benchmark's viewport probe did it
on the Sun ZX: XGL's automatic mapping is turned off and a shorter device
viewport set for the clear alone, then XGL's own mapping of the whole window
is restored before anything is drawn. Such a clear is slower than clearing
everything, by about 2 ms at 320x240 and in proportion to the view beyond
that, so `xgl_keepbar` -1 does this only where the view above the bar is at
most 100,000 pixels; 1 does it at any size and 0 never.

The bar's rows have to hold nothing but the bar. A frame with the console
showing, a menu, a dialog, the loading plaque, an intermission, a chat line
or a center message clears and draws everything as before, and so do the two
frames after one, since each buffer still holds what that frame left. The
bar is also drawn again when the window is moved, mapped or exposed, and
every 128 frames whatever the game says. If the device's viewport is not the
window's rows counted from the top, or the window is scaled, the bar is not
kept at all; a line on the terminal says so.

### Display mapping

The 3D context uses the benchmark's automatic full-raster VDC mapping.
Quake's view rectangle is selected through the VDC window and view clipping,
so geometry is drawn with XGL's own device viewport. This preserves
`viewsize` and the space reserved for the status bar. The one explicit
`XGL_CTX_DC_VIEWPORT` is the shorter one a clear uses to leave the status
bar alone, described above. Native UI uses device coordinates.

While the opaque console slides over the view, the 3D clip boundary follows
its lower edge without changing the camera or scaling the exposed scene.
One device pixel overlaps beneath the panel to avoid seams. Fully covered
views submit no 3D geometry; simulation continues normally.
