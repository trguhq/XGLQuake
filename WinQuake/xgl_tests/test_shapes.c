/* Host contracts of the texture shape analysis and of cutting a face along
 * a texture's shapes. Nothing here draws or measures time. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quakedef.h"
#include "xgl_render.h"

byte xgl_palette[768];

#define IMAGES 8
static struct {
    int width, height;
    unsigned int revision;
    byte texels[256 * 256];
} images[IMAGES];
static texture_t textures[IMAGES];
static unsigned int next_revision;
static int texel_calls, audio_calls;

unsigned int XGL_TextureRevision(int id)
{
    return id >= 1 && id <= IMAGES ? images[id - 1].revision : 0;
}
const byte *XGL_TextureTexels(int id, int *width, int *height)
{
    assert(id >= 1 && id <= IMAGES && images[id - 1].revision);
    ++texel_calls;
    *width = images[id - 1].width; *height = images[id - 1].height;
    return images[id - 1].texels;
}
void XGL_AudioUpdate(void) { ++audio_calls; }

/* The pictures. Every palette entry is the gray of its own index, so a
 * texel's value is its color. */
static int flat(int x, int y) { (void)x; (void)y; return 200; }
static int noise(int x, int y)
{
    unsigned int n = (unsigned int)x * 73856093u ^ (unsigned int)y * 19349663u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return 60 + (int)((n >> 8) % 120);
}
static int ramp(int x, int y)
{
    return 128 + (int)(60 * sin(x * (2 * 3.14159265 / 64)) * cos(y * (2 * 3.14159265 / 64)));
}
/* A light ground with a dark panel on it and a thin bright strip across. */
static int panel(int x, int y)
{
    if (y >= 52 && y < 56) return 250;
    if (x >= 16 && x < 40 && y >= 12 && y < 44) return 40;
    return 200;
}
/* The same shapes in other colors: another frame of an animation. */
static int panel_lit(int x, int y)
{
    if (y >= 52 && y < 56) return 120;
    if (x >= 16 && x < 40 && y >= 12 && y < 44) return 90;
    return 30;
}
/* A dark band 32 texels wide, upright: its sides are the only edges. */
static int stripe(int x, int y) { (void)y; return x >= 20 && x < 52 ? 40 : 200; }
/* Dark below a line that climbs one texel in two, light above. */
static int slope(int x, int y) { return 2 * y - x > 40 ? 40 : 200; }

static texture_t *make(int index, int width, int height, int (*picture)(int, int))
{
    int x, y;
    assert(index >= 0 && index < IMAGES && width <= 256 && height <= 256);
    images[index].width = width; images[index].height = height;
    images[index].revision = ++next_revision;
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) images[index].texels[y * width + x] = (byte)picture(x, y);
    memset(&textures[index], 0, sizeof(textures[index]));
    textures[index].gl_texturenum = index + 1;
    textures[index].width = (unsigned)width; textures[index].height = (unsigned)height;
    return &textures[index];
}

/* The node whose piece holds a texel when every cut is made. */
static int node_at(const xgl_shape_tree_t *tree, int x, int y)
{
    const xgl_shape_node_t *node;
    float s, t;
    int index;
    s = (float)((x - tree->origin[0] + tree->width) % tree->width) + .5f;
    t = (float)((y - tree->origin[1] + tree->height) % tree->height) + .5f;
    for (index = 0;;) {
        node = &tree->nodes[index];
        if (node->child[0] < 0) return index;
        index = node->child[node->a * s + node->b * t >= node->d];
    }
}

static int near_value(int value, int wanted) { return abs(value - wanted) <= 3; }

