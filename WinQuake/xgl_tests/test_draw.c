/* Host-only semantic UI contract checks. No XGL font or rasterization model. */
#include <assert.h>
#include <stdarg.h>
#include "../xgl_draw.c"

typedef struct {
    int kind, color;
    float x, y, width, height;
    char text[256];
} test_op;

static test_op ops[1024];
static int num_ops, file_opens, file_reads, file_closes, file_header[2];
static struct { qpic_t pic; byte more[64*64]; } wad_storage;
#define wad_picture wad_storage.pic
/* A picture with pixels: a lamp four pixels square in the middle of
 * nothing, its third row another color. */
static struct { qpic_t pic; byte more[64]; } lamp_picture;
byte xgl_palette[768];
viddef_t vid;
void Cvar_RegisterVariable(cvar_t *variable) { (void)variable; }
static int identity_long(int value) { return value; }
int (*LittleLong)(int) = identity_long;

void Sys_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}

void S_ExtraUpdate(void) {}

void W_CleanupName(char *in, char *out)
{
    int i;
    memset(out, 0, 16);
    for (i = 0; i < 16 && in[i]; ++i)
        out[i] = in[i] >= 'A' && in[i] <= 'Z' ? in[i]+'a'-'A' : in[i];
}

void *W_GetLumpName(char *name)
{
    if (!strcmp(name,"lamp")) {
        int x, y;
        lamp_picture.pic.width = lamp_picture.pic.height = 8;
        for (y = 0; y < 8; ++y)
            for (x = 0; x < 8; ++x)
                ((byte *)&lamp_picture + 8)[y*8+x] = (byte)(x < 2 || x > 5 || y < 2 || y > 5 ? 255 : (y == 4 ? 20 : 10));
        return &lamp_picture.pic;
    }
    wad_picture.width = !strcmp(name,"backtile") ? 64 : 24;
    wad_picture.height = wad_picture.width;
    return &wad_picture;
}

int COM_OpenFile(char *name, int *handle)
{
    ++file_opens;
    *handle = 1;
    if (strstr(name,"menu.lmp")) {
        file_header[0] = 144;
        file_header[1] = !strcmp(name,"gfx/mainmenu.lmp") ? 100 : 60;
    } else if (strstr(name,"menuplyr")) {
        file_header[0] = 64;
        file_header[1] = 64;
    } else {
        file_header[0] = 80;
        file_header[1] = 32;
    }
    return 8+file_header[0]*file_header[1];
}

int Sys_FileRead(int handle, void *data, int count)
{
    assert(handle == 1 && count == 8);
    ++file_reads;
    memcpy(data,file_header,8);
    return 8;
}

void COM_CloseFile(int handle)
{
    assert(handle == 1);
    ++file_closes;
}

static test_op *next_op(int kind)
{
    test_op *op;
    assert(num_ops < (int)(sizeof(ops)/sizeof(ops[0])));
    op = &ops[num_ops++];
    memset(op,0,sizeof(*op));
    op->kind = kind;
    return op;
}

void XGL_UIText(float x, float y, const char *text, int color,
                float cell, float height)
{
    test_op *op;
    op = next_op('T');
    assert(strlen(text) < sizeof(op->text) && cell > 0 && height > 0);
    strcpy(op->text,text);
    op->x=x; op->y=y; op->width=cell; op->height=height; op->color=color;
}

void XGL_UIFill(float x, float y, float width, float height, int color, float opacity)
{
    test_op *op;
    assert(width > 0 && height > 0 && opacity >= 0 && opacity <= 1);
    op = next_op('Q');
    op->x=x; op->y=y; op->width=width; op->height=height; op->color=color;
}

void XGL_UIFillRGB(float x, float y, float width, float height, const float rgb[3], float opacity)
{
    test_op *op;
    assert(width > 0 && height > 0 && opacity == 1);
    op = next_op('P');
    op->x=x; op->y=y; op->width=width; op->height=height;
    op->color=(int)(rgb[0]*255+.5f) | (int)(rgb[1]*255+.5f) << 8 | (int)(rgb[2]*255+.5f) << 16;
}

