/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/* xgl_shapes.c: wall textures divided into pieces of nearly one color.
 *
 * A wall is drawn as colored triangles, a few to each repeat of its texture.
 * Colors that run smoothly between vertices show a texture's grain but smear
 * its shapes: a panel, a light strip, a groove. Where a texture has such
 * shapes, the wall is cut along them instead and each piece drawn in one
 * color. This file finds the cuts, once for each texture.
 *
 * The repeat of the texture is divided by the straight cut that most reduces
 * the squared color error, then either part by its best cut, and so on. Cuts
 * along the texture's axes are tried at every texel, both singly and as the
 * two sides of a thin band. The best of them is then tilted if the edge it
 * found lies in different places in the two halves of the part, and kept if
 * the color really steps across the tilted line along its whole length. A
 * part with a tilted side is also tried with a cut parallel to that side,
 * since an outline is often a thin line of its own color.
 *
 * A texture is drawn this way only if as many pieces as a smooth mesh would
 * have triangles for describe it distinctly better than that mesh does. Most
 * rock, brick and rough metal does not qualify and is left alone. */
#include "quakedef.h"
#include "xgl_render.h"
#include "xgl_audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Largest texture examined, along either side. */
#define XGL_SHAPE_SIDE 256
#define XGL_SHAPE_NODES (2 * XGL_SHAPE_PIECES)
/* No part is cut thinner than this many texels along an axis. */
#define XGL_SHAPE_LEAST 2
/* A cut must remove this much squared error per texel of the repeat to be
 * considered at all, and, once the texture is known to have shapes, this
 * much in all: half of what a wall asks of a cut at the default detail
 * settings, so nothing any mesh could use is left out. */
#define XGL_SHAPE_STOP 1.0
#define XGL_SHAPE_STOP_GAIN 40000.0
/* The smooth mesh the pieces are compared with has a vertex every so many
 * texels, and a piece is reckoned to cost 3.3 of its triangles. */
#define XGL_SHAPE_GRID 16
#define XGL_SHAPE_PIECE_TRIANGLES 3.3
/* Pieces must leave at most this share of the smooth mesh's error, and at
 * least one level of color less, for the texture to be drawn as pieces. */
#define XGL_SHAPE_BETTER .85
#define XGL_SHAPE_MARGIN 1.0
/* Tilted cuts are tried only if upright and level ones come this close. On
 * the shareware maps tilting never brings a texture more than four
 * hundredths nearer, so none that begins above this gets under the mark. */
#define XGL_SHAPE_HOPE .90
/* A thin band is at most this many texels across. */
#define XGL_SHAPE_BAND 12
/* A tilted cut must gain this much more than the best upright one, lie at
 * least this many texels differently in the two halves, and have the color
 * step this many levels across it. */
#define XGL_SHAPE_TILT_WORTH 1.10
#define XGL_SHAPE_TILT_LEAST 2
#define XGL_SHAPE_EDGE_STEP 14.0
#define XGL_SHAPE_EDGE_AGREE .35
/* How far in from a tilted side a parallel cut is looked for. */
#define XGL_SHAPE_SIDE_REACH 20
/* The share of a piece's darkest and of its brightest texels left out of
 * its color. */
#define XGL_SHAPE_TRIM .2
#define XGL_SHAPE_TEXTURES 2048
/* The most frames of one animated texture that are looked at. */
#define XGL_SHAPE_FRAMES 64

extern byte xgl_palette[768];

/* A part of the repeat while it is being divided. */
typedef struct {
    /* The cut chosen for it: a*x + b*y = d at texel centers, and for a band
     * the second cut at d2. gain is per piece added, total for the whole. */
    float a, b, d, d2, gain, total;
    /* Channel sums and the sum of squares over its texels; their number;
     * the columns and rows that bound it. */
    double sum[4];
    int n, x0, x1, y0, y1;
    /* The best gain of an upright and of a level cut, whichever was chosen. */
    double best[2];
    /* The tilted cuts that bound it, newest first. */
    float side[2][3];
    short child[2];
    byte axis, band, paired, second, rect, tilted, sides;
    byte color[3];
} xgl_shape_part_t;

/* A way to divide a part: one cut, or the two sides of a thin band. */
typedef struct {
    double gain, total;
    float d, d2;
    int band;
} xgl_shape_cut_t;

static struct {
    int width, height, count;
    /* Each texel from the chosen origin: red, green, blue, and the sum of
     * their squares. */
    unsigned int (*texel)[4];
    /* For each row, the sums of its first x texels. */
    unsigned int (*row)[4];
    /* The sums over the first x texels of the first y rows. */
    double (*table)[4];
    /* The columns of each row that belong to each part. */
    short (*span)[2];
    xgl_shape_part_t part[XGL_SHAPE_NODES];
    /* For every upright cut x = c, [0], and every level cut y = c, [1], of
     * the window last profiled: the sums over the far side. */
    double far_sum[2][XGL_SHAPE_SIDE + 2][4];
    int far_n[2][XGL_SHAPE_SIDE + 2];
} xgl_shape;

#define XGL_SHAPE_TEXEL(x, y) xgl_shape.texel[(y) * xgl_shape.width + (x)]
#define XGL_SHAPE_ROW(x, y) xgl_shape.row[(y) * (xgl_shape.width + 1) + (x)]
#define XGL_SHAPE_TABLE(x, y) xgl_shape.table[(y) * (xgl_shape.width + 1) + (x)]
/* The spans of a part, one for each row of the texture. */
#define XGL_SHAPE_SPANS(part) (xgl_shape.span + (part) * xgl_shape.height)

typedef struct {
    /* The texture's revision when it was examined; zero for one that has
     * not been. */
    unsigned int revision;
    /* Its own division, and the one that walls of it are cut along: for an
     * animated texture, that of whichever frame has most to show. `wall` is
     * good while `resolved` is set. */
    xgl_shape_tree_t *tree, *wall;
    int resolved;
    /* Its colors over a division made from another texture, with that
     * division and its own revision when they were worked out. */
    byte *colors;
    const xgl_shape_tree_t *colors_tree;
    unsigned int colors_revision;
} xgl_shape_entry_t;

/* Indexed by texture number less one. */
static xgl_shape_entry_t *xgl_shape_entries;
static int xgl_shape_examined, xgl_shape_found;

static void XGL_ShapeBox (int x0, int y0, int x1, int y1, double *sum)
{
    const double *a = XGL_SHAPE_TABLE(x1, y1), *b = XGL_SHAPE_TABLE(x1, y0);
    const double *c = XGL_SHAPE_TABLE(x0, y1), *d = XGL_SHAPE_TABLE(x0, y0);
    sum[0] = a[0] - b[0] - c[0] + d[0]; sum[1] = a[1] - b[1] - c[1] + d[1];
    sum[2] = a[2] - b[2] - c[2] + d[2]; sum[3] = a[3] - b[3] - c[3] + d[3];
}

/* The squared error of n texels about their mean color. */
static double XGL_ShapeError (const double *sum, int n)
{
    if (n <= 0) return 0;
    return sum[3] - (sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]) / n;
}

/* The sums of every column and of every row of a texture, color by color
 * and of the squares, in one pass. A line of 256 texels comes to less than
 * fifty million, so whole numbers hold them exactly. */
static unsigned int xgl_shape_columns[XGL_SHAPE_SIDE][4], xgl_shape_rows[XGL_SHAPE_SIDE][4];

static void XGL_ShapeLines (const byte *texels, int width, int height)
{
    const byte *color;
    unsigned int red, green, blue, squares, sum[4], *column;
    int x, y;
    memset(xgl_shape_columns, 0, (size_t)width * sizeof(xgl_shape_columns[0]));
    for (y = 0; y < height; ++y) {
        sum[0] = sum[1] = sum[2] = sum[3] = 0;
        column = xgl_shape_columns[0];
        for (x = 0; x < width; ++x, column += 4) {
            color = xgl_palette + 3 * *texels++;
            red = color[0]; green = color[1]; blue = color[2];
            squares = red * red + green * green + blue * blue;
            sum[0] += red; sum[1] += green; sum[2] += blue; sum[3] += squares;
            column[0] += red; column[1] += green; column[2] += blue; column[3] += squares;
        }
        memcpy(xgl_shape_rows[y], sum, sizeof(sum));
    }
}

/* Where the repeat should begin along one axis. A repeating pattern has no
 * natural edge, and a border through the middle of a shape would cut it in
 * two for nothing. Given the sums of the `length` lines that lie across the
 * axis, each of `across` texels, finds the pair of cuts that best divides
 * the ring of them into two runs and begins at one of them; zero when no
 * pair explains even a fiftieth of the texture. */
static int XGL_ShapeOrigin (unsigned int (*sums)[4], int length, int across)
{
    static double line[XGL_SHAPE_SIDE + 1][4];
    double total[4], part[4], rest[4], whole, best, gain;
    int i, j, c, n1, n2, origin, other, step, pass, from, to, low, high;
    for (c = 0; c < 4; ++c) line[0][c] = 0;
    for (i = 0; i < length; ++i)
        for (c = 0; c < 4; ++c) line[i + 1][c] = line[i][c] + sums[i][c];
    for (c = 0; c < 4; ++c) total[c] = line[length][c];
    whole = XGL_ShapeError(total, length * across);
    if (!(whole > 0)) return 0;
    best = 0; origin = other = 0;
    /* Every second or fourth place first, then the places about the best. */
    step = length >= 128 ? 4 : (length >= 64 ? 2 : 1);
    for (pass = 0; pass < 2; ++pass) {
        if (pass && step == 1) break;
        from = pass ? origin - step + 1 : 0; to = pass ? origin + step - 1 : length - 1;
        if (from < 0) from = 0;
        if (to > length - 1) to = length - 1;
        for (i = from; i <= to; i += pass ? 1 : step) {
            low = i + XGL_SHAPE_LEAST; high = i + length - XGL_SHAPE_LEAST;
            if (high > length) high = length;
            if (pass) {
                if (low < other - step + 1) low = other - step + 1;
                if (high > other + step - 1) high = other + step - 1;
            }
            for (j = low; j <= high; j += pass ? 1 : step) {
                for (c = 0; c < 4; ++c) { part[c] = line[j][c] - line[i][c]; rest[c] = total[c] - part[c]; }
                n1 = (j - i) * across; n2 = (length - (j - i)) * across;
                if (n1 <= 0 || n2 <= 0) continue;
                gain = whole - XGL_ShapeError(part, n1) - XGL_ShapeError(rest, n2);
                if (gain > best) { best = gain; origin = i; other = j; }
            }
        }
    }
    return best >= whole * .02 ? origin : 0;
}

/* The error of the ordinary kind of mesh on this texture: vertices every
 * XGL_SHAPE_GRID texels, each the mean of a box three quarters as wide,
 * colors running smoothly between them. Every second texel is compared, the
 * other ones on the next row, so that a striped texture is judged fairly. */
