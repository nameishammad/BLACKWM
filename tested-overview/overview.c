#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasestr */
#include <math.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>

#ifdef USE_XFT
#include <X11/Xft/Xft.h>
#endif

#define MAX_WINDOWS 64

typedef struct {
    Window      win;
    Pixmap      pixmap;
    Picture     picture;
    int         w, h;

    /* window ki asal (real) screen geometry - open/close animation yahin se
     * shuru/khatam hoti hai */
    double      ox, oy, ow, oh;

    /* target (dst_*) aur abhi ki animated value (curr_*) */
    double      dst_x, dst_y, dst_w, dst_h, dst_rot, dst_shade, dst_glow, dst_alpha;
    double      curr_x, curr_y, curr_w, curr_h, curr_rot, curr_shade, curr_glow, curr_alpha;

    double      quad[4][2];   /* last render ka projected quad (hit-test ke liye) */
    int         quad_valid;

    char        title[128];
    Pixmap      icon_pm;
    Picture     icon_pic;
    int         icon_w, icon_h;
    XRenderPictFormat *fmt;   /* cached: refresh me baar baar server roundtrip nahi */
} ClientWin;

static Display *dpy;
static int       scr;
static Window    root;
static Window    overlay = 0;
static Picture   overlay_pic = 0;
static Picture   wallpaper_pic = 0;
static int       screen_w, screen_h;

static ClientWin clients[MAX_WINDOWS];
static int        nclients = 0;
static int        selected = 0;

static Atom NET_CLIENT_LIST, NET_ACTIVE_WINDOW;
static Atom XROOTPMAP_ID, ESETROOT_PMAP_ID;


/* ---------------- Helpers ---------------- */

static int x_error_handler(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    return 0;
}

static Atom intern(const char *name) { return XInternAtom(dpy, name, False); }

static Window get_currently_focused_window(void) {
    Window focused = None;

    Atom actual_type; int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, root, NET_ACTIVE_WINDOW, 0, 1,
        False, XA_WINDOW, &actual_type, &actual_format,
        &nitems, &bytes_after, &prop) == Success && prop) {
        if (nitems > 0) focused = *(Window *)prop;
        XFree(prop);
        }

        if (focused == None || focused == PointerRoot || focused == root) {
            int revert_to;
            XGetInputFocus(dpy, &focused, &revert_to);
        }

        return focused;
}

static int get_client_list(Window **out) {
    Atom actual_type; int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    int status = XGetWindowProperty(dpy, root, NET_CLIENT_LIST, 0, MAX_WINDOWS,
                                    False, XA_WINDOW, &actual_type, &actual_format,
                                    &nitems, &bytes_after, &prop);

    if (status != Success || !prop) { *out = NULL; return 0; }
    *out = (Window *)prop;
    return (int)nitems;
}

static int window_is_viewable(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return 0;
    return wa.map_state == IsViewable;
}

/* xwinwrap (aur is jaise live-wallpaper tools) root pixmap wali convention
 * (_XROOTPMAP_ID / ESETROOT_PMAP_ID) follow nahi karte - wo apni khud ki
 * window me directly video/animation render karte hain, root pe kuch bhi
 * "advertise" nahi karte. Isliye root pixmap dhoondne ke bajaye seedha
 * xwinwrap ki window WM_CLASS se dhoondo aur usay normal client window ki
 * tarah live composite se capture karo. */
static Window find_wallpaper_window(void) {
    Window dummy_root, dummy_parent, *children = NULL;
    unsigned int nchildren = 0;
    Window found = None;

    if (!XQueryTree(dpy, root, &dummy_root, &dummy_parent, &children, &nchildren))
        return None;

    /* Pass 1: class-hint match (normal xwinwrap window, non -ov mode) */
    for (unsigned int i = 0; i < nchildren && found == None; i++) {
        XClassHint ch;
        if (XGetClassHint(dpy, children[i], &ch)) {
            if ((ch.res_name  && strcasestr(ch.res_name,  "xwinwrap")) ||
                (ch.res_class && strcasestr(ch.res_class, "xwinwrap"))) {
                found = children[i];
                }
                if (ch.res_name)  XFree(ch.res_name);
                if (ch.res_class) XFree(ch.res_class);
        }
    }

    /* Pass 2 (fallback): agar -ov (Composite Overlay Window) mode use ho raha
     * hai to xwinwrap ki actual render window ka koi WM_CLASS nahi hota, Pass 1
     * kabhi match nahi karega. Us surat me sabse neeche (bottommost - XQueryTree
     * children bottom-to-top order me aate hain) jo bhi fullscreen,
     * override-redirect, viewable window mile usay wallpaper maan lo. */
    if (found == None) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (!XGetWindowAttributes(dpy, children[i], &wa)) continue;
            if (wa.map_state != IsViewable) continue;
            if (!wa.override_redirect) continue;
            if (wa.width < screen_w - 2 || wa.height < screen_h - 2) continue;
            found = children[i];
            break; /* pehla match hi bottommost hoga (bottom-to-top order) */
        }
    }

    if (found != None) {
        XClassHint dbg;
        if (XGetClassHint(dpy, found, &dbg)) {
            fprintf(stderr, "overview: wallpaper window mila -> 0x%lx (class: %s/%s)\n",
                    found, dbg.res_name ? dbg.res_name : "?", dbg.res_class ? dbg.res_class : "?");
            if (dbg.res_name)  XFree(dbg.res_name);
            if (dbg.res_class) XFree(dbg.res_class);
        } else {
            fprintf(stderr, "overview: wallpaper window mila -> 0x%lx (no WM_CLASS, fallback match)\n", found);
        }
    } else {
        fprintf(stderr, "overview: wallpaper window NAHI mila (na class match, na fullscreen override-redirect window)\n");
    }

    if (children) XFree(children);
    return found;
}

static Window   wallpaper_win = 0;
static Pixmap   wallpaper_pm  = 0;
/* feh/nitrogen/xsetroot/hsetroot jaise static-wallpaper tools jo pixmap
 * root window par _XROOTPMAP_ID / ESETROOT_PMAP_ID property se "share" karte
 * hain (XSetCloseDownMode RetainPermanent), wo pixmap unki apni property hai -
 * hum sirf ID padh rahe hain, humne khud CreatePixmap nahi kiya. Isliye us
 * pixmap ko kabhi XFreePixmap NAHI karna - warna user ka actual desktop
 * wallpaper hi destroy ho jayega. Ye flag batata hai ki wallpaper_pm hamara
 * apna banaya hua hai (xwinwrap composite capture - jo humein free karna hai)
 * ya sirf reference hai (static root pixmap - jo humein free nahi karna). */
static int      wallpaper_pm_owned = 0;

/* xwinwrap jaisa live wallpaper na mile to static root-pixmap wallpaper
 * dhoondo - ye woh convention hai jo feh/nitrogen/xsetroot/hsetroot follow
 * karte hain: apna pixmap XSetCloseDownMode(RetainPermanent) se banate hain
 * aur root window ki _XROOTPMAP_ID (aur legacy ESETROOT_PMAP_ID) property
 * me uski ID likh dete hain. */