static void check_tree(const xgl_shape_tree_t *tree)
{
    const xgl_shape_node_t *node;
    int i, leaves, pairs;
    assert(tree->count >= 3 && (tree->count & 1) && tree->count <= 2 * XGL_SHAPE_PIECES - 1);
    assert(tree->origin[0] >= 0 && tree->origin[0] < tree->width);
    assert(tree->origin[1] >= 0 && tree->origin[1] < tree->height);
    assert(tree->colors && !tree->steps_key);
    leaves = pairs = 0;
    for (i = 0; i < tree->count; ++i) {
        node = &tree->nodes[i];
        if (node->child[0] < 0) { ++leaves; assert(node->child[1] < 0); continue; }
        /* Cuts are in the order they were found: a node's children come
         * after it, side by side. */
        assert(node->child[0] > i && node->child[1] == node->child[0] + 1 && node->child[1] < tree->count);
        assert(node->gain > 0 && node->width > 0);
        assert((node->flags & XGL_SHAPE_AXIS) <= 2);
        if ((node->flags & XGL_SHAPE_AXIS) == 0) assert(node->a == 1 && node->b == 0);
        if ((node->flags & XGL_SHAPE_AXIS) == 1) assert(node->a == 0 && node->b == 1);
        if (node->flags & XGL_SHAPE_PAIRED) {
            /* The band's other side is cut from the far part at once. */
            ++pairs;
            assert(tree->nodes[node->child[1]].flags & XGL_SHAPE_SECOND);
            assert(tree->nodes[node->child[1]].child[0] == node->child[1] + 1);
            assert(tree->nodes[node->child[1]].gain == node->gain);
        }
    }
    assert(leaves == (tree->count + 1) / 2);
    (void)pairs;
}

static void test_what_has_shapes(void)
{
    xgl_shape_tree_t *tree;
    int examined, shaped, calls;
    XGL_ShapesClear();
    /* Nothing to follow: one color, grain, and a smooth swell, which a
     * smooth mesh shows better than any pieces would. */
    assert(!XGL_TextureShapes(make(0, 64, 64, flat)));
    assert(!XGL_TextureShapes(make(1, 64, 64, noise)));
    assert(!XGL_TextureShapes(make(2, 64, 64, ramp)));
    XGL_ShapesCount(&examined, &shaped);
    assert(examined == 3 && shaped == 0);
    /* Too small or too large to look at, or no texture at all. */
    assert(!XGL_TextureShapes(make(3, 4, 64, panel)));
    assert(!XGL_TextureShapes(NULL));
    textures[3].gl_texturenum = 0;
    assert(!XGL_TextureShapes(&textures[3]));
    textures[3].gl_texturenum = IMAGES + 1;
    assert(!XGL_TextureShapes(&textures[3]));
    XGL_ShapesCount(&examined, &shaped);
    assert(examined == 3 && shaped == 0);
    /* A panel and a strip. */
    tree = XGL_TextureShapes(make(4, 64, 64, panel));
    assert(tree && tree->width == 64 && tree->height == 64);
    check_tree(tree);
    XGL_ShapesCount(&examined, &shaped);
    assert(examined == 4 && shaped == 1);
    assert(audio_calls > 0); /* Sound is kept fed while a texture is examined. */
    /* Each piece has the color of what fills it. */
    assert(near_value(tree->colors[3 * node_at(tree, 28, 28)], 40));
    assert(near_value(tree->colors[3 * node_at(tree, 17, 13)], 40));
    assert(near_value(tree->colors[3 * node_at(tree, 38, 42)], 40));
    assert(near_value(tree->colors[3 * node_at(tree, 4, 4)], 200));
    assert(near_value(tree->colors[3 * node_at(tree, 50, 30)], 200));
    assert(near_value(tree->colors[3 * node_at(tree, 28, 48)], 200));
    assert(near_value(tree->colors[3 * node_at(tree, 3, 54)], 250));
    assert(near_value(tree->colors[3 * node_at(tree, 60, 53)], 250));
    /* Asked again, it is not examined again. */
    calls = texel_calls;
    assert(XGL_TextureShapes(&textures[4]) == tree && texel_calls == calls);
    /* Loaded again with other texels, it is. */
    make(4, 64, 64, flat);
    assert(!XGL_TextureShapes(&textures[4]) && texel_calls == calls + 1);
    XGL_ShapesCount(&examined, &shaped);
    assert(examined == 5 && shaped == 1);
    XGL_ShapesClear();
    XGL_ShapesCount(&examined, &shaped);
    assert(examined == 0 && shaped == 0);
}

