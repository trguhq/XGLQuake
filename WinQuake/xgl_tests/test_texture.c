/* CPU indexed-material regression tests. GPL-2.0-or-later. */
#include "quakedef.h"
#include "xgl_render.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>

byte xgl_palette[768];
static jmp_buf error_jump;
static int expect_error;
static char error_message[256];
void XGL_AudioUpdate(void) {}
void XGL_TextureMarkMap (int id);
void XGL_TextureClearMap (void);

void Sys_Error (char *format, ...)
{
    va_list args;
    va_start (args, format);
    vsnprintf (error_message, sizeof(error_message), format, args);
    va_end (args);
    if (expect_error) longjmp (error_jump, 1);
    fprintf (stderr, "unexpected engine error: %s\n", error_message);
    abort ();
}

static void sample_is (int id, float s, float t, int index, const byte *translation)
{
    float rgb[3];
    int channel, sampled;
    assert (XGL_SampleTextureIndex (id, s, t, &sampled));
    assert ((translation ? translation[sampled] : sampled) == index);
    assert (XGL_SampleTexture (id, s, t, rgb, translation));
    for (channel = 0; channel < 3; ++channel)
        assert (fabs(rgb[channel] - xgl_palette[index*3+channel]/255.0) < .000001);
}

static void test_index_samples(void)
{
    byte pixels[6] = {0, 1, 2, 3, 4, 255};
    byte translation[256];
    float rgb[3], s, t;
    int opaque, cutout, x, y, index, expected, i;
    opaque = XGL_LoadTexture("index-opaque", 3, 2, pixels, false, false);
    cutout = XGL_LoadTexture("index-cutout", 3, 2, pixels, false, true);
    for (i = 0; i < 256; ++i) translation[i] = (byte)(255 - i);
    /* Include exact tile boundaries, negative repetition, and non-power-of-2
     * dimensions; compare the direct index and both legacy palette paths. */
    for (y = -4; y <= 4; ++y)
        for (x = -8; x <= 8; ++x) {
            s = x / 4.0f; t = y / 4.0f;
            expected = pixels[((y % 4 + 4) % 4) / 2 * 3 + ((x % 4 + 4) % 4) * 3 / 4];
            assert(XGL_SampleTextureIndex(opaque, s, t, &index));
            assert(index == expected);
            sample_is(opaque, s, t, expected, NULL);
            sample_is(opaque, s, t, translation[expected], translation);
            index = -1;
            if (expected == 255) {
                assert(!XGL_SampleTextureIndex(cutout, s, t, &index));
                assert(index == -1);
                assert(!XGL_SampleTexture(cutout, s, t, rgb, translation));
            } else {
                assert(XGL_SampleTextureIndex(cutout, s, t, &index));
                assert(index == expected);
            }
        }
    index = -1;
    assert(!XGL_SampleTextureIndex(0, 0, 0, &index));
    assert(!XGL_SampleTextureIndex(-1, 0, 0, &index));
    assert(!XGL_SampleTextureIndex(100000, 0, 0, &index));
    assert(!XGL_SampleTextureIndex(opaque, (float)HUGE_VAL, 0, &index));
    assert(!XGL_SampleTextureIndex(opaque, 0, -(float)HUGE_VAL, &index));
    assert(!XGL_SampleTextureIndex(opaque, (float)strtod("nan", NULL), 0, &index));
    assert(!XGL_SampleTextureIndex(opaque, 0, (float)strtod("nan", NULL), &index));
    assert(!XGL_SampleTextureIndex(opaque, 1e9f, 0, &index));
    assert(!XGL_SampleTextureIndex(opaque, 0, -1e9f, &index));
    assert(!XGL_SampleTextureIndex(opaque, 0, 0, NULL));
    assert(!XGL_SampleTexture(opaque, 0, 0, NULL, NULL));
    assert(index == -1);
    XGL_TextureShutdown();
    assert(!XGL_SampleTextureIndex(opaque, 0, 0, &index));
    assert(index == -1);
}