static Pixmap get_root_pixmap(void) {
    Atom actual_type; int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    Pixmap pm = None;

    if (XGetWindowProperty(dpy, root, XROOTPMAP_ID, 0, 1, False, XA_PIXMAP,
        &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
        if (nitems > 0) pm = *(Pixmap *)prop;
        XFree(prop);
        }

        if (pm == None) {
            prop = NULL;
            if (XGetWindowProperty(dpy, root, ESETROOT_PMAP_ID, 0, 1, False, XA_PIXMAP,
                &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
                if (nitems > 0) pm = *(Pixmap *)prop;
                XFree(prop);
                }
        }

        return pm;
}

static void grab_wallpaper_picture(void) {
    /* Diagnostic/stability fix: do not capture the live xwinwrap/mpv window.
     * GPU/Present-backed wallpaper capture can show corrupted vertical strips
     * in the overview. Use the normal root wallpaper pixmap fallback instead. */
    wallpaper_win = None;

    if (wallpaper_win != None) {
        XWindowAttributes wa;
        if (XGetWindowAttributes(dpy, wallpaper_win, &wa)) {
            Pixmap pm = XCompositeNameWindowPixmap(dpy, wallpaper_win);
            if (pm) {
                XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, wa.visual);
                if (fmt) {
                    XRenderPictureAttributes pa; memset(&pa, 0, sizeof(pa));
                    Picture pic = XRenderCreatePicture(dpy, pm, fmt, 0, &pa);
                    if (pic) {
                        wallpaper_pm  = pm;
                        wallpaper_pic = pic;
                        wallpaper_pm_owned = 1;
                        fprintf(stderr, "overview: live wallpaper (xwinwrap) capture ho gaya\n");
                        return;
                    }
                }
                XFreePixmap(dpy, pm);
            }
        }
        wallpaper_win = None;
    }

    /* Fallback: static wallpaper (feh/nitrogen/xsetroot/hsetroot) */
    Pixmap root_pm = get_root_pixmap();
    if (root_pm == None) {
        fprintf(stderr, "overview: na live wallpaper mila, na static root pixmap - background plain rahega\n");
        return;
    }

    XWindowAttributes rwa;
    if (!XGetWindowAttributes(dpy, root, &rwa)) return;

    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, rwa.visual);
    if (!fmt) return;

    XRenderPictureAttributes pa; memset(&pa, 0, sizeof(pa));
    Picture pic = XRenderCreatePicture(dpy, root_pm, fmt, 0, &pa);
    if (!pic) return;

    wallpaper_pm  = root_pm;
    wallpaper_pic = pic;
    wallpaper_pm_owned = 0; /* shared pixmap - hum owner nahi, isay free nahi karna */
    fprintf(stderr, "overview: static root-pixmap wallpaper mila (0x%lx)\n", root_pm);
}

/* Live wallpaper hai to har idle tick pe re-name karo, warna refresh_client_picture
 * jaisa hi freeze ho jayegi (Present/GPU flip stale pixmap). */
static void refresh_wallpaper_picture(void) {
    if (wallpaper_win == None) return;

    Pixmap new_pm = XCompositeNameWindowPixmap(dpy, wallpaper_win);
    if (!new_pm) return;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, wallpaper_win, &wa)) { XFreePixmap(dpy, new_pm); return; }

    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, wa.visual);
    if (!fmt) { XFreePixmap(dpy, new_pm); return; }

    XRenderPictureAttributes pa; memset(&pa, 0, sizeof(pa));
    Picture new_pic = XRenderCreatePicture(dpy, new_pm, fmt, 0, &pa);
    if (!new_pic) { XFreePixmap(dpy, new_pm); return; }

    Picture old_pic = wallpaper_pic;
    Pixmap  old_pm  = wallpaper_pm;

    wallpaper_pm  = new_pm;
    wallpaper_pic = new_pic;

    if (old_pic) XRenderFreePicture(dpy, old_pic);
    if (old_pm)  XFreePixmap(dpy, old_pm);
}

static int grab_window_picture(ClientWin *c) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, c->win, &wa)) return 0;
    c->w = wa.width; c->h = wa.height;
    if (c->w <= 10 || c->h <= 10) return 0;

    c->pixmap = XCompositeNameWindowPixmap(dpy, c->win);
    if (!c->pixmap) return 0;

    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, wa.visual);
    if (!fmt) {
        XFreePixmap(dpy, c->pixmap);
        c->pixmap = 0;
        return 0;
    }

    XRenderPictureAttributes pa;
    pa.subwindow_mode = IncludeInferiors;
    c->fmt = fmt;
    c->picture = XRenderCreatePicture(dpy, c->pixmap, fmt, CPSubwindowMode, &pa);
    return c->picture != 0;
}

static void free_client(ClientWin *c) {
    if (c->icon_pic) { XRenderFreePicture(dpy, c->icon_pic); c->icon_pic = 0; }
    if (c->icon_pm)  { XFreePixmap(dpy, c->icon_pm); c->icon_pm = 0; }
    if (c->picture) { XRenderFreePicture(dpy, c->picture); c->picture = 0; }
    if (c->pixmap)  { XFreePixmap(dpy, c->pixmap); c->pixmap = 0; }
}

static void refresh_client_picture(ClientWin *c) {
    /* GPU windows (mpv waghera) buffer flip karti hain, isliye pixmap dobara
     * "name" karna parta hai. Lekin XGetWindowAttributes / FindVisualFormat
     * jaise sync roundtrips yahan se hata diye - format cached hai, aur size
     * XGetGeometry se (sirf ek roundtrip) lete hain. */
    if (!c->fmt) return;

    Pixmap new_pm = XCompositeNameWindowPixmap(dpy, c->win);
    if (!new_pm) return;

    Window rr; int gx, gy; unsigned int gw, gh, gb, gd;
    if (!XGetGeometry(dpy, new_pm, &rr, &gx, &gy, &gw, &gh, &gb, &gd) ||
        gw <= 10 || gh <= 10) {
        XFreePixmap(dpy, new_pm);
        return;
    }

    XRenderPictureAttributes pa;
    pa.subwindow_mode = IncludeInferiors;
    Picture new_pic = XRenderCreatePicture(dpy, new_pm, c->fmt, CPSubwindowMode, &pa);
    if (!new_pic) {
        XFreePixmap(dpy, new_pm);
        return;
    }

    Picture old_pic = c->picture;
    Pixmap old_pm  = c->pixmap;

    c->pixmap  = new_pm;
    c->picture = new_pic;
    c->w = (int)gw;
    c->h = (int)gh;

    if (old_pic) XRenderFreePicture(dpy, old_pic);
    if (old_pm)  XFreePixmap(dpy, old_pm);
}

/* =====================================================================
 *  Coverflow style look + open/close animations
 * ===================================================================== */

#define CR          14          /* card corner radius (px) */
#define BT          2           /* border thickness (px) */
#define GLOW_G      40          /* glow kitni door tak phailti hai */
#define GLOW_RC     (CR + 2)
#define GLOW_Q      (GLOW_RC + GLOW_G)
#define ROT_MAX     0.62        /* side cards ka yaw angle (radians, ~36 deg) */
#define TMP_W       640
#define TMP_H       256
#define MAX_AUX     64
#define PAD         4           /* card surface ke gird transparent padding */

static Visual   *ov_visual32 = NULL;
static Colormap  ov_cmap = 0;

static Pixmap  back_pm, surf_pm, refl_pm, tmp_pm, amask_pm, col_pm, vig_pm;
static Picture back_pic, surf_pic, refl_pic, tmp_pic, amask_pic, col_pic, vig_pic;
static Picture cut_pic[4], ring_pic[4], glow_corner[4], glow_edge[4];
static Pixmap  aux_pm[MAX_AUX];
static Picture aux_pic[MAX_AUX];
static int     naux = 0;
static int     gfx_ready = 0;

static Atom NET_WM_WINDOW_OPACITY;

static double ui_p = 0.0;       /* 0 = band, 1 = poora open (master animation) */
static double ui_target = 1.0;
static double last_opacity = -1.0;
static int    g_closing = 0;   /* close animation chal rahi hai */

#ifdef USE_XFT
static XftDraw *xft_draw = NULL;
static XftFont *font_label = NULL, *font_big = NULL, *font_small = NULL;
#endif

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* XRender premultiplied colour chahta hai */
static XRenderColor mkcolor(double r, double g, double b, double a) {
    XRenderColor c;
    a = clampd(a, 0, 1);
    c.red   = (unsigned short)(clampd(r, 0, 1) * a * 65535);
    c.green = (unsigned short)(clampd(g, 0, 1) * a * 65535);
    c.blue  = (unsigned short)(clampd(b, 0, 1) * a * 65535);
    c.alpha = (unsigned short)(a * 65535);
    return c;
}

/* ---------------- Small GPU-side resources ---------------- */

/* tightly packed pixel data ko server pixmap me upload karo (depth 8 ya 32) */
static Picture upload_image(const void *data, int w, int h, int depth,
                            int repeat, Pixmap *pm_out) {
    int bpp = (depth == 8) ? 8 : 32;
    int stride = ((w * bpp + 31) / 32) * 4;
    int row = w * bpp / 8;
    unsigned char *buf = calloc((size_t)stride, (size_t)h);
    for (int y = 0; y < h; y++)
        memcpy(buf + (size_t)y * stride, (const unsigned char *)data + (size_t)y * row, row);

    Pixmap pm = XCreatePixmap(dpy, root, w, h, depth);
    XRenderPictFormat *fmt = XRenderFindStandardFormat(dpy,
        depth == 8 ? PictStandardA8 : PictStandardARGB32);
    XRenderPictureAttributes pa; memset(&pa, 0, sizeof(pa));
    pa.repeat = repeat ? RepeatNormal : RepeatNone;
    Picture p = XRenderCreatePicture(dpy, pm, fmt, CPRepeat, &pa);

    GC gc = XCreateGC(dpy, pm, 0, NULL);
    XImage *img = XCreateImage(dpy, DefaultVisual(dpy, scr), depth, ZPixmap, 0,
                               (char *)buf, w, h, 32, stride);
    if (img) {
        XPutImage(dpy, pm, gc, img, 0, 0, 0, 0, w, h);
        img->data = NULL;
        XDestroyImage(img);
    }
    XFreeGC(dpy, gc);
    free(buf);
    if (pm_out) *pm_out = pm;
    return p;
}