void XGL_UIFlush(void) { (void)next_op('F'); }

static void reset_ops(void)
{
    assert(!ui_run_length && !ui_number_length);
    num_ops=0;
}

static int has_text(const char *text)
{
    int i;
    for (i=0;i<num_ops;++i)
        if (ops[i].kind=='T' && !strcmp(ops[i].text,text)) return 1;
    return 0;
}

static void test_metadata_and_menus(void)
{
    qpic_t *main_pic, *sp, *mp, *wad;
    int opens;
    main_pic=Draw_CachePic("gfx/mainmenu.lmp");
    assert(main_pic->width==144 && main_pic->height==100);
    opens=file_opens;
    assert(Draw_CachePic("gfx/mainmenu.lmp")==main_pic && file_opens==opens);
    assert(file_opens==file_reads && file_reads==file_closes);
    wad=Draw_PicFromWad("NUM_0");
    assert(wad->width==24 && wad->height==24 && Draw_PicFromWad("num_0")==wad);
    reset_ops();
    Draw_TransPic(72,32,main_pic);
    assert(num_ops==5 && has_text("SINGLE PLAYER") && has_text("MULTIPLAYER"));
    assert(has_text("OPTIONS") && has_text("HELP") && has_text("QUIT"));
    assert(ops[0].x==72 && ops[0].y==35 && ops[4].y==115);
    sp=Draw_CachePic("gfx/sp_menu.lmp");
    reset_ops();
    Draw_Pic(72,32,sp);
    assert(num_ops==3 && has_text("NEW GAME") && has_text("LOAD") && has_text("SAVE"));
    mp=Draw_CachePic("gfx/mp_menu.lmp");
    reset_ops();
    Draw_Pic(72,32,mp);
    assert(num_ops==3 && has_text("JOIN GAME") && has_text("NEW GAME") && has_text("SETUP"));
}

static void test_cells_and_order(void)
{
    int i;
    reset_ops();
    Draw_Character(0,-4,'A');
    Draw_Character(8,-4,'B');
    assert(num_ops==0);
    Draw_Fill(0,10,10,4,7);
    assert(num_ops==2 && ops[0].kind=='T' && !strcmp(ops[0].text,"AB"));
    assert(ops[0].y==-4 && ops[0].width==8 && ops[0].color==UI_WHITE);
    assert(ops[1].kind=='Q');
    Draw_Character(16,16,'C'+128);
    Draw_Character(24,16,'D'+128);
    Draw_Character(32,16,'E');
    assert(ops[2].kind=='T' && !strcmp(ops[2].text,"CD") && ops[2].color==UI_GOLD);
    XGL_DrawFlush();
    assert(num_ops==5 && !strcmp(ops[3].text,"E") && ops[3].x==32 && ops[4].kind=='F');
    reset_ops();
    for(i=0;i<4;++i) Draw_Character(i*8,32,128+i);
    assert(num_ops>0);
    for(i=0;i<num_ops;++i) assert(ops[i].kind=='Q');
    reset_ops();
    Draw_Character(0,-8,'X');
    Draw_Character(320,0,'X');
    assert(!num_ops && !ui_run_length);
}