static void test_filtered_materials(void)
{
    byte checker[16], odd[3] = {0, 0, 255}, opaque[4] = {1, 1, 1, 1};
    byte translation[256];
    float rgb[3], wrapped[3], expected;
    unsigned int revision;
    int id, transparent, i, channel;
    for (i = 0; i < 16; ++i) checker[i] = ((i / 4 + i % 4) & 1) ? 255 : 0;
    id = XGL_LoadTexture("checker", 4, 4, checker, true, false);
    revision = XGL_TextureRevision(id);
    assert(revision && !XGL_TextureHasAlpha(id));
    assert(XGL_SampleTextureFiltered(id, .125f, .125f, 1, 1, rgb, NULL));
    for (channel = 0; channel < 3; ++channel) {
        expected = (xgl_palette[channel] + xgl_palette[255 * 3 + channel]) / 510.0f;
        assert(fabs(rgb[channel] - expected) <= 1.0 / 255);
    }
    assert(XGL_SampleTextureFiltered(id, -.125f, 2.125f, .5f, .5f, wrapped, NULL));
    for (channel = 0; channel < 3; ++channel) assert(fabs(rgb[channel] - wrapped[channel]) < .00001);
    assert(XGL_SampleTextureFiltered(id, .125f, .125f, .1f, .1f, rgb, NULL));
    for (channel = 0; channel < 3; ++channel)
        assert(fabs(rgb[channel] - xgl_palette[channel] / 255.0f) < .00001);
    assert(XGL_LoadTexture("checker", 4, 4, checker, true, false) == id);
    assert(XGL_TextureRevision(id) == revision);
    checker[0] = 17;
    assert(XGL_LoadTexture("checker", 4, 4, checker, true, false) == id);
    assert(XGL_TextureRevision(id) != revision);
    assert(XGL_SampleTextureFiltered(id, .125f, .125f, .1f, .1f, rgb, NULL));
    for (channel = 0; channel < 3; ++channel)
        assert(fabs(rgb[channel] - xgl_palette[17 * 3 + channel] / 255.0f) < .00001);
    for (i = 0; i < 256; ++i) translation[i] = (byte)i;
    translation[0] = translation[255] = translation[17] = 29;
    assert(XGL_SampleTextureFiltered(id, .5f, .5f, 1, 1, rgb, translation));
    for (channel = 0; channel < 3; ++channel)
        assert(fabs(rgb[channel] - xgl_palette[29 * 3 + channel] / 255.0f) < .00001);
    transparent = XGL_LoadTexture("cutout", 4, 4, checker, true, true);
    assert(XGL_TextureHasAlpha(transparent));
    assert(!XGL_SampleTextureFiltered(transparent, .375f, .125f, 1, 1, rgb, NULL));
    assert(!XGL_SampleTextureFiltered(transparent, .375f, .125f, 1, 1, rgb, translation));
    assert(XGL_SampleTextureFiltered(transparent, .125f, .125f, 1, 1, rgb, NULL));
    id = XGL_LoadTexture("opaque-cutout", 2, 2, opaque, true, true);
    assert(!XGL_TextureHasAlpha(id));
    id = XGL_LoadTexture("odd", 3, 1, odd, true, false);
    assert(XGL_SampleTextureFiltered(id, .5f, .5f, 10, 10, rgb, NULL));
    for (channel = 0; channel < 3; ++channel) {
        expected = (2 * xgl_palette[channel] + xgl_palette[255 * 3 + channel]) / 765.0f;
        assert(fabs(rgb[channel] - expected) <= 1.0 / 255);
    }
    assert(!XGL_TextureRevision(0) && !XGL_TextureHasAlpha(0));
    XGL_TextureShutdown();
}

/* The mesh builder's sampler: the mean of a box of texels centered on a
 * point, from the reduced images, with integer arithmetic only. */