static Picture upload_aux(const void *data, int w, int h, int depth, int repeat) {
    Pixmap pm = 0;
    Picture p = upload_image(data, w, h, depth, repeat, &pm);
    if (naux < MAX_AUX) { aux_pm[naux] = pm; aux_pic[naux] = p; naux++; }
    return p;
}

static Picture make_corner(int corner, int ring) {
    unsigned char *buf = malloc(CR * CR);
    int left = (corner == 0 || corner == 3);
    int top  = (corner == 0 || corner == 1);
    double cx = left ? CR : 0, cy = top ? CR : 0;
    for (int py = 0; py < CR; py++) {
        for (int px = 0; px < CR; px++) {
            double d = hypot(px + 0.5 - cx, py + 0.5 - cy);
            double cov = clampd(CR - d + 0.5, 0, 1);
            double v = ring ? cov * (1.0 - clampd(CR - BT - d + 0.5, 0, 1)) : 1.0 - cov;
            buf[py * CR + px] = (unsigned char)(v * 255.0 + 0.5);
        }
    }
    Picture p = upload_aux(buf, CR, CR, 8, 0);
    free(buf);
    return p;
}

/* Soft cyan glow ka texture - 9-slice (4 corners + 4 repeat strips) */
static void build_glow(void) {
    int N = 2 * GLOW_Q;
    uint32_t *tex = malloc((size_t)N * N * 4);
    double sigma = GLOW_G * 0.42;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            double rx = fabs(x + 0.5 - GLOW_Q), ry = fabs(y + 0.5 - GLOW_Q);
            double qx = rx - (GLOW_RC - CR), qy = ry - (GLOW_RC - CR);
            double d = hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - CR;
            double a = (d <= 0) ? 0.9 : 0.9 * exp(-d * d / (2 * sigma * sigma));
            a *= clampd((GLOW_G - d) / 10.0, 0, 1);
            unsigned A = (unsigned)(a * 255 + 0.5);
            unsigned R = (unsigned)(0.10 * a * 255 + 0.5);
            unsigned G = (unsigned)(0.85 * a * 255 + 0.5);
            unsigned B = (unsigned)(1.00 * a * 255 + 0.5);
            tex[y * N + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }

    uint32_t *piece = malloc((size_t)GLOW_Q * GLOW_Q * 4);
    int ox[4] = { 0, GLOW_Q, GLOW_Q, 0 };
    int oy[4] = { 0, 0, GLOW_Q, GLOW_Q };
    for (int k = 0; k < 4; k++) {
        for (int y = 0; y < GLOW_Q; y++)
            for (int x = 0; x < GLOW_Q; x++)
                piece[y * GLOW_Q + x] = tex[(oy[k] + y) * N + ox[k] + x];
        glow_corner[k] = upload_aux(piece, GLOW_Q, GLOW_Q, 32, 0);
    }

    /* edge strips: 0 top (1xQ), 1 right (Qx1), 2 bottom (1xQ), 3 left (Qx1) */
    for (int y = 0; y < GLOW_Q; y++) piece[y] = tex[y * N + GLOW_Q - 1];
    glow_edge[0] = upload_aux(piece, 1, GLOW_Q, 32, 1);
    for (int y = 0; y < GLOW_Q; y++) piece[y] = tex[(GLOW_Q + y) * N + GLOW_Q - 1];
    glow_edge[2] = upload_aux(piece, 1, GLOW_Q, 32, 1);
    for (int x = 0; x < GLOW_Q; x++) piece[x] = tex[(GLOW_Q - 1) * N + GLOW_Q + x];
    glow_edge[1] = upload_aux(piece, GLOW_Q, 1, 32, 1);
    for (int x = 0; x < GLOW_Q; x++) piece[x] = tex[(GLOW_Q - 1) * N + x];
    glow_edge[3] = upload_aux(piece, GLOW_Q, 1, 32, 1);

    free(piece);
    free(tex);
}

static void build_vignette(void) {
    XRenderPictFormat *argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    vig_pm  = XCreatePixmap(dpy, root, screen_w, screen_h, 32);
    vig_pic = XRenderCreatePicture(dpy, vig_pm, argb, 0, NULL);

    XRenderColor clear = { 0, 0, 0, 0 };
    XRenderFillRectangle(dpy, PictOpSrc, vig_pic, &clear, 0, 0, screen_w, screen_h);

    XRadialGradient rg;
    double cx = screen_w / 2.0, cy = screen_h * 0.48;
    rg.inner.x = rg.outer.x = XDoubleToFixed(cx);
    rg.inner.y = rg.outer.y = XDoubleToFixed(cy);
    rg.inner.radius = XDoubleToFixed(screen_h * 0.30);
    rg.outer.radius = XDoubleToFixed(hypot(screen_w, screen_h) * 0.55);
    XFixed stops[2] = { XDoubleToFixed(0.0), XDoubleToFixed(1.0) };
    XRenderColor cols[2] = { mkcolor(0.0, 0.02, 0.04, 0.42), mkcolor(0.0, 0.0, 0.02, 0.80) };
    Picture grad = XRenderCreateRadialGradient(dpy, &rg, stops, cols, 2);
    XRenderPictureAttributes pa; pa.repeat = RepeatPad;
    XRenderChangePicture(dpy, grad, CPRepeat, &pa);
    XRenderComposite(dpy, PictOpSrc, grad, None, vig_pic, 0, 0, 0, 0, 0, 0, screen_w, screen_h);
    XRenderFreePicture(dpy, grad);
}

static void init_gfx(void) {
    if (gfx_ready) return;
    XRenderPictFormat *argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    XRenderPictFormat *a8   = XRenderFindStandardFormat(dpy, PictStandardA8);
    XRenderPictureAttributes rep; memset(&rep, 0, sizeof(rep)); rep.repeat = RepeatNormal;

    back_pm = XCreatePixmap(dpy, root, screen_w, screen_h, 32);
    back_pic = XRenderCreatePicture(dpy, back_pm, argb, 0, NULL);
    surf_pm = XCreatePixmap(dpy, root, screen_w + 2 * PAD, screen_h + 2 * PAD, 32);
    surf_pic = XRenderCreatePicture(dpy, surf_pm, argb, 0, NULL);
    refl_pm = XCreatePixmap(dpy, root, screen_w + 2 * PAD, screen_h + 2 * PAD, 32);
    refl_pic = XRenderCreatePicture(dpy, refl_pm, argb, 0, NULL);
    tmp_pm = XCreatePixmap(dpy, root, TMP_W, TMP_H, 32);
    tmp_pic = XRenderCreatePicture(dpy, tmp_pm, argb, 0, NULL);

    amask_pm = XCreatePixmap(dpy, root, 1, 1, 8);
    amask_pic = XRenderCreatePicture(dpy, amask_pm, a8, CPRepeat, &rep);
    col_pm = XCreatePixmap(dpy, root, 1, 1, 32);
    col_pic = XRenderCreatePicture(dpy, col_pm, argb, CPRepeat, &rep);

    for (int k = 0; k < 4; k++) {
        cut_pic[k]  = make_corner(k, 0);
        ring_pic[k] = make_corner(k, 1);
    }
    build_glow();
    build_vignette();

#ifdef USE_XFT
    if (ov_visual32) {
        xft_draw = XftDrawCreate(dpy, back_pm, ov_visual32, ov_cmap);
        font_label = XftFontOpenName(dpy, scr, "sans-serif:size=11");
        font_big   = XftFontOpenName(dpy, scr, "sans-serif:bold:size=24");
        font_small = XftFontOpenName(dpy, scr, "sans-serif:size=11");
    }
#endif
    gfx_ready = 1;
}