static double XGL_ShapeSmooth (void)
{
    static float grid[17][17][3];
    /* Where each column lies among the grid's: the same for every row. */
    static float along[XGL_SHAPE_SIDE];
    static short column[XGL_SHAPE_SIDE][2];
    const unsigned int *texel;
    double sum[4], part[4], error;
    float fx, fy, value, difference;
    int w, h, gw, gh, gx, gy, nx, ny, x, y, c, half, x0, y0, x1, y1, i, j, skip, taken;
    w = xgl_shape.width; h = xgl_shape.height;
    gw = w / XGL_SHAPE_GRID; if (gw < 1) gw = 1;
    gh = h / XGL_SHAPE_GRID; if (gh < 1) gh = 1;
    half = XGL_SHAPE_GRID * 3 / 8;
    for (gy = 0; gy < gh; ++gy)
        for (gx = 0; gx < gw; ++gx) {
            sum[0] = sum[1] = sum[2] = 0;
            /* The box about a grid point may run over the border, into the
             * next repeat: up to four rectangles of this one. */
            for (j = 0; j < 2; ++j)
                for (i = 0; i < 2; ++i) {
                    x0 = gx * w / gw - half; x1 = gx * w / gw + half;
                    y0 = gy * h / gh - half; y1 = gy * h / gh + half;
                    if (x1 - x0 > w) { x0 = 0; x1 = w; }
                    if (y1 - y0 > h) { y0 = 0; y1 = h; }
                    if (i == 0) {
                        if (x0 < 0) { x0 += w; x1 = w; } else if (x1 > w) x1 = w;
                    } else {
                        if (x0 >= 0 && x1 <= w) continue;
                        if (x0 < 0) x0 = 0; else { x1 -= w; x0 = 0; }
                    }
                    if (j == 0) {
                        if (y0 < 0) { y0 += h; y1 = h; } else if (y1 > h) y1 = h;
                    } else {
                        if (y0 >= 0 && y1 <= h) continue;
                        if (y0 < 0) y0 = 0; else { y1 -= h; y0 = 0; }
                    }
                    if (x1 <= x0 || y1 <= y0) continue;
                    XGL_ShapeBox(x0, y0, x1, y1, part);
                    for (c = 0; c < 3; ++c) sum[c] += part[c];
                }
            x0 = 2 * half < w ? 2 * half : w; y0 = 2 * half < h ? 2 * half : h;
            for (c = 0; c < 3; ++c) grid[gy][gx][c] = (float)(sum[c] / (x0 * y0));
        }
    for (x = 0; x < w; ++x) {
        fx = (float)x * gw / w;
        gx = (int)fx;
        along[x] = fx - gx;
        column[x][0] = (short)gx; column[x][1] = (short)((gx + 1) % gw);
    }
    error = 0; taken = 0;
    skip = w >= 32 && h >= 32 ? 2 : 1;
    for (y = 0; y < h; ++y) {
        if ((y & 31) == 31) XGL_AudioUpdate();
        fy = (float)y * gh / h;
        gy = (int)fy; fy -= gy; ny = (gy + 1) % gh;
        for (x = skip == 2 ? (y & 1) : 0; x < w; x += skip, ++taken) {
            fx = along[x]; gx = column[x][0]; nx = column[x][1];
            texel = XGL_SHAPE_TEXEL(x, y);
            for (c = 0; c < 3; ++c) {
                value = (grid[gy][gx][c] * (1 - fx) + grid[gy][nx][c] * fx) * (1 - fy) +
                        (grid[ny][gx][c] * (1 - fx) + grid[ny][nx][c] * fx) * fy;
                difference = value - (float)texel[c];
                error += difference * difference;
            }
        }
    }
    return taken ? sqrt(error / (3.0 * taken)) : 0;
}

/* The columns [*low, *high) of row y, among [xl, xr), whose centers lie on
 * the side of a line where a*x + b*y >= d. */
static void XGL_ShapeSide (float a, float b, float d, int y, int xl, int xr, int *low, int *high)
{
    float cross;
    int k;
    if (a == 0) {
        *low = xl;
        *high = b * (y + .5f) >= d ? xr : xl;
        return;
    }
    cross = (d - b * (y + .5f)) / a - .5f;
    if (a > 0) {
        k = (int)ceil(cross);
        if (k < xl) k = xl;
        if (k > xr) k = xr;
        *low = k; *high = xr;
    } else {
        k = (int)floor(cross) + 1;
        if (k < xl) k = xl;
        if (k > xr) k = xr;
        *low = xl; *high = k;
    }
}

/* Sums over the far side of a line within a window of a part. */
static void XGL_ShapeFar (int index, float a, float b, float d,
                          int ya, int yb, int xa, int xb, double *far_sum, int *far_n)
{
    const xgl_shape_part_t *part = &xgl_shape.part[index];
    const short (*span)[2] = XGL_SHAPE_SPANS(index);
    const unsigned int *high_row, *low_row;
    unsigned int sum[3];
    double squares;
    int y, low, high, n, xl, xr;
    sum[0] = sum[1] = sum[2] = 0; squares = 0; n = 0;
    if (ya < part->y0) ya = part->y0;
    if (yb > part->y1) yb = part->y1;
    for (y = ya; y < yb; ++y) {
        xl = span[y][0] > xa ? span[y][0] : xa; xr = span[y][1] < xb ? span[y][1] : xb;
        if (xl >= xr) continue;
        XGL_ShapeSide(a, b, d, y, xl, xr, &low, &high);
        if (high <= low) continue;
        high_row = XGL_SHAPE_ROW(high, y); low_row = XGL_SHAPE_ROW(low, y);
        sum[0] += high_row[0] - low_row[0]; sum[1] += high_row[1] - low_row[1];
        sum[2] += high_row[2] - low_row[2]; squares += high_row[3] - low_row[3];
        n += high - low;
    }
    far_sum[0] = sum[0]; far_sum[1] = sum[1]; far_sum[2] = sum[2]; far_sum[3] = squares;
    *far_n = n;
}

/* What cutting a part along a line would gain. */
static double XGL_ShapeTry (int index, float a, float b, float d, double whole)
{
    const xgl_shape_part_t *part = &xgl_shape.part[index];
    double far_sum[4], near_sum[4];
    int n, c;
    XGL_ShapeFar(index, a, b, d, part->y0, part->y1, part->x0, part->x1, far_sum, &n);
    if (n < 2 * XGL_SHAPE_LEAST || part->n - n < 2 * XGL_SHAPE_LEAST) return 0;
    for (c = 0; c < 4; ++c) near_sum[c] = part->sum[c] - far_sum[c];
    return whole - XGL_ShapeError(far_sum, n) - XGL_ShapeError(near_sum, part->n - n);
}

/* What taking out the band between two parallel lines would gain, for each
 * of the two pieces that adds. */
static double XGL_ShapeTryBand (int index, float a, float b, float low, float high, double whole)
{
    const xgl_shape_part_t *part = &xgl_shape.part[index];
    double first[4], second[4], near_sum[4], between[4];
    int n1, n2, c;
    XGL_ShapeFar(index, a, b, low, part->y0, part->y1, part->x0, part->x1, first, &n1);
    XGL_ShapeFar(index, a, b, high, part->y0, part->y1, part->x0, part->x1, second, &n2);
    if (n1 - n2 < 2 || n2 < 2 * XGL_SHAPE_LEAST || part->n - n1 < 2 * XGL_SHAPE_LEAST) return 0;
    for (c = 0; c < 4; ++c) { near_sum[c] = part->sum[c] - first[c]; between[c] = first[c] - second[c]; }
    return (whole - XGL_ShapeError(near_sum, part->n - n1) - XGL_ShapeError(between, n1 - n2) -
            XGL_ShapeError(second, n2)) * .5;
}

/* Whether the color steps across a line along its length: the difference
 * between a strip three texels wide either side of it must point the same
 * way in each quarter of the stretch that lies within the part's box, in
 * every quarter when strict and in all but one otherwise. A line that only
 * balances two unlike corners has no such step. */
static int XGL_ShapeEdgeHolds (int index, int family, float a, float b, float d, int strict)
{
    const xgl_shape_part_t *part = &xgl_shape.part[index];
    double outer[4], middle[4], inner[4], step[4][3], mean[3], dot, length2;
    float width, at, low, high, end, swap;
    int q, c, n0, n1, n2, valid[4], count, agree, ya, yb, xa, xb, from, to;
    width = 3 * (float)sqrt(a * a + b * b);
    low = (float)(family == 0 ? part->y0 : part->x0); high = (float)(family == 0 ? part->y1 : part->x1);
    if (family == 0 ? b != 0 : a != 0) {
        /* The heights (or columns) between which the line crosses the box. */
        if (family == 0) { at = (d - a * part->x0) / b; end = (d - a * part->x1) / b; }
        else { at = (d - b * part->y0) / a; end = (d - b * part->y1) / a; }
        if (at > end) { swap = at; at = end; end = swap; }
        if (at > low) low = at;
        if (end < high) high = end;
    }
    from = (int)floor(low); to = (int)ceil(high);
    if (to - from < 8) return 0;
    count = 0;
    mean[0] = mean[1] = mean[2] = 0;
    for (q = 0; q < 4; ++q) {
        ya = part->y0; yb = part->y1; xa = part->x0; xb = part->x1;
        if (family == 0) { ya = from + (to - from) * q / 4; yb = from + (to - from) * (q + 1) / 4; }
        else { xa = from + (to - from) * q / 4; xb = from + (to - from) * (q + 1) / 4; }
        XGL_ShapeFar(index, a, b, d - width, ya, yb, xa, xb, outer, &n0);
        XGL_ShapeFar(index, a, b, d, ya, yb, xa, xb, middle, &n1);
        XGL_ShapeFar(index, a, b, d + width, ya, yb, xa, xb, inner, &n2);
        valid[q] = n0 - n1 >= 2 && n1 - n2 >= 2;
        if (!valid[q]) continue;
        for (c = 0; c < 3; ++c) {
            step[q][c] = (middle[c] - inner[c]) / (n1 - n2) - (outer[c] - middle[c]) / (n0 - n1);
            mean[c] += step[q][c];
        }
        ++count;
    }
    if (count < 3) return 0;
    length2 = 0;
    for (c = 0; c < 3; ++c) { mean[c] /= count; length2 += mean[c] * mean[c]; }
    if (length2 < XGL_SHAPE_EDGE_STEP * XGL_SHAPE_EDGE_STEP) return 0;
    agree = 0;
    for (q = 0; q < 4; ++q) {
        if (!valid[q]) continue;
        dot = step[q][0] * mean[0] + step[q][1] * mean[1] + step[q][2] * mean[2];
        if (dot >= XGL_SHAPE_EDGE_AGREE * length2) ++agree;
    }
    if (strict) return count == 4 && agree == 4;
    return agree >= 3 && agree >= count - 1;
}

