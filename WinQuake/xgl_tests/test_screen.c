/* Console coverage selection; no native device or game data required. */
#include <assert.h>
#include "../xgl_screen.c"

client_state_t cl;
client_static_t cls;
viddef_t vid;
keydest_t key_dest;
qboolean con_forcedup;
double host_frametime;
int con_notifylines;

static model_t world;
static int resize_calls, sbar_calls, console_calls, console_lines, notify_calls;
/* Quake's count of the buffers the current status bar has been drawn on. */
static int sb_updates;

void Con_CheckResize(void) { ++resize_calls; }
void Sbar_Changed(void) { ++sbar_calls; sb_updates = 0; }
void Con_DrawNotify(void) { ++notify_calls; }
void Con_DrawConsole(int lines, qboolean drawinput)
{
    assert(drawinput);
    ++console_calls;
    console_lines = lines;
}

static void reset_screen(void)
{
    memset(&cl, 0, sizeof(cl));
    memset(&cls, 0, sizeof(cls));
    memset(&vid, 0, sizeof(vid));
    cl.worldmodel = &world;
    cls.signon = SIGNONS;
    vid.width = 640;
    vid.height = 480;
    vid.numpages = 2;
    key_dest = key_game;
    con_forcedup = false;
    host_frametime = .1;
    scr_conspeed.value = 300;
    scr_con_current = scr_conlines = 0;
    scr_drawdialog = scr_drawloading = false;
    clearconsole = clearnotify = con_notifylines = 0;
    resize_calls = sbar_calls = console_calls = notify_calls = 0;
    console_lines = -1;
}

static void test_console_animation(void)
{
    reset_screen();
    key_dest = key_console;
    SCR_SetUpToDrawConsole();
    assert(resize_calls == 1 && sbar_calls == 1 && !con_forcedup);
    assert(scr_conlines == 240 && XGL_ConsoleClipLines() == 30);
    SCR_SetUpToDrawConsole();
    assert(XGL_ConsoleClipLines() == 60);
    host_frametime = 1;
    SCR_SetUpToDrawConsole();
    assert(XGL_ConsoleClipLines() == 240);

    /* Closing console remains opaque after input returns to the game. */
    key_dest = key_game;
    host_frametime = .25;
    SCR_SetUpToDrawConsole();
    assert(scr_conlines == 0 && XGL_ConsoleClipLines() == 165);
    SCR_DrawConsole();
    assert(console_calls == 1 && console_lines == 165 && notify_calls == 0);
    host_frametime = .5;
    SCR_SetUpToDrawConsole();
    assert(XGL_ConsoleClipLines() == 15);
    host_frametime = .1;
    SCR_SetUpToDrawConsole();
    assert(XGL_ConsoleClipLines() == 0);
    SCR_DrawConsole();
    assert(console_calls == 1 && notify_calls == 1);
}

static void test_console_fractional_height(void)
{
    reset_screen();
    key_dest = key_console;
    scr_conspeed.value = 0;
    scr_con_current = 123.75f;
    SCR_SetUpToDrawConsole();
    SCR_DrawConsole();
    assert(XGL_ConsoleClipLines() == 123 && console_lines == 123);

    scr_con_current = .75f;
    SCR_SetUpToDrawConsole();
    SCR_DrawConsole();
    /* Con_DrawConsole returns without drawing when its integer height is 0. */
    assert(XGL_ConsoleClipLines() == 0 && console_lines == 0);
}

static void test_console_ui_branches(void)
{
    int intermission, destination;
    reset_screen();
    scr_con_current = 123.75f;
    for (intermission = 0; intermission <= 3; ++intermission) {
        cl.intermission = intermission;
        for (destination = key_game; destination <= key_menu; ++destination) {
            key_dest = (keydest_t)destination;
            if (key_dest == key_game && (intermission == 1 || intermission == 2))
                assert(XGL_ConsoleClipLines() == 0);
            else
                assert(XGL_ConsoleClipLines() == 123);
        }
    }
    cl.intermission = 0;
    key_dest = key_game;
    scr_drawdialog = true;
    assert(XGL_ConsoleClipLines() == 0);
    scr_drawdialog = false;

    /* Loading setup returns before changing a stale console height. */
    scr_drawloading = true;
    cl.worldmodel = NULL;
    SCR_SetUpToDrawConsole();
    assert(resize_calls == 1 && scr_con_current == 123.75f);
    assert(!con_forcedup && XGL_ConsoleClipLines() == 0);
    scr_drawloading = false;

    /* Menus can add more opaque background; only claim console coverage. */
    cl.worldmodel = &world;
    key_dest = key_menu;
    assert(XGL_ConsoleClipLines() == 123);
    scr_con_current = 0;
    assert(XGL_ConsoleClipLines() == 0);
}