static void free_gfx(void) {
    if (!gfx_ready) return;
#ifdef USE_XFT
    if (xft_draw) { XftDrawDestroy(xft_draw); xft_draw = NULL; }
    if (font_label) { XftFontClose(dpy, font_label); font_label = NULL; }
    if (font_big)   { XftFontClose(dpy, font_big);   font_big = NULL; }
    if (font_small) { XftFontClose(dpy, font_small); font_small = NULL; }
#endif
    for (int i = 0; i < naux; i++) {
        XRenderFreePicture(dpy, aux_pic[i]);
        XFreePixmap(dpy, aux_pm[i]);
    }
    naux = 0;
    XRenderFreePicture(dpy, back_pic);  XFreePixmap(dpy, back_pm);
    XRenderFreePicture(dpy, surf_pic);  XFreePixmap(dpy, surf_pm);
    XRenderFreePicture(dpy, refl_pic);  XFreePixmap(dpy, refl_pm);
    XRenderFreePicture(dpy, tmp_pic);   XFreePixmap(dpy, tmp_pm);
    XRenderFreePicture(dpy, amask_pic); XFreePixmap(dpy, amask_pm);
    XRenderFreePicture(dpy, col_pic);   XFreePixmap(dpy, col_pm);
    XRenderFreePicture(dpy, vig_pic);   XFreePixmap(dpy, vig_pm);
    gfx_ready = 0;
}

/* 1x1 repeat mask jiski alpha hum set karte hain */
static Picture set_alpha_mask(double a) {
    XRenderColor c = { 0, 0, 0, (unsigned short)(clampd(a, 0, 1) * 65535) };
    XRenderFillRectangle(dpy, PictOpSrc, amask_pic, &c, 0, 0, 1, 1);
    return amask_pic;
}

static Picture set_color_src(XRenderColor c) {
    XRenderFillRectangle(dpy, PictOpSrc, col_pic, &c, 0, 0, 1, 1);
    return col_pic;
}

static void set_identity(Picture p) {
    XTransform xf; memset(&xf, 0, sizeof(xf));
    xf.matrix[0][0] = xf.matrix[1][1] = xf.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(dpy, p, &xf);
}

/* ---------------- Window metadata (title / icon / geometry) ---------------- */

static void fetch_title(ClientWin *c) {
    c->title[0] = 0;
    Atom utf8 = intern("UTF8_STRING"), nm = intern("_NET_WM_NAME");
    Atom type; int fmt; unsigned long n, after; unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, c->win, nm, 0, 32, False, utf8, &type, &fmt,
                           &n, &after, &prop) == Success && prop) {
        if (n > 0) snprintf(c->title, sizeof(c->title), "%s", (char *)prop);
        XFree(prop);
    }
    if (!c->title[0]) {
        char *name = NULL;
        if (XFetchName(dpy, c->win, &name) && name) {
            snprintf(c->title, sizeof(c->title), "%s", name);
            XFree(name);
        }
    }
    if (!c->title[0]) {
        XClassHint ch;
        if (XGetClassHint(dpy, c->win, &ch)) {
            snprintf(c->title, sizeof(c->title), "%s", ch.res_class ? ch.res_class : "");
            if (ch.res_name)  XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        }
    }
}

static void fetch_icon(ClientWin *c) {
    Atom a = intern("_NET_WM_ICON");
    Atom type; int fmt; unsigned long n, after; unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, c->win, a, 0, 1 << 20, False, XA_CARDINAL,
                           &type, &fmt, &n, &after, &prop) != Success || !prop) return;
    if (fmt != 32 || n < 3) { XFree(prop); return; }

    unsigned long *d = (unsigned long *)prop;
    unsigned long best = 0; int bw = 0, bh = 0;
    for (unsigned long i = 0; i + 2 < n; ) {
        long w = (long)d[i], h = (long)d[i + 1];
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024 ||
            i + 2 + (unsigned long)w * (unsigned long)h > n) break;
        int take = 0;
        if (bw == 0) take = 1;
        else if (bw < 32 && w > bw) take = 1;
        else if (bw >= 32 && w >= 32 && w < bw) take = 1;
        if (take) { best = i; bw = (int)w; bh = (int)h; }
        i += 2 + (unsigned long)w * (unsigned long)h;
    }

    if (bw > 0) {
        uint32_t *px = malloc((size_t)bw * bh * 4);
        for (int k = 0; k < bw * bh; k++) {
            unsigned long v = d[best + 2 + k];
            unsigned A = (v >> 24) & 255, R = (v >> 16) & 255, G = (v >> 8) & 255, B = v & 255;
            R = R * A / 255; G = G * A / 255; B = B * A / 255;
            px[k] = (A << 24) | (R << 16) | (G << 8) | B;
        }
        c->icon_pic = upload_image(px, bw, bh, 32, 0, &c->icon_pm);
        c->icon_w = bw; c->icon_h = bh;
        free(px);
    }
    XFree(prop);
}

static void fetch_origin(ClientWin *c) {
    int rx = 0, ry = 0; Window child;
    XTranslateCoordinates(dpy, c->win, root, 0, 0, &rx, &ry, &child);
    c->ox = rx; c->oy = ry; c->ow = c->w; c->oh = c->h;
}

/* ---------------- Carousel Layout ---------------- */

static void layout_carousel(void) {
    if (nclients == 0) return;

    double W = screen_w;
    double max_h = screen_h * 0.44;
    double max_w = screen_w * 0.46;
    double center_x = W / 2.0;
    double center_y = screen_h * 0.47;

    for (int i = 0; i < nclients; i++) {
        ClientWin *c = &clients[i];
        int offset = i - selected;
        double ad = fabs((double)offset);

        /* chhoti windows (dialogs etc.) ko max size tak upscale nahi karte */
        double base_h = c->h, base_w = c->w;
        if (base_h > max_h) { base_w *= max_h / base_h; base_h = max_h; }
        if (base_w > max_w) { base_h *= max_w / base_w; base_w = max_w; }

        double scale = 1.0;
        if (ad >= 1) scale = 0.74 * pow(0.90, ad - 1);
        if (scale < 0.42) scale = 0.42;

        c->dst_w = base_w * scale;
        c->dst_h = base_h * scale;

        double cx = center_x;
        if (offset != 0) {
            double dir = offset > 0 ? 1.0 : -1.0;
            double dist = 0.245 + 0.13 * (1.0 - pow(0.55, ad - 1)) / 0.45;
            cx = center_x + dir * W * dist;
        }
        c->dst_x = cx - c->dst_w / 2.0;
        c->dst_y = center_y - c->dst_h / 2.0;

        c->dst_rot   = (offset == 0) ? 0.0 : (offset > 0 ? -ROT_MAX : ROT_MAX);
        c->dst_shade = (offset == 0) ? 0.0 : clampd(0.22 + 0.08 * (ad - 1), 0, 0.6);
        c->dst_glow  = (offset == 0) ? 1.0 : 0.0;
        c->dst_alpha = (ad <= 4) ? 1.0 : clampd(1.0 - 0.35 * (ad - 4), 0, 1);
    }
}

/* Close animation: sab cards apni asal window position par wapas jate hain,
 * "keep" wala card poora visible rehta hai baaki fade out. */
static void layout_close(int keep) {
    for (int i = 0; i < nclients; i++) {
        ClientWin *c = &clients[i];
        c->dst_x = c->ox; c->dst_y = c->oy;
        c->dst_w = c->ow; c->dst_h = c->oh;
        c->dst_rot = 0; c->dst_shade = 0; c->dst_glow = 0;
        c->dst_alpha = (i == keep) ? 1.0 : 0.0;
    }
}

static void set_initial_from_origin(void) {
    for (int i = 0; i < nclients; i++) {
        ClientWin *c = &clients[i];
        c->curr_x = c->ox; c->curr_y = c->oy;
        c->curr_w = c->ow; c->curr_h = c->oh;
        c->curr_rot = 0; c->curr_shade = 0; c->curr_glow = 0; c->curr_alpha = 1.0;
    }
}

/* Time-based exponential smoothing. Returns 1 agar abhi kuch move ho raha hai. */
static int step_animation(double dt, double speed) {
    double k = 1.0 - exp(-speed * dt);
    int moving = 0;

#define STEP(field, eps) do { \
        double d_ = c->dst_##field - c->curr_##field; \
        if (fabs(d_) > (eps)) { c->curr_##field += d_ * k; moving = 1; } \
        else c->curr_##field = c->dst_##field; \
    } while (0)

    for (int i = 0; i < nclients; i++) {
        ClientWin *c = &clients[i];
        STEP(x, 0.3); STEP(y, 0.3); STEP(w, 0.3); STEP(h, 0.3);
        STEP(rot, 0.002); STEP(shade, 0.004); STEP(glow, 0.004); STEP(alpha, 0.004);
    }
#undef STEP

    double d = ui_target - ui_p;
    if (fabs(d) > 0.004) { ui_p += d * k; moving = 1; } else ui_p = ui_target;
    return moving;
}