static void test_pictures_and_translation(void)
{
    byte translation[256];
    qpic_t *pic;
    int i, shirt, pants;
    for(i=0;i<256;++i) translation[i]=(byte)i;
    translation[TOP_RANGE+8]=42;
    translation[BOTTOM_RANGE+8]=73;
    pic=Draw_CachePic("gfx/menuplyr.lmp");
    reset_ops();
    Draw_TransPicTranslate(10,20,pic,translation);
    shirt=pants=0;
    for(i=0;i<num_ops;++i) {
        assert(ops[i].kind=='Q');
        if(ops[i].color==42) shirt=1;
        if(ops[i].color==73) pants=1;
    }
    assert(shirt && pants && pic->width==64 && pic->height==64);
    reset_ops();
    Draw_Pic(0,0,Draw_PicFromWad("num_colon"));
    assert(has_text(":"));
    Draw_Pic(24,0,Draw_PicFromWad("num_slash"));
    assert(has_text("/"));
    Draw_Pic(48,0,Draw_PicFromWad("anum_7"));
    assert(!has_text("7")); /* A digit waits for the rest of its number. */
    XGL_DrawFlush();
    assert(has_text("7"));
    reset_ops();
    Draw_Pic(0,0,Draw_PicFromWad("inv_shotgun"));
    assert(has_text("SG") && ops[num_ops-1].color==8);
    reset_ops();
    Draw_Pic(0,0,Draw_PicFromWad("inv2_shotgun"));
    assert(has_text("SG") && ops[num_ops-1].color==UI_GOLD);
    reset_ops();
    Draw_Pic(0,0,Draw_PicFromWad("inva1_shotgun"));
    assert(has_text("SG") && ops[num_ops-1].color==UI_WHITE);
    reset_ops();
    Draw_Pic(0,0,Draw_PicFromWad("inva2_shotgun"));
    assert(has_text("SG") && ops[num_ops-1].color==UI_GOLD);
    reset_ops();
    Draw_Pic(0,0,Draw_PicFromWad("face_inv2"));
    shirt=pants=0;
    for(i=0;i<num_ops;++i) {
        if(ops[i].color==UI_WHITE) shirt=1;
        if(ops[i].color==UI_GOLD) pants=1;
    }
    assert(shirt && pants);
    pic=Draw_CachePic("gfx/custom_mod.lmp");
    reset_ops();
    Draw_Pic(0,0,pic);
    assert(num_ops>1 && has_text("CUSTOM MOD"));
    reset_ops();
    Draw_BeginDisc();
    assert(num_ops==0);
    XGL_DrawFlush();
    assert(has_text("IO") && ops[num_ops-1].kind=='F');
    Draw_EndDisc();
    reset_ops();
    XGL_DrawFlush();
    assert(num_ops==1 && ops[0].kind=='F');
}

static void test_number_runs(void)
{
    qpic_t *digits[10], *alternate, *minus;
    char name[16];
    int i;
    for (i = 0; i < 10; ++i) {
        sprintf(name, "num_%d", i);
        digits[i] = Draw_PicFromWad(name);
    }
    alternate = Draw_PicFromWad("anum_3");
    minus = Draw_PicFromWad("num_minus");
    /* Sbar_DrawNum sends "100" as three 24-pixel pictures. They become one
     * native text call, centred over the cells the digits occupy. */
    reset_ops();
    Draw_TransPic(24,0,digits[1]);
    Draw_TransPic(48,0,digits[0]);
    Draw_TransPic(72,0,digits[0]);
    assert(num_ops==0);
    /* A gap, another colour or another row starts a new number. */
    Draw_TransPic(120,0,digits[5]);
    assert(num_ops==1 && ops[0].kind=='T' && !strcmp(ops[0].text,"100"));
    assert(ops[0].width==10 && ops[0].height==16 && ops[0].color==UI_WHITE);
    assert(ops[0].x==24+(72-30)*.5f && ops[0].y==4);
    Draw_TransPic(144,0,alternate);
    assert(num_ops==2 && !strcmp(ops[1].text,"5") && ops[1].x==120+(24-10)*.5f);
    Draw_TransPic(168,24,digits[9]);
    assert(num_ops==3 && !strcmp(ops[2].text,"3") && ops[2].color==251);
    /* Any other drawing ends the number first, keeping painter's order. */
    Draw_Fill(0,100,8,8,7);
    assert(num_ops==5 && ops[3].kind=='T' && !strcmp(ops[3].text,"9") && ops[4].kind=='Q');
    Draw_TransPic(0,48,minus);
    Draw_TransPic(24,48,digits[7]);
    Draw_Character(100,48,'A');
    assert(num_ops==6 && !strcmp(ops[5].text,"-7") && ui_run_length==1);
    Draw_TransPic(0,72,digits[4]);
    assert(num_ops==7 && !strcmp(ops[6].text,"A") && ui_number_length==1);
    XGL_DrawFlush();
    assert(num_ops==9 && !strcmp(ops[7].text,"4") && ops[8].kind=='F');
    /* Longer than the buffer: split, never overrun. */
    reset_ops();
    for (i = 0; i < XGL_DRAW_NUMBER + 2; ++i) Draw_TransPic(i*24,0,digits[i%10]);
    XGL_DrawFlush();
    assert(num_ops==3 && strlen(ops[0].text)==XGL_DRAW_NUMBER && !strcmp(ops[1].text,"23"));
}