/* The line through two places on opposite sides of a part's box: on its top
 * and bottom for a cut that began upright (family 0), on its left and right
 * for one that began level. */
static void XGL_ShapeLine (const xgl_shape_part_t *part, int family, float p, float q,
                           float *a, float *b, float *d)
{
    if (family == 0) {
        *a = (float)(part->y1 - part->y0); *b = p - q;
        *d = *a * p + *b * part->y0;
    } else {
        *a = p - q; *b = (float)(part->x1 - part->x0);
        *d = *a * part->x0 + *b * p;
    }
}

/* For every upright cut and every level cut of a window of a part, the sums
 * over the far side, left in xgl_shape.far_sum. */
static void XGL_ShapeProfiles (int index, int ya, int yb, int xa, int xb)
{
    static unsigned int column[XGL_SHAPE_SIDE + 1][4];
    static int column_n[XGL_SHAPE_SIDE + 1];
    const xgl_shape_part_t *part = &xgl_shape.part[index];
    const short (*span)[2] = XGL_SHAPE_SPANS(index);
    const unsigned int *texel, *high_row, *low_row;
    double (*far_sum)[4];
    int *far_n;
    int c, k, x, y, xl, xr;
    if (part->rect) {
        /* A rectangle's parts are rectangles: four lookups each. */
        for (c = xa; c <= xb; ++c) {
            XGL_ShapeBox(c, ya, xb, yb, xgl_shape.far_sum[0][c]);
            xgl_shape.far_n[0][c] = (xb - c) * (yb - ya);
        }
        for (c = ya; c <= yb; ++c) {
            XGL_ShapeBox(xa, c, xb, yb, xgl_shape.far_sum[1][c]);
            xgl_shape.far_n[1][c] = (xb - xa) * (yb - c);
        }
        return;
    }
    if (ya < part->y0) ya = part->y0;
    if (yb > part->y1) yb = part->y1;
    /* Column sums over the window, then running sums from the right. */
    for (c = xa; c <= xb; ++c) {
        column[c][0] = column[c][1] = column[c][2] = column[c][3] = 0;
        column_n[c] = 0;
    }
    for (y = ya; y < yb; ++y) {
        xl = span[y][0] > xa ? span[y][0] : xa; xr = span[y][1] < xb ? span[y][1] : xb;
        if (xl >= xr) continue;
        texel = XGL_SHAPE_TEXEL(xl, y);
        for (x = xl; x < xr; ++x, texel += 4) {
            column[x][0] += texel[0]; column[x][1] += texel[1];
            column[x][2] += texel[2]; column[x][3] += texel[3];
            ++column_n[x];
        }
    }
    far_sum = xgl_shape.far_sum[0]; far_n = xgl_shape.far_n[0];
    for (k = 0; k < 4; ++k) far_sum[xb][k] = 0;
    far_n[xb] = 0;
    for (c = xb - 1; c >= xa; --c) {
        for (k = 0; k < 4; ++k) far_sum[c][k] = far_sum[c + 1][k] + column[c][k];
        far_n[c] = far_n[c + 1] + column_n[c];
    }
    /* Row sums, running from the bottom. */
    far_sum = xgl_shape.far_sum[1]; far_n = xgl_shape.far_n[1];
    for (k = 0; k < 4; ++k) far_sum[yb][k] = 0;
    far_n[yb] = 0;
    for (y = yb - 1; y >= ya; --y) {
        xl = span[y][0] > xa ? span[y][0] : xa; xr = span[y][1] < xb ? span[y][1] : xb;
        for (k = 0; k < 4; ++k) far_sum[y][k] = far_sum[y + 1][k];
        far_n[y] = far_n[y + 1];
        if (xl >= xr) continue;
        high_row = XGL_SHAPE_ROW(xr, y); low_row = XGL_SHAPE_ROW(xl, y);
        for (k = 0; k < 4; ++k) far_sum[y][k] += (double)(high_row[k] - low_row[k]);
        far_n[y] += xr - xl;
    }
}

/* Among the cuts of one family that were last profiled, between low and
 * high: the best single cut, and the best pair with a thin band between. */
static void XGL_ShapeFamily (int family, int low, int high, xgl_shape_cut_t *single, xgl_shape_cut_t *band)
{
    static double far_error[XGL_SHAPE_SIDE + 2];
    double (*far_sum)[4] = xgl_shape.far_sum[family];
    const int *far_n = xgl_shape.far_n[family];
    double near_sum[4], middle[4], whole, gain, near_error;
    int c, w, k, n, other;
    single->gain = single->total = 0; single->band = 0; single->d = single->d2 = 0;
    band->gain = band->total = 0; band->band = 1; band->d = band->d2 = 0;
    n = far_n[low];
    if (n < 4 * XGL_SHAPE_LEAST) return;
    whole = XGL_ShapeError(far_sum[low], n);
    /* Every band that ends at a place asks for the error beyond it. */
    for (c = low + XGL_SHAPE_LEAST; c <= high - XGL_SHAPE_LEAST; ++c)
        far_error[c] = XGL_ShapeError(far_sum[c], far_n[c]);
    for (c = low + XGL_SHAPE_LEAST; c <= high - XGL_SHAPE_LEAST; ++c) {
        if (far_n[c] < 2 * XGL_SHAPE_LEAST || n - far_n[c] < 2 * XGL_SHAPE_LEAST) continue;
        for (k = 0; k < 4; ++k) near_sum[k] = far_sum[low][k] - far_sum[c][k];
        near_error = XGL_ShapeError(near_sum, n - far_n[c]);
        gain = whole - near_error - far_error[c];
        if (gain > single->total) { single->total = single->gain = gain; single->d = (float)c; }
        for (w = 2; w <= XGL_SHAPE_BAND; ++w) {
            other = c + w;
            if (other > high - XGL_SHAPE_LEAST) break;
            if (far_n[other] < 2 * XGL_SHAPE_LEAST || far_n[c] - far_n[other] < 2) continue;
            for (k = 0; k < 4; ++k) middle[k] = far_sum[c][k] - far_sum[other][k];
            gain = whole - near_error - XGL_ShapeError(middle, far_n[c] - far_n[other]) -
                   far_error[other];
            if (gain > band->total) {
                band->total = gain; band->gain = gain * .5;
                band->d = (float)c; band->d2 = (float)other;
            }
        }
    }
}

/* A part's best division by upright or level cuts. */
static void XGL_ShapeBest (int index)
{
    xgl_shape_part_t *part = &xgl_shape.part[index];
    xgl_shape_cut_t single[2], band[2], *cut;
    int family;
    part->gain = part->total = 0; part->axis = 0; part->band = 0;
    part->a = 1; part->b = 0; part->d = part->d2 = -1;
    part->best[0] = part->best[1] = 0;
    if (part->n < 4 * XGL_SHAPE_LEAST) return;
    XGL_ShapeProfiles(index, part->y0, part->y1, part->x0, part->x1);
    XGL_ShapeFamily(0, part->x0, part->x1, &single[0], &band[0]);
    XGL_ShapeFamily(1, part->y0, part->y1, &single[1], &band[1]);
    cut = NULL;
    for (family = 0; family < 2; ++family) {
        part->best[family] = single[family].gain > band[family].gain ? single[family].gain : band[family].gain;
        if (single[family].gain > 0 && (!cut || single[family].gain > cut->gain)) {
            cut = &single[family]; part->axis = (byte)family;
        }
        if (band[family].gain > 0 && (!cut || band[family].gain > cut->gain)) {
            cut = &band[family]; part->axis = (byte)family;
        }
    }
    if (!cut) return;
    part->gain = (float)cut->gain; part->total = (float)cut->total; part->band = (byte)cut->band;
    part->a = part->axis ? 0.0f : 1.0f; part->b = part->axis ? 1.0f : 0.0f;
    part->d = cut->d; part->d2 = cut->d2;
}

/* What a tilted cut through two places gains, or, for half > 0, a band of
 * that half width about the line, per piece. Gives back the line and how
 * far the band's sides lie either way of it. */
static double XGL_ShapeTilted (int index, int family, float p, float q, float half,
                               double whole, float *a, float *b, float *d, float *shift)
{
    XGL_ShapeLine(&xgl_shape.part[index], family, p, q, a, b, d);
    *shift = 0;
    if (!(half > 0)) return XGL_ShapeTry(index, *a, *b, *d, whole);
    *shift = half * (family == 0 ? *a : *b);
    return XGL_ShapeTryBand(index, *a, *b, *d - *shift, *d + *shift, whole);
}

/* Move the ends of a tilted cut or band two texels, then one, at a time
 * while that gains. Never steeper than one across for one along. */
static double XGL_ShapeClimb (int index, int family, float half, double whole, double gain, float *line)
{
    const xgl_shape_part_t *part = &xgl_shape.part[index];
    double trial, best;
    float p, q, tp, tq, bp, bq, a, b, d, shift, reach;
    int step, pass, move;
    reach = (float)(family == 0 ? part->y1 - part->y0 : part->x1 - part->x0);
    p = line[0]; q = line[1];
    for (step = 2; step >= 1; --step)
        for (pass = 0; pass < 6; ++pass) {
            best = gain; bp = p; bq = q;
            for (move = 0; move < 4; ++move) {
                tp = p + ((move == 0) - (move == 1)) * step;
                tq = q + ((move == 2) - (move == 3)) * step;
                if (fabs(tq - tp) > reach) continue;
                trial = XGL_ShapeTilted(index, family, tp, tq, half, whole, &a, &b, &d, &shift);
                if (trial > best) { best = trial; bp = tp; bq = tq; }
            }
            if (!(best > gain)) break;
            gain = best; p = bp; q = bq;
        }
    line[0] = p; line[1] = q;
    return gain;
}

/* A sloping outline shows as a middling upright or level cut. To find the
 * slope, the edge, or the thin band, is located separately in the two
 * halves of the part; if it lies in different places, the line through both
 * is tried and adjusted, and kept if it is worth more than the upright cut
 * and the color steps across it. Done only for a part about to be cut. */