/* picom jaise compositor window opacity property follow karte hain */
static void apply_ui_opacity(void) {
    double v = clampd(ui_p * 1.15, 0, 1);
    if (fabs(v - last_opacity) < 0.003) return;
    last_opacity = v;
    unsigned long val = (unsigned long)(v * 4294967295.0);
    XChangeProperty(dpy, overlay, NET_WM_WINDOW_OPACITY, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&val, 1);
}

/* ---------------- Rendering primitives ---------------- */

static void set_scale_transform(Picture pic, double sx, double sy) {
    if (sx <= 0.001 || sy <= 0.001) return;
    XTransform xf; memset(&xf, 0, sizeof(xf));
    xf.matrix[0][0] = XDoubleToFixed(1.0 / sx);
    xf.matrix[1][1] = XDoubleToFixed(1.0 / sy);
    xf.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(dpy, pic, &xf);
    XRenderSetPictureFilter(dpy, pic, FilterBilinear, NULL, 0);
}

static void cut_corners(Picture p, int x, int y, int w, int h) {
    if (w < 2 * CR || h < 2 * CR) return;
    XRenderComposite(dpy, PictOpOutReverse, cut_pic[0], None, p, 0, 0, 0, 0, x, y, CR, CR);
    XRenderComposite(dpy, PictOpOutReverse, cut_pic[1], None, p, 0, 0, 0, 0, x + w - CR, y, CR, CR);
    XRenderComposite(dpy, PictOpOutReverse, cut_pic[2], None, p, 0, 0, 0, 0, x + w - CR, y + h - CR, CR, CR);
    XRenderComposite(dpy, PictOpOutReverse, cut_pic[3], None, p, 0, 0, 0, 0, x, y + h - CR, CR, CR);
}

static void draw_border(Picture p, int x, int y, int w, int h, XRenderColor col) {
    if (w < 2 * CR || h < 2 * CR) return;
    XRenderFillRectangle(dpy, PictOpOver, p, &col, x + CR, y, w - 2 * CR, BT);
    XRenderFillRectangle(dpy, PictOpOver, p, &col, x + CR, y + h - BT, w - 2 * CR, BT);
    XRenderFillRectangle(dpy, PictOpOver, p, &col, x, y + CR, BT, h - 2 * CR);
    XRenderFillRectangle(dpy, PictOpOver, p, &col, x + w - BT, y + CR, BT, h - 2 * CR);

    Picture src = set_color_src(col);
    XRenderComposite(dpy, PictOpOver, src, ring_pic[0], p, 0, 0, 0, 0, x, y, CR, CR);
    XRenderComposite(dpy, PictOpOver, src, ring_pic[1], p, 0, 0, 0, 0, x + w - CR, y, CR, CR);
    XRenderComposite(dpy, PictOpOver, src, ring_pic[2], p, 0, 0, 0, 0, x + w - CR, y + h - CR, CR, CR);
    XRenderComposite(dpy, PictOpOver, src, ring_pic[3], p, 0, 0, 0, 0, x, y + h - CR, CR, CR);
}

/* Surface (0..sw, 0..sh) ko 4 points ke quad par perspective-project karke
 * back_pic me composite karo. XRender ka projective transform use hota hai. */
static void project_pic(Picture src, int sw, int sh, double q[4][2], Picture mask) {
    double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
    for (int i = 0; i < 4; i++) {
        if (q[i][0] < minx) minx = q[i][0];
        if (q[i][0] > maxx) maxx = q[i][0];
        if (q[i][1] < miny) miny = q[i][1];
        if (q[i][1] > maxy) maxy = q[i][1];
    }
    int bx0 = (int)floor(minx) - 1, by0 = (int)floor(miny) - 1;
    int bx1 = (int)ceil(maxx) + 1,  by1 = (int)ceil(maxy) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > screen_w) bx1 = screen_w;
    if (by1 > screen_h) by1 = screen_h;
    int bw = bx1 - bx0, bh = by1 - by0;
    if (bw <= 0 || bh <= 0) return;

    int c0x = (int)((minx + maxx) / 2.0), c0y = (int)((miny + maxy) / 2.0);
    double x0 = q[0][0] - c0x, y0 = q[0][1] - c0y;
    double x1 = q[1][0] - c0x, y1 = q[1][1] - c0y;
    double x2 = q[2][0] - c0x, y2 = q[2][1] - c0y;
    double x3 = q[3][0] - c0x, y3 = q[3][1] - c0y;

    /* unit square -> quad homography (Heckbert) */
    double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double a, b, c, d, e, f, g, h;
    double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    double den = dx1 * dy2 - dx2 * dy1;
    if ((fabs(sx) < 1e-9 && fabs(sy) < 1e-9) || fabs(den) < 1e-12) {
        a = x1 - x0; b = x3 - x0; c = x0;
        d = y1 - y0; e = y3 - y0; f = y0;
        g = 0; h = 0;
    } else {
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
        a = x1 - x0 + g * x1; b = x3 - x0 + h * x3; c = x0;
        d = y1 - y0 + g * y1; e = y3 - y0 + h * y3; f = y0;
    }

    /* inverse of M = [[a b c][d e f][g h 1]] via adjugate */
    double m[9] = { a, b, c, d, e, f, g, h, 1.0 };
    double inv[9];
    inv[0] =  (m[4] * m[8] - m[5] * m[7]);
    inv[1] = -(m[1] * m[8] - m[2] * m[7]);
    inv[2] =  (m[1] * m[5] - m[2] * m[4]);
    inv[3] = -(m[3] * m[8] - m[5] * m[6]);
    inv[4] =  (m[0] * m[8] - m[2] * m[6]);
    inv[5] = -(m[0] * m[5] - m[2] * m[3]);
    inv[6] =  (m[3] * m[7] - m[4] * m[6]);
    inv[7] = -(m[0] * m[7] - m[1] * m[6]);
    inv[8] =  (m[0] * m[4] - m[1] * m[3]);
    double det = m[0] * inv[0] + m[1] * inv[3] + m[2] * inv[6];
    if (fabs(det) < 1e-12) return;

    double P[9];
    for (int i = 0; i < 9; i++) P[i] = inv[i] / det;
    for (int j = 0; j < 3; j++) { P[j] *= sw; P[3 + j] *= sh; }
    /* card surface me (PAD, PAD) se shuru hota hai */
    for (int j = 0; j < 3; j++) { P[j] += PAD * P[6 + j]; P[3 + j] += PAD * P[6 + j]; }

    /* local (c0) coordinates se absolute screen coordinates: server negative
     * src offsets ko clip kar deta tha, isliye translation matrix me absorb karo */
    for (int i = 0; i < 3; i++)
        P[i * 3 + 2] = P[i * 3 + 2] - P[i * 3] * c0x - P[i * 3 + 1] * c0y;

    /* 16.16 fixed point ki precision ke liye matrix ko scale up karo */
    double mx = 0;
    for (int i = 0; i < 9; i++) if (fabs(P[i]) > mx) mx = fabs(P[i]);
    if (mx < 1e-12) return;
    double K = 30000.0 / mx;
    if (K > 1e5) K = 1e5;

    XTransform xf;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            xf.matrix[i][j] = XDoubleToFixed(P[i * 3 + j] * K);
    XRenderSetPictureTransform(dpy, src, &xf);
    XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);

    XRenderComposite(dpy, PictOpOver, src, mask, back_pic,
                     bx0, by0, 0, 0, bx0, by0, bw, bh);
}

#ifdef USE_XFT
static void draw_text(XftFont *font, double x, double y, const char *text,
                      double r, double g, double b, double a) {
    if (!xft_draw || !font || !text[0] || a < 0.02) return;
    XRenderColor rc = mkcolor(r, g, b, a);
    XftColor xc;
    if (!XftColorAllocValue(dpy, ov_visual32, ov_cmap, &rc, &xc)) return;
    XftDrawStringUtf8(xft_draw, &xc, font, (int)x, (int)y, (const FcChar8 *)text, (int)strlen(text));
    XftColorFree(dpy, ov_visual32, ov_cmap, &xc);
}