/* A picture is told apart by name once, when first asked for; drawing it
 * looks nothing up. Boxes with an edge take two rectangles. */
static void test_descriptors_and_panels(void)
{
    static qpic_t foreign;
    xgl_draw_pic *asset;
    qpic_t *bar, *weapon, *tiny;
    int i;
    bar = Draw_PicFromWad("sbar");
    asset = ui_find_pic(bar);
    assert(asset && &asset->pic == bar && asset->kind == UI_PIC_PANEL);
    weapon = Draw_PicFromWad("inva3_lightng");
    asset = ui_find_pic(weapon);
    assert(asset->kind == UI_PIC_WEAPON && asset->color == UI_WHITE && !strcmp(asset->label, "LG"));
    assert(ui_find_pic(Draw_PicFromWad("face3"))->detail == 3);
    assert(ui_find_pic(Draw_PicFromWad("face_quad"))->detail == UI_FACE_QUAD);
    assert(ui_find_pic(Draw_PicFromWad("box_tl"))->detail == (UI_BOX_TOP | UI_BOX_LEFT));
    assert(ui_find_pic(Draw_PicFromWad("box_br"))->detail == (UI_BOX_BOTTOM | UI_BOX_RIGHT));
    assert(ui_find_pic(Draw_PicFromWad("box_mm"))->detail == 0);
    assert(ui_find_pic(Draw_PicFromWad("num_minus"))->kind == UI_PIC_DIGIT);
    assert(ui_find_pic(Draw_PicFromWad("num_12"))->kind == UI_PIC_LABEL);
    assert(ui_find_pic(Draw_PicFromWad("help3"))->detail == 3);
    assert(ui_find_pic(Draw_CachePic("gfx/unheard_of.lmp"))->kind == UI_PIC_OTHER);
    /* Only the handles given out are descriptors. */
    assert(!ui_find_pic(&foreign) && !ui_find_pic(NULL));
    assert(!ui_find_pic((qpic_t *)((byte *)bar + 4)));
    assert(!ui_find_pic(&ui_pics[ui_num_pics].pic));
    foreign.width = 40; foreign.height = 16;
    reset_ops();
    Draw_Pic(0,0,&foreign);
    assert(has_text("PICTURE"));
    /* Edge color under the whole box, the fill over its inside. */
    reset_ops();
    wad_picture.width = 24;
    Draw_Pic(10,20,bar);
    assert(num_ops==2 && ops[0].kind=='Q' && ops[1].kind=='Q');
    assert(ops[0].x==10 && ops[0].y==20 && ops[0].width==24 && ops[0].height==24);
    assert(ops[0].color==UI_EDGE);
    assert(ops[1].x==11 && ops[1].y==21 && ops[1].width==22 && ops[1].height==22);
    assert(ops[1].color==UI_DARK);
    reset_ops();
    Draw_Pic(0,0,weapon);
    assert(num_ops==3 && ops[0].color==UI_WHITE && ops[1].color==UI_DARK);
    assert(ops[2].kind=='T' && !strcmp(ops[2].text,"LG") && ops[2].color==UI_WHITE);
    /* Too small to have an inside: the edge is all there is. */
    tiny = Draw_CachePic("gfx/ibar.lmp");
    tiny->width = tiny->height = 2;
    reset_ops();
    Draw_Pic(5,5,tiny);
    assert(num_ops==1 && ops[0].color==UI_EDGE && ops[0].width==2 && ops[0].height==2);
    /* The status bar's pictures in one go: the same requests each time. */
    reset_ops();
    for (i = 0; i < 2; ++i) {
        Draw_Pic(0,216,bar);
        Draw_Pic(24,216,Draw_PicFromWad("num_1"));
        Draw_Pic(48,216,Draw_PicFromWad("num_0"));
        Draw_Pic(112,216,Draw_PicFromWad("face1"));
        XGL_DrawFlush();
        if (!i) { assert(num_ops==9); reset_ops(); }
    }
    assert(num_ops==9 && ops[2].kind=='T' && !strcmp(ops[2].text,"10"));
}