static void XGL_ShapeTilt (int index)
{
    xgl_shape_part_t *part = &xgl_shape.part[index];
    const short (*span)[2] = XGL_SHAPE_SPANS(index);
    xgl_shape_cut_t single[2], band[2];
    double whole, gain, trial, cx, cy;
    float line[2], at[2], a, b, d, shift, slope, half, low_d, high_d, unit, other, lo, hi, best_d;
    int family, low, high, middle, half_index, banded, w, side, e, y, rows, found;
    part->tilted = 1;
    if (part->n < 64 || part->x1 - part->x0 < 6 || part->y1 - part->y0 < 6) return;
    whole = XGL_ShapeError(part->sum, part->n);
    for (family = 0; family < 2; ++family) {
        if (!(part->best[family] > 0)) continue;
        low = family == 0 ? part->y0 : part->x0; high = family == 0 ? part->y1 : part->x1;
        middle = (low + high) / 2;
        for (half_index = 0; half_index < 2; ++half_index) {
            if (family == 0) {
                XGL_ShapeProfiles(index, half_index ? middle : low, half_index ? high : middle, part->x0, part->x1);
                XGL_ShapeFamily(0, part->x0, part->x1, &single[half_index], &band[half_index]);
            } else {
                XGL_ShapeProfiles(index, part->y0, part->y1, half_index ? middle : low, half_index ? high : middle);
                XGL_ShapeFamily(1, part->y0, part->y1, &single[half_index], &band[half_index]);
            }
        }
        banded = band[0].gain > single[0].gain && band[1].gain > single[1].gain &&
                 fabs((band[0].d2 - band[0].d) - (band[1].d2 - band[1].d)) <= 3;
        if (banded) {
            at[0] = (band[0].d + band[0].d2) * .5f; at[1] = (band[1].d + band[1].d2) * .5f;
            half = ((band[0].d2 - band[0].d) + (band[1].d2 - band[1].d)) * .25f;
        } else {
            if (!(single[0].gain > 0) || !(single[1].gain > 0)) continue;
            at[0] = single[0].d; at[1] = single[1].d; half = 0;
        }
        if (fabs(at[1] - at[0]) < XGL_SHAPE_TILT_LEAST) continue;
        /* The two places are the line's at a quarter and three quarters. */
        slope = (at[1] - at[0]) * .5f;
        line[0] = at[0] - slope; line[1] = at[1] + slope;
        if (!banded) { line[0] = (float)floor(line[0] + .5f); line[1] = (float)floor(line[1] + .5f); }
        if (fabs(line[1] - line[0]) > (float)(high - low)) continue;
        gain = XGL_ShapeTilted(index, family, line[0], line[1], half, whole, &a, &b, &d, &shift);
        gain = XGL_ShapeClimb(index, family, half, whole, gain, line);
        if (line[0] == line[1] || !(gain > part->gain * XGL_SHAPE_TILT_WORTH)) continue;
        XGL_ShapeTilted(index, family, line[0], line[1], half, whole, &a, &b, &d, &shift);
        if (!XGL_ShapeEdgeHolds(index, family, a, b, d - shift, 1) ||
            (banded && !XGL_ShapeEdgeHolds(index, family, a, b, d + shift, 0))) continue;
        low_d = d - shift; high_d = d + shift;
        if (!banded) {
            /* One side of a thin band, perhaps: look for the other, parallel
             * to it and within a few texels either way. */
            unit = family == 0 ? a : b;
            for (side = -1; side <= 1; side += 2)
                for (w = 2; w <= XGL_SHAPE_BAND; ++w) {
                    other = d + side * w * unit;
                    lo = side < 0 ? other : d; hi = side < 0 ? d : other;
                    trial = XGL_ShapeTryBand(index, a, b, lo, hi, whole);
                    if (trial > gain && XGL_ShapeEdgeHolds(index, family, a, b, other, 0)) {
                        gain = trial; banded = 1; low_d = lo; high_d = hi;
                    }
                }
        }
        part->gain = (float)gain; part->total = (float)(banded ? gain * 2 : gain);
        part->axis = 2; part->band = (byte)banded;
        part->a = a; part->b = b; part->d = low_d; part->d2 = high_d;
    }
    if (!part->sides) return;
    /* An outline is often a thin line of its own color along the edge it
     * outlines: beside each tilted side of the part, try a cut parallel to
     * it a few texels in. */
    cx = cy = 0; rows = 0;
    for (y = part->y0; y < part->y1; ++y)
        if (span[y][0] < span[y][1]) { cx += (span[y][0] + span[y][1]) * .5; cy += y + .5; ++rows; }
    if (!rows) return;
    cx /= rows; cy /= rows;
    for (e = 0; e < part->sides; ++e) {
        a = part->side[e][0]; b = part->side[e][1]; d = part->side[e][2];
        unit = (float)sqrt(a * a + b * b);
        if (a * cx + b * cy < d) unit = -unit;
        gain = part->gain * XGL_SHAPE_TILT_WORTH; found = 0; best_d = d;
        for (w = 2; w <= XGL_SHAPE_SIDE_REACH; ++w) {
            other = d + w * unit;
            trial = XGL_ShapeTry(index, a, b, other, whole);
            if (trial > gain) { gain = trial; best_d = other; found = 1; }
        }
        if (!found || !XGL_ShapeEdgeHolds(index, fabs(a) >= fabs(b) ? 0 : 1, a, b, best_d, 0)) continue;
        part->gain = part->total = (float)gain;
        part->axis = 2; part->band = 0;
        part->a = a; part->b = b; part->d = part->d2 = best_d;
    }
}

/* The color a set of texels is drawn in, from their sums and number at
 * each level of brightness: the mean with the darkest and the brightest
 * fifth left out, or of all of them when they are few. */
static void XGL_ShapeTrimmedColor (unsigned int (*bins)[4], unsigned int n, byte *color)
{
    double sum[3], take;
    unsigned int skip, keep, seen, used, count;
    int c, level;
    if (!n) return;
    skip = n < 16 ? 0 : (unsigned int)(n * XGL_SHAPE_TRIM);
    keep = n - 2 * skip;
    sum[0] = sum[1] = sum[2] = 0; seen = used = 0;
    for (level = 0; level < 256 && used < keep; ++level) {
        count = bins[level][3];
        if (!count) continue;
        if (seen + count <= skip) { seen += count; continue; }
        /* The part of this level's texels that falls in the kept range. */
        take = (double)count;
        if (seen < skip) take -= skip - seen;
        if (used + take > keep) take = keep - used;
        for (c = 0; c < 3; ++c) sum[c] += bins[level][c] * (take / count);
        used += (unsigned int)(take + .5);
        seen += count;
    }
    if (used) for (c = 0; c < 3; ++c) color[c] = (byte)(sum[c] / used + .5);
}

/* The color a piece is drawn in: the mean of its texels with the darkest
 * and the brightest fifth left out. A thin line or a few bright specks that
 * no cut has yet taken out then do not tint the whole piece: it shows the
 * color of what mostly fills it. */
static void XGL_ShapeBodyColor (int index)
{
    static unsigned int bins[256][4];
    xgl_shape_part_t *part = &xgl_shape.part[index];
    const short (*span)[2] = XGL_SHAPE_SPANS(index);
    const unsigned int *texel;
    int x, y, level;
    if (part->n < 16) return;
    memset(bins, 0, sizeof(bins));
    for (y = part->y0; y < part->y1; ++y) {
        if (span[y][0] >= span[y][1]) continue;
        texel = XGL_SHAPE_TEXEL(span[y][0], y);
        for (x = span[y][0]; x < span[y][1]; ++x, texel += 4) {
            level = (int)((texel[0] * 77 + texel[1] * 150 + texel[2] * 29) >> 8);
            bins[level][0] += texel[0]; bins[level][1] += texel[1]; bins[level][2] += texel[2];
            ++bins[level][3];
        }
    }
    XGL_ShapeTrimmedColor(bins, (unsigned int)part->n, part->color);
}

/* Give a part the texels of its parent on one side of the parent's cut. */
static void XGL_ShapeTake (int index, int parent, int far)
{
    xgl_shape_part_t *part = &xgl_shape.part[index];
    const xgl_shape_part_t *from = &xgl_shape.part[parent];
    const short (*source)[2] = XGL_SHAPE_SPANS(parent);
    short (*span)[2] = XGL_SHAPE_SPANS(index);
    const unsigned int *high_row, *low_row;
    int y, low, high, c;
    memset(part, 0, sizeof(*part));
    part->child[0] = part->child[1] = -1;
    part->x0 = xgl_shape.width; part->y0 = xgl_shape.height;
    memset(span, 0, (size_t)xgl_shape.height * sizeof(span[0]));
    for (y = from->y0; y < from->y1; ++y) {
        if (source[y][0] >= source[y][1]) continue;
        XGL_ShapeSide(from->a, from->b, from->d, y, source[y][0], source[y][1], &low, &high);
        if (!far) {
            /* The rest of the row: it lies wholly to one side of the far part. */
            if (high <= low) { low = source[y][0]; high = source[y][1]; }
            else if (low == source[y][0]) { low = high; high = source[y][1]; }
            else { high = low; low = source[y][0]; }
        }
        if (high <= low) continue;
        span[y][0] = (short)low; span[y][1] = (short)high;
        high_row = XGL_SHAPE_ROW(high, y); low_row = XGL_SHAPE_ROW(low, y);
        for (c = 0; c < 4; ++c) part->sum[c] += (double)(high_row[c] - low_row[c]);
        part->n += high - low;
        if (low < part->x0) part->x0 = low;
        if (high > part->x1) part->x1 = high;
        if (y < part->y0) part->y0 = y;
        part->y1 = y + 1;
    }
    if (!part->n) { part->x0 = part->x1 = from->x0; part->y0 = part->y1 = from->y0; }
    part->rect = (byte)(from->rect && from->axis < 2);
    for (c = 0; c < 3; ++c)
        part->color[c] = part->n ? (byte)(part->sum[c] / part->n + .5) : from->color[c];
    if (from->axis == 2) {
        part->side[0][0] = from->a; part->side[0][1] = from->b; part->side[0][2] = from->d;
        part->sides = 1;
    }
    for (c = 0; c < from->sides && part->sides < 2; ++c) {
        memcpy(part->side[part->sides], from->side[c], sizeof(part->side[0]));
        ++part->sides;
    }
}

/* Divide the repeat, most telling cut first, and say whether the texture
 * has shapes. First with upright and level cuts only, as far as the verdict
 * looks: a texture nowhere near having shapes is left at that. One that may
 * have them is divided again from the start with tilting, and, if the
 * verdict holds, all the way. */