static int text_width(XftFont *font, const char *text) {
    XGlyphInfo gi;
    XftTextExtentsUtf8(dpy, font, (const FcChar8 *)text, (int)strlen(text), &gi);
    return gi.xOff;
}

static void draw_text_centered(XftFont *font, double cx, double y, const char *text,
                               double r, double g, double b, double a) {
    if (!font) return;
    draw_text(font, cx - text_width(font, text) / 2.0, y, text, r, g, b, a);
}

/* Title ko width me fit karo (end me ... laga kar) */
static void fit_text(XftFont *font, const char *in, char *out, size_t outsz, int maxw) {
    snprintf(out, outsz, "%s", in);
    int guard = 200;
    while (guard-- > 0 && text_width(font, out) > maxw) {
        size_t n = strlen(out);
        if (n <= 4) break;
        n -= 1;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;  /* utf8 boundary */
        if (n > 3) n -= 3;
        out[n] = 0;
        size_t m = strlen(out);
        if (m + 4 < outsz) memcpy(out + m, "...", 4);
    }
}
#endif

/* ---------------- Card drawing ---------------- */

static void draw_glow(int x, int y, int w, int h, double strength) {
    if (strength < 0.02) return;
    if (w < 2 * GLOW_RC + 2 || h < 2 * GLOW_RC + 2) return;
    Picture m = set_alpha_mask(strength);
    const int G = GLOW_G, Q = GLOW_Q, Rc = GLOW_RC;

    XRenderComposite(dpy, PictOpOver, glow_corner[0], m, back_pic, 0, 0, 0, 0, x - G,          y - G,          Q, Q);
    XRenderComposite(dpy, PictOpOver, glow_corner[1], m, back_pic, 0, 0, 0, 0, x + w - Rc,     y - G,          Q, Q);
    XRenderComposite(dpy, PictOpOver, glow_corner[2], m, back_pic, 0, 0, 0, 0, x + w - Rc,     y + h - Rc,     Q, Q);
    XRenderComposite(dpy, PictOpOver, glow_corner[3], m, back_pic, 0, 0, 0, 0, x - G,          y + h - Rc,     Q, Q);

    if (w - 2 * Rc > 0) {
        XRenderComposite(dpy, PictOpOver, glow_edge[0], m, back_pic, 0, 0, 0, 0, x + Rc, y - G,      w - 2 * Rc, Q);
        XRenderComposite(dpy, PictOpOver, glow_edge[2], m, back_pic, 0, 0, 0, 0, x + Rc, y + h - Rc, w - 2 * Rc, Q);
    }
    if (h - 2 * Rc > 0) {
        XRenderComposite(dpy, PictOpOver, glow_edge[3], m, back_pic, 0, 0, 0, 0, x - G,      y + Rc, Q, h - 2 * Rc);
        XRenderComposite(dpy, PictOpOver, glow_edge[1], m, back_pic, 0, 0, 0, 0, x + w - Rc, y + Rc, Q, h - 2 * Rc);
    }
}

/* Icon wala chhota dark pill, card surface ke neeche-beech me */
static void draw_pill(int w, int h, ClientWin *c) {
    if (!c->icon_pic || w < 150 || h < 100) return;
    const int pw = 64, ph = 38, isz = 26;

    XRenderColor dark = mkcolor(0.04, 0.07, 0.10, 0.84);
    XRenderFillRectangle(dpy, PictOpSrc, tmp_pic, &dark, 0, 0, pw, ph);
    cut_corners(tmp_pic, 0, 0, pw, ph);

    set_scale_transform(c->icon_pic, (double)isz / c->icon_w, (double)isz / c->icon_h);
    XRenderComposite(dpy, PictOpOver, c->icon_pic, None, tmp_pic, 0, 0, 0, 0,
                     (pw - isz) / 2, (ph - isz) / 2, isz, isz);

    XRenderComposite(dpy, PictOpOver, tmp_pic, None, surf_pic, 0, 0, 0, 0,
                     PAD + (w - pw) / 2, PAD + h - ph - 12, pw, ph);
}

static void build_card_surface(ClientWin *c, int w, int h) {
    XRenderColor clear = { 0, 0, 0, 0 };
    set_identity(surf_pic);
    /* poora area (padding samet) pehle transparent - stale pixels nahi */
    XRenderFillRectangle(dpy, PictOpSrc, surf_pic, &clear, 0, 0, w + 2 * PAD, h + 2 * PAD);

    /* opaque dark base - transparent windows ke neeche glow na jhalke */
    XRenderColor base = mkcolor(0.05, 0.06, 0.09, 1.0);
    XRenderFillRectangle(dpy, PictOpSrc, surf_pic, &base, PAD, PAD, w, h);

    set_scale_transform(c->picture, (double)w / c->w, (double)h / c->h);
    XRenderComposite(dpy, PictOpOver, c->picture, None, surf_pic, 0, 0, 0, 0, PAD, PAD, w, h);

    if (c->curr_shade > 0.01) {
        XRenderColor sh = mkcolor(0.0, 0.0, 0.02, c->curr_shade);
        XRenderFillRectangle(dpy, PictOpOver, surf_pic, &sh, PAD, PAD, w, h);
    }

    if (!g_closing || ui_p > 0.5) draw_pill(w, h, c);
    /* close ke akhir me real window ke corners square hain - card ke round
     * corners/border achanak ghayab hone se pop aata tha */
    if (!g_closing || ui_p > 0.12) cut_corners(surf_pic, PAD, PAD, w, h);

    double gl = clampd(c->curr_glow, 0, 1);
    XRenderColor bc = mkcolor(0.78 + (0.25 - 0.78) * gl,
                              0.84 + (0.95 - 0.84) * gl,
                              0.95 + (1.00 - 0.95) * gl,
                              (0.40 + 0.60 * gl) * (g_closing ? clampd(ui_p * 2.0, 0, 1) : 1.0));
    if (bc.alpha > 0x0200) draw_border(surf_pic, PAD, PAD, w, h, bc);
}

static void render_reflection(int w, int h, double a, double q[4][2]) {
    double ra = 0.36 * ui_p * a;
    if (ra < 0.02) return;

    set_identity(surf_pic);
    XRenderComposite(dpy, PictOpSrc, surf_pic, None, refl_pic, 0, 0, 0, 0, 0, 0, w + 2 * PAD, h + 2 * PAD);

    XLinearGradient lg;
    lg.p1.x = XDoubleToFixed(0); lg.p1.y = XDoubleToFixed(PAD + h * 0.50);
    lg.p2.x = XDoubleToFixed(0); lg.p2.y = XDoubleToFixed(PAD + h);
    XFixed stops[2] = { XDoubleToFixed(0.0), XDoubleToFixed(1.0) };
    XRenderColor cols[2] = { { 0, 0, 0, 0 }, { 0, 0, 0, (unsigned short)(ra * 65535) } };
    Picture grad = XRenderCreateLinearGradient(dpy, &lg, stops, cols, 2);
    XRenderPictureAttributes pa; pa.repeat = RepeatPad;
    XRenderChangePicture(dpy, grad, CPRepeat, &pa);
    XRenderComposite(dpy, PictOpInReverse, grad, None, refl_pic, 0, 0, 0, 0, 0, 0, w + 2 * PAD, h + 2 * PAD);
    XRenderFreePicture(dpy, grad);

    double yb = (q[2][1] + q[3][1]) / 2.0 + 4.0;
    double rq[4][2];
    for (int k = 0; k < 4; k++) { rq[k][0] = q[k][0]; rq[k][1] = 2.0 * yb - q[k][1]; }
    project_pic(refl_pic, w, h, rq, None);
}