static void test_console_forced_full(void)
{
    reset_screen();
    cl.worldmodel = NULL;
    SCR_SetUpToDrawConsole();
    assert(con_forcedup && scr_conlines == 480);
    assert(XGL_ConsoleClipLines() == 480);
    SCR_DrawConsole();
    assert(console_lines == 480);

    reset_screen();
    cls.signon = SIGNONS - 1;
    SCR_SetUpToDrawConsole();
    assert(con_forcedup && XGL_ConsoleClipLines() == 480);
    cls.signon = SIGNONS;
    SCR_SetUpToDrawConsole();
    assert(!con_forcedup && XGL_ConsoleClipLines() == 450);
}

/* The rest of a frame, as far as the screen code can see it. The events
 * are the order in which the frame asked for things. */
static char events[256];
static int event_count, keep_request, keep_streak, bar_draws;
static void event(char code)
{
    assert(event_count + 1 < (int)sizeof(events));
    events[event_count++] = code; events[event_count] = 0;
}
cvar_t crosshair, cl_crossx, cl_crossy;
double realtime;
qboolean r_cache_thrash, con_initialized;
refdef_t r_refdef;
void Sys_Error(char *format, ...) { (void)format; abort(); }
qboolean scr_skipupdate;
void XGL_SMPUpdate(void) {}
void XGL_SetFrameKeep(int lines) { keep_request = lines; event('k'); }
void XGL_BeginFrame(void)
{
    /* The backend counts the frames running that left the rows alone. */
    keep_streak = keep_request ? keep_streak + 1 : 0;
    keep_request = 0;
    event('B');
}
int XGL_FrameKeepStreak(void) { return keep_streak; }
void XGL_SetConsoleClip(int lines) { (void)lines; }
void V_RenderView(void) { event('V'); }
void V_UpdatePalette(void) {}
double XGL_ProfileNow(void) { return 0; }
void XGL_ProfileAdd(xgl_profile_section_t section, double start) { (void)section; (void)start; }
void XGL_PolyBlend(void) {}
void XGL_DrawFlush(void) {}
void XGL_EndFrame(void) { event('E'); }
void M_Draw(void) {}
void Sbar_IntermissionOverlay(void) { event('I'); }
void Sbar_FinaleOverlay(void) {}
/* Quake's own rule: after a change, draw once for each buffer. */
void Sbar_Draw(void)
{
    if (sb_updates >= (int)vid.numpages) return;
    ++sb_updates; ++bar_draws;
    event('S');
}
void Draw_TileClear(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void Draw_FadeScreen(void) {}
void Draw_Character(int x, int y, int num) { (void)x; (void)y; (void)num; }
void Draw_Pic(int x, int y, qpic_t *pic) { (void)x; (void)y; (void)pic; }
qpic_t *Draw_CachePic(char *path) { static qpic_t pic; (void)path; return &pic; }
void Cvar_Set(char *name, char *value) { (void)name; (void)value; abort(); }
void Con_Printf(char *format, ...) { (void)format; }

static void frame(void)
{
    event_count = 0; events[0] = 0; bar_draws = 0;
    SCR_UpdateScreen();
}

/* An unchanged status bar is left where it is; whatever could have drawn
 * over its rows makes the frame clear and draw everything, as before. */
static void test_status_bar_kept(void)
{
    int i;
    reset_screen();
    scr_initialized = con_initialized = true;
    vid.width = 320; vid.height = 240;
    scr_viewsize.value = 100; scr_fov.value = 90;
    oldscreensize = 0; vid.recalc_refdef = 1;
    xgl_keepbar.value = -1;
    keep_streak = keep_request = 0; sb_updates = 0; xgl_bar_frames = 0;
    scr_centertime_off = 0;
    sbar_calls = 0;
    clearconsole = clearnotify = vid.numpages; /* No console was up lately. */
    /* The first frame works out the view, then asks for the bar's rows to
     * be left, before the clear. Both buffers still need a bar. */
    frame();
    assert(sb_lines == 48 && r_refdef.vrect.height == 192);
    assert(!strcmp(events, "kBVSE") && keep_streak == 1);
    assert(XGL_StatusBarKeepLines() == 48);
    frame();
    assert(!strcmp(events, "kBVSE") && keep_streak == 2);
    /* From the third frame the bar is not drawn... */
    for (i = 0; i < 4; ++i) {
        frame();
        assert(!strcmp(events, "kBVE") && !bar_draws);
    }
    /* It is drawn again every so often regardless, once for each buffer. */
    for (i = 6; i < XGL_KEEPBAR_REFRESH; ++i) {
        frame();
        assert(!bar_draws);
    }
    frame(); assert(bar_draws == 1);
    frame(); assert(bar_draws == 1);
    frame(); assert(!bar_draws);
    /* ...until the game says something on it changed: twice, for the two
     * buffers. */
    sb_updates = 0;
    frame(); assert(bar_draws == 1);
    frame(); assert(bar_draws == 1);
    frame(); assert(!bar_draws);
    /* A frame that cleared everything (here the console is on its way
     * down) draws the bar, and so do the two frames after it. */
    scr_con_current = 40; scr_conspeed.value = 400; host_frametime = .05;
    frame();
    assert(keep_streak == 0 && bar_draws == 1 && console_calls == 1);
    frame();
    assert(scr_con_current == 0 && keep_streak == 1 && bar_draws == 1);
    frame(); assert(keep_streak == 2 && bar_draws == 1);
    /* Quake itself asks for the bar twice more after a console; one of
     * those requests falls on a frame that would have drawn it anyway. */
    frame(); assert(keep_streak == 3 && bar_draws == 1);
    frame(); assert(keep_streak == 4 && !bar_draws);
    /* Each of these rules it out. */
    assert(XGL_StatusBarKeepLines() == 48);
    key_dest = key_menu; assert(!XGL_StatusBarKeepLines());
    key_dest = key_message; assert(!XGL_StatusBarKeepLines());
    key_dest = key_game;
    scr_drawdialog = true; assert(!XGL_StatusBarKeepLines()); scr_drawdialog = false;
    scr_drawloading = true; assert(!XGL_StatusBarKeepLines()); scr_drawloading = false;
    scr_centertime_off = .5f; assert(!XGL_StatusBarKeepLines()); scr_centertime_off = 0;
    scr_con_current = 1; assert(!XGL_StatusBarKeepLines()); scr_con_current = 0;
    cl.intermission = 1; assert(!XGL_StatusBarKeepLines()); cl.intermission = 0;
    xgl_keepbar.value = 0; assert(!XGL_StatusBarKeepLines());
    /* Left to itself it is for small windows only; 1 asks for it anyway. */
    xgl_keepbar.value = -1;
    vid.width = 400; vid.height = 298; /* 400 x 250 above the bar */
    assert(XGL_StatusBarKeepLines() == 48);
    vid.height = 300;
    assert(!XGL_StatusBarKeepLines());
    vid.width = 640; vid.height = 480;
    assert(!XGL_StatusBarKeepLines());
    xgl_keepbar.value = 1;
    assert(XGL_StatusBarKeepLines() == 48);
    /* The smaller bar is kept the same way; no bar, nothing to keep. */
    xgl_keepbar.value = -1; vid.width = 320; vid.height = 240;
    scr_viewsize.value = 110;
    frame();
    assert(sb_lines == 24 && XGL_StatusBarKeepLines() == 24);
    scr_viewsize.value = 120;
    frame();
    assert(sb_lines == 0 && !XGL_StatusBarKeepLines() && keep_streak == 0);
    /* An intermission shows no bar at all. */
    scr_viewsize.value = 100;
    frame();
    cl.intermission = 1; vid.recalc_refdef = 1;
    frame();
    assert(!strcmp(events, "kBVIE") && keep_streak == 0);
}

int main(void)
{
    test_status_bar_kept();
    test_console_animation();
    test_console_fractional_height();
    test_console_ui_branches();
    test_console_forced_full();
    puts("XGL screen: status bar keeping, console animation, integer coverage and UI exclusions passed");
    return 0;
}