static void test_bands_origin_and_slopes(void)
{
    xgl_shape_tree_t *tree;
    const xgl_shape_node_t *node;
    int i, bands, tilted, strip;
    float x, y;
    XGL_ShapesClear();
    /* The thin strip is taken out as a band: two cuts that go together,
     * and the node knows how thin the thinner part is. */
    tree = XGL_TextureShapes(make(0, 64, 64, panel));
    assert(tree);
    bands = 0; strip = -1;
    for (i = 0; i < tree->count; ++i) {
        node = &tree->nodes[i];
        if (node->child[0] < 0 || !(node->flags & XGL_SHAPE_PAIRED)) continue;
        ++bands;
        if ((node->flags & XGL_SHAPE_AXIS) == 1 && fabs(tree->nodes[node->child[1]].width - 4) < .01f) strip = i;
    }
    assert(bands >= 1 && strip >= 0);
    /* The repeat is taken to begin where the picture has an edge, so that
     * the border between repeats is not one more line across a shape. */
    tree = XGL_TextureShapes(make(1, 64, 64, stripe));
    assert(tree);
    check_tree(tree);
    assert(tree->origin[0] == 20 || tree->origin[0] == 52);
    assert(near_value(tree->colors[3 * node_at(tree, 30, 5)], 40));
    assert(near_value(tree->colors[3 * node_at(tree, 5, 60)], 200));
    /* An edge that slopes is followed by a tilted cut. */
    tree = XGL_TextureShapes(make(2, 64, 64, slope));
    assert(tree);
    check_tree(tree);
    tilted = 0;
    for (i = 0; i < tree->count; ++i) {
        node = &tree->nodes[i];
        if (node->child[0] < 0 || (node->flags & XGL_SHAPE_AXIS) != 2) continue;
        ++tilted;
        assert(node->a != 0 && node->b != 0);
    }
    assert(tilted >= 1);
    /* Either side of the edge, well clear of it, has its own color. */
    for (i = 0; i < 40; ++i) {
        x = (float)(i * 37 % 64); y = (float)(i * 23 % 64);
        if (fabs(2 * y - x - 40) < 6 || x < 2 || x > 61 || y < 2 || y > 61) continue;
        assert(near_value(tree->colors[3 * node_at(tree, (int)x, (int)y)], 2 * y - x > 40 ? 40 : 200));
    }
    XGL_ShapesClear();
}

static void test_animated_frames(void)
{
    xgl_shape_tree_t *tree;
    texture_t *plain, *first, *second, *small;
    const byte *colors;
    int i;
    XGL_ShapesClear();
    /* Three frames in a ring: one with nothing to show and two with the
     * same shapes in other colors. All are cut along one division. */
    plain = make(0, 64, 64, flat);
    first = make(1, 64, 64, panel);
    second = make(2, 64, 64, panel_lit);
    plain->anim_total = first->anim_total = second->anim_total = 3;
    plain->anim_next = first; first->anim_next = second; second->anim_next = plain;
    tree = XGL_TextureShapes(plain);
    assert(tree && XGL_TextureShapes(first) == tree && XGL_TextureShapes(second) == tree);
    /* It is the division of the first of the frames that have the most
     * pieces, and that frame's colors are the division's own. */
    assert(XGL_ShapeColors(tree, first) == tree->colors);
    assert(near_value(tree->colors[3 * node_at(tree, 28, 28)], 40));
    /* Another frame laid over it has its own color in every piece. */
    colors = XGL_ShapeColors(tree, second);
    assert(colors && colors != tree->colors);
    assert(near_value(colors[3 * node_at(tree, 28, 28)], 90));
    assert(near_value(colors[3 * node_at(tree, 4, 4)], 30));
    assert(near_value(colors[3 * node_at(tree, 3, 54)], 120));
    assert(XGL_ShapeColors(tree, second) == colors); /* Worked out once. */
    colors = XGL_ShapeColors(tree, plain);
    assert(colors);
    for (i = 0; i < tree->count; ++i) assert(colors[3 * i] == 200 && colors[3 * i + 2] == 200);
    /* A frame of another size cannot share the division or be laid over it. */
    small = make(3, 32, 32, flat);
    small->anim_total = 3;
    second->anim_next = small; small->anim_next = plain;
    make(1, 64, 64, panel); /* Loading one forgets what was settled. */
    first->anim_total = 3; first->anim_next = second;
    assert(XGL_TextureShapes(small) == NULL);
    tree = XGL_TextureShapes(plain);
    assert(tree && XGL_TextureShapes(second) == tree);
    assert(!XGL_ShapeColors(tree, small));
    /* The other sequence of an animated texture is of the same family. */
    plain->anim_next = plain; plain->anim_total = 1;
    first->anim_next = first; first->anim_total = 1;
    plain->alternate_anims = first; first->alternate_anims = plain;
    make(0, 64, 64, flat);
    plain->anim_next = plain; plain->anim_total = 1; plain->alternate_anims = first;
    tree = XGL_TextureShapes(plain);
    assert(tree && XGL_TextureShapes(first) == tree && XGL_ShapeColors(tree, first) == tree->colors);
    /* When the frame the division came from is loaded again and asked
     * about, nothing keeps pointing at the old division. */
    make(1, 64, 64, flat);
    first->anim_next = first; first->anim_total = 1; first->alternate_anims = plain;
    assert(!XGL_TextureShapes(first) && !XGL_TextureShapes(plain));
    assert(!XGL_ShapeColors(NULL, plain));
    XGL_ShapesClear();
}