static void render_card(ClientWin *c, int ad) {
    c->quad_valid = 0;
    int w = (int)(c->curr_w + 0.5), h = (int)(c->curr_h + 0.5);
    if (w < 8 || h < 8) return;
    if (w > screen_w) w = screen_w;
    if (h > screen_h) h = screen_h;

    double a = clampd(c->curr_alpha, 0, 1);
    if (a < 0.01) return;

    double cx = c->curr_x + c->curr_w / 2.0, cy = c->curr_y + c->curr_h / 2.0;
    double hw = w / 2.0, hh = h / 2.0;
    double rot = c->curr_rot;
    double cs = cos(rot), sn = sin(rot);
    double dist = 2.2 * w;
    double sxs[4] = { -1, 1, 1, -1 }, sys[4] = { -1, -1, 1, 1 };
    double q[4][2];
    for (int k = 0; k < 4; k++) {
        double x = sxs[k] * hw;
        double X = x * cs, Z = x * sn;
        double s = dist / (dist + Z);
        q[k][0] = cx + X * s;
        q[k][1] = cy + sys[k] * hh * s;
        c->quad[k][0] = q[k][0]; c->quad[k][1] = q[k][1];
    }
    c->quad_valid = 1;

    /* Cull: jo card poora screen se bahar hai usay draw hi mat karo */
    {
        double mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9;
        for (int k = 0; k < 4; k++) {
            if (q[k][0] < mnx) mnx = q[k][0];
            if (q[k][0] > mxx) mxx = q[k][0];
            if (q[k][1] < mny) mny = q[k][1];
            if (q[k][1] > mxy) mxy = q[k][1];
        }
        if (mxx < 0 || mnx > screen_w || mxy < 0 || mny > screen_h) return;
    }

    /* glow (sirf center card ke liye) */
    if (c->curr_glow > 0.02 && fabs(rot) < 0.2)
        draw_glow((int)floor(cx - hw + 0.5), (int)floor(cy - hh + 0.5), w, h, c->curr_glow * ui_p);

    build_card_surface(c, w, h);

    if (ad <= 2) render_reflection(w, h, a, q);   /* pehle ad<=4 tha: 9 cards ki reflection har frame */

    Picture mask = (a < 0.995) ? set_alpha_mask(a) : None;
    if (fabs(rot) < 0.004) {
        set_identity(surf_pic);
        int dx = (int)floor(cx - hw + 0.5), dy = (int)floor(cy - hh + 0.5);
        XRenderComposite(dpy, PictOpOver, surf_pic, mask, back_pic, PAD, PAD, 0, 0, dx, dy, w, h);
    } else {
        project_pic(surf_pic, w, h, q, mask);
    }

#ifdef USE_XFT
    if (ad <= 3 && font_label && c->title[0]) {
        char buf[160];
        fit_text(font_label, c->title, buf, sizeof(buf), (int)(w * 0.95) + 20);
        double tx = (q[0][0] + q[1][0]) / 2.0;
        double ty = fmin(q[0][1], q[1][1]) - 12.0;
        double ta = a * ui_p * (0.55 + 0.45 * clampd(c->curr_glow, 0, 1));
        double gl = clampd(c->curr_glow, 0, 1);
        draw_text_centered(font_label, tx, ty, buf,
                           0.80 + (0.85 - 0.80) * gl, 0.86 + (0.98 - 0.86) * gl, 0.92 + (1.0 - 0.92) * gl, ta);
    }
#endif
}