static int XGL_ShapeDivide (double smooth)
{
    xgl_shape_part_t *part, *rest_part;
    double whole, rest, pieces_error, texels;
    int i, best, leaves, pass, first, judged, structured, enough, w, h;
    w = xgl_shape.width; h = xgl_shape.height;
    texels = 3.0 * w * h;
    /* As many pieces as the triangles of the smooth mesh would pay for. */
    enough = (int)(2 * (w / XGL_SHAPE_GRID > 1 ? w / XGL_SHAPE_GRID : 1) *
                   (h / XGL_SHAPE_GRID > 1 ? h / XGL_SHAPE_GRID : 1) / XGL_SHAPE_PIECE_TRIANGLES);
    if (enough < 2) enough = 2;
    part = &xgl_shape.part[0];
    whole = XGL_ShapeError(part->sum, part->n);
    structured = 0;
    for (pass = 0; pass < 2; ++pass) {
        xgl_shape.count = 1; leaves = 1; rest = whole; judged = 0;
        part = &xgl_shape.part[0];
        part->child[0] = part->child[1] = -1; part->tilted = part->paired = 0;
        XGL_ShapeBest(0);
        pieces_error = sqrt(whole / texels);
        while (leaves < XGL_SHAPE_PIECES && xgl_shape.count + 2 <= XGL_SHAPE_NODES) {
            XGL_AudioUpdate();
            best = -1;
            for (i = 0; i < xgl_shape.count; ++i)
                if (xgl_shape.part[i].child[0] < 0 && xgl_shape.part[i].gain > 0 &&
                    (best < 0 || xgl_shape.part[i].gain > xgl_shape.part[best].gain)) best = i;
            if (best < 0 || xgl_shape.part[best].gain < XGL_SHAPE_STOP * w * h) break;
            if (judged && xgl_shape.part[best].gain < XGL_SHAPE_STOP_GAIN) break;
            part = &xgl_shape.part[best];
            if (pass && !part->tilted) XGL_ShapeTilt(best);
            first = xgl_shape.count;
            XGL_ShapeTake(first, best, 0);
            XGL_ShapeTake(first + 1, best, 1);
            part->child[0] = (short)first; part->child[1] = (short)(first + 1);
            xgl_shape.count += 2; ++leaves;
            rest -= part->total;
            if (part->band && xgl_shape.count + 2 <= XGL_SHAPE_NODES) {
                /* The band's other side, cut from the far part at once. */
                rest_part = &xgl_shape.part[first + 1];
                rest_part->a = part->a; rest_part->b = part->b; rest_part->d = part->d2;
                rest_part->axis = part->axis; rest_part->gain = part->gain;
                rest_part->second = rest_part->tilted = 1;
                part->paired = 1;
                XGL_ShapeTake(xgl_shape.count, first + 1, 0);
                XGL_ShapeTake(xgl_shape.count + 1, first + 1, 1);
                rest_part->child[0] = (short)xgl_shape.count;
                rest_part->child[1] = (short)(xgl_shape.count + 1);
                XGL_ShapeBest(first);
                XGL_ShapeBest(xgl_shape.count); XGL_ShapeBest(xgl_shape.count + 1);
                xgl_shape.count += 2; ++leaves;
            } else {
                part->band = 0;
                XGL_ShapeBest(first); XGL_ShapeBest(first + 1);
            }
            if (judged) continue;
            pieces_error = sqrt((rest > 0 ? rest : 0) / texels);
            if (leaves < enough) continue;
            judged = 1;
            structured = pieces_error < smooth * XGL_SHAPE_BETTER && smooth - pieces_error >= XGL_SHAPE_MARGIN;
            /* Tilting may yet win a near miss, so the first pass ends here. */
            if (!pass || !structured) break;
        }
        if (!judged)
            structured = pieces_error < smooth * XGL_SHAPE_BETTER && smooth - pieces_error >= XGL_SHAPE_MARGIN;
        if (!pass && !(pieces_error < smooth * XGL_SHAPE_HOPE)) break;
    }
    return structured;
}

/* About how many texels across a part is: its area over its longer side. */
static float XGL_ShapeWidth (const xgl_shape_part_t *part)
{
    int longer = part->x1 - part->x0 > part->y1 - part->y0 ? part->x1 - part->x0 : part->y1 - part->y0;
    return longer > 0 ? (float)part->n / longer : 0;
}

/* Examine one texture. Returns its division, or NULL when it has no shapes
 * worth following or there was no memory to look. */
static xgl_shape_node_t *XGL_ShapeAnalyse (const byte *texels, int width, int height,
                                           int *origin, int *count, byte **colors)
{
    xgl_shape_part_t *part;
    xgl_shape_node_t *nodes, *node;
    const byte *color;
    unsigned int *texel, *row;
    double *table, *above;
    void *memory;
    float near_width, far_width;
    size_t texel_bytes, row_bytes, table_bytes, span_bytes;
    int x, y, c, i, sx, sy;
    texel_bytes = (size_t)width * height * sizeof(xgl_shape.texel[0]);
    row_bytes = (size_t)(width + 1) * height * sizeof(xgl_shape.row[0]);
    table_bytes = (size_t)(width + 1) * (height + 1) * sizeof(xgl_shape.table[0]);
    span_bytes = (size_t)XGL_SHAPE_NODES * height * sizeof(xgl_shape.span[0]);
    /* The table of doubles first, for its alignment. */
    memory = malloc(table_bytes + texel_bytes + row_bytes + span_bytes);
    if (!memory) return NULL;
    xgl_shape.table = (double (*)[4])memory;
    xgl_shape.texel = (unsigned int (*)[4])((byte *)memory + table_bytes);
    xgl_shape.row = (unsigned int (*)[4])((byte *)memory + table_bytes + texel_bytes);
    xgl_shape.span = (short (*)[2])((byte *)memory + table_bytes + texel_bytes + row_bytes);
    xgl_shape.width = width; xgl_shape.height = height;
    XGL_ShapeLines(texels, width, height);
    origin[0] = XGL_ShapeOrigin(xgl_shape_columns, width, height);
    origin[1] = XGL_ShapeOrigin(xgl_shape_rows, height, width);
    part = &xgl_shape.part[0];
    memset(part, 0, sizeof(*part));
    table = XGL_SHAPE_TABLE(0, 0);
    for (x = 0; x <= width; ++x, table += 4) table[0] = table[1] = table[2] = table[3] = 0;
    for (y = 0; y < height; ++y) {
        if ((y & 31) == 31) XGL_AudioUpdate();
        sy = (y + origin[1]) % height;
        texel = XGL_SHAPE_TEXEL(0, y); row = XGL_SHAPE_ROW(0, y);
        table = XGL_SHAPE_TABLE(0, y + 1); above = XGL_SHAPE_TABLE(0, y);
        row[0] = row[1] = row[2] = row[3] = 0;
        table[0] = table[1] = table[2] = table[3] = 0;
        sx = origin[0];
        for (x = 0; x < width; ++x, texel += 4, row += 4, table += 4, above += 4) {
            color = xgl_palette + 3 * texels[sy * width + sx];
            if (++sx == width) sx = 0;
            texel[0] = color[0]; texel[1] = color[1]; texel[2] = color[2];
            texel[3] = (unsigned int)color[0] * color[0] + (unsigned int)color[1] * color[1] +
                       (unsigned int)color[2] * color[2];
            /* The row so far is a whole number; the table adds it to the
             * rows above. */
            for (c = 0; c < 4; ++c) {
                row[4 + c] = row[c] + texel[c];
                table[4 + c] = above[4 + c] + row[4 + c];
            }
        }
        XGL_SHAPE_SPANS(0)[y][0] = 0; XGL_SHAPE_SPANS(0)[y][1] = (short)width;
        /* row has come to the whole row's sums. */
        for (c = 0; c < 4; ++c) part->sum[c] += row[c];
    }
    part->n = width * height;
    part->x1 = width; part->y1 = height; part->rect = 1;
    part->child[0] = part->child[1] = -1;
    for (c = 0; c < 3; ++c) part->color[c] = (byte)(part->sum[c] / part->n + .5);
    nodes = NULL;
    if (XGL_ShapeDivide(XGL_ShapeSmooth()) && xgl_shape.count >= 3) {
        nodes = (xgl_shape_node_t *)malloc((size_t)xgl_shape.count * sizeof(*nodes));
        *colors = (byte *)malloc((size_t)xgl_shape.count * 3);
        if (!nodes || !*colors) {
            free(nodes); free(*colors);
            nodes = NULL; *colors = NULL;
        }
    }
    if (nodes) {
        *count = xgl_shape.count;
        for (i = 0; i < xgl_shape.count; ++i) {
            if ((i & 15) == 15) XGL_AudioUpdate();
            part = &xgl_shape.part[i]; node = &nodes[i];
            if (i) XGL_ShapeBodyColor(i);
            node->a = part->a; node->b = part->b; node->d = part->d;
            node->gain = part->gain; node->width = 0;
            node->child[0] = part->child[0]; node->child[1] = part->child[1];
            memcpy(*colors + 3 * i, part->color, 3);
            node->flags = part->axis;
            if (part->child[0] < 0) continue;
            if (part->paired) node->flags |= XGL_SHAPE_PAIRED;
            if (part->second) node->flags |= XGL_SHAPE_SECOND;
            near_width = XGL_ShapeWidth(&xgl_shape.part[part->child[0]]);
            far_width = XGL_ShapeWidth(&xgl_shape.part[part->child[1]]);
            if (far_width < near_width) { node->flags |= XGL_SHAPE_THIN_FAR; node->width = far_width; }
            else node->width = near_width;
        }
    }
    free(memory);
    xgl_shape.table = NULL; xgl_shape.texel = NULL; xgl_shape.row = NULL; xgl_shape.span = NULL;
    return nodes;
}

/* A division is going away: nothing may point at it any longer. Which
 * division each animated texture is cut along is worked out again. */
static void XGL_ShapeForget (xgl_shape_entry_t *entry)
{
    xgl_shape_entry_t *other;
    int i;
    for (i = 0, other = xgl_shape_entries; i < XGL_SHAPE_TEXTURES; ++i, ++other) {
        other->resolved = 0;
        other->wall = NULL;
        if (entry->tree && other->colors_tree == entry->tree) {
            free(other->colors);
            other->colors = NULL; other->colors_tree = NULL;
        }
    }
    if (entry->tree) {
        free(entry->tree->colors);
        free(entry->tree->nodes);
        free(entry->tree);
        entry->tree = NULL;
    }
    /* Its own colors over another division are of its old texels. */
    free(entry->colors);
    entry->colors = NULL; entry->colors_tree = NULL;
}

void XGL_ShapesClear (void)
{
    xgl_shape_entry_t *entry;
    int i;
    if (xgl_shape_entries)
        for (i = 0, entry = xgl_shape_entries; i < XGL_SHAPE_TEXTURES; ++i, ++entry) {
            if (entry->tree) {
                free(entry->tree->colors);
                free(entry->tree->nodes);
                free(entry->tree);
            }
            free(entry->colors);
        }
    free(xgl_shape_entries);
    xgl_shape_entries = NULL;
    xgl_shape_examined = xgl_shape_found = 0;
}

/* The textures of a new map are loaded. What is known of a texture that
 * is still what it was stays; what is known of one that has gone, or whose
 * place another has, goes. Which division each animated texture is cut
 * along is worked out again in any case: the new map's frames may be linked
 * differently. */