static float piece_area(const xgl_shape_piece_t *piece)
{
    float sum = 0;
    int i, j;
    for (i = 0; i < piece->count; ++i) {
        j = (i + 1) % piece->count;
        sum += piece->corner[i][0] * piece->corner[j][1] - piece->corner[j][0] * piece->corner[i][1];
    }
    return (float)fabs(sum) * .5f;
}

static float triangle_area(const float *a, const float *b, const float *c)
{
    return (float)fabs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) * .5f;
}

/* Cut a face and check what every cut face must satisfy; with a picture,
 * also that each piece has the picture's color at its middle, which holds
 * when every cut is made. Returns the number of pieces, or what the cutter
 * said when there are none. */
static xgl_shape_piece_t cut_pieces[XGL_SHAPE_TRIANGLES];
static int check_cut(const xgl_shape_tree_t *tree, const xgl_shape_terms_t *terms,
                     float (*st)[2], int count, float longest, int (*picture)(int, int))
{
    const xgl_shape_piece_t *piece, *other;
    float area, face_area, low[2], high[2], dx, dy, length, along, across, fan;
    const float *a, *b, *q;
    int pieces, p, o, i, j, k, triangles, x, y;
    pieces = XGL_ShapeCutFace(tree, terms, st, count, longest);
    if (pieces <= 0) {
        assert(!XGL_ShapePiece(0, &cut_pieces[0]));
        return pieces;
    }
    assert(pieces >= 2 && pieces <= XGL_SHAPE_TRIANGLES);
    assert(!XGL_ShapePiece(-1, &cut_pieces[0]) && !XGL_ShapePiece(pieces, &cut_pieces[0]));
    face_area = 0;
    for (i = 1; i + 1 < count; ++i) face_area += triangle_area(st[0], st[i], st[i + 1]);
    low[0] = high[0] = st[0][0]; low[1] = high[1] = st[0][1];
    for (i = 1; i < count; ++i)
        for (k = 0; k < 2; ++k) {
            if (st[i][k] < low[k]) low[k] = st[i][k];
            if (st[i][k] > high[k]) high[k] = st[i][k];
        }
    area = 0; triangles = 0;
    for (p = 0; p < pieces; ++p) {
        piece = &cut_pieces[p];
        assert(XGL_ShapePiece(p, &cut_pieces[p]));
        assert(piece->count >= 3 && piece->count <= XGL_SHAPE_CORNERS);
        assert(piece->node > 0 && piece->node < tree->count && piece->width >= 1);
        area += piece_area(piece);
        triangles += piece->center ? piece->count : piece->count - 2;
        for (i = 0; i < piece->count; ++i)
            for (k = 0; k < 2; ++k)
                assert(piece->corner[i][k] >= low[k] - .01f && piece->corner[i][k] <= high[k] + .01f);
        /* Drawn as a fan from its first corner, it has no empty triangle
         * and the fan covers it exactly. */
        if (!piece->center) {
            fan = 0;
            for (i = 1; i + 1 < piece->count; ++i) {
                assert(triangle_area(piece->corner[0], piece->corner[i], piece->corner[i + 1]) >= .001f);
                fan += triangle_area(piece->corner[0], piece->corner[i], piece->corner[i + 1]);
            }
            assert(fabs(fan - piece_area(piece)) < .01f * (1 + fan));
        }
        /* No longer than asked, for the light's sake. */
        for (k = 0; k < 2; ++k) {
            dx = piece->corner[0][k]; dy = dx;
            for (i = 1; i < piece->count; ++i) {
                if (piece->corner[i][k] < dx) dx = piece->corner[i][k];
                if (piece->corner[i][k] > dy) dy = piece->corner[i][k];
            }
            assert(dy - dx <= longest + .01f);
        }
        /* Its color is that of the texture where it lies. */
        if (picture) {
            x = (int)floor(piece->middle[0]); y = (int)floor(piece->middle[1]);
            x = ((x % tree->width) + tree->width) % tree->width;
            y = ((y % tree->height) + tree->height) % tree->height;
            assert(near_value(tree->colors[3 * piece->node], picture(x, y)));
        }
    }
    /* The pieces cover the face, no more and no less. */
    assert(fabs(area - face_area) < .001f * face_area + .5f);
    assert(triangles <= XGL_SHAPE_TRIANGLES);
    /* No corner of one piece lies part way along a side of another: where
     * it would, the other has taken it as a corner of its own. */
    for (p = 0; p < pieces; ++p) {
        piece = &cut_pieces[p];
        for (i = 0; i < piece->count; ++i) {
            a = piece->corner[i]; b = piece->corner[(i + 1) % piece->count];
            dx = b[0] - a[0]; dy = b[1] - a[1];
            length = (float)sqrt(dx * dx + dy * dy);
            if (length < .01f) continue;
            for (o = 0; o < pieces; ++o) {
                if (o == p) continue;
                other = &cut_pieces[o];
                for (j = 0; j < other->count; ++j) {
                    q = other->corner[j];
                    along = ((q[0] - a[0]) * dx + (q[1] - a[1]) * dy) / length;
                    across = ((q[0] - a[0]) * dy - (q[1] - a[1]) * dx) / length;
                    assert(!(fabs(across) < .002f && along > .01f && along < length - .01f));
                }
            }
        }
    }
    return pieces;
}