#ifdef USE_XFT
static void draw_clock_panel(void) {
    if (!font_big || !font_small || ui_p < 0.02) return;
    const int pw = 260, ph = 88;
    int px = (screen_w - pw) / 2, py = 22;

    XRenderColor fill = mkcolor(0.04, 0.07, 0.10, 0.78);
    XRenderFillRectangle(dpy, PictOpSrc, tmp_pic, &fill, 0, 0, pw, ph);
    cut_corners(tmp_pic, 0, 0, pw, ph);
    draw_border(tmp_pic, 0, 0, pw, ph, mkcolor(0.25, 0.85, 1.0, 0.55));

    Picture m = set_alpha_mask(ui_p);
    XRenderComposite(dpy, PictOpOver, tmp_pic, m, back_pic, 0, 0, 0, 0, px, py, pw, ph);

    time_t t = time(NULL);
    struct tm tmv; localtime_r(&t, &tmv);
    char day[48], date[64];
    strftime(day, sizeof(day), "%A", &tmv);
    for (char *p = day; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    strftime(date, sizeof(date), "%d %b %Y   %I:%M %p", &tmv);
    for (char *p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

    draw_text_centered(font_big, screen_w / 2.0, py + 42, day, 0.92, 0.98, 1.0, ui_p);
    draw_text_centered(font_small, screen_w / 2.0, py + 70, date, 0.45, 0.88, 1.0, ui_p * 0.9);
}
#endif

static void rebuild_background(void) {
    if (wallpaper_pic) {
        XRenderComposite(dpy, PictOpSrc, wallpaper_pic, None, back_pic,
                         0, 0, 0, 0, 0, 0, screen_w, screen_h);
    } else {
        XRenderColor bg = { 0x0808, 0x0d0d, 0x1414, 0xffff };
        XRenderFillRectangle(dpy, PictOpSrc, back_pic, &bg, 0, 0, screen_w, screen_h);
    }
    XRenderComposite(dpy, PictOpOver, vig_pic, None, back_pic,
                     0, 0, 0, 0, 0, 0, screen_w, screen_h);
}

static void render_all(void) {
    rebuild_background();

#ifdef USE_XFT
    draw_clock_panel();
#endif

    for (int i = 0; i < nclients; i++) clients[i].quad_valid = 0;

    if (nclients > 0) {
        int maxd = selected;
        if (nclients - 1 - selected > maxd) maxd = nclients - 1 - selected;
        /* door wale pehle, center sabse aakhir me (upar) */
        for (int d = maxd; d >= 0; d--) {
            if (selected - d >= 0) render_card(&clients[selected - d], d);
            if (d > 0 && selected + d < nclients) render_card(&clients[selected + d], d);
        }
    }

    XRenderComposite(dpy, PictOpSrc, back_pic, None, overlay_pic,
                     0, 0, 0, 0, 0, 0, screen_w, screen_h);
    XSync(dpy, False);   /* server ke saath pace karo - queue me frames jama na hon */
}

/* ---------------- Input helpers ---------------- */

static int point_in_quad(double q[4][2], double px, double py) {
    int pos = 0, neg = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        double cr = (q[j][0] - q[i][0]) * (py - q[i][1]) - (q[j][1] - q[i][1]) * (px - q[i][0]);
        if (cr > 0) pos = 1; else if (cr < 0) neg = 1;
    }
    return !(pos && neg);
}

static int hit_test(int px, int py) {
    for (int d = 0; d < nclients; d++) {
        int cand[2] = { selected - d, selected + d };
        for (int k = 0; k < (d == 0 ? 1 : 2); k++) {
            int i = cand[k];
            if (i < 0 || i >= nclients) continue;
            if (clients[i].quad_valid && clients[i].curr_alpha > 0.2 &&
                point_in_quad(clients[i].quad, px, py))
                return i;
        }
    }
    return -1;
}

static void select_index(int idx) {
    if (nclients == 0) return;
    selected = ((idx % nclients) + nclients) % nclients;
    layout_carousel();
}

static void activate_window(Window w) {
    XEvent ev; memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = NET_ACTIVE_WINDOW;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XRaiseWindow(dpy, w);
    XSetInputFocus(dpy, w, RevertToParent, CurrentTime);
    XFlush(dpy);
}

static void teardown_clients(void) {
    for (int i = 0; i < nclients; i++) free_client(&clients[i]);
    nclients = 0;
}

static int build_client_list(Window current_focused_win, int *focused_index_out) {
    Window *list = NULL;
    int n = get_client_list(&list);
    nclients = 0;
    if (n <= 0) { if (list) XFree(list); return 0; }

    int focused_idx = -1;

    for (int i = 0; i < n && nclients < MAX_WINDOWS; i++) {
        if (!window_is_viewable(list[i])) continue;

        ClientWin *c = &clients[nclients];
        memset(c, 0, sizeof(*c));
        c->win = list[i];

        if (grab_window_picture(c)) {
            if (c->win == current_focused_win) focused_idx = nclients;
            fetch_origin(c);
            fetch_title(c);
            fetch_icon(c);
            nclients++;
        }
    }
    XFree(list);

    if (focused_idx == -1) {
        Window active = get_currently_focused_window();
        for (int i = 0; i < nclients; i++) {
            if (clients[i].win == active) { focused_idx = i; break; }
        }
        if (focused_idx == -1) focused_idx = 0;
    }
    if (focused_index_out) *focused_index_out = focused_idx;
    return nclients;
}

static void create_overlay(void) {
    XVisualInfo vinfo;
    if (XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) {
        ov_visual32 = vinfo.visual;
    } else {
        vinfo.visual = DefaultVisual(dpy, scr);
        vinfo.depth  = DefaultDepth(dpy, scr);
        ov_visual32 = NULL;
    }
    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
    ov_cmap = swa.colormap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
    ButtonPressMask | PointerMotionMask | StructureNotifyMask;
    overlay = XCreateWindow(dpy, root, 0, 0, screen_w, screen_h, 0,
                            vinfo.depth, InputOutput, vinfo.visual,
                            CWOverrideRedirect | CWColormap | CWBackPixel |
                            CWBorderPixel | CWEventMask, &swa);
    XRenderPictFormat *ofmt = XRenderFindVisualFormat(dpy, vinfo.visual);
    XRenderPictureAttributes opa; memset(&opa, 0, sizeof(opa));
    overlay_pic = XRenderCreatePicture(dpy, overlay, ofmt, 0, &opa);

    grab_wallpaper_picture();
    init_gfx();

    /* map hone se pehle opacity 0 - open animation me fade-in hoga */
    ui_p = 0.0; ui_target = 1.0; last_opacity = -1.0;
    apply_ui_opacity();

    XMapRaised(dpy, overlay);
}

static void destroy_overlay(void) {
    free_gfx();
    if (wallpaper_pic) { XRenderFreePicture(dpy, wallpaper_pic); wallpaper_pic = 0; }
    if (wallpaper_pm && wallpaper_pm_owned) { XFreePixmap(dpy, wallpaper_pm); }
    wallpaper_pm = 0;
    wallpaper_pm_owned = 0;
    wallpaper_win = 0;
    if (overlay_pic) { XRenderFreePicture(dpy, overlay_pic); overlay_pic = 0; }
    if (overlay) { XDestroyWindow(dpy, overlay); overlay = 0; }
}

/* ---------------- Main switcher loop ---------------- */

static Window run_switcher(Window focused, int offset_direction) {
    int focused_idx = 0;

    if (build_client_list(focused, &focused_idx) == 0)
        return None;

    selected = focused_idx;

    if (offset_direction != 0 && nclients > 0)
        selected = (selected + offset_direction + nclients) % nclients;

    create_overlay();
    layout_carousel();
    set_initial_from_origin();   /* open animation: windows apni jagah se nikalti hain */

    /* Hotkey daemon (sxhkd etc.) abhi tak keyboard grab hold kar sakta hai
     * jab tak trigger key release nahi hoti - isliye retry loop. */
    int kb_ok = 0, ptr_ok = 0;
    for (int attempt = 0; attempt < 20 && !kb_ok; attempt++) {
        int r = XGrabKeyboard(dpy, overlay, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        if (r == GrabSuccess) { kb_ok = 1; break; }
        usleep(10000);
    }
    for (int attempt = 0; attempt < 20 && !ptr_ok; attempt++) {
        int r = XGrabPointer(dpy, overlay, True, ButtonPressMask | PointerMotionMask,
                             GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        if (r == GrabSuccess) { ptr_ok = 1; break; }
        usleep(10000);
    }

    if (!kb_ok) {
        fprintf(stderr, "overview: keyboard grab fail ho gaya, exiting cleanly\n");
        if (ptr_ok) XUngrabPointer(dpy, CurrentTime);
        destroy_overlay();
        teardown_clients();
        XFlush(dpy);
        return None;
    }

    render_all();

    Window result = None;
    int want_close = 0, closing = 0, need_render = 0;
    double close_deadline = 0;
    int close_keep = 0;
    double speed = 13.0;
    double last = now_sec(), last_refresh = last;
    int xfd = ConnectionNumber(dpy);
    unsigned int tick = 0;

    for (;;) {
        if (!closing) {
            while (XPending(dpy) && !want_close) {
                XEvent e;
                XNextEvent(dpy, &e);

                switch (e.type) {
                    case Expose:
                        need_render = 1;
                        break;

                    case MotionNotify: {
                        int hit = hit_test(e.xmotion.x, e.xmotion.y);
                        if (hit >= 0 && hit != selected) select_index(hit);
                        break;
                    }

                    case ButtonPress:
                        if (e.xbutton.button == Button1) {
                            int hit = hit_test(e.xbutton.x, e.xbutton.y);
                            if (hit >= 0) { result = clients[hit].win; want_close = 1; }
                        } else if (e.xbutton.button == Button4) {
                            select_index(selected - 1);
                        } else if (e.xbutton.button == Button5) {
                            select_index(selected + 1);
                        } else {
                            want_close = 1;
                        }
                        break;

                    case KeyPress: {
                        KeySym ks = XLookupKeysym(&e.xkey, 0);
                        if (ks == XK_Escape) {
                            want_close = 1;
                        } else if (ks == XK_Tab || ks == XK_Right) {
                            int fwd = !(e.xkey.state & ShiftMask);
                            select_index(fwd ? selected + 1 : selected - 1);
                        } else if (ks == XK_ISO_Left_Tab || ks == XK_Left) {
                            select_index(selected - 1);
                        } else if (ks == XK_Return) {
                            result = clients[selected].win;
                            want_close = 1;
                        }
                        break;
                    }

                    case KeyRelease: {
                        KeySym ks = XLookupKeysym(&e.xkey, 0);
                        if (ks == XK_Super_L || ks == XK_Super_R) {
                            result = clients[selected].win;
                            want_close = 1;
                        }
                        break;
                    }
                }
            }
        }

        if (want_close && !closing) {
            closing = 1;
            int keep = focused_idx;
            if (result != None) {
                for (int i = 0; i < nclients; i++)
                    if (clients[i].win == result) { keep = i; break; }
                /* real window pehle hi raise/focus kar do - overlay ke neeche
                 * hoti hai, fade-out ke saath asli window nazar aa jati hai */
                activate_window(result);
            }
            close_keep = keep;
            layout_close(keep);
            ui_target = 0.0;
            speed = 17.0;
            g_closing = 1;
            close_deadline = now_sec() + 1.0;
        }

        double t = now_sec();
        double dt = t - last;
        last = t;
        if (dt > 0.05) dt = 0.05;

        int moving = step_animation(dt, speed);
        apply_ui_opacity();

        /* live content (video, terminal) ke liye ~30fps pixmap refresh */
        if (t - last_refresh >= 0.033) {
            last_refresh = t;
            tick++;
            refresh_wallpaper_picture();
            /* Pehle har 4th tick pe SAARI windows refresh hoti thin (N roundtrips
             * ek hi frame me = stutter). Ab: selected har tick, uske paas wali
             * har doosre tick, baaki me se sirf ek round-robin. */
            static int rr_idx = 0;
            int rc = closing ? close_keep : selected;
            for (int i = 0; i < nclients; i++) {
                int ad = abs(i - rc);
                if (ad == 0 || (ad == 1 && (tick & 1)))
                    refresh_client_picture(&clients[i]);
            }
            if (nclients > 0) {
                rr_idx = (rr_idx + 1) % nclients;
                if (abs(rr_idx - selected) > 1 && abs(rr_idx - selected) <= 6)
                    refresh_client_picture(&clients[rr_idx]);
            }
            need_render = 1;
        }

        if (moving || need_render) {
            render_all();
            need_render = 0;
        }

        if (closing && (!moving || t > close_deadline)) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        struct timeval tv = { 0, moving ? 8000 : 20000 };
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }

    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    g_closing = 0;

    XSync(dpy, False);
    destroy_overlay();
    teardown_clients();
    XFlush(dpy);

    return result;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "overview: cannot open X display\n");
        return 1;
    }

    XSetErrorHandler(x_error_handler);

    scr  = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    screen_w = DisplayWidth(dpy, scr);
    screen_h = DisplayHeight(dpy, scr);

    int ev, err;

    if (!XCompositeQueryExtension(dpy, &ev, &err) ||
        !XRenderQueryExtension(dpy, &ev, &err)) {
        fprintf(stderr, "overview: XComposite/XRender extensions required\n");
        XCloseDisplay(dpy);
        return 1;
    }

    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectAutomatic);

    NET_CLIENT_LIST   = intern("_NET_CLIENT_LIST");
    NET_ACTIVE_WINDOW = intern("_NET_ACTIVE_WINDOW");
    XROOTPMAP_ID      = intern("_XROOTPMAP_ID");
    ESETROOT_PMAP_ID  = intern("ESETROOT_PMAP_ID");
    NET_WM_WINDOW_OPACITY = intern("_NET_WM_WINDOW_OPACITY");

    Window focused = get_currently_focused_window();

    /* activation ab run_switcher ke andar close animation se pehle hoti hai */
    run_switcher(focused, 0);

    XCloseDisplay(dpy);
    return 0;
}