void XGL_ShapesSweep (void)
{
    xgl_shape_entry_t *entry;
    int i;
    if (!xgl_shape_entries) return;
    for (i = 0, entry = xgl_shape_entries; i < XGL_SHAPE_TEXTURES; ++i, ++entry) {
        if (entry->revision && XGL_TextureRevision(i + 1) != entry->revision) {
            XGL_ShapeForget(entry);
            entry->revision = 0;
        }
    }
    for (i = 0, entry = xgl_shape_entries; i < XGL_SHAPE_TEXTURES; ++i, ++entry) {
        entry->resolved = 0;
        entry->wall = NULL;
    }
}

void XGL_ShapesCount (int *examined, int *shaped)
{
    if (examined) *examined = xgl_shape_examined;
    if (shaped) *shaped = xgl_shape_found;
}

/* A texture's record with its own division up to date, examining it if it
 * has not been or has been loaded again since; NULL for one that is no
 * texture. */
static xgl_shape_entry_t *XGL_ShapeEntry (texture_t *texture)
{
    xgl_shape_entry_t *entry;
    xgl_shape_tree_t *tree;
    const byte *texels;
    unsigned int revision;
    int id, width, height;
    if (!texture) return NULL;
    id = texture->gl_texturenum;
    if (id <= 0 || id > XGL_SHAPE_TEXTURES) return NULL;
    revision = XGL_TextureRevision(id);
    if (!revision) return NULL;
    if (!xgl_shape_entries) {
        xgl_shape_entries = (xgl_shape_entry_t *)calloc(XGL_SHAPE_TEXTURES, sizeof(*xgl_shape_entries));
        if (!xgl_shape_entries) return NULL;
    }
    entry = &xgl_shape_entries[id - 1];
    if (entry->revision == revision) return entry;
    if (entry->revision) XGL_ShapeForget(entry);
    entry->revision = revision;
    entry->resolved = 0;
    texels = XGL_TextureTexels(id, &width, &height);
    if (!texels || width > XGL_SHAPE_SIDE || height > XGL_SHAPE_SIDE ||
        width < 4 * XGL_SHAPE_LEAST || height < 4 * XGL_SHAPE_LEAST) return entry;
    ++xgl_shape_examined;
    tree = (xgl_shape_tree_t *)calloc(1, sizeof(*tree));
    if (!tree) return entry;
    tree->nodes = XGL_ShapeAnalyse(texels, width, height, tree->origin, &tree->count, &tree->colors);
    if (!tree->nodes) {
        free(tree);
        return entry;
    }
    tree->width = width; tree->height = height;
    entry->tree = tree;
    ++xgl_shape_found;
    return entry;
}

/* Add the frames of one sequence of an animation to a list. */
static int XGL_ShapeSequence (texture_t *first, texture_t **list, int count)
{
    texture_t *texture = first;
    int loops;
    for (loops = 0; texture && loops < XGL_SHAPE_FRAMES / 2 && count < XGL_SHAPE_FRAMES; ++loops) {
        list[count++] = texture;
        texture = texture->anim_next;
        if (texture == first) break;
    }
    return count;
}

xgl_shape_tree_t *XGL_TextureShapes (texture_t *texture)
{
    texture_t *frames[XGL_SHAPE_FRAMES];
    xgl_shape_entry_t *entry, *other;
    xgl_shape_tree_t *best;
    int i, count, best_id;
    entry = XGL_ShapeEntry(texture);
    if (!entry) return NULL;
    if (entry->resolved) return entry->wall;
    if (!texture->anim_total) {
        entry->wall = entry->tree;
        entry->resolved = 1;
        return entry->wall;
    }
    /* A wall keeps its mesh while its texture goes through its frames, so
     * every frame, of either sequence, is cut along one division: that of
     * the frame with the most pieces, which shows the texture's shapes at
     * their plainest. A glow that is faint in one frame is still cut out. */
    count = XGL_ShapeSequence(texture, frames, 0);
    if (texture->alternate_anims) count = XGL_ShapeSequence(texture->alternate_anims, frames, count);
    /* All of them first: examining one forgets what was settled before. */
    for (i = 0; i < count; ++i) XGL_ShapeEntry(frames[i]);
    best = NULL; best_id = 0;
    for (i = 0; i < count; ++i) {
        other = XGL_ShapeEntry(frames[i]);
        if (!other || !other->tree) continue;
        if (!best || other->tree->count > best->count ||
            (other->tree->count == best->count && frames[i]->gl_texturenum < best_id)) {
            best = other->tree;
            best_id = frames[i]->gl_texturenum;
        }
    }
    for (i = 0; i < count; ++i) {
        other = XGL_ShapeEntry(frames[i]);
        if (!other) continue;
        /* A frame of another size cannot share it. */
        other->wall = best && (int)frames[i]->width == best->width &&
                      (int)frames[i]->height == best->height ? best : other->tree;
        other->resolved = 1;
    }
    return entry->wall;
}

const byte *XGL_ShapeColors (const xgl_shape_tree_t *tree, texture_t *texture)
{
    xgl_shape_entry_t *entry;
    const xgl_shape_node_t *node;
    const byte *texels, *rgb;
    unsigned int (*bins)[256][4], *bin, n;
    short *parent;
    byte *colors;
    unsigned int revision;
    int id, width, height, x, y, sx, sy, level, index, i;
    if (!tree || !texture || !xgl_shape_entries) return NULL;
    id = texture->gl_texturenum;
    if (id <= 0 || id > XGL_SHAPE_TEXTURES) return NULL;
    revision = XGL_TextureRevision(id);
    if (!revision) return NULL;
    entry = &xgl_shape_entries[id - 1];
    if (entry->tree == tree && entry->revision == revision) return tree->colors;
    if (entry->colors && entry->colors_tree == tree && entry->colors_revision == revision)
        return entry->colors;
    /* Another texture laid over the division: each node's texels are found
     * by going down the cuts, and colored as the division's own are. */
    texels = XGL_TextureTexels(id, &width, &height);
    if (!texels || width != tree->width || height != tree->height) return NULL;
    colors = (byte *)malloc((size_t)tree->count * 3);
    bins = (unsigned int (*)[256][4])calloc((size_t)tree->count, sizeof(*bins));
    parent = (short *)malloc((size_t)tree->count * sizeof(*parent));
    if (!colors || !bins || !parent) {
        free(colors); free(bins); free(parent);
        return NULL;
    }
    for (y = 0; y < height; ++y) {
        if ((y & 15) == 15) XGL_AudioUpdate();
        sy = (y + tree->origin[1]) % height;
        for (x = 0; x < width; ++x) {
            sx = (x + tree->origin[0]) % width;
            rgb = xgl_palette + 3 * texels[sy * width + sx];
            level = (rgb[0] * 77 + rgb[1] * 150 + rgb[2] * 29) >> 8;
            for (index = 0;;) {
                bin = bins[index][level];
                bin[0] += rgb[0]; bin[1] += rgb[1]; bin[2] += rgb[2]; ++bin[3];
                node = &tree->nodes[index];
                if (node->child[0] < 0) break;
                index = node->child[node->a * (x + .5f) + node->b * (y + .5f) >= node->d];
            }
        }
    }
    parent[0] = 0;
    for (i = 0; i < tree->count; ++i) {
        node = &tree->nodes[i];
        if (node->child[0] >= 0) parent[node->child[0]] = parent[node->child[1]] = (short)i;
        /* A node with no texel of its own is as the part it was cut from. */
        if (i) memcpy(colors + 3 * i, colors + 3 * parent[i], 3);
        else colors[0] = colors[1] = colors[2] = 0;
        n = 0;
        for (level = 0; level < 256; ++level) n += bins[i][level][3];
        XGL_ShapeTrimmedColor(bins[i], n, colors + 3 * i);
    }
    free(bins); free(parent);
    free(entry->colors);
    entry->colors = colors;
    entry->colors_tree = tree;
    entry->colors_revision = revision;
    return colors;
}

/* ---- A face cut along a division ----------------------------------------
 * The face's outline, in texture coordinates, is cut by the division's
 * lines into pieces. Along an axis on which a piece has not yet been cut,
 * the piece runs across every repeat of the texture that the face shows,
 * and a cut on that axis is made in all of them at once, with the borders
 * between them. Pieces share corners along their sides: where the corner of
 * one lies part way along the side of another, the other takes it as a
 * corner of its own, so no seam can open between their triangles. */

/* Pieces of a face while it is cut, their corners, and the cuts. */
#define XGL_CUT_ITEMS 384
#define XGL_CUT_POOL 12288
#define XGL_CUT_LINES 1024
/* Corners that lie along cuts: at most two for each corner of the pieces. */
#define XGL_CUT_ENDS (6 * XGL_SHAPE_TRIANGLES + 64)
/* The corners of the pieces as they are drawn. A piece has at most two more
 * than it has triangles. */
#define XGL_CUT_OUTLINE (3 * XGL_SHAPE_TRIANGLES + XGL_SHAPE_CORNERS)
/* A piece smaller than this many square texels stays with its neighbor. */
#define XGL_CUT_SLIVER 6.0f

typedef struct {
    float s, t;
    /* The cut along which the side from this corner to the next lies; -1
     * for a side on the face's own outline. */
    short line;
} xgl_cut_corner_t;

typedef struct {
    int first, count;
    /* Where its repeat of the texture begins, along each axis that has been
     * cut on; along one that has not, the piece runs across every repeat. */
    float base[2];
    short node;
    byte free_axis[2];
    /* Its outline as it is drawn, once the face is cut: where, how many
     * corners, and whether as a fan from its middle. */
    short outline, corners, center;
} xgl_cut_item_t;

typedef struct {
    float along, s, t;
    short line;
} xgl_cut_end_t;

static struct {
    const xgl_shape_tree_t *tree;
    xgl_cut_corner_t pool[XGL_CUT_POOL];
    xgl_cut_item_t items[XGL_CUT_ITEMS];
    /* The direction each cut runs in. */
    float line[XGL_CUT_LINES][2];
    /* The corners that lie along each cut, in order along it: those of
     * cut `n` are from line_first[n] up to line_first[n + 1]. */
    xgl_cut_end_t ends[XGL_CUT_ENDS];
    int line_first[XGL_CUT_LINES + 1];
    float outline[XGL_CUT_OUTLINE][2];
    int pool_count, item_count, line_count, end_count, outline_count;
    /* The least triangles the pieces so far can come to. */
    int least;
} xgl_cut;

/* Whether a node's cut is made. A band's two cuts are made together or not
 * at all. */
static int XGL_CutWanted (const xgl_shape_tree_t *tree, const xgl_shape_node_t *node,
                          const xgl_shape_terms_t *terms)
{
    if (node->child[0] < 0 || node->child[1] > terms->most || node->gain < terms->worth) return 0;
    return !(node->flags & XGL_SHAPE_PAIRED) || tree->nodes[node->child[1]].child[1] <= terms->most;
}