/* A picture of the status bar's kind is drawn from its own pixels: in
 * rectangles that cover just what is not transparent, each in the mean
 * color of what it covers, however the picture is named. */
static void test_pictures_from_pixels(void)
{
    qpic_t *lamp, *digit;
    int i, x, y, covered[64], color;
    for (i = 0; i < 256; ++i) {
        xgl_palette[3*i] = (byte)i; xgl_palette[3*i+1] = (byte)(255-i); xgl_palette[3*i+2] = 7;
    }
    lamp = Draw_PicFromWad("lamp");
    assert(lamp->width == 8 && lamp->height == 8);
    /* Switched off, it is the stand-in: a box with its name. */
    reset_ops();
    Draw_Pic(100,50,lamp);
    assert(has_text("L") || has_text("LA") || has_text("LAMP") || num_ops >= 2);
    for (i = 0; i < num_ops; ++i) assert(ops[i].kind != 'P');
    xgl_hudpieces.value = 16;
    reset_ops();
    Draw_Pic(100,50,lamp);
    memset(covered, 0, sizeof(covered));
    assert(num_ops == 3);
    for (i = 0; i < num_ops; ++i) {
        assert(ops[i].kind == 'P');
        color = ops[i].y == 54 ? 20 : 10;
        assert(ops[i].color == (color | (255-color) << 8 | 7 << 16));
        for (y = (int)ops[i].y - 50; y < (int)(ops[i].y + ops[i].height) - 50; ++y)
            for (x = (int)ops[i].x - 100; x < (int)(ops[i].x + ops[i].width) - 100; ++x) ++covered[y*8+x];
    }
    for (i = 0; i < 64; ++i) assert(covered[i] == (((const byte *)&lamp_picture + 8)[i] != 255));
    /* Allowed one color, it keeps its outline: one rectangle, the mean of
     * twelve pixels of one color and four of the other. */
    xgl_hudpieces.value = 1;
    reset_ops();
    Draw_Pic(0,0,lamp);
    assert(num_ops == 1 && ops[0].kind == 'P' && ops[0].x == 2 && ops[0].y == 2);
    assert(ops[0].width == 4 && ops[0].height == 4 && (ops[0].color & 255) == 13);
    /* A digit is its picture too, here all of one color, and no text. */
    xgl_hudpieces.value = 16;
    digit = Draw_PicFromWad("num_5");
    reset_ops();
    Draw_TransPic(0,0,digit);
    XGL_DrawFlush();
    assert(num_ops == 2 && ops[0].kind == 'P' && ops[0].width == 24 && ops[0].height == 24);
    xgl_hudpieces.value = 0;
    reset_ops();
}

int main(void)
{
    memset(&vid,0,sizeof(vid));
    vid.width=vid.conwidth=320;
    vid.height=vid.conheight=240;
    /* Null pixel buffers intentionally prove these adapters never touch a
     * software framebuffer. Native calls are recorded, not rendered. */
    Draw_Init();
    assert(draw_disc->width==24 && draw_backtile->width==64);
    test_metadata_and_menus();
    test_cells_and_order();
    test_pictures_and_translation();
    test_number_runs();
    test_descriptors_and_panels();
    test_pictures_from_pixels();
    puts("XGL UI adapter semantic tests passed (no device rendering)");
    return 0;
}