static void set_square(float (*st)[2], float s, float t, float size)
{
    st[0][0] = s; st[0][1] = t;
    st[1][0] = s + size; st[1][1] = t;
    st[2][0] = s + size; st[2][1] = t + size;
    st[3][0] = s; st[3][1] = t + size;
}

static void test_cut_faces(void)
{
    xgl_shape_tree_t *tree;
    xgl_shape_terms_t all, terms;
    float st[8][2];
    int pieces, cuts, fewer, i;
    XGL_ShapesClear();
    tree = XGL_TextureShapes(make(0, 64, 64, panel));
    assert(tree);
    all.most = 2 * (XGL_SHAPE_PIECES - 1); all.worth = 0; all.thin = 0;
    /* Every cut of the division, and the pieces they make of one repeat. */
    cuts = XGL_ShapeCuts(tree, &all);
    assert(cuts == (tree->count - 1) / 2);
    set_square(st, (float)tree->origin[0], (float)tree->origin[1], 64);
    assert(check_cut(tree, &all, st, 4, 128, panel) == cuts + 1);
    /* Four repeats and a bit, set anywhere and across the texture's own
     * borders: the same shapes, each in its place. */
    set_square(st, -37, 9, 150);
    pieces = check_cut(tree, &all, st, 4, 128, panel);
    assert(pieces > 2 * (cuts + 1));
    /* Along an axis on which it has not been cut, a piece runs across the
     * repeats: the strip is one piece the width of the face. Halved where
     * it is longer than asked. */
    terms = all;
    for (terms.most = 2; terms.most <= all.most && XGL_ShapeCuts(tree, &terms) < 2; terms.most += 2) {}
    set_square(st, 0, 0, 256);
    fewer = check_cut(tree, &terms, st, 4, 256, NULL);
    assert(fewer > 0 && fewer < 4 * 4 * 3);
    assert(check_cut(tree, &terms, st, 4, 64, NULL) > fewer);
    /* A face with a corner along one of its sides, as maps have them, and
     * one that is no rectangle. */
    st[0][0] = 0; st[0][1] = 0; st[1][0] = 50; st[1][1] = 0; st[2][0] = 100; st[2][1] = 0;
    st[3][0] = 100; st[3][1] = 90; st[4][0] = 0; st[4][1] = 90;
    assert(check_cut(tree, &all, st, 5, 128, panel) > 2);
    st[0][0] = 10; st[0][1] = 5; st[1][0] = 120; st[1][1] = 30; st[2][0] = 70; st[2][1] = 110;
    assert(check_cut(tree, &all, st, 3, 128, panel) > 2);
    /* Terms that ask more make fewer cuts, never others. */
    terms = all;
    fewer = cuts;
    for (i = 0; i < 6; ++i) {
        terms.worth = (float)(1000 << (2 * i));
        assert(XGL_ShapeCuts(tree, &terms) <= fewer);
        fewer = XGL_ShapeCuts(tree, &terms);
    }
    assert(fewer < cuts);
    terms = all; terms.most = 0;
    assert(!XGL_ShapeCuts(tree, &terms));
    set_square(st, 0, 0, 128);
    assert(check_cut(tree, &terms, st, 4, 128, panel) == 0);
    /* A part too narrow to see is not cut off: the strip, four texels
     * across, goes when five are asked. What is left is still whole. */
    terms = all; terms.thin = 5;
    assert(XGL_ShapeCuts(tree, &terms) < cuts);
    pieces = check_cut(tree, &all, st, 4, 128, panel);
    assert(check_cut(tree, &terms, st, 4, 128, NULL) < pieces);
    /* A face that shows nothing the division cuts is left alone. */
    make(1, 64, 64, stripe);
    tree = XGL_TextureShapes(&textures[1]);
    assert(tree);
    set_square(st, (float)tree->origin[0] + 4, 3, 20);
    assert(check_cut(tree, &all, st, 4, 128, stripe) == 0);
    /* Too many pieces for one face, or no face at all. */
    tree = XGL_TextureShapes(&textures[0]);
    set_square(st, 0, 0, 4096);
    assert(check_cut(tree, &all, st, 4, 128, panel) == -1);
    set_square(st, 0, 0, 128);
    assert(XGL_ShapeCutFace(tree, &all, st, 2, 128) == -1);
    /* Slopes: pieces with tilted sides share their corners as well. */
    tree = XGL_TextureShapes(make(2, 64, 64, slope));
    assert(tree);
    set_square(st, -20, -30, 200);
    assert(check_cut(tree, &all, st, 4, 128, NULL) > 4);
    st[0][0] = 3; st[0][1] = 7; st[1][0] = 190; st[1][1] = 40; st[2][0] = 150; st[2][1] = 170;
    st[3][0] = 20; st[3][1] = 120;
    assert(check_cut(tree, &all, st, 4, 128, NULL) > 4);
    XGL_ShapesClear();
}

int main(void)
{
    int i;
    for (i = 0; i < 256; ++i)
        xgl_palette[i * 3] = xgl_palette[i * 3 + 1] = xgl_palette[i * 3 + 2] = (byte)i;
    test_what_has_shapes();
    test_bands_origin_and_slopes();
    test_animated_frames();
    test_cut_faces();
    puts("XGL texture shape contracts passed");
    return 0;
}