/* The node a piece goes on as when a cut it should have would leave a part
 * too narrow to see: the wider part's. */
static int XGL_CutWider (const xgl_shape_node_t *node)
{
    return node->child[(node->flags & XGL_SHAPE_THIN_FAR) ? 0 : 1];
}

int XGL_ShapeCuts (const xgl_shape_tree_t *tree, const xgl_shape_terms_t *terms)
{
    short pending[XGL_SHAPE_PIECES];
    const xgl_shape_node_t *node;
    int depth, cuts, index;
    cuts = depth = 0;
    index = 0;
    for (;;) {
        node = &tree->nodes[index];
        if (XGL_CutWanted(tree, node, terms)) {
            if (node->width < terms->thin) { index = XGL_CutWider(node); continue; }
            ++cuts;
            if (depth < XGL_SHAPE_PIECES) pending[depth++] = node->child[1];
            index = node->child[0];
            continue;
        }
        if (!depth) break;
        index = pending[--depth];
    }
    return cuts;
}

/* A new cut; its number, or -1 when there are too many. */
static int XGL_CutNewLine (float a, float b)
{
    float length = (float)sqrt(a * a + b * b);
    if (xgl_cut.line_count == XGL_CUT_LINES || !(length > 0)) return -1;
    xgl_cut.line[xgl_cut.line_count][0] = -b / length;
    xgl_cut.line[xgl_cut.line_count][1] = a / length;
    return xgl_cut.line_count++;
}

static float XGL_CutArea (int first, int count)
{
    const xgl_cut_corner_t *corner = xgl_cut.pool + first, *next;
    float sum = 0;
    int i;
    for (i = 0; i < count; ++i) {
        next = xgl_cut.pool + first + (i + 1) % count;
        sum += corner[i].s * next->t - next->s * corner[i].t;
    }
    return (float)fabs(sum) * .5f;
}

static void XGL_CutExtent (int first, int count, int axis, float *low, float *high)
{
    const xgl_cut_corner_t *corner = xgl_cut.pool + first;
    float value;
    int i;
    *low = *high = axis ? corner->t : corner->s;
    for (i = 1; i < count; ++i) {
        value = axis ? corner[i].t : corner[i].s;
        if (value < *low) *low = value;
        if (value > *high) *high = value;
    }
}

/* Both parts of a convex polygon either side of a*s + b*t = d, added to the
 * pool: part 0 where it is <= d, part 1 where it is >= d. Their sides along
 * the cut are marked with the cut's number, and both have exactly the same
 * new corners. Returns 0 when there is no room. */
static int XGL_CutSplit (int first, int count, float a, float b, float d, int line,
                         int *part, int *part_count)
{
    static xgl_cut_corner_t far_part[2 * XGL_SHAPE_CORNERS];
    static float distance[XGL_SHAPE_CORNERS];
    byte on[2][2 * XGL_SHAPE_CORNERS];
    const xgl_cut_corner_t *from, *to;
    xgl_cut_corner_t *near_part, corner;
    float da, db, f;
    int i, n[2], start;
    start = xgl_cut.pool_count;
    /* A convex polygon gains at most two corners in all; room for any. */
    if (count > XGL_SHAPE_CORNERS || start + 4 * count > XGL_CUT_POOL) return 0;
    from = xgl_cut.pool + first;
    for (i = 0; i < count; ++i, ++from) distance[i] = a * from->s + b * from->t - d;
    near_part = xgl_cut.pool + start;
    n[0] = n[1] = 0;
    for (i = 0; i < count; ++i) {
        from = xgl_cut.pool + first + i;
        da = distance[i];
        if (da <= 0) { near_part[n[0]] = *from; on[0][n[0]++] = da == 0; }
        if (da >= 0) { far_part[n[1]] = *from; on[1][n[1]++] = da == 0; }
        db = distance[i + 1 < count ? i + 1 : 0];
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
            to = xgl_cut.pool + first + (i + 1 < count ? i + 1 : 0);
            f = da / (da - db);
            corner.s = from->s + (to->s - from->s) * f;
            corner.t = from->t + (to->t - from->t) * f;
            /* A cut along an axis lands on it exactly. */
            if (b == 0) corner.s = d / a;
            if (a == 0) corner.t = d / b;
            corner.line = from->line;
            near_part[n[0]] = corner; on[0][n[0]++] = 1;
            far_part[n[1]] = corner; on[1][n[1]++] = 1;
        }
    }
    if (n[0] > XGL_SHAPE_CORNERS || n[1] > XGL_SHAPE_CORNERS) return 0;
    for (i = 0; i < n[0]; ++i)
        if (on[0][i] && on[0][i + 1 < n[0] ? i + 1 : 0]) near_part[i].line = (short)line;
    for (i = 0; i < n[1]; ++i)
        if (on[1][i] && on[1][i + 1 < n[1] ? i + 1 : 0]) far_part[i].line = (short)line;
    memcpy(near_part + n[0], far_part, (size_t)n[1] * sizeof(far_part[0]));
    part[0] = start; part[1] = start + n[0];
    part_count[0] = n[0]; part_count[1] = n[1];
    xgl_cut.pool_count = start + n[0] + n[1];
    return 1;
}

/* Cut a piece at each of a rising list of places across one axis. A part
 * too small to matter stays with its neighbor. The parts replace the piece
 * and follow it in the list; returns how many, or -1 when out of room. */
static int XGL_CutSlice (int index, int axis, const float *places, int count)
{
    xgl_cut_item_t *item = &xgl_cut.items[index], *piece;
    float a = axis ? 0.0f : 1.0f, b = axis ? 1.0f : 0.0f;
    int i, n, line, part[2], part_count[2], first, corners;
    first = item->first; corners = item->count;
    xgl_cut.least -= corners - 2;
    n = 0;
    for (i = 0; i < count; ++i) {
        line = XGL_CutNewLine(a, b);
        if (line < 0 || !XGL_CutSplit(first, corners, a, b, places[i], line, part, part_count)) return -1;
        if (part_count[0] < 3 || XGL_CutArea(part[0], part_count[0]) < XGL_CUT_SLIVER) continue;
        if (part_count[1] < 3 || XGL_CutArea(part[1], part_count[1]) < XGL_CUT_SLIVER) break;
        /* Room for this part and for the last. */
        if (xgl_cut.item_count + 2 > XGL_CUT_ITEMS) return -1;
        if (n) {
            piece = &xgl_cut.items[xgl_cut.item_count++];
            *piece = *item;
        } else piece = item;
        piece->first = part[0]; piece->count = part_count[0];
        xgl_cut.least += part_count[0] - 2;
        ++n;
        first = part[1]; corners = part_count[1];
    }
    if (n) {
        piece = &xgl_cut.items[xgl_cut.item_count++];
        *piece = *item;
    } else piece = item;
    piece->first = first; piece->count = corners;
    xgl_cut.least += corners - 2;
    return n + 1;
}

/* Make a piece's next cut. Along an axis the texture has not yet been cut
 * on, the cut repeats with the texture across the whole piece, and so does
 * the border between repeats; a tilted cut is made in one repeat, so the
 * piece is first cut free of its neighbors. Returns 0 when out of room. */
static int XGL_CutItem (int index)
{
    const xgl_shape_tree_t *tree = xgl_cut.tree;
    xgl_cut_item_t *item = &xgl_cut.items[index], *piece;
    const xgl_shape_node_t *node = &tree->nodes[item->node];
    float places[130], low, high, period, place, middle, d, s, t;
    int axis, bind, count, i, k, n, first, last, line, part[2], part_count[2], added;
    axis = node->flags & XGL_SHAPE_AXIS;
    bind = -1;
    if (axis == 2) {
        if (item->free_axis[0]) bind = 0; else if (item->free_axis[1]) bind = 1;
        if (bind < 0) {
            d = node->d + node->a * item->base[0] + node->b * item->base[1];
            line = XGL_CutNewLine(node->a, node->b);
            if (line < 0 || !XGL_CutSplit(item->first, item->count, node->a, node->b, d, line, part, part_count))
                return 0;
            if (part_count[0] < 3 || part_count[1] < 3 ||
                XGL_CutArea(part[0], part_count[0]) < XGL_CUT_SLIVER ||
                XGL_CutArea(part[1], part_count[1]) < XGL_CUT_SLIVER) {
                /* All but a sliver lies to one side: it goes there whole. */
                s = t = 0;
                for (i = 0; i < item->count; ++i) {
                    s += xgl_cut.pool[item->first + i].s; t += xgl_cut.pool[item->first + i].t;
                }
                item->node = node->child[node->a * s + node->b * t >= d * item->count];
                return 1;
            }
            if (xgl_cut.item_count == XGL_CUT_ITEMS) return 0;
            xgl_cut.least += part_count[0] + part_count[1] - item->count - 2;
            piece = &xgl_cut.items[xgl_cut.item_count++];
            *piece = *item;
            piece->first = part[1]; piece->count = part_count[1]; piece->node = node->child[1];
            item->first = part[0]; item->count = part_count[0]; item->node = node->child[0];
            return 1;
        }
        axis = bind;
    }
    period = (float)(axis ? tree->height : tree->width);
    XGL_CutExtent(item->first, item->count, axis, &low, &high);
    count = 0;
    if (item->free_axis[axis]) {
        first = (int)floor(low / period); last = (int)floor(high / period);
        for (k = first; k <= last && count < 128; ++k) {
            place = k * period;
            if (place > low && place < high) places[count++] = place;
            if (bind >= 0) continue;
            place = k * period + node->d;
            if (place > low && place < high) places[count++] = place;
        }
    } else {
        place = item->base[axis] + node->d;
        if (place > low && place < high) places[count++] = place;
    }
    added = xgl_cut.item_count;
    n = XGL_CutSlice(index, axis, places, count);
    if (n < 0) return 0;
    for (i = 0; i < n; ++i) {
        piece = i ? &xgl_cut.items[added + i - 1] : item;
        XGL_CutExtent(piece->first, piece->count, axis, &low, &high);
        middle = (low + high) * .5f;
        if (piece->free_axis[axis]) {
            piece->base[axis] = (float)floor(middle / period) * period;
            piece->free_axis[axis] = 0;
        }
        if (bind < 0) piece->node = node->child[middle - piece->base[axis] >= node->d];
    }
    return 1;
}

/* Halve the pieces that are too long for the baked light to be carried
 * across between their corners. Returns 0 when out of room. */