static void test_box_samples(void)
{
    byte pixels[16 * 16], rgb[3], cut[4 * 4], odd[12 * 8];
    int id, x, y, c, alpha, wide;
    /* Left half index 10, right half index 200. */
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 16; ++x) pixels[y * 16 + x] = (byte)(x < 8 ? 10 : 200);
    id = XGL_LoadTexture("box", 16, 16, pixels, false, false);
    assert(id > 0);
    /* Below four texels: the nearest texel, no filtering. */
    assert(XGL_SampleTextureBox(id, 7.9f, 3, 2, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[10 * 3 + c]);
    assert(XGL_SampleTextureBox(id, 8.1f, 3, 3.9f, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[200 * 3 + c]);
    /* A four-texel box inside one half is that half's color... */
    assert(XGL_SampleTextureBox(id, 4, 4, 4, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[10 * 3 + c]);
    /* ...and centered on the boundary it is the mean of both: the point's
     * own surroundings, not whichever cell it happens to fall in. */
    assert(XGL_SampleTextureBox(id, 8, 4, 4, rgb));
    for (c = 0; c < 3; ++c)
        assert(rgb[c] == (xgl_palette[10 * 3 + c] + xgl_palette[200 * 3 + c] + 1) / 2);
    /* A wider box the same; wider than the texture, its overall mean. */
    assert(XGL_SampleTextureBox(id, 8, 12, 8, rgb));
    for (c = 0; c < 3; ++c)
        assert(rgb[c] == (xgl_palette[10 * 3 + c] + xgl_palette[200 * 3 + c] + 1) / 2);
    assert(XGL_SampleTextureBox(id, 3, 5, 1000, rgb));
    for (c = 0; c < 3; ++c)
        assert(abs(rgb[c] - (xgl_palette[10 * 3 + c] + xgl_palette[200 * 3 + c]) / 2) <= 1);
    /* Coordinates repeat in both directions, also across the seam. */
    assert(XGL_SampleTextureBox(id, 16 * 5 + 4, -16 * 3 + 4, 4, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[10 * 3 + c]);
    assert(XGL_SampleTextureBox(id, 16, 0, 4, rgb));
    for (c = 0; c < 3; ++c)
        assert(rgb[c] == (xgl_palette[10 * 3 + c] + xgl_palette[200 * 3 + c] + 1) / 2);
    assert(XGL_SampleTextureBox(id, 0, 0, 4, rgb));
    for (c = 0; c < 3; ++c)
        assert(rgb[c] == (xgl_palette[10 * 3 + c] + xgl_palette[200 * 3 + c] + 1) / 2);
    /* Invalid input is refused; nothing is written. */
    rgb[0] = 77;
    assert(!XGL_SampleTextureBox(0, 0, 0, 4, rgb) && !XGL_SampleTextureBox(id, 0, 0, 4, NULL));
    assert(!XGL_SampleTextureBox(id, (float)HUGE_VAL, 0, 4, rgb) && rgb[0] == 77);
    /* A cutout material is never averaged across its holes. */
    memset(cut, 255, sizeof(cut));
    cut[5] = 9;
    alpha = XGL_LoadTexture("box-cutout", 4, 4, cut, false, true);
    assert(XGL_SampleTextureBox(alpha, 1.5f, 1.5f, 4, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[9 * 3 + c]);
    /* Sizes that are not powers of two repeat correctly too. */
    for (y = 0; y < 8; ++y)
        for (x = 0; x < 12; ++x) odd[y * 12 + x] = (byte)(x < 6 ? 30 : 90);
    wide = XGL_LoadTexture("box-odd", 12, 8, odd, false, false);
    assert(XGL_SampleTextureBox(wide, 12 * 7 + 2, 8 * 9 + 3, 2, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[30 * 3 + c]);
    assert(XGL_SampleTextureBox(wide, -12 * 4 + 9, -8 * 2 + 3, 2, rgb));
    for (c = 0; c < 3; ++c) assert(rgb[c] == xgl_palette[90 * 3 + c]);
    assert(XGL_SampleTextureBox(wide, 6, 4, 4, rgb));
    for (c = 0; c < 3; ++c)
        assert(rgb[c] == (xgl_palette[30 * 3 + c] + xgl_palette[90 * 3 + c] + 1) / 2);
    XGL_TextureShutdown();
}

/* The reduced images against their definition, for sizes whose levels are
 * made the quick way (four whole cells to each new one), the general way
 * (a last odd row or column that stands for fewer texels) and both. Each
 * level is read through the box sampler, which returns the rounded mean of
 * the four cells about a point. */
#define REDUCED_SIDE 48
static void test_reduced_images(void)
{
    static const int sizes[5][2] = {{32, 16}, {24, 12}, {20, 10}, {48, 48}, {6, 2}};
    static byte pixels[REDUCED_SIDE * REDUCED_SIDE];
    static byte images[2][REDUCED_SIDE * REDUCED_SIDE * 3];
    byte rgb[3], *previous, *current;
    unsigned int seed, sum, count, weight, wx, wy;
    int size, width, height, w, h, nw, nh, level, block, cell, x, y, sx, sy, c, id;
    int px, py, x0, x1, y0, y1, checked;
    char name[32];
    seed = 12345; checked = 0;
    for (size = 0; size < 5; ++size) {
        width = sizes[size][0]; height = sizes[size][1];
        for (x = 0; x < width * height; ++x) {
            seed = seed * 1103515245U + 12345U;
            pixels[x] = (byte)(seed >> 16);
        }
        sprintf(name, "reduced-%d", size);
        id = XGL_LoadTexture(name, width, height, pixels, false, false);
        assert(id > 0);
        previous = images[0]; current = images[1];
        for (x = 0; x < width * height; ++x)
            for (c = 0; c < 3; ++c) previous[x * 3 + c] = xgl_palette[pixels[x] * 3 + c];
        w = width; h = height;
        for (level = 0; (w > 1 || h > 1) && level < 8; ++level) {
            nw = (w + 1) / 2; nh = (h + 1) / 2; block = 1 << level;
            /* Each cell of this level from those of the one before, every
             * one weighted by the texels it stands for. */
            for (y = 0; y < nh; ++y)
                for (x = 0; x < nw; ++x)
                    for (c = 0; c < 3; ++c) {
                        sum = count = 0;
                        for (sy = 2 * y; sy < 2 * y + 2 && sy < h; ++sy)
                            for (sx = 2 * x; sx < 2 * x + 2 && sx < w; ++sx) {
                                wx = (unsigned int)(width - sx * block < block ? width - sx * block : block);
                                wy = (unsigned int)(height - sy * block < block ? height - sy * block : block);
                                weight = wx * wy;
                                sum += weight * previous[(sy * w + sx) * 3 + c];
                                count += weight;
                            }
                        current[(y * nw + x) * 3 + c] = (byte)((sum + count / 2) / count);
                    }
            /* A box of four cells' width chooses this level. */
            cell = 2 << level;
            for (py = 0; py < height; py += 1 + level)
                for (px = 0; px < width; px += 1 + level) {
                    x1 = (px + cell / 2) >> (level + 1); y1 = (py + cell / 2) >> (level + 1);
                    x0 = x1 - 1; y0 = y1 - 1;
                    if (x0 < 0) x0 = nw - 1;
                    if (y0 < 0) y0 = nh - 1;
                    if (x1 >= nw) x1 = 0;
                    if (y1 >= nh) y1 = 0;
                    assert(XGL_SampleTextureBox(id, (float)px, (float)py, (float)(4 << level), rgb));
                    for (c = 0; c < 3; ++c)
                        assert(rgb[c] == (byte)((current[(y0 * nw + x0) * 3 + c] + current[(y0 * nw + x1) * 3 + c] +
                                                 current[(y1 * nw + x0) * 3 + c] + current[(y1 * nw + x1) * 3 + c] + 2) >> 2));
                    ++checked;
                }
            previous = current; current = current == images[0] ? images[1] : images[0];
            w = nw; h = nh;
        }
    }
    assert(checked > 3000);
    XGL_TextureShutdown();
}

/* From one map to the next. A texture both have, the same texels under the
 * same name, is the same material throughout: its number, its revision and
 * its reduced images stay, so nothing worked out from it is done again. One
 * the new map does not ask for goes when the map has loaded, or sooner if
 * its room or its place is wanted. */
static void test_map_change(void)
{
    static byte wall[16 * 16], floor_a[16 * 16], floor_b[16 * 16], megabyte[1024 * 1024];
    byte before[3], after[3], tiny;
    unsigned int wall_revision, floor_revision, skin_revision;
    int wall_id, floor_id, gone_id, skin_id, borrowed_id, id, i, first;
    char name[32];
    for (i = 0; i < 16 * 16; ++i) {
        wall[i] = (byte)(i * 7); floor_a[i] = (byte)(i * 3); floor_b[i] = (byte)(i * 5 + 1);
    }
    wall_id = XGL_LoadTexture("wall", 16, 16, wall, true, false);
    floor_id = XGL_LoadTexture("floor", 16, 16, floor_a, true, false);
    gone_id = XGL_LoadTexture("gone", 16, 16, floor_b, true, false);
    borrowed_id = XGL_LoadTexture("borrowed", 16, 16, floor_a, true, false);
    skin_id = XGL_LoadTexture("progs/soldier.mdl_0", 16, 16, wall, true, false);
    XGL_TextureMarkMap(wall_id); XGL_TextureMarkMap(floor_id);
    XGL_TextureMarkMap(gone_id); XGL_TextureMarkMap(borrowed_id);
    /* The reduced images exist once a box has been sampled. */
    assert(XGL_SampleTextureBox(wall_id, 5, 9, 8, before));
    wall_revision = XGL_TextureRevision(wall_id);
    floor_revision = XGL_TextureRevision(floor_id);
    skin_revision = XGL_TextureRevision(skin_id);
    assert(wall_revision && floor_revision && skin_revision);

    XGL_TextureClearMap();
    /* The same texture again: nothing changes. */
    assert(XGL_LoadTexture("wall", 16, 16, wall, true, false) == wall_id);
    XGL_TextureMarkMap(wall_id);
    assert(XGL_TextureRevision(wall_id) == wall_revision);
    /* The same name with other texels: the same number, a new revision. */
    assert(XGL_LoadTexture("floor", 16, 16, floor_b, true, false) == floor_id);
    XGL_TextureMarkMap(floor_id);
    assert(XGL_TextureRevision(floor_id) != floor_revision);
    /* Asked for by someone who is not the map: it is theirs now. */
    assert(XGL_LoadTexture("borrowed", 16, 16, floor_a, true, false) == borrowed_id);
    /* Until the map has loaded, the one nobody asked for is still there. */
    assert(XGL_TextureRevision(gone_id));
    XGL_TextureSweepMap();
    assert(!XGL_TextureRevision(gone_id));
    assert(XGL_TextureRevision(wall_id) == wall_revision);
    assert(XGL_TextureRevision(borrowed_id) && XGL_TextureRevision(skin_id) == skin_revision);
    assert(XGL_SampleTextureBox(wall_id, 5, 9, 8, after) && !memcmp(before, after, 3));
    sample_is(floor_id, .5f / 16, .5f / 16, floor_b[0], NULL);
    /* Another map change with nothing asked for again takes the map's and
     * leaves the others'. */
    XGL_TextureClearMap();
    XGL_TextureSweepMap();
    assert(!XGL_TextureRevision(wall_id) && !XGL_TextureRevision(floor_id));
    assert(XGL_TextureRevision(borrowed_id) && XGL_TextureRevision(skin_id) == skin_revision);
    XGL_TextureShutdown();

    /* Seven megabytes of one map and seven of the next do not fit together
     * in eight: the going map's are released as their room is wanted. */
    memset(megabyte, 9, sizeof(megabyte));
    for (i = 0; i < 7; ++i) {
        sprintf(name, "old%d", i);
        megabyte[0] = (byte)i;
        id = XGL_LoadTexture(name, 1024, 1024, megabyte, true, false);
        assert(id == i + 1);
        XGL_TextureMarkMap(id);
    }
    XGL_TextureClearMap();
    for (i = 0; i < 7; ++i) {
        sprintf(name, "new%d", i);
        megabyte[0] = (byte)(100 + i);
        id = XGL_LoadTexture(name, 1024, 1024, megabyte, true, false);
        assert(id > 0);
        XGL_TextureMarkMap(id);
        sample_is(id, 0, 0, 100 + i, NULL);
    }
    XGL_TextureSweepMap();
    megabyte[0] = 100;
    assert(XGL_LoadTexture("new0", 1024, 1024, megabyte, true, false) > 0);
    XGL_TextureShutdown();

    /* Every place taken by a going map: a new texture still finds one. */
    first = 0;
    for (i = 0; ; ++i) {
        sprintf(name, "place%d", i);
        tiny = (byte)i;
        expect_error = 1;
        if (setjmp(error_jump)) break;
        id = XGL_LoadTexture(name, 1, 1, &tiny, true, false);
        expect_error = 0;
        if (!first) first = id;
        XGL_TextureMarkMap(id);
        assert(i < 100000);
    }
    expect_error = 0;
    assert(strstr(error_message, "slots exhausted") && i > 100);
    XGL_TextureClearMap();
    tiny = 77;
    id = XGL_LoadTexture("fresh", 1, 1, &tiny, true, false);
    assert(id > 0);
    sample_is(id, 0, 0, 77, NULL);
    assert(!XGL_TextureRevision(id == first ? first + 1 : first));
    XGL_TextureShutdown();
}

int main (void)
{
    byte pixels[8] = {0, 1, 255, 7, 8, 9, 10, 11};
    byte translate[256];
    byte *large;
    float rgb[3];
    int alias, sprite, replacement, i;
    unsigned int revision;
    char name[32];
    for (i = 0; i < 256; ++i)
    {
        xgl_palette[i*3] = i;
        xgl_palette[i*3+1] = 255-i;
        xgl_palette[i*3+2] = (i*37)&255;
        translate[i] = i;
    }
    translate[1] = 17;
    XGL_TextureInit ();
    alias = XGL_LoadTexture ("progs/player.mdl_0", 4, 2, pixels, true, false);
    assert (alias > 0);
    assert (alias == XGL_LoadTexture ("progs/player.mdl_0", 4, 2, pixels, true, false));
    sample_is (alias, .375f, .25f, 1, NULL);
    sample_is (alias, -.625f, 2.25f, 1, NULL);
    sample_is (alias, 1.375f, -.75f, 17, translate);
    sample_is (alias, .625f, .25f, 255, NULL); /* opaque skins retain index 255 */
    pixels[1] = 29;
    sample_is (alias, .375f, .25f, 1, NULL); /* registry owns its copy */
    assert (alias == XGL_LoadTexture ("progs/player.mdl_0", 4, 2, pixels, false, false));
    sample_is (alias, .375f, .25f, 29, NULL);
    sprite = XGL_LoadTexture ("progs/flame.spr:group0:0", 4, 2, pixels, true, true);
    revision = XGL_TextureRevision(sprite);
    assert(XGL_TextureHasAlpha(sprite) && !XGL_TextureHasAlpha(alias));
    XGL_TextureMarkMap (sprite);
    assert (!XGL_SampleTexture (sprite, .625f, .25f, rgb, NULL));
    translate[255] = 0;
    assert (!XGL_SampleTexture (sprite, .625f, .25f, rgb, translate));
    sample_is (sprite, .875f, .75f, 11, NULL);
    assert (!XGL_SampleTexture (0, 0, 0, rgb, NULL));
    assert (!XGL_SampleTexture (alias, (float)HUGE_VAL, 0, rgb, NULL));
    /* A going map's material stays, as it is, until the next has loaded. */
    XGL_TextureClearMap ();
    XGL_TextureClearMap ();
    sample_is (sprite, .875f, .75f, 11, NULL);
    assert(XGL_TextureRevision(sprite) == revision);
    XGL_TextureSweepMap ();
    XGL_TextureSweepMap ();
    assert (!XGL_SampleTexture (sprite, 0, 0, rgb, NULL));
    assert(!XGL_TextureRevision(sprite));
    sample_is (alias, .375f, .25f, 29, NULL);
    XGL_TextureInit ();
    sample_is (alias, .375f, .25f, 29, NULL);
    replacement = XGL_LoadTexture ("maps/next.bsp:brick", 4, 2, pixels, true, false);
    assert (replacement == sprite && replacement != alias);
    assert(XGL_TextureRevision(replacement) != revision);
    XGL_TextureMarkMap (replacement);
    XGL_TextureClearMap ();
    XGL_TextureSweepMap ();
    assert(!XGL_TextureRevision(replacement));
    sample_is (alias, .375f, .25f, 29, NULL);
    XGL_TextureShutdown ();
    assert (!XGL_SampleTexture (alias, 0, 0, rgb, NULL));

    test_map_change();

    test_index_samples();
    test_filtered_materials();
    test_reduced_images();
    test_box_samples();

    /* Exact budget exhaustion must fail without evicting live alias IDs. */
    large = (byte *)malloc (1024*1024);
    assert (large);
    memset (large, 7, 1024*1024);
    for (i = 0; i < 8; ++i)
    {
        sprintf (name, "large%d", i);
        assert (XGL_LoadTexture (name, 1024, 1024, large, false, false) == i+1);
        if (i == 6) {
            /* This optional mip chain nearly fills the remaining MiB. The
             * eighth indexed asset must evict it rather than fail loading. */
            assert(XGL_SampleTextureFiltered(1, .5f, .5f, 1, 1, rgb, NULL));
        }
    }
    expect_error = 1;
    if (!setjmp(error_jump))
    {
        XGL_LoadTexture ("over-budget", 1, 1, large, false, false);
        assert (!"material budget was not enforced");
    }
    assert (strstr(error_message, "8 MiB"));
    expect_error = 0;
    sample_is (1, 0, 0, 7, NULL);
    sample_is (8, 0, 0, 7, NULL);
    assert(XGL_SampleTextureFiltered(1, .5f, .5f, 1, 1, rgb, NULL));
    for (i = 0; i < 3; ++i) assert(fabs(rgb[i] - xgl_palette[7*3+i]/255.0f) < .00001);
    expect_error = 1;
    if (!setjmp(error_jump))
    {
        XGL_LoadTexture ("oversize", 2048, 2048, large, false, false);
        assert (!"image size bound was not enforced");
    }
    assert (strstr(error_message, "dimensions"));
    expect_error = 0;
    free (large);
    XGL_TextureShutdown ();
    XGL_TextureShutdown ();
    puts ("XGL CPU materials: palette, translation, coverage, repeat, lifecycle and bounds passed");
    return 0;
}