static int XGL_CutForLight (float longest)
{
    const xgl_cut_item_t *item;
    float low[2], high[2], middle;
    int i, axis, n;
    for (i = 0; i < xgl_cut.item_count; ++i)
        for (;;) {
            item = &xgl_cut.items[i];
            XGL_CutExtent(item->first, item->count, 0, &low[0], &high[0]);
            XGL_CutExtent(item->first, item->count, 1, &low[1], &high[1]);
            axis = high[0] - low[0] >= high[1] - low[1] ? 0 : 1;
            if (high[axis] - low[axis] <= longest) break;
            middle = (low[axis] + high[axis]) * .5f;
            n = XGL_CutSlice(i, axis, &middle, 1);
            if (n < 0) return 0;
            if (n < 2) break;
        }
    return 1;
}

/* Every corner that lies along a cut, gathered cut by cut and in order
 * along each: the corners a piece on the other side of the cut may have to
 * take. Returns 0 when there are too many. */
static int XGL_CutEnds (void)
{
    static short fill[XGL_CUT_LINES];
    const xgl_cut_item_t *item;
    const xgl_cut_corner_t *corner;
    xgl_cut_end_t *end, moved;
    int i, j, k, pass, total, line[2], first, last;
    memset(fill, 0, (size_t)xgl_cut.line_count * sizeof(fill[0]));
    total = 0;
    /* First how many lie along each cut, then each to its place. */
    for (pass = 0; pass < 2; ++pass) {
        for (i = 0; i < xgl_cut.item_count; ++i) {
            item = &xgl_cut.items[i];
            for (j = 0; j < item->count; ++j) {
                corner = xgl_cut.pool + item->first + j;
                /* It begins the side after it and ends the side before. */
                line[0] = corner->line;
                line[1] = xgl_cut.pool[item->first + (j ? j : item->count) - 1].line;
                for (k = 0; k < 2; ++k) {
                    if (line[k] < 0 || (k && line[1] == line[0])) continue;
                    if (!pass) { ++fill[line[k]]; ++total; continue; }
                    end = &xgl_cut.ends[xgl_cut.line_first[line[k]] + fill[line[k]]++];
                    end->line = (short)line[k];
                    end->s = corner->s; end->t = corner->t;
                    end->along = corner->s * xgl_cut.line[line[k]][0] + corner->t * xgl_cut.line[line[k]][1];
                }
            }
        }
        if (pass) break;
        if (total > XGL_CUT_ENDS) return 0;
        total = 0;
        for (i = 0; i < xgl_cut.line_count; ++i) {
            xgl_cut.line_first[i] = total;
            total += fill[i];
            fill[i] = 0;
        }
        xgl_cut.line_first[xgl_cut.line_count] = total;
    }
    xgl_cut.end_count = total;
    /* A cut has few corners along it: each is put in order by insertion. */
    for (i = 0; i < xgl_cut.line_count; ++i) {
        first = xgl_cut.line_first[i]; last = xgl_cut.line_first[i + 1];
        for (j = first + 1; j < last; ++j) {
            moved = xgl_cut.ends[j];
            for (k = j; k > first && xgl_cut.ends[k - 1].along > moved.along; --k)
                xgl_cut.ends[k] = xgl_cut.ends[k - 1];
            xgl_cut.ends[k] = moved;
        }
    }
    return 1;
}

static float XGL_CutTriangleArea (float (*corner)[2], int a, int b, int c)
{
    return (float)fabs((corner[b][0] - corner[a][0]) * (corner[c][1] - corner[a][1]) -
                       (corner[c][0] - corner[a][0]) * (corner[b][1] - corner[a][1])) * .5f;
}

/* Work out how a piece is drawn: its outline with the corners of other
 * pieces that lie along its sides added, so that no corner meets the middle
 * of another triangle's edge, and begun at a corner from which a fan of
 * triangles leaves none empty. The other piece's own corner is used: the
 * two then meet exactly. Returns the triangles it comes to, or -1 when
 * there is no room. */
static int XGL_CutOutline (xgl_cut_item_t *item)
{
    static float outline[XGL_SHAPE_CORNERS][2];
    const xgl_cut_corner_t *corner, *next;
    const xgl_cut_end_t *end;
    const float *direction;
    float (*out)[2];
    float from, to, low, high, last;
    int i, n, lower, upper, middle, last_end, k, step, apex, best, empty, fewest;
    n = 0;
    for (i = 0; i < item->count; ++i) {
        corner = xgl_cut.pool + item->first + i;
        next = xgl_cut.pool + item->first + (i + 1) % item->count;
        if (n == XGL_SHAPE_CORNERS) return -1;
        outline[n][0] = corner->s; outline[n][1] = corner->t; ++n;
        if (corner->line < 0) continue;
        direction = xgl_cut.line[corner->line];
        from = corner->s * direction[0] + corner->t * direction[1];
        to = next->s * direction[0] + next->t * direction[1];
        low = (from < to ? from : to) + .001f; high = (from < to ? to : from) - .001f;
        if (!(low < high)) continue;
        /* The corners along this cut that lie between its ends. */
        lower = xgl_cut.line_first[corner->line]; last_end = upper = xgl_cut.line_first[corner->line + 1];
        while (lower < upper) {
            middle = (lower + upper) / 2;
            if (xgl_cut.ends[middle].along <= low) lower = middle + 1;
            else upper = middle;
        }
        for (upper = lower; upper < last_end && xgl_cut.ends[upper].along < high; ++upper) {}
        /* In the order the side runs, each place once. */
        step = from < to ? 1 : -1;
        last = from;
        for (k = step > 0 ? lower : upper - 1; k >= lower && k < upper; k += step) {
            end = &xgl_cut.ends[k];
            if (fabs(end->along - last) < .001f) continue;
            if (n == XGL_SHAPE_CORNERS) return -1;
            outline[n][0] = end->s; outline[n][1] = end->t; ++n;
            last = end->along;
        }
    }
    /* A fan from a corner that leaves no empty triangle; from the middle
     * when every corner would leave one. */
    best = 0; fewest = n;
    for (apex = 0; apex < n && fewest; ++apex) {
        empty = 0;
        for (i = 1; i + 1 < n; ++i)
            if (XGL_CutTriangleArea(outline, apex, (apex + i) % n, (apex + i + 1) % n) < .001f) ++empty;
        if (empty < fewest) { fewest = empty; best = apex; }
    }
    if (xgl_cut.outline_count + n > XGL_CUT_OUTLINE) return -1;
    item->outline = (short)xgl_cut.outline_count;
    item->corners = (short)n;
    item->center = fewest != 0;
    out = xgl_cut.outline + xgl_cut.outline_count;
    for (i = 0; i < n; ++i) {
        k = (best + i) % n;
        out[i][0] = outline[k][0]; out[i][1] = outline[k][1];
    }
    xgl_cut.outline_count += n;
    return item->center ? n : n - 2;
}

int XGL_ShapeCutFace (const xgl_shape_tree_t *tree, const xgl_shape_terms_t *terms,
                      float (*st)[2], int count, float longest)
{
    const xgl_shape_node_t *node;
    xgl_cut_item_t *item;
    xgl_cut_corner_t *corner;
    int i, n, total;
    xgl_cut.tree = tree;
    xgl_cut.item_count = xgl_cut.line_count = xgl_cut.end_count = xgl_cut.outline_count = 0;
    if (count < 3 || count > XGL_SHAPE_CORNERS) return -1;
    xgl_cut.pool_count = count;
    corner = xgl_cut.pool;
    for (i = 0; i < count; ++i, ++corner) {
        corner->s = st[i][0] - tree->origin[0]; corner->t = st[i][1] - tree->origin[1];
        corner->line = -1;
    }
    item = &xgl_cut.items[0];
    item->first = 0; item->count = count; item->node = 0;
    item->free_axis[0] = item->free_axis[1] = 1; item->base[0] = item->base[1] = 0;
    xgl_cut.item_count = 1;
    xgl_cut.least = count - 2;
    /* Each piece in turn, and the pieces cut from it after it, as far as
     * the terms go. */
    for (i = 0; i < xgl_cut.item_count; ++i) {
        if ((i & 15) == 15) XGL_AudioUpdate();
        for (;;) {
            item = &xgl_cut.items[i];
            node = &tree->nodes[item->node];
            if (!XGL_CutWanted(tree, node, terms)) break;
            if (node->width < terms->thin) { item->node = (short)XGL_CutWider(node); continue; }
            if (!XGL_CutItem(i) || xgl_cut.least > XGL_SHAPE_TRIANGLES) {
                xgl_cut.item_count = 0;
                return -1;
            }
        }
    }
    if (xgl_cut.item_count < 2) {
        xgl_cut.item_count = 0;
        return 0;
    }
    if (XGL_CutForLight(longest) && xgl_cut.least <= XGL_SHAPE_TRIANGLES && XGL_CutEnds()) {
        /* What they do come to, with the corners they take from each
         * other. Nothing has yet been asked about the light on them. */
        total = 0;
        for (i = 0; i < xgl_cut.item_count && total >= 0; ++i) {
            if ((i & 31) == 31) XGL_AudioUpdate();
            n = XGL_CutOutline(&xgl_cut.items[i]);
            total = n < 0 || total + n > XGL_SHAPE_TRIANGLES ? -1 : total + n;
        }
    } else total = -1;
    if (total < 0) {
        xgl_cut.item_count = 0;
        return -1;
    }
    return xgl_cut.item_count;
}

int XGL_ShapePiece (int index, xgl_shape_piece_t *piece)
{
    const xgl_shape_tree_t *tree = xgl_cut.tree;
    const xgl_cut_item_t *item;
    float (*outline)[2];
    float extent[2][2], longer;
    int i;
    if (index < 0 || index >= xgl_cut.item_count) return 0;
    item = &xgl_cut.items[index];
    outline = xgl_cut.outline + item->outline;
    piece->count = item->corners;
    piece->center = item->center;
    for (i = 0; i < item->corners; ++i) {
        piece->corner[i][0] = outline[i][0] + tree->origin[0];
        piece->corner[i][1] = outline[i][1] + tree->origin[1];
    }
    piece->middle[0] = piece->middle[1] = 0;
    for (i = 0; i < item->count; ++i) {
        piece->middle[0] += xgl_cut.pool[item->first + i].s;
        piece->middle[1] += xgl_cut.pool[item->first + i].t;
    }
    piece->middle[0] = piece->middle[0] / item->count + tree->origin[0];
    piece->middle[1] = piece->middle[1] / item->count + tree->origin[1];
    XGL_CutExtent(item->first, item->count, 0, &extent[0][0], &extent[0][1]);
    XGL_CutExtent(item->first, item->count, 1, &extent[1][0], &extent[1][1]);
    longer = extent[0][1] - extent[0][0] > extent[1][1] - extent[1][0] ?
             extent[0][1] - extent[0][0] : extent[1][1] - extent[1][0];
    piece->width = longer > 0 ? XGL_CutArea(item->first, item->count) / longer : 1;
    if (piece->width < 1) piece->width = 1;
    piece->node = item->node;
    return 1;
}
