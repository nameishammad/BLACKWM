#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib-xrender.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 256
#define MAX_DESKTOPS 32
#define CARD_W 220
#define CARD_H 150
#define CARD_PAD 16
#define GROUP_PAD 24
#define DESKBAR_Y 22
#define DESKBAR_H 130
#define DESKBOX_PAD 14
#define DESKLABEL_H 24
#define PANEL_BOTTOM 18
#define PANEL_INNER_PAD 30
#define PANEL_RADIUS 16
#define LABEL_H 46
#define MINI_PAD 0
#define MINI_TITLE_H 16
#define MAIN_CARD_W 200
#define MAIN_CARD_H 180
#define MAIN_CARD_PAD 28
#define CARD_RADIUS 10.0
#define DESKBOX_RADIUS 8.0
#define DESKBOX_OPACITY 1.0
#define DESKBOX_OPACITY_SELECTED 1.0
#define DESKBOX_WIN_BACKING 0.0
#define MAIN_TOP_MARGIN 26
#define MAX_DESKTOP_NAMES 64
#define FRAME_INTERVAL_MS 16
#define ANIM_FRAME_INTERVAL_MS 16
#define SEL_ANIM_MS 180
#define LAUNCH_ANIM_MS 260
#define GRID_ANIM_MS 220

typedef struct {
  Window win;
  char title[256];
  long desktop;
  cairo_surface_t *icon;
  int x, y, w, h;
  Pixmap thumb_pixmap;
  cairo_surface_t *thumb_surf;
  int real_w, real_h;
  int real_x, real_y;
  Damage damage;
  struct timeval last_refresh;
  int redirected;
  int mini_x, mini_y, mini_w, mini_h;   // OUTER rect (frame/border samet kar)
  int cmini_x, cmini_y, cmini_w, cmini_h; // sirf client content ka rect
  int ext_l, ext_r, ext_t, ext_b;       // decoration/frame extents (real px)
  Window dialog_owner_win;
  int is_dialog;
  int dialog_owner_idx;
} Client;

typedef struct {
  long id;
  char name[128];
  int x, y, w, h;
  int has_apps;
} Group;

static Display *dpy;
static Window root;
static Window overview;
static int scr;
static int sw, sh;
static cairo_surface_t *csurf;
static cairo_t *cr;
static cairo_surface_t *bufsurf;
static cairo_t *bcr;
static Pixmap init_bg_pm = None;
static Visual *root_visual;
static int have_composite = 0;
static int have_damage = 0;
static int damage_evt_base = 0, damage_err_base = 0;
static Window cm_owner_win = None;
static int became_cm = 0;
static cairo_surface_t *wallpaper_surf = NULL;
static cairo_surface_t *wallpaper_thumb_surf = NULL;
// wallpaper_surf ki ASAL pixel size (root pixmap / xwinwrap window ki).
// Ye screen (sw x sh) se alag ho sakti hai; isi farq se panel mein
// wallpaper zoom/crop hota tha, isliye sab jagah isi size se scale karte hain.
static int wallpaper_w = 0, wallpaper_h = 0;
// KDE/GNOME jesa blurred+dimmed full-screen background (sw x sh, cached).
static cairo_surface_t *wallpaper_blur_surf = NULL;
static int wallpaper_thumb_w = 0, wallpaper_thumb_h = 0;
// xwinwrap (ya kisi bhi override-redirect, full-screen "live wallpaper"
// window) ko live composite karne ke liye state. Aise wallpaper apps
// _XROOTPMAP_ID/ESETROOT_PMAP_ID atom set NAHI karte (wo sirf
// feh/nitrogen/hsetroot jese static wallpaper setter set karte hain),
// isliye sirf un atoms par bharosa karne se overview purani/default
// wallpaper dikhata reh jata hai jab xwinwrap chal raha ho.
static Window xwinwrap_win = None;
static Damage xwinwrap_damage = None;
static int need_redraw = 0;
static struct timespec last_draw_ts = {0, 0};
static int sel_anim_active = 0;
static struct timespec sel_anim_start_ts;
static double sel_anim_from_x, sel_anim_from_y, sel_anim_from_w,
    sel_anim_from_h;
static double sel_anim_to_x, sel_anim_to_y, sel_anim_to_w, sel_anim_to_h;
// Overview khulte waqt poori tarah fade+scale-in hone ke liye launch
// animation state (overview map hone k baad trigger hota hai).
static int launch_anim_active = 0;
static struct timespec launch_anim_start_ts;
// Main grid (skippy-xd wale window-card section) ke cards launch par ya
// desktop switch par pop-in (scale+fade) hone ke liye state.
static int grid_anim_active = 0;
static struct timespec grid_anim_start_ts;
static Client clients[MAX_CLIENTS];
static int nclients = 0;
static Group groups[MAX_DESKTOPS];
static int ngroups = 0;
static long current_desktop = 0;
static long selected_desktop = 0;
static int have_desktops = 0;
static char desktop_names[MAX_DESKTOP_NAMES][128];
static int ndesktop_names = 0;
static Window active_window = None;
static Window last_focused[MAX_DESKTOPS];
static int deskbar_box_h = DESKBAR_H;
// Desktop boxes ki Y position (screen ke BOTTOM par; layout_desktop_bar
// isay har dafa sh/box_h ke hisaab se set karta hai).
static int deskbar_y = 600;
// Desktop boxes ab screen ke LEFT par vertical column mein hain.
static int deskbar_x = 22;
static int deskbar_box_w = 160;
static int mini_hidden[MAX_CLIENTS];
// Har client ka REAL on-screen stacking rank (XQueryTree root ke
// bottom-to-top child order se). Chota rank = neeche, bara rank = upar.
// Ye load_stacking_order() mein bharta hai aur draw() deskbar mini-boxes
// ko isi asal Z-order ke mutabiq paint karta hai — is se popup/dialog ko
// hamesha "sab se upar" force nahi kiya jata, sirf jahan woh REAL screen
// par actually upar ho wahi upar dikhta hai.
static int client_stack_rank[MAX_CLIENTS];
static int debug_enabled = 0;
static int focused_client_idx = -1;
#define FOCUS_AREA_GRID 0
#define FOCUS_AREA_DESKBAR 1
static int focus_area = FOCUS_AREA_GRID;
static int dnd_active = 0;
static int dnd_moved = 0;
static int dnd_client_idx = -1;
static int dnd_press_x = 0, dnd_press_y = 0;
static int dnd_cur_x = 0, dnd_cur_y = 0;
// Tab keyboard modifier nahi hoti, isliye Tab+Arrow combo detect karne ke
// liye khud track karte hain ke Tab is waqt dabi hui hai ya nahi.
static int tab_held = 0;
#define DND_THRESHOLD 6
static Atom A_NET_CLIENT_LIST, A_NET_WM_DESKTOP, A_NET_WM_NAME, A_UTF8_STRING,
    A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP, A_NET_DESKTOP_NAMES,
    A_NET_ACTIVE_WINDOW, A_NET_WM_ICON, A_WM_NAME, A_XROOTPMAP_ID,
    A_ESETROOT_PMAP_ID, A_WM_LAST_FOCUS_PER_WS, A_WM_DIALOG_OWNER,
    A_WM_WORKSPACE_MODE, A_NET_FRAME_EXTENTS;

// Har workspace ka mode, wm.c se mila (0 = reel/slide, 1 = tiling,
// 2 = floating). -1 = abhi tak WM se pata nahi chala (property missing
// ya purana wm.c) - is surat mein safe default "sab windows dikhao"
// (tiling/floating jaisa) rakha jata hai, taake naya WM na hone par
// bhi purana visual barqarar rahe.
static int workspace_mode[MAX_DESKTOPS];

static void init_atoms(void) {
  A_NET_CLIENT_LIST = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
  A_NET_WM_DESKTOP = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
  A_NET_WM_NAME = XInternAtom(dpy, "_NET_WM_NAME", False);
  A_UTF8_STRING = XInternAtom(dpy, "UTF8_STRING", False);
  A_NET_NUMBER_OF_DESKTOPS = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
  A_NET_CURRENT_DESKTOP = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
  A_NET_DESKTOP_NAMES = XInternAtom(dpy, "_NET_DESKTOP_NAMES", False);
  A_NET_ACTIVE_WINDOW = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
  A_NET_WM_ICON = XInternAtom(dpy, "_NET_WM_ICON", False);
  A_WM_NAME = XInternAtom(dpy, "WM_NAME", False);
  A_XROOTPMAP_ID = XInternAtom(dpy, "_XROOTPMAP_ID", False);
  A_ESETROOT_PMAP_ID = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);
  A_WM_LAST_FOCUS_PER_WS = XInternAtom(dpy, "_WM_LAST_FOCUS_PER_WS", False);
  A_WM_DIALOG_OWNER = XInternAtom(dpy, "_WM_DIALOG_OWNER", False);
  A_WM_WORKSPACE_MODE = XInternAtom(dpy, "_WM_WORKSPACE_MODE", False);
  A_NET_FRAME_EXTENTS = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
}

static unsigned char *get_prop(Window w, Atom prop, Atom type,
                               unsigned long *nitems_out) {
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *data = NULL;
  if (XGetWindowProperty(dpy, w, prop, 0, ~0L, False, type, &actual_type,
                         &actual_format, &nitems, &bytes_after,
                         &data) != Success) {
    return NULL;
  }
  if (actual_type == None) {
    if (data)
      XFree(data);
    return NULL;
  }
  if (nitems_out)
    *nitems_out = nitems;
  return data;
}

static void load_last_focus_from_wm(void) {
  if (A_WM_LAST_FOCUS_PER_WS == None)
    return;
  unsigned long n;
  unsigned char *data = get_prop(root, A_WM_LAST_FOCUS_PER_WS, XA_WINDOW, &n);
  if (!data) {
    if (debug_enabled)
      fprintf(stderr,
              "[wsoverview] _WM_LAST_FOCUS_PER_WS: property missing/empty\n");
    return;
  }
  Window *arr = (Window *)data;
  unsigned long count = n < (unsigned long)MAX_DESKTOPS ? n : MAX_DESKTOPS;
  for (unsigned long i = 0; i < count; i++) {
    if (arr[i] != None) {
      if (debug_enabled && last_focused[i] != arr[i])
        fprintf(stderr, "[wsoverview] last_focused[%lu]: 0x%lx -> 0x%lx\n", i,
                (unsigned long)last_focused[i], (unsigned long)arr[i]);
      last_focused[i] = arr[i];
    }
  }
  XFree(data);
}

static void load_workspace_modes_from_wm(void) {
  if (A_WM_WORKSPACE_MODE == None)
    return;
  unsigned long n;
  unsigned char *data = get_prop(root, A_WM_WORKSPACE_MODE, XA_CARDINAL, &n);
  if (!data) {
    if (debug_enabled)
      fprintf(stderr,
              "[wsoverview] _WM_WORKSPACE_MODE: property missing/empty\n");
    return;
  }
  long *arr = (long *)data;
  unsigned long count = n < (unsigned long)MAX_DESKTOPS ? n : MAX_DESKTOPS;
  for (unsigned long i = 0; i < count; i++) {
    if (debug_enabled && workspace_mode[i] != (int)arr[i])
      fprintf(stderr, "[wsoverview] workspace_mode[%lu]: %d -> %ld\n", i,
              workspace_mode[i], arr[i]);
    workspace_mode[i] = (int)arr[i];
  }
  XFree(data);
}

static Window get_dialog_owner(Window w) {
  unsigned long n;
  unsigned char *data = get_prop(w, A_WM_DIALOG_OWNER, XA_WINDOW, &n);
  if (!data || n < 1)
    return None;
  Window owner = *(Window *)data;
  XFree(data);
  return owner;
}

static char *get_window_title(Window w) {
  static char buf[256];
  unsigned long n;
  unsigned char *data = get_prop(w, A_NET_WM_NAME, A_UTF8_STRING, &n);
  if (data) {
    snprintf(buf, sizeof(buf), "%.*s", (int)n, (char *)data);
    XFree(data);
    return buf;
  }
  char *name = NULL;
  if (XFetchName(dpy, w, &name) && name) {
    snprintf(buf, sizeof(buf), "%s", name);
    XFree(name);
    return buf;
  }
  snprintf(buf, sizeof(buf), "(untitled)");
  return buf;
}

static cairo_surface_t *get_window_icon(Window w) {
  unsigned long n;
  unsigned char *raw = get_prop(w, A_NET_WM_ICON, XA_CARDINAL, &n);
  if (!raw)
    return NULL;
  unsigned long *data = (unsigned long *)raw;
  unsigned long i = 0;
  long best_w = 0, best_h = 0;
  unsigned long best_off = 0;

  while (i + 2 <= n) {
    unsigned long iw = data[i], ih = data[i + 1];
    if (iw == 0 || ih == 0 || i + 2 + iw * ih > n)
      break;
    if ((long)(iw * ih) > best_w * best_h) {
      best_w = iw;
      best_h = ih;
      best_off = i + 2;
    }
    i += 2 + iw * ih;
  }
  if (best_w == 0) {
    XFree(raw);
    return NULL;
  }

  cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
  unsigned char *dst = cairo_image_surface_get_data(surf);
  int stride = cairo_image_surface_get_stride(surf);
  for (long y = 0; y < best_h; y++) {
    uint32_t *row = (uint32_t *)(dst + y * stride);
    for (long x = 0; x < best_w; x++) {
      unsigned long px = data[best_off + y * best_w + x];
      row[x] = (uint32_t)px;
    }
  }
  cairo_surface_mark_dirty(surf);
  XFree(raw);
  return surf;
}

static cairo_surface_t *get_wallpaper_surface(void) {
  unsigned long n;
  unsigned char *data = get_prop(root, A_XROOTPMAP_ID, XA_PIXMAP, &n);
  if (!data)
    data = get_prop(root, A_ESETROOT_PMAP_ID, XA_PIXMAP, &n);
  if (!data)
    return NULL;

  Pixmap pm = *(Pixmap *)data;
  XFree(data);
  if (pm == None)
    return NULL;

  Window rr;
  int px, py;
  unsigned int pw, ph, pbw, pdepth;
  if (!XGetGeometry(dpy, pm, &rr, &px, &py, &pw, &ph, &pbw, &pdepth))
    return NULL;

  cairo_surface_t *surf =
      cairo_xlib_surface_create(dpy, pm, root_visual, pw, ph);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return NULL;
  }
  wallpaper_w = (int)pw;
  wallpaper_h = (int)ph;
  return surf;
}

static Window find_xwinwrap_window(void) {
  Window rroot, parent, *kids = NULL;
  unsigned int nkids = 0;
  Window found = None;
  if (!XQueryTree(dpy, root, &rroot, &parent, &kids, &nkids))
    return None;
  for (unsigned int i = 0; i < nkids; i++) {
    if (kids[i] == overview)
      continue;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, kids[i], &wa))
      continue;
    if (!wa.override_redirect || wa.class == InputOnly)
      continue;
    if (wa.map_state != IsViewable)
      continue;
    if (wa.width < sw - 2 || wa.height < sh - 2)
      continue; // poori screen jaisi honi chahiye
    found = kids[i];
    break;
  }
  if (kids)
    XFree(kids);
  return found;
}

static int setup_live_wallpaper(void) {
  xwinwrap_win = find_xwinwrap_window();
  if (xwinwrap_win == None || !have_composite)
    return 0;

  XCompositeRedirectWindow(dpy, xwinwrap_win, CompositeRedirectAutomatic);

  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, xwinwrap_win, &wa))
    return 0;

  Pixmap pm = XCompositeNameWindowPixmap(dpy, xwinwrap_win);
  if (pm == None)
    return 0;

  cairo_surface_t *surf =
      cairo_xlib_surface_create(dpy, pm, wa.visual, wa.width, wa.height);
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    return 0;
  }

  if (wallpaper_surf)
    cairo_surface_destroy(wallpaper_surf);
  wallpaper_surf = surf;
  wallpaper_w = wa.width;
  wallpaper_h = wa.height;

  if (have_damage)
    xwinwrap_damage = XDamageCreate(dpy, xwinwrap_win, XDamageReportNonEmpty);

  return 1;
}

// Ek surface ko dst_w x dst_h par bilinear filter se downscale karta hai.
static cairo_surface_t *downscale_image_surface(cairo_surface_t *src, int src_w,
                                                int src_h, int dst_w,
                                                int dst_h) {
  cairo_surface_t *dst =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dst_w, dst_h);
  cairo_t *cr = cairo_create(dst);
  cairo_scale(cr, (double)dst_w / (double)src_w, (double)dst_h / (double)src_h);
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_destroy(cr);
  return dst;
}

// wallpaper_surf (full-res live X pixmap) ko ek dafa chhoti cached
// thumbnail image surface mein bana leta hai. Bade downscale ratio ko
// ek hi step mein karne se aliasing/quality loss hota hai, isliye
// baar baar aadha (mipmap style) karte hue target size tak pahunchte hain.
static void build_wallpaper_thumbnail(void) {
  if (wallpaper_thumb_surf) {
    cairo_surface_destroy(wallpaper_thumb_surf);
    wallpaper_thumb_surf = NULL;
  }
  if (!wallpaper_surf)
    return;

  int src_w = wallpaper_w > 0 ? wallpaper_w : sw;
  int src_h = wallpaper_h > 0 ? wallpaper_h : sh;
  const int target_w =
      480; // itni resolution kaafi hai chhote box previews ke liye
  int target_h = (int)((double)sh * target_w / (double)sw);
  if (target_h < 1)
    target_h = 1;

  if (sw <= target_w) {
    // wallpaper already chhota hai, seedha snapshot le lo
    wallpaper_thumb_surf =
        downscale_image_surface(wallpaper_surf, src_w, src_h, sw, sh);
    wallpaper_thumb_w = sw;
    wallpaper_thumb_h = sh;
    return;
  }

  cairo_surface_t *cur = wallpaper_surf;
  int cur_w = src_w, cur_h = src_h, owns_cur = 0;

  while (cur_w / 2 >= target_w && cur_h / 2 >= target_h) {
    int nw = cur_w / 2, nh = cur_h / 2;
    cairo_surface_t *next = downscale_image_surface(cur, cur_w, cur_h, nw, nh);
    if (owns_cur)
      cairo_surface_destroy(cur);
    cur = next;
    owns_cur = 1;
    cur_w = nw;
    cur_h = nh;
  }

  wallpaper_thumb_surf =
      downscale_image_surface(cur, cur_w, cur_h, target_w, target_h);
  if (owns_cur)
    cairo_surface_destroy(cur);

  wallpaper_thumb_w = target_w;
  wallpaper_thumb_h = target_h;
}


// ---- KDE/GNOME style blurred background ------------------------------
static void blur_pass(unsigned char *d, int w, int h, int stride, int r) {
  unsigned char *tmp = malloc((size_t)stride * h);
  if (!tmp)
    return;
  int win = 2 * r + 1;
  // horizontal
  for (int y = 0; y < h; y++) {
    unsigned char *row = d + (size_t)y * stride;
    unsigned char *o = tmp + (size_t)y * stride;
    for (int ch = 0; ch < 4; ch++) {
      long sum = 0;
      for (int k = -r; k <= r; k++) {
        int xx = k < 0 ? 0 : (k >= w ? w - 1 : k);
        sum += row[xx * 4 + ch];
      }
      for (int x = 0; x < w; x++) {
        o[x * 4 + ch] = (unsigned char)(sum / win);
        int add = x + r + 1, sub = x - r;
        if (add >= w) add = w - 1;
        if (sub < 0) sub = 0;
        sum += row[add * 4 + ch] - row[sub * 4 + ch];
      }
    }
  }
  // vertical
  for (int x = 0; x < w; x++) {
    for (int ch = 0; ch < 4; ch++) {
      long sum = 0;
      for (int k = -r; k <= r; k++) {
        int yy = k < 0 ? 0 : (k >= h ? h - 1 : k);
        sum += tmp[(size_t)yy * stride + x * 4 + ch];
      }
      for (int y = 0; y < h; y++) {
        d[(size_t)y * stride + x * 4 + ch] = (unsigned char)(sum / win);
        int add = y + r + 1, sub = y - r;
        if (add >= h) add = h - 1;
        if (sub < 0) sub = 0;
        sum += tmp[(size_t)add * stride + x * 4 + ch] -
               tmp[(size_t)sub * stride + x * 4 + ch];
      }
    }
  }
  free(tmp);
}

// wallpaper thumbnail ko chhota karke gaussian-jesa blur (3x box blur) karta
// hai, phir poori screen size tak smooth upscale + halka dark overlay bake
// kar ke cache kar leta hai. Live wallpaper mein har frame par rebuild na ho
// is liye ~250ms throttle hai.
static void build_blurred_background(void) {
  static struct timespec last = {0, 0};
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (wallpaper_blur_surf) {
    double el = (now.tv_sec - last.tv_sec) * 1000.0 +
                (now.tv_nsec - last.tv_nsec) / 1e6;
    if (el < 250.0)
      return;
  }
  last = now;
  if (!wallpaper_thumb_surf)
    return;

  int bw = 160;
  int bh = (int)((double)sh * bw / (double)sw);
  if (bh < 1)
    bh = 1;
  cairo_surface_t *small = downscale_image_surface(
      wallpaper_thumb_surf, wallpaper_thumb_w, wallpaper_thumb_h, bw, bh);
  cairo_surface_flush(small);
  unsigned char *d = cairo_image_surface_get_data(small);
  int stride = cairo_image_surface_get_stride(small);
  if (d) {
    for (int p = 0; p < 3; p++)
      blur_pass(d, bw, bh, stride, 5);
    cairo_surface_mark_dirty(small);
  }

  if (wallpaper_blur_surf)
    cairo_surface_destroy(wallpaper_blur_surf);
  wallpaper_blur_surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
  cairo_t *c = cairo_create(wallpaper_blur_surf);
  cairo_scale(c, (double)sw / bw, (double)sh / bh);
  cairo_set_source_surface(c, small, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend(cairo_get_source(c), CAIRO_EXTEND_PAD);
  cairo_paint(c);
  cairo_identity_matrix(c);
  cairo_set_source_rgba(c, 0, 0, 0, 0.38);
  cairo_paint(c);
  cairo_destroy(c);
  cairo_surface_destroy(small);
}

static void free_thumbnail(Client *c) {
  if (c->thumb_surf) {
    cairo_surface_destroy(c->thumb_surf);
    c->thumb_surf = NULL;
  }
  if (c->thumb_pixmap) {
    XFreePixmap(dpy, c->thumb_pixmap);
    c->thumb_pixmap = None;
  }
  if (c->damage) {
    XDamageDestroy(dpy, c->damage);
    c->damage = 0;
  }
}

static void get_window_thumbnail(Client *c) {
  c->thumb_pixmap = None;
  c->thumb_surf = NULL;
  c->damage = 0;
  if (!have_composite)
    return;

  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, c->win, &wa))
    return;
  c->real_w = wa.width;
  c->real_h = wa.height;

  Window child;
  int rx, ry;
  if (XTranslateCoordinates(dpy, c->win, root, 0, 0, &rx, &ry, &child)) {
    c->real_x = rx;
    c->real_y = ry;
  } else {
    c->real_x = wa.x;
    c->real_y = wa.y;
  }

  if (!c->redirected) {
    XCompositeRedirectWindow(dpy, c->win, CompositeRedirectAutomatic);
    c->redirected = 1;
  }

  XSelectInput(dpy, c->win, StructureNotifyMask);

  if (wa.map_state != IsViewable)
    return;

  Pixmap pm = XCompositeNameWindowPixmap(dpy, c->win);
  if (pm == None)
    return;

  XRenderPictFormat *xrfmt = XRenderFindVisualFormat(dpy, wa.visual);
  cairo_surface_t *surf;
  if (xrfmt) {
    surf = cairo_xlib_surface_create_with_xrender_format(
        dpy, pm, ScreenOfDisplay(dpy, scr), xrfmt, wa.width, wa.height);
  } else {
    surf = cairo_xlib_surface_create(dpy, pm, wa.visual, wa.width, wa.height);
  }
  if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surf);
    XFreePixmap(dpy, pm);
    return;
  }
  c->thumb_pixmap = pm;
  c->thumb_surf = surf;

  if (have_damage) {
    c->damage = XDamageCreate(dpy, c->win, XDamageReportNonEmpty);
  }
}

static void refresh_client_pixmap(Client *c) {
  free_thumbnail(c);
  get_window_thumbnail(c);
}

static void refresh_thumbnail(Client *c) {
  if (!c->thumb_surf)
    return;

  cairo_surface_flush(c->thumb_surf);
  cairo_surface_mark_dirty(c->thumb_surf);
}

static void load_desktop_names(void) {
  ndesktop_names = 0;
  unsigned long n;
  unsigned char *data = get_prop(root, A_NET_DESKTOP_NAMES, A_UTF8_STRING, &n);
  if (!data)
    return;
  char *p = (char *)data;
  unsigned long used = 0;
  while (used < n && ndesktop_names < MAX_DESKTOP_NAMES) {
    size_t len = strnlen(p, n - used);
    snprintf(desktop_names[ndesktop_names],
             sizeof(desktop_names[ndesktop_names]), "%.*s", (int)len, p);
    ndesktop_names++;
    p += len + 1;
    used += len + 1;
  }
  XFree(data);
}

static long get_window_desktop(Window w) {
  unsigned long n;
  unsigned char *data = get_prop(w, A_NET_WM_DESKTOP, XA_CARDINAL, &n);
  if (!data)
    return -1;
  long d = *(long *)data;
  XFree(data);
  return d;
}

// clients[] ka order _NET_CLIENT_LIST (mapping/launch order) se aata hai,
// jo REAL screen stacking (Z-order) nahi hota. Ye function XQueryTree se
// root ke bachon ka asal bottom-to-top stacking order leta hai aur har
// client ko uska real rank de deta hai, taake deskbar mini-boxes windows
// ko unki asal on-screen stacking ke mutabiq draw kar sakein (na ke
// launch-order ya "dialog hamesha sab se upar" wale forced rule se).
static void load_stacking_order(void) {
  for (int i = 0; i < nclients; i++)
    client_stack_rank[i] = i; // fallback: purana mapping-order behavior

  Window rroot, parent, *kids = NULL;
  unsigned int nkids = 0;
  if (!XQueryTree(dpy, root, &rroot, &parent, &kids, &nkids))
    return;

  // kids[] bottom-to-top order mein aata hai — jitni baad mein window
  // mile utni hi real screen par upar (front) hoti hai.
  for (unsigned int k = 0; k < nkids; k++) {
    for (int i = 0; i < nclients; i++) {
      if (clients[i].win == kids[k]) {
        client_stack_rank[i] = (int)k;
        break;
      }
    }
  }
  if (kids)
    XFree(kids);
}

static void load_clients(void) {
  unsigned long n;
  unsigned char *data = get_prop(root, A_NET_CLIENT_LIST, XA_WINDOW, &n);
  nclients = 0;
  if (data) {
    Window *wins = (Window *)data;
    for (unsigned long i = 0; i < n && nclients < MAX_CLIENTS; i++) {
      XWindowAttributes wa;
      if (!XGetWindowAttributes(dpy, wins[i], &wa))
        continue;
      if (wa.override_redirect)
        continue;
      Client *c = &clients[nclients++];
      c->win = wins[i];
      snprintf(c->title, sizeof(c->title), "%s", get_window_title(wins[i]));
      c->desktop = get_window_desktop(wins[i]);

      Window child;
      int rx, ry;

      if (XTranslateCoordinates(dpy, c->win, root, 0, 0, &rx, &ry, &child)) {

        c->real_x = rx;
        c->real_y = ry;

      } else {

        c->real_x = wa.x;
        c->real_y = wa.y;
      }

      c->real_w = wa.width;
      c->real_h = wa.height;
      c->icon = get_window_icon(wins[i]);
      c->dialog_owner_win = get_dialog_owner(wins[i]);
      c->is_dialog = (c->dialog_owner_win != None);
      c->dialog_owner_idx = -1;
      get_window_thumbnail(c);
    }
    XFree(data);
  } else {
    Window rroot, parent, *kids;
    unsigned int nkids;
    if (XQueryTree(dpy, root, &rroot, &parent, &kids, &nkids)) {
      for (unsigned int i = 0; i < nkids && nclients < MAX_CLIENTS; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, kids[i], &wa))
          continue;
        if (wa.override_redirect)
          continue;
        Client *c = &clients[nclients++];
        c->win = kids[i];
        snprintf(c->title, sizeof(c->title), "%s", get_window_title(kids[i]));
        c->desktop = get_window_desktop(kids[i]);
        Window child;
        int rx, ry;

        if (XTranslateCoordinates(dpy, c->win, root, 0, 0, &rx, &ry, &child)) {

          c->real_x = rx;
          c->real_y = ry;

        } else {

          c->real_x = wa.x;
          c->real_y = wa.y;
        }

        c->real_w = wa.width;
        c->real_h = wa.height;

        c->icon = get_window_icon(kids[i]);

        c->dialog_owner_win = get_dialog_owner(kids[i]);
        c->is_dialog = (c->dialog_owner_win != None);
        c->dialog_owner_idx = -1;

        get_window_thumbnail(c);
      }
      if (kids)
        XFree(kids);
    }
  }

  unsigned long dn;
  long num_desktops = 0;
  unsigned char *nd =
      get_prop(root, A_NET_NUMBER_OF_DESKTOPS, XA_CARDINAL, &dn);
  have_desktops = (nd != NULL);
  if (nd) {
    num_desktops = *(long *)nd;
    XFree(nd);
  }

  unsigned char *cd = get_prop(root, A_NET_CURRENT_DESKTOP, XA_CARDINAL, &dn);
  if (cd) {
    current_desktop = *(long *)cd;
    XFree(cd);
  }

  active_window = None;
  unsigned char *aw = get_prop(root, A_NET_ACTIVE_WINDOW, XA_WINDOW, &dn);
  if (aw) {
    active_window = *(Window *)aw;
    XFree(aw);
  }

  load_desktop_names();

  ngroups = 0;
  if (have_desktops && num_desktops > 0) {
    if (num_desktops > MAX_DESKTOPS)
      num_desktops = MAX_DESKTOPS;
    for (long d = 0; d < num_desktops; d++) {
      int gi = ngroups++;
      groups[gi].id = d;
      groups[gi].has_apps = 0;
      if (d < ndesktop_names && desktop_names[d][0])
        snprintf(groups[gi].name, sizeof(groups[gi].name), "%.127s",
                 desktop_names[d]);
      else
        snprintf(groups[gi].name, sizeof(groups[gi].name), "Desktop %ld",
                 d + 1);
    }

    int kept = 0;
    for (int i = 0; i < nclients; i++) {
      long d = clients[i].desktop;
      int known = (d >= 0 && d < num_desktops);
      if (!known) {
        free_thumbnail(&clients[i]);
        if (clients[i].icon) {
          cairo_surface_destroy(clients[i].icon);
          clients[i].icon = NULL;
        }
        if (have_composite && clients[i].redirected) {
          XCompositeUnredirectWindow(dpy, clients[i].win,
                                     CompositeRedirectAutomatic);
        }
        continue;
      }
      if (kept != i)
        clients[kept] = clients[i];
      kept++;
    }
    nclients = kept;
  } else {
    for (int i = 0; i < nclients; i++) {
      long d = clients[i].desktop;
      int gi = -1;
      for (int g = 0; g < ngroups; g++) {
        if (groups[g].id == d) {
          gi = g;
          break;
        }
      }
      if (gi < 0 && ngroups < MAX_DESKTOPS) {
        gi = ngroups++;
        groups[gi].id = d;
        groups[gi].has_apps = 0;
        if (d < 0)
          snprintf(groups[gi].name, sizeof(groups[gi].name), "Windows");
        else if (d < ndesktop_names && desktop_names[d][0])
          snprintf(groups[gi].name, sizeof(groups[gi].name), "%.127s",
                   desktop_names[d]);
        else
          snprintf(groups[gi].name, sizeof(groups[gi].name), "Desktop %ld",
                   d + 1);
      }
    }
  }
  if (ngroups == 0) {
    groups[0].id = -1;
    groups[0].has_apps = 0;
    snprintf(groups[0].name, sizeof(groups[0].name), "Windows");
    ngroups = 1;
  }

  selected_desktop = current_desktop;
  int found = 0;
  for (int g = 0; g < ngroups; g++) {
    if (groups[g].id == selected_desktop) {
      found = 1;
      break;
    }
  }
  if (!found)
    selected_desktop = groups[0].id;
  for (int i = 0; i < nclients; i++) {
    if (!clients[i].is_dialog)
      continue;
    clients[i].dialog_owner_idx = -1;
    for (int j = 0; j < nclients; j++) {
      if (j == i)
        continue;
      if (clients[j].win == clients[i].dialog_owner_win) {
        clients[i].dialog_owner_idx = j;
        break;
      }
    }
    if (clients[i].dialog_owner_idx < 0)
      clients[i].is_dialog = 0;
  }

  load_stacking_order();
}

// Window ke gird titlebar/border/frame ki real pixels mein extents nikalta
// hai. Pehle mini-box sirf client content (XTranslateCoordinates se) ke rect
// par bana tha, is liye agar WM ne window ko frame/titlebar/border ke andar
// rakha ho to preview mein window right aur top se chhoti ho kar gap deti
// thi (frame wali jagah wallpaper nazar aati thi). Ab frame ko bhi shamil
// karte hain.
static void update_frame_extents(Client *c, int rx, int ry, int rw, int rh,
                                 int client_bw) {
  long l = 0, r = 0, t = 0, b = 0;

  // 1) asal parent frame window (agar WM ne reparent kiya ho)
  Window w = c->win, parent = None, rroot, *kids = NULL;
  unsigned int nk = 0;
  int guard = 0;
  while (guard++ < 8 && XQueryTree(dpy, w, &rroot, &parent, &kids, &nk)) {
    if (kids)
      XFree(kids);
    kids = NULL;
    if (parent == None || parent == root)
      break;
    w = parent;
  }
  int have = 0;
  if (w != c->win) {
    XWindowAttributes fa;
    Window child;
    int fx, fy;
    if (XGetWindowAttributes(dpy, w, &fa) &&
        XTranslateCoordinates(dpy, w, root, 0, 0, &fx, &fy, &child)) {
      long fl = rx - fx + fa.border_width;
      long ft = ry - fy + fa.border_width;
      long fr = (fx + fa.width + fa.border_width) - (rx + rw);
      long fb = (fy + fa.height + fa.border_width) - (ry + rh);
      if (fl >= 0 && ft >= 0 && fr >= 0 && fb >= 0 && fl < 300 && ft < 300 &&
          fr < 300 && fb < 300) {
        l = fl; t = ft; r = fr; b = fb;
        have = 1;
      }
    }
  }

  // 2) _NET_FRAME_EXTENTS (left, right, top, bottom)
  if (!have && A_NET_FRAME_EXTENTS != None) {
    unsigned long n = 0;
    unsigned char *d = get_prop(c->win, A_NET_FRAME_EXTENTS, XA_CARDINAL, &n);
    if (d) {
      if (n >= 4) {
        long *e = (long *)d;
        l = e[0]; r = e[1]; t = e[2]; b = e[3];
      }
      XFree(d);
    }
  }

  // 3) client ka apna X border
  l += client_bw; r += client_bw; t += client_bw; b += client_bw;

  if (l < 0 || l > 300) l = 0;
  if (r < 0 || r > 300) r = 0;
  if (t < 0 || t > 300) t = 0;
  if (b < 0 || b > 300) b = 0;
  c->ext_l = (int)l; c->ext_r = (int)r; c->ext_t = (int)t; c->ext_b = (int)b;
}

// Dock/bar (polybar etc.) ki reserved top/bottom strip nikalta hai
// (_NET_WM_STRUT_PARTIAL / _NET_WM_STRUT). Deskbar boxes sirf is work
// area ko map karte hain, taake bar wali khali strip box mein gap na bane.
static void get_work_area_struts(long *top, long *bottom) {
  *top = 0;
  *bottom = 0;
  Atom a_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
  Atom a_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);

  Window r, p, *kids = NULL;
  unsigned int nk = 0;
  if (!XQueryTree(dpy, root, &r, &p, &kids, &nk))
    return;

  for (unsigned int i = 0; i < nk; i++) {
    unsigned long n = 0;
    unsigned char *d = get_prop(kids[i], a_partial, XA_CARDINAL, &n);
    if (!d || n < 4) {
      if (d)
        XFree(d);
      n = 0;
      d = get_prop(kids[i], a_strut, XA_CARDINAL, &n);
    }
    if (!d)
      continue;
    if (n >= 4) {
      long *v = (long *)d; // left, right, top, bottom
      if (v[2] > *top)
        *top = v[2];
      if (v[3] > *bottom)
        *bottom = v[3];
    }
    XFree(d);
  }
  if (kids)
    XFree(kids);

  // WM agar _NET_WORKAREA set karta hai to usme dock (cairo-dock/plank,
  // chahe strut na dein) ki reserved jagah already minus hoti hai.
  {
    Atom a_wa = XInternAtom(dpy, "_NET_WORKAREA", False);
    unsigned long n = 0;
    unsigned char *d = get_prop(root, a_wa, XA_CARDINAL, &n);
    if (d) {
      if (n >= 4) {
        long *v = (long *)d; // x, y, w, h
        long wt = v[1];
        long wb = (long)sh - (v[1] + v[3]);
        if (wt > *top && wt >= 0 && wt <= sh / 4)
          *top = wt;
        if (wb > *bottom && wb >= 0 && wb <= sh / 4)
          *bottom = wb;
      }
      XFree(d);
    }
  }

  if (*top < 0 || *top > sh / 4)
    *top = 0;
  if (*bottom < 0 || *bottom > sh / 4)
    *bottom = 0;
}

// Live desktop ki bari window se seekha hua bottom gap yaad rakhne ke liye
// (file cache). Jab live desktop ki window resize ho jaye to wo "bari
// window" wali shart (>=85% height) poori nahi karti aur reference gum ho
// jata tha — us surat mein baaki (resize na hue) workspaces ke boxes mein
// gap aa jata tha. Ab pichli dafa seekha hua gap file se wapas mil jata hai.
static void gap_cache_path(char *buf, size_t n) {
  snprintf(buf, n, "/tmp/wsoverview_gap3_%u_%dx%d", (unsigned)getuid(), sw,
           sh);
}

static long gap_cache_load(void) {
  char path[256];
  gap_cache_path(path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  long v = -1;
  if (fscanf(f, "%ld", &v) != 1)
    v = -1;
  fclose(f);
  if (v < 0 || v > sh / 4)
    return -1;
  return v;
}

static void gap_cache_save(long v) {
  char path[256];
  gap_cache_path(path, sizeof path);
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "%ld\n", v);
  fclose(f);
}

static void layout_desktop_bar(void) {
  /*
   * WM slide/workspace switching can move windows between their real screen
   * positions and their parked/off-screen positions without changing the
   * client list.  Keep real_x/real_y/real_w/real_h current before calculating
   * any desktop thumbnail.  In particular this prevents a popup/dialog on an
   * inactive workspace from being drawn at a stale position.
   */
  for (int i = 0; i < nclients; i++) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, clients[i].win, &wa))
      continue;

    Window child;
    int rx, ry;
    if (XTranslateCoordinates(dpy, clients[i].win, root, 0, 0, &rx, &ry,
                              &child)) {
      clients[i].real_x = rx;
      clients[i].real_y = ry;
    } else {
      clients[i].real_x = wa.x;
      clients[i].real_y = wa.y;
    }
    clients[i].real_w = wa.width;
    clients[i].real_h = wa.height;
    update_frame_extents(&clients[i], clients[i].real_x, clients[i].real_y,
                         wa.width, wa.height, wa.border_width);
    if (debug_enabled)
      fprintf(stderr, "[wsoverview] frame ext win=0x%lx l=%d r=%d t=%d b=%d\n",
              (unsigned long)clients[i].win, clients[i].ext_l, clients[i].ext_r,
              clients[i].ext_t, clients[i].ext_b);
  }

  /*
   * Live (current) desktop ki bari windows se bottom margin seekho — wahan
   * coordinates asal hote hain (bar/gap/border sab shamil). Inactive
   * (parked) workspaces ki windows ko isi margin par bottom se anchor karte
   * hain, taake un ke boxes mein bottom gap current wale jaisa hi rahe.
   */
  // Polybar (top/bottom ya dono) ki reserved strip pehle nikalo — live gap
  // ka reference isi ke hisaab se lete hain.
  long work_top = 0, work_bot = 0;
  get_work_area_struts(&work_top, &work_bot);
  long work_h = (long)sh - work_top - work_bot;
  if (work_h < sh / 2) {
    work_top = 0;
    work_bot = 0;
    work_h = sh;
  }

  int have_live_ref = 0;
  long live_gap_b = 0; // bottom bar + extra gap (screen bottom se)
  {
    // Sirf "extra" gap (window ka bar ke baad wala margin: tiling gap,
    // border waghera) track aur cache karte hain. Bar ki height alag se
    // har dafa strut se aati hai — is liye bar add/remove/resize karne
    // par purana cache galat bottom gap nahi deta (pehle top+bottom dono
    // polybar lagane par parked boxes mein gap aa jata tha).
    long extra = -1;
    long best_oh = 0;
    for (int i = 0; i < nclients; i++) {
      Client *c = &clients[i];
      if (!have_desktops || c->desktop != current_desktop || c->is_dialog ||
          c->real_w <= 0 || c->real_h <= 0)
        continue;
      long oh = (long)c->real_h + c->ext_t + c->ext_b;
      if (oh * 100 < work_h * 85)
        continue;
      long gb = (long)sh - ((long)c->real_y + c->real_h + c->ext_b);
      if (gb < 0 || gb > sh / 3)
        continue;
      long ex = gb - work_bot;
      if (ex < 0)
        ex = 0;
      // Reference = live desktop ki SAB SE BARI (maximized/tiled) window.
      // Pehle yahan "sab se chhota gap" + purani cache ka min liya jata
      // tha, jis se dock/bar badalne (e.g. cairo-dock jo apps ko neeche
      // se gap deta hai) ke baad purana chhota gap atak jata tha aur
      // doosre workspaces ke boxes mein apps neeche khisak kar upar gap
      // dikhate thay. Ab hamesha LIVE asal position se seekhte hain.
      if (oh > best_oh || (oh == best_oh && ex < extra)) {
        best_oh = oh;
        extra = ex;
      }
    }

    long cached = gap_cache_load();
    if (extra >= 0) {
      // Live window lagbhag poora work area bhar rahi ho (maximized/tiled)
      // to ye asal gap hai: cache ko OVERWRITE karo. Warna (sab live
      // windows resize hui hui hain) pichla bharosemand cache use karo.
      if (best_oh * 100 >= work_h * 92 || cached < 0) {
        if (cached != extra)
          gap_cache_save(extra);
      } else {
        extra = cached;
      }
    } else if (cached >= 0) {
      extra = cached;
    }
    if (extra >= 0) {
      live_gap_b = work_bot + extra;
      have_live_ref = 1;
    }
  }

  if (debug_enabled)
    fprintf(stderr,
            "[wsoverview] REF have_live_ref=%d live_gap_b=%ld work_top=%ld "
            "work_bot=%ld work_h=%ld sh=%d cur_desktop=%ld\n",
            have_live_ref, live_gap_b, work_top, work_bot, work_h, sh,
            current_desktop);

  int n = (ngroups > 0 ? ngroups : 1) + 1; // +1 = "+" tile (KDE jesa)
  double aspect = (double)sw / (double)sh;
  int box_h = DESKBAR_H;
  int box_w = (int)(box_h * aspect + 0.2);

  // Boxes LEFT par vertical column mein: har box ke neeche naam ki patti
  // (DESKLABEL_H) aur boxes ke darmiyan DESKBOX_PAD.
  int avail_h = sh - DESKBAR_Y * 2;
  int total_h = n * (box_h + DESKLABEL_H) + (n - 1) * DESKBOX_PAD;
  if (total_h > avail_h) {
    double scale = (double)(avail_h - n * DESKLABEL_H - (n - 1) * DESKBOX_PAD) /
                   (double)(n * box_h);
    if (scale < 0.05)
      scale = 0.05;
    box_h = (int)(box_h * scale + 0.5);
    box_w = (int)(box_w * scale + 0.5);
  }
  // Column screen ki width ka 20% se zyada na ho.
  if (box_w > (int)(sw * 0.20)) {
    double scale = (double)(sw * 0.20) / (double)box_w;
    box_h = (int)(box_h * scale + 0.5);
    box_w = (int)(box_w * scale + 0.5);
  }
  if (box_h < 40)
    box_h = 40;
  if (box_w < 60)
    box_w = 60;
  deskbar_box_h = box_h;
  deskbar_box_w = box_w;
  deskbar_x = DESKBAR_Y;

  total_h = n * (box_h + DESKLABEL_H) + (n - 1) * DESKBOX_PAD;
  int gy = (sh - total_h) / 2;
  if (gy < DESKBAR_Y)
    gy = DESKBAR_Y;
  deskbar_y = gy;

  for (int i = 0; i < nclients; i++) {
    clients[i].mini_w = 0;
    clients[i].mini_h = 0;
    clients[i].cmini_w = 0;
    clients[i].cmini_h = 0;
  }

  for (int g = 0; g < ngroups; g++) {
    groups[g].x = deskbar_x;
    groups[g].y = gy;
    groups[g].w = box_w;
    groups[g].h = box_h;
    gy += box_h + DESKLABEL_H + DESKBOX_PAD;

    int count = 0;

    for (int i = 0; i < nclients; i++) {
      if (clients[i].desktop == groups[g].id)
        count++;
    }

    groups[g].has_apps = (count > 0);

    if (count == 0)
      continue;

    int inner_x = groups[g].x + MINI_PAD;
    int inner_y = groups[g].y + MINI_PAD;

    int inner_w = groups[g].w - MINI_PAD * 2;
    int inner_h = groups[g].h - MINI_PAD * 2;

    if (inner_w < 1)
      inner_w = 1;

    if (inner_h < 1)
      inner_h = 1;

    long bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int have_bbox = 0;
    for (int i = 0; i < nclients; i++) {
      Client *c = &clients[i];
      if (c->desktop != groups[g].id || c->real_w <= 0 || c->real_h <= 0 ||
          mini_hidden[i])
        continue;
      if (!have_bbox) {
        bx0 = c->real_x;
        by0 = c->real_y;
        bx1 = c->real_x + c->real_w;
        by1 = c->real_y + c->real_h;
        have_bbox = 1;
      } else {
        if (c->real_x < bx0)
          bx0 = c->real_x;
        if (c->real_y < by0)
          by0 = c->real_y;
        if (c->real_x + c->real_w > bx1)
          bx1 = c->real_x + c->real_w;
        if (c->real_y + c->real_h > by1)
          by1 = c->real_y + c->real_h;
      }
    }

    long origin_x = 0, origin_y = 0;
    long span_w = sw, span_h = sh;
    if (have_desktops && groups[g].id == current_desktop) {
      origin_x = 0;
      origin_y = 0;
      span_w = sw;
      span_h = sh;
    } else if (have_desktops) {
      origin_x = (long)sw * (groups[g].id + 2);
      origin_y = 0;
      span_w = sw;
      span_h = sh;
    } else if (have_bbox && (bx0 < 0 || by0 < 0 || bx1 > sw || by1 > sh)) {
      origin_x = bx0;
      origin_y = by0;
      span_w = bx1 - bx0;
      span_h = by1 - by0;
      if (span_w < 1)
        span_w = 1;
      if (span_h < 1)
        span_h = 1;
    }

    if (debug_enabled) {
      fprintf(stderr,
              "[wsoverview] desktop=%ld (%s) bbox=(%ld,%ld)-(%ld,%ld) "
              "screen=%dx%d -> origin=(%ld,%ld) span=%ldx%ld %s\n",
              groups[g].id, groups[g].name, bx0, by0, bx1, by1, sw, sh,
              origin_x, origin_y, span_w, span_h,
              (origin_x != 0 || origin_y != 0 || span_w != sw || span_h != sh)
                  ? "[OFF-SCREEN MODE]"
                  : "[NORMAL SCREEN MODE]");
      for (int i = 0; i < nclients; i++) {
        Client *c = &clients[i];
        if (c->desktop != groups[g].id)
          continue;
        fprintf(stderr,
                "[wsoverview]   client win=0x%lx title=\"%s\" "
                "x=%d y=%d w=%d h=%d\n",
                (unsigned long)c->win, c->title, c->real_x, c->real_y,
                c->real_w, c->real_h);
      }
    }

    // Pehle yahan sirf uniform scale (aspect-preserve) use hota tha, jis
    // se agar box ka available inner area (MINI_TITLE_H/padding ki wajah
    // se) screen ke asal aspect ratio se thora mismatch hota, to preview
    // chhota reh jata aur left-right (ya top-bottom) khali gap aa jata
    // tha. Ab wallpaper preview ki tarah alag X/Y scale use kar ke box
    // ko poora fill kiya jata hai (auto-fit, koi gap nahi).
    double scale_x = (double)inner_w / (double)span_w;
    // Desktop-mode mein sirf work area (screen minus polybar strut) map
    // hota hai, is liye bar wali top strip box mein gap nahi banti.
    long map_top = have_desktops ? work_top : 0;
    double scale_y =
        (double)inner_h / (double)(have_desktops ? work_h : span_h);

    int preview_x = inner_x;
    int preview_y = inner_y;

    for (int i = 0; i < nclients; i++) {

      Client *c = &clients[i];

      if (c->desktop != groups[g].id)
        continue;

      if (c->real_w <= 0 || c->real_h <= 0)
        continue;

      if (mini_hidden[i])
        continue;

      // Pehle yahan dialogs (copy popup waghera) ke liye alag "budget"
      // based chhoti size force ki jati thi (owner box ka 60%, apna
      // alag dscale), lekin position (mini_x/mini_y) still asal
      // real_x/real_y se hi calculate hoti thi. Is wajah se dialog ka
      // size real proportion se match nahi karta tha, jis se box apni
      // asal (overview se pehle wali) jagah se thora shift/misaligned
      // dikhta tha. Ab dialogs bhi baaki normal windows ki tarah hi
      // sirf real_x/real_y aur scale_x/scale_y se calculate hote hain,
      // taake unki position+size hubahu wahi ho jo overview khulne se
      // pehle thi.
      int reel_mode = (groups[g].id >= 0 && groups[g].id < MAX_DESKTOPS &&
                       workspace_mode[groups[g].id] == 0);

      /*
       * SLIDE/REEL POSITION FIX
       *
       * Inactive slide workspaces mein WM window ko parked coordinate par
       * rakhta hai.  Parked X ko seedha preview mein map karna resized
       * windows ke liye reliable nahi hai: resize ke baad WM ka parked X
       * window ke current width/slot ke saath change ho sakta hai, jis se
       * thumbnail left edge par chala jata hai.
       *
       * The workspace itself is always one screen wide.  For an inactive
       * reel desktop, derive the preview position from the window's
       * workspace-local offset after removing the desktop parking origin.
       *
       * Keep the local X/Y offset, but clamp it to the actual workspace
       * bounds.  Do NOT center the window merely because the workspace is
       * inactive.  This makes a resized window keep the same relative
       * position it had on its real workspace.
       */
      long local_x = (long)c->real_x - origin_x;
      long local_y = (long)c->real_y - origin_y;

      // Ye "fit inside workspace" clamp SIRF inactive/parked reel
      // desktops ke liye chalni chahiye (jahan real_x/real_y ek parked
      // band-offset hai, real on-screen position nahi - dekhein upar
      // wala comment). Live/current desktop (ya tiling/floating mode)
      // ki windows ke real_x/real_y hamesha asal on-screen coordinates
      // hote hain, isliye wahan clamp NAHI lagani chahiye - warna agar
      // user window ko jaan-boojh kar screen se bahar (off-screen)
      // drag kare (jaise left/bottom edge se bahar), to preview use
      // galat tarah se poora box ke andar "fit" dikha deta hai, jabke
      // real screen par window clipped/cut-off hoti hai. Pehle yahan
      // reel_mode compute hota tha lekin is check mein use hi nahi
      // hota tha - isi wajah se ye bug current/live workspace par bhi
      // (jahan clamp bilkul zaroori nahi thi) laag ho raha tha.
      int is_live_group = (groups[g].id == current_desktop);
      if (reel_mode && !is_live_group) {
        if (local_x < 0)
          local_x = 0;
        if (local_y < 0)
          local_y = 0;

        /*
         * The right/bottom edge is the important part after a resize:
         * keep the complete window inside the workspace rather than letting
         * a changed width turn its X into a negative/left-clamped preview.
         */
        long max_x = (long)sw - c->real_w;
        long max_y = (long)sh - c->real_h;

        if (max_x < 0)
          max_x = 0;
        if (max_y < 0)
          max_y = 0;

        if (local_x > max_x)
          local_x = max_x;
        if (local_y > max_y)
          local_y = max_y;

        /*
         * INACTIVE (parked) WORKSPACE GAP FIX
         *
         * Current desktop ki windows ke coordinates asal hote hain, is liye
         * wahan box bilkul theek dikhta hai. Parked workspaces mein local_x
         * sirf parking-origin ghata kar reconstruct hota hai, jo WM ke
         * parking offset se thora off ho sakta hai - isi se right side par
         * (aur kabhi top par) gap aa jata tha. Jo window taqreeban poori
         * workspace jitni (frame samet >= 85%) ho, uski position asal mein
         * lagbhag centered hi hoti hai, is liye usay frame samet centre
         * kar dete hain; is se left/right (aur top/bottom) gap barabar
         * rehta hai aur koi akela one-sided gap nahi bachta. Chhoti windows
         * ki reconstructed position waise hi rehti hai.
         */
        long outer_w = (long)c->real_w + c->ext_l + c->ext_r;
        long outer_h = (long)c->real_h + c->ext_t + c->ext_b;
        if (outer_w * 100 >= (long)sw * 85)
          local_x = c->ext_l + ((long)sw - outer_w) / 2;
        // Live reference ho to bottom se anchor karo (live desktop wala
        // bottom margin). Reference na ho (e.g. live workspace par koi
        // bari window nahi) to pehle gap = 0 maan kar window ko screen ke
        // bottom se chipka diya jata tha, jis se dock/bar wale setup mein
        // doosre boxes mein neeche gap ghayab aur upar bara gap aa jata
        // tha. Ab us surat mein window ki asal real_y hi rehne dete hain.
        // DEBUG LOG se pata chala: parked windows ka real_y bilkul SAHI
        // hota hai (WM sirf X par park karta hai), jabke live_gap_b cache
        // se aaya purana gap tha (dock lagne se pehle ka) — anchor karne
        // se window 40 ki jagah 166 par chali jati thi (upar bara gap,
        // neeche gap ghayab). Isliye ab asal real_y par hi bharosa karte
        // hain; anchor sirf tab jab window ka neeche wala kinara screen se
        // bahar nikle (matlab real_y bharosemand nahi).
        if (have_live_ref && outer_h * 100 >= work_h * 85 &&
            local_y + (long)c->real_h + c->ext_b > (long)sh) {
          long gb = live_gap_b;
          local_y = (long)sh - gb - c->real_h - c->ext_b;
          if (local_y < c->ext_t)
            local_y = c->ext_t;
        }

        if (debug_enabled)
          fprintf(stderr,
                  "[wsoverview] parked desk=%ld win=0x%lx real=(%d,%d) "
                  "origin_x=%ld -> local=(%ld,%ld) size=%dx%d\n",
                  groups[g].id, (unsigned long)c->win, c->real_x, c->real_y,
                  origin_x, local_x, local_y, c->real_w, c->real_h);
      }

      /*
       * Pehle yahan mini_x/mini_w alag alag (int) truncate hote thay:
       * left edge neeche aur width neeche round hoti thi, is liye window
       * ka right edge 1-2px andar reh jata tha aur box ke right side par
       * gap aa jata tha (windows right se thori left lagti thin). Ab
       * left/right aur top/bottom EDGES ko alag alag nearest pixel par
       * round karte hain (width = right_edge - left_edge), aur agar koi
       * edge box ki kinaray ke 2px ke andar ho to usay exactly kinaray par
       * snap kar dete hain.
       */
      // Client content ke edges
      long ly = local_y - map_top;
      double cx0 = preview_x + (double)local_x * scale_x;
      double cy0 = preview_y + (double)ly * scale_y;
      double cx1 = preview_x + (double)(local_x + c->real_w) * scale_x;
      double cy1 = preview_y + (double)(ly + c->real_h) * scale_y;
      // OUTER (frame/titlebar/border samet) edges
      double fx0 = preview_x + (double)(local_x - c->ext_l) * scale_x;
      double fy0 = preview_y + (double)(ly - c->ext_t) * scale_y;
      double fx1 = preview_x + (double)(local_x + c->real_w + c->ext_r) * scale_x;
      double fy1 = preview_y + (double)(ly + c->real_h + c->ext_b) * scale_y;
      // Bar ke upar/neeche tak failay (fullscreen) windows box se bahar
      // na nikalein.
      {
        double ytop = inner_y, ybot = inner_y + inner_h;
        if (cy0 < ytop) cy0 = ytop;
        if (fy0 < ytop) fy0 = ytop;
        if (cy1 > ybot) cy1 = ybot;
        if (fy1 > ybot) fy1 = ybot;
      }

      const double snap = 2.0;
      double bx0_ = inner_x, by0_ = inner_y;
      double bx1_ = inner_x + inner_w, by1_ = inner_y + inner_h;
      if (fabs(fx0 - bx0_) < snap) fx0 = bx0_;
      if (fabs(fy0 - by0_) < snap) fy0 = by0_;
      if (fabs(fx1 - bx1_) < snap) fx1 = bx1_;
      if (fabs(fy1 - by1_) < snap) fy1 = by1_;
      if (fabs(cx0 - bx0_) < snap) cx0 = bx0_;
      if (fabs(cy0 - by0_) < snap) cy0 = by0_;
      if (fabs(cx1 - bx1_) < snap) cx1 = bx1_;
      if (fabs(cy1 - by1_) < snap) cy1 = by1_;

      c->mini_x = (int)floor(fx0 + 0.5);
      c->mini_y = (int)floor(fy0 + 0.5);
      c->mini_w = (int)floor(fx1 + 0.5) - c->mini_x;
      c->mini_h = (int)floor(fy1 + 0.5) - c->mini_y;
      c->cmini_x = (int)floor(cx0 + 0.5);
      c->cmini_y = (int)floor(cy0 + 0.5);
      c->cmini_w = (int)floor(cx1 + 0.5) - c->cmini_x;
      c->cmini_h = (int)floor(cy1 + 0.5) - c->cmini_y;

      if (c->cmini_w < 2) c->cmini_w = 2;
      if (c->cmini_h < 2) c->cmini_h = 2;
      if (c->mini_w < 2)
        c->mini_w = 2;

      if (c->mini_h < 2)
        c->mini_h = 2;
    }
  }
}

// Main window-grid wala rounded "panel" (KDE overview ka wallpaper wala
// bara rectangle). Deskbar + label + search ke neeche, sides par margin.
static void panel_rect(int *x, int *y, int *w, int *h) {
  // Boxes ab LEFT column mein hain: panel unke daayen taraf, baqi screen
  // mein (focus ring ke liye thori jagah chhor kar).
  *x = deskbar_x + deskbar_box_w + 36;
  *y = DESKBAR_Y;
  *w = sw - *x - (int)(sw * 0.04);
  *h = sh - *y - PANEL_BOTTOM;
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
}

// Cards ko order barqarar rakhte hue rows mein baantta hai: har row ki
// width limit se zyada na ho. row_start[] bharta hai, rows ki tadaad deta hai.
static int pack_rows(const double *nat_w, int count, double scale, double gap,
                     double limit, int *row_start) {
  int nrows = 0;
  double row_w = 0;
  row_start[0] = 0;
  for (int k = 0; k < count; k++) {
    double w = nat_w[k] * scale;
    if (k == row_start[nrows]) {
      row_w = w;
    } else if (row_w + gap + w <= limit) {
      row_w += gap + w;
    } else {
      nrows++;
      row_start[nrows] = k;
      row_w = w;
    }
  }
  nrows++;
  row_start[nrows] = count;
  return nrows;
}

static void layout_main_grid(void) {
  int pnx, pny, pnw, pnh;
  panel_rect(&pnx, &pny, &pnw, &pnh);
  int area_x = pnx + PANEL_INNER_PAD;
  int area_y = pny + PANEL_INNER_PAD;
  int area_w = pnw - PANEL_INNER_PAD * 2;
  int area_h = pnh - PANEL_INNER_PAD * 2;
  // Cards ke darmiyan gap screen ki width ke hisaab se (chhoti screen par tight,
  // badi par khula).
  double gap = area_w / 55.0;
  if (gap < 18) gap = 18;
  if (gap > gap + 8) gap = gap + 8;
  if (area_w < 1)
    area_w = 1;
  if (area_h < 1)
    area_h = 1;

  int idxs[MAX_CLIENTS];
  int count = 0;
  for (int i = 0; i < nclients; i++) {
    if (clients[i].desktop == selected_desktop)
      idxs[count++] = i;
    else {
      clients[i].w = 0;
      clients[i].h = 0;
    }
  }
  if (count == 0) {
    focused_client_idx = -1;
    return;
  }

  double nat_w[MAX_CLIENTS], nat_h[MAX_CLIENTS];
  for (int k = 0; k < count; k++) {
    Client *c = &clients[idxs[k]];
    if (c->real_w > 0 && c->real_h > 0) {
      nat_w[k] = c->real_w;
      nat_h[k] = c->real_h;
    } else {
      nat_w[k] = MAIN_CARD_W;
      nat_h[k] = MAIN_CARD_H;
    }
  }

  // *** FIX FOR SINGLE WINDOW ***
  if (count == 1) {
    Client *c = &clients[idxs[0]];

    // Window ki natural aspect ratio maintain karein
    double aspect_ratio = (double)nat_w[0] / nat_h[0];

    // 70% area tak, lekin natural size se 1.5x se zyada upscale nahi (warna
    // blurry lagti hai).
    double max_w = area_w * 0.70;
    double max_h = area_h * 0.70 - LABEL_H;

    double w = max_w;
    double h = w / aspect_ratio;
    if (h > max_h) {
      h = max_h;
      w = h * aspect_ratio;
    }
    if (w > nat_w[0] * 1.5) {
      w = nat_w[0] * 1.5;
      h = w / aspect_ratio;
    }

    // Minimum size limit
    if (w < 100)
      w = 100;
    if (h < 100)
      h = 100;

    // Center position
    c->w = (int)w;
    c->h = (int)h + LABEL_H;
    c->x = area_x + (area_w - c->w) / 2;
    c->y = area_y + (area_h - c->h) / 2;

    focused_client_idx = idxs[0];
    return;
  }
  // *** END FIX ***

  double lo = 0.02, hi = 4.0;
  for (int iter = 0; iter < 40; iter++) {
    double s = (lo + hi) / 2.0;
    double row_w = 0, row_h = 0, total_h = 0;
    int started = 0;
    for (int k = 0; k < count; k++) {
      double w = nat_w[k] * s, h = nat_h[k] * s + LABEL_H;
      if (!started) {
        row_w = w;
        row_h = h;
        started = 1;
      } else if (row_w + gap + w <= area_w) {
        row_w += gap + w;
        if (h > row_h)
          row_h = h;
      } else {
        total_h += row_h + gap;
        row_w = w;
        row_h = h;
      }
    }
    total_h += row_h;
    if (total_h <= area_h)
      lo = s;
    else
      hi = s;
  }

  double scale = lo;
  // Max scale limit
  double max_scale = 1.0; // KDE jesa: natural size se bara nahi
  if (scale > max_scale)
    scale = max_scale;
  if (scale < 0.03)
    scale = 0.03;

  int row_start[MAX_CLIENTS + 1];
  double row_h_arr[MAX_CLIENTS];
  int nrows = 0;
  double grid_h = 0;

  // FIX: pehle rows ko "balance" karne ke baad rows ki composition badal jati
  // thi (jaise ek tall/resized window ab doosri row mein chali jati), jis se
  // total height area_h se barh jati thi aur neeche wali apps panel (box) se
  // bahar nikal jati thin. Ab balance ke baad height check hoti hai: agar fit
  // na ho to unbalanced packing par wapas jate hain, aur phir bhi na ho to
  // scale ko ghata kar dobara try karte hain.
  for (int attempt = 0; attempt < 40; attempt++) {
    nrows = pack_rows(nat_w, count, scale, gap, area_w, row_start);
    int base_start[MAX_CLIENTS + 1];
    int base_rows = nrows;
    for (int i = 0; i <= base_rows; i++)
      base_start[i] = row_start[i];

    // Rows balance karo (rows ki tadaad wahi, minimum width limit dhoondo).
    double lo_w = 0, hi_w = area_w;
    for (int k = 0; k < count; k++)
      if (nat_w[k] * scale > lo_w)
        lo_w = nat_w[k] * scale;
    int tmp_start[MAX_CLIENTS + 1];
    for (int it = 0; it < 30; it++) {
      double mid = (lo_w + hi_w) / 2.0;
      if (pack_rows(nat_w, count, scale, gap, mid, tmp_start) <= base_rows)
        hi_w = mid;
      else
        lo_w = mid;
    }
    nrows = pack_rows(nat_w, count, scale, gap, hi_w, row_start);

    for (int pass = 0; pass < 2; pass++) {
      grid_h = 0;
      for (int r = 0; r < nrows; r++) {
        double rh = 0;
        for (int k = row_start[r]; k < row_start[r + 1]; k++) {
          double h = nat_h[k] * scale + LABEL_H;
          if (h > rh)
            rh = h;
        }
        row_h_arr[r] = rh;
        grid_h += rh;
      }
      grid_h += (nrows - 1) * gap;
      if (grid_h <= area_h || pass == 1)
        break;
      // balanced layout overflow kar rahi hai -> unbalanced par wapas
      nrows = base_rows;
      for (int i = 0; i <= base_rows; i++)
        row_start[i] = base_start[i];
    }

    if (grid_h <= area_h || scale <= 0.02)
      break;
    // abhi bhi fit nahi: scale ghatao aur dobara try karo
    double f = (area_h - (nrows - 1) * gap) / (grid_h - (nrows - 1) * gap);
    if (f > 0.98)
      f = 0.98;
    scale *= f;
  }

  double start_y = area_y + (area_h - grid_h) / 2.0;
  if (start_y < area_y)
    start_y = area_y;

  double cy = start_y;
  for (int r = 0; r < nrows; r++) {
    int first = row_start[r], last = row_start[r + 1];
    double row_w = 0;
    for (int k = first; k < last; k++) {
      row_w += nat_w[k] * scale;
      if (k > first)
        row_w += gap;
    }
    double cx = area_x + (area_w - row_w) / 2.0;
    for (int k = first; k < last; k++) {
      Client *c = &clients[idxs[k]];
      double w = nat_w[k] * scale, h = nat_h[k] * scale + LABEL_H;
      c->x = (int)cx;
      c->y = (int)(cy + (row_h_arr[r] - h) / 2.0);
      c->w = (int)w;
      c->h = (int)h;
      cx += w + gap;
    }
    cy += row_h_arr[r] + gap;
  }

  if (focused_client_idx < 0 || focused_client_idx >= nclients ||
      clients[focused_client_idx].desktop != selected_desktop ||
      clients[focused_client_idx].w <= 0 ||
      clients[focused_client_idx].h <= 0) {
    focused_client_idx = -1;

    // Pehle actual focused window (_NET_ACTIVE_WINDOW) dhoondo — sirf
    // "pehli mili" window mat le lo, warna frame hamesha sab se pehle
    // launch hui app par chala jata hai chahe focus kahin aur ho.
    for (int i = 0; i < nclients; i++) {
      if (clients[i].desktop == selected_desktop && clients[i].w > 0 &&
          clients[i].h > 0 && clients[i].win == active_window) {
        focused_client_idx = i;
        break;
      }
    }

    // active_window is desktop par nahi mili (ya set hi nahi thi) —
    // tabhi fallback k tor par pehli available window le lo.
    if (focused_client_idx < 0) {
      for (int i = 0; i < nclients; i++) {
        if (clients[i].desktop == selected_desktop && clients[i].w > 0 &&
            clients[i].h > 0) {
          focused_client_idx = i;
          break;
        }
      }
    }
  }
}

static void detect_slide_hidden(void) {
  for (int i = 0; i < nclients; i++)
    mini_hidden[i] = 0;

  for (int gi = 0; gi < ngroups; gi++) {
    long dsk_id = groups[gi].id;

    int dsk_count = 0;

    for (int i = 0; i < nclients; i++) {
      Client *c = &clients[i];

      if (c->desktop != dsk_id)
        continue;

      if (c->real_w <= 0 || c->real_h <= 0)
        continue;

      dsk_count++;
    }

    if (dsk_count >= 2) {

      int keep = -1;

      for (int i = 0; i < nclients; i++) {
        Client *c = &clients[i];

        if (c->desktop != dsk_id)
          continue;

        if (c->real_w <= 0 || c->real_h <= 0)
          continue;

        if (c->win == active_window) {
          keep = i;
          break;
        }
      }

      if (keep < 0 && dsk_id >= 0 && dsk_id < MAX_DESKTOPS) {

        Window remembered = last_focused[dsk_id];

        if (remembered != None) {

          for (int i = 0; i < nclients; i++) {

            Client *c = &clients[i];

            if (c->desktop != dsk_id)
              continue;

            if (c->real_w <= 0 || c->real_h <= 0)
              continue;

            if (c->win == remembered) {
              keep = i;
              break;
            }
          }
        }
      }

      if (keep < 0) {

        for (int i = 0; i < nclients; i++) {

          Client *c = &clients[i];

          if (c->desktop != dsk_id)
            continue;

          if (c->real_w <= 0 || c->real_h <= 0)
            continue;

          keep = i;
        }
      }

      // NOTE: pehle yahan "keep" window agar taqreeban fullscreen size ki
      // hoti (>=85% screen) to baaki saari sibling windows ko mini-box se
      // poori tarah hide kar deta tha (mini_hidden[i] = 1). Isi wajah se
      // jab koi transparent app (kitty/wezterm) apne peeche wali app jesi
      // (ya us se bari) size ki hoti thi, wo fullscreen-jaisi samjhi jati
      // aur peeche wali app poori tarah gayab ho jati thi — jis se
      // transparent front-app ka box bilkul khali/see-through dikhta tha,
      // jaise koi app hi maujood na ho. Ab siblings ko mini-box se hide
      // nahi kiya jata — har window apni asal jagah par dikhai degi, chahe
      // front wali window kitni bhi bari kyun na ho.
      (void)keep;
    }
  }
}

static void layout(void) {
  layout_desktop_bar();
  layout_main_grid();
}

static double ease_out_cubic(double t) {
  double p = t - 1.0;
  return p * p * p + 1.0;
}

// Ek animation ka 0..1 (eased) progress deta hai, elapsed time start_ts se
// naap kar. duration puri ho chuki ho to *active_flag ko khud 0 kar deta
// hai (agla frame se woh animation skip ho jaye).
static double anim_progress(struct timespec *start_ts, double duration_ms,
                            int *active_flag) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double elapsed_ms = (now.tv_sec - start_ts->tv_sec) * 1000.0 +
                      (now.tv_nsec - start_ts->tv_nsec) / 1000000.0;
  double t = elapsed_ms / duration_ms;
  if (t >= 1.0) {
    if (active_flag)
      *active_flag = 0;
    return 1.0;
  }
  return ease_out_cubic(t);
}

static double get_launch_anim_progress(void) {
  if (!launch_anim_active)
    return 1.0;
  return anim_progress(&launch_anim_start_ts, LAUNCH_ANIM_MS,
                       &launch_anim_active);
}

static double get_grid_anim_progress(void) {
  if (!grid_anim_active)
    return 1.0;
  return anim_progress(&grid_anim_start_ts, GRID_ANIM_MS, &grid_anim_active);
}

// Koi bhi animation (selection slide, grid pop-in, launch fade) chal rahi
// ho to true — event loop isay fast (~60fps) pacing ke liye check karta
// hai.
static int any_anim_active(void) {
  return sel_anim_active || grid_anim_active || launch_anim_active;
}

static int find_group_index(long id) {
  for (int g = 0; g < ngroups; g++)
    if (groups[g].id == id)
      return g;
  return -1;
}

// Selection-highlight ka abhi ka animated rectangle nikalta hai — agar
// slide chal rahi ho to from->to ke beech interpolate karta hai, warna
// seedha selected_desktop wali box ka rect deta hai. Animation khatam ho
// chuki ho to yahin sel_anim_active clear kar deta hai.
static int get_selection_anim_rect(double *ox, double *oy, double *ow,
                                   double *oh) {
  int gi = find_group_index(selected_desktop);
  if (gi < 0)
    return 0;
  if (!sel_anim_active) {
    *ox = groups[gi].x;
    *oy = groups[gi].y;
    *ow = groups[gi].w;
    *oh = groups[gi].h;
    return 1;
  }
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double elapsed_ms = (now.tv_sec - sel_anim_start_ts.tv_sec) * 1000.0 +
                      (now.tv_nsec - sel_anim_start_ts.tv_nsec) / 1000000.0;
  double t = elapsed_ms / SEL_ANIM_MS;
  if (t >= 1.0) {
    sel_anim_active = 0;
    *ox = groups[gi].x;
    *oy = groups[gi].y;
    *ow = groups[gi].w;
    *oh = groups[gi].h;
    return 1;
  }
  double e = ease_out_cubic(t);
  *ox = sel_anim_from_x + (sel_anim_to_x - sel_anim_from_x) * e;
  *oy = sel_anim_from_y + (sel_anim_to_y - sel_anim_from_y) * e;
  *ow = sel_anim_from_w + (sel_anim_to_w - sel_anim_from_w) * e;
  *oh = sel_anim_from_h + (sel_anim_to_h - sel_anim_from_h) * e;
  return 1;
}

// selected_desktop ko new_id par change karta hai aur highlight slider ko
// purani box se nayi box ki taraf animate karna shuru karta hai. Ye har
// jagah direct "selected_desktop = X" assignment ki jagah use hota hai
// taake switch hamesha animate ho.
static void set_selected_desktop(long new_id) {
  if (new_id == selected_desktop)
    return;

  double fx, fy, fw, fh;
  if (!get_selection_anim_rect(&fx, &fy, &fw, &fh)) {
    fx = fy = fw = fh = 0;
  }

  selected_desktop = new_id;

  int ngi = find_group_index(selected_desktop);
  if (ngi >= 0) {
    sel_anim_from_x = fx;
    sel_anim_from_y = fy;
    sel_anim_from_w = fw;
    sel_anim_from_h = fh;
    sel_anim_to_x = groups[ngi].x;
    sel_anim_to_y = groups[ngi].y;
    sel_anim_to_w = groups[ngi].w;
    sel_anim_to_h = groups[ngi].h;
    clock_gettime(CLOCK_MONOTONIC, &sel_anim_start_ts);
    sel_anim_active = 1;
    grid_anim_start_ts = sel_anim_start_ts;
    grid_anim_active = 1;
  }
}

static int desktop_box_at(int x, int y, int *hit);

static void rounded_rect(cairo_t *c, double x, double y, double w, double h,
                         double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(c);
  cairo_arc(c, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(c, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(c);
}

// Text center mein, maxw se lamba ho to UTF-8 safe "..." truncate, halka shadow.
static void draw_text_center(cairo_t *c, const char *txt, double cx,
                             double baseline, double size, double maxw,
                             double alpha, int bold) {
  cairo_save(c);
  cairo_select_font_face(c, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         bold ? CAIRO_FONT_WEIGHT_BOLD
                              : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(c, size);
  char buf[300], tmp[320];
  snprintf(buf, sizeof buf, "%s", txt ? txt : "");
  snprintf(tmp, sizeof tmp, "%s", buf);
  cairo_text_extents_t e;
  cairo_text_extents(c, tmp, &e);
  size_t len = strlen(buf);
  while (e.x_advance > maxw && len > 0) {
    len--;
    while (len > 0 && (buf[len] & 0xC0) == 0x80)
      len--;
    buf[len] = 0;
    snprintf(tmp, sizeof tmp, "%s...", buf);
    cairo_text_extents(c, tmp, &e);
  }
  double x = cx - e.x_advance / 2.0;
  cairo_set_source_rgba(c, 0, 0, 0, 0.55 * alpha);
  cairo_move_to(c, x + 1, baseline + 1);
  cairo_show_text(c, tmp);
  cairo_set_source_rgba(c, 1, 1, 1, alpha);
  cairo_move_to(c, x, baseline);
  cairo_show_text(c, tmp);
  cairo_restore(c);
}

// Card ke andar asal thumbnail ka rect (neeche LABEL_H title/icon ke liye).
static void card_thumb_rect(const Client *c, double *x, double *y, double *w,
                            double *h) {
  double ax = c->x, ay = c->y;
  double aw = c->w, ah = c->h - LABEL_H;
  if (ah < 4) ah = 4;
  if (c->real_w > 0 && c->real_h > 0) {
    double s = aw / c->real_w;
    double s2 = ah / c->real_h;
    if (s2 < s) s = s2;
    *w = c->real_w * s;
    *h = c->real_h * s;
  } else {
    *w = aw;
    *h = ah;
  }
  *x = ax + (aw - *w) / 2.0;
  *y = ay + (ah - *h) / 2.0;
}

// Wallpaper ko poori screen (sw x sh) par bilkul usi tarah paint karta hai
// jaise real screen par dikhta hai: wallpaper ki asal size se screen size
// tak scale hota hai (na zoom, na crop). Panel aur blurred background dono
// isi coordinate mapping par chalte hain, is liye panel ke andar wallpaper
// blur wale hisse se pixel-to-pixel align rehta hai.
static void paint_wallpaper_rect(cairo_t *c, double x, double y, double w,
                                 double h) {
  if (!wallpaper_surf)
    return;
  int ww = wallpaper_w > 0 ? wallpaper_w : sw;
  int wh = wallpaper_h > 0 ? wallpaper_h : sh;
  cairo_save(c);
  cairo_translate(c, x, y);
  cairo_scale(c, w / ww, h / wh);
  cairo_set_source_surface(c, wallpaper_surf, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
  cairo_paint(c);
  cairo_restore(c);
}

static void paint_wallpaper_screen(cairo_t *c) {
  paint_wallpaper_rect(c, 0, 0, sw, sh);
}

static void draw(void) {
  /*
   * IMPORTANT: X stacking changes independently of _NET_CLIENT_LIST and can
   * change during workspace switches, focus changes and popup creation.
   * Always take a fresh bottom-to-top snapshot immediately before drawing the
   * mini desktop previews.  The preview renderer below sorts by this rank,
   * so the popup/dialog is painted exactly where it is stacked on the real
   * desktop instead of using stale launch-order information.
   */
  XSync(dpy, False);
  load_stacking_order();

  int launching = launch_anim_active;
  double lp = get_launch_anim_progress();
  double lscale = 0.94 + 0.06 * lp;
  double lalpha = lp;
  if (launching) {
    cairo_save(bcr);
    cairo_translate(bcr, sw / 2.0, sh / 2.0);
    cairo_scale(bcr, lscale, lscale);
    cairo_translate(bcr, -sw / 2.0, -sh / 2.0);
    cairo_push_group(bcr);
  }

  // 1) poori screen par blurred+dimmed wallpaper (KDE/GNOME overview jesa)
  if (wallpaper_blur_surf) {
    cairo_save(bcr);
    cairo_set_source_surface(bcr, wallpaper_blur_surf, 0, 0);
    cairo_paint(bcr);
    cairo_restore(bcr);
  } else if (wallpaper_surf) {
    cairo_save(bcr);
    paint_wallpaper_screen(bcr);
    cairo_set_source_rgba(bcr, 0, 0, 0, 0.55);
    cairo_paint(bcr);
    cairo_restore(bcr);
  } else {
    cairo_set_source_rgba(bcr, 0, 0, 0, 0.85);
    cairo_paint(bcr);
  }

  // 2) beech mein rounded panel: asal (sharp) wallpaper, sides par blur dikhe
  {
    int px, py, pw, ph;
    panel_rect(&px, &py, &pw, &ph);
    for (int s = 3; s >= 1; s--) { // soft shadow
      cairo_set_source_rgba(bcr, 0, 0, 0, 0.10);
      rounded_rect(bcr, px - s * 3, py - s * 2, pw + s * 6, ph + s * 6,
                   PANEL_RADIUS + s * 3);
      cairo_fill(bcr);
    }
    cairo_save(bcr);
    rounded_rect(bcr, px, py, pw, ph, PANEL_RADIUS);
    cairo_clip(bcr);
    if (wallpaper_surf) {
      // Poora wallpaper panel ke andar scale hokar fit (GNOME/KDE jesa),
      // pehle ye 1:1 screen coordinates par paint hota tha is liye sirf
      // beech ka zoomed hissa nazar aata tha.
      paint_wallpaper_rect(bcr, px, py, pw, ph);
      cairo_set_source_rgba(bcr, 0, 0, 0, 0.18);
      cairo_paint(bcr);
    } else {
      cairo_set_source_rgba(bcr, 0.08, 0.08, 0.12, 0.9);
      cairo_paint(bcr);
    }
    cairo_restore(bcr);
    cairo_set_source_rgba(bcr, 1, 1, 1, 0.14);
    cairo_set_line_width(bcr, 1.0);
    rounded_rect(bcr, px + 0.5, py + 0.5, pw - 1, ph - 1, PANEL_RADIUS);
    cairo_stroke(bcr);
  }

  for (int g = 0; g < ngroups; g++) {
    Group *grp = &groups[g];
    int selected = grp->id == selected_desktop;

    if (dnd_active && dnd_moved) {
      int hit = 0;
      int hg = desktop_box_at(dnd_cur_x, dnd_cur_y, &hit);
      if (hit && hg == g) {
        cairo_set_source_rgba(bcr, 1.0, 0.85, 0.35, 1.0);
        cairo_set_line_width(bcr, 3.0);
        rounded_rect(bcr, grp->x - 1.5, grp->y - 1.5, grp->w + 3, grp->h + 3,
                     DESKBOX_RADIUS + 2);
        cairo_stroke(bcr);
      }
    }

    // box ke neeche halka shadow
    for (int sh_i = 4; sh_i >= 1; sh_i--) {
      cairo_set_source_rgba(bcr, 0, 0, 0, 0.06);
      rounded_rect(bcr, grp->x - sh_i, grp->y - sh_i + 3, grp->w + sh_i * 2,
                   grp->h + sh_i * 2, DESKBOX_RADIUS + sh_i);
      cairo_fill(bcr);
    }

    // Box ka poora content (wallpaper + mini windows) ek group mein draw hota
    // hai taake opacity ek saath lage (windows aapas mein se na jhalken).
    // Group sirf tab jab opacity sach mein <1 ho. Opacity 1.0 par intermediate
    // group surface se resample/blur ka risk aata hai, is liye seedha draw.
    const double box_alpha =
        selected ? DESKBOX_OPACITY_SELECTED : DESKBOX_OPACITY;
    const int use_group = box_alpha < 0.999;
    if (use_group)
      cairo_push_group(bcr);

    if (wallpaper_thumb_surf) {
      int inner_x = grp->x + MINI_PAD;
      int inner_y = grp->y + MINI_PAD;
      int inner_w = grp->w - MINI_PAD * 2;
      int inner_h = grp->h - MINI_PAD * 2;
      if (inner_w > 0 && inner_h > 0) {
        double sxw = (double)inner_w / (double)wallpaper_thumb_w;
        double syh = (double)inner_h / (double)wallpaper_thumb_h;
        cairo_save(bcr);
        rounded_rect(bcr, inner_x, inner_y, inner_w, inner_h, DESKBOX_RADIUS);
        cairo_clip(bcr);
        cairo_translate(bcr, inner_x, inner_y);
        cairo_scale(bcr, sxw, syh);
        cairo_set_source_surface(bcr, wallpaper_thumb_surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_GOOD);
        // cairo_pattern_set_filter(cairo_get_source(bcr),
        //                          CAIRO_FILTER_GOOD);
        cairo_paint(bcr);
        cairo_restore(bcr);
      }
    }

    int mini_clip_x = grp->x + MINI_PAD;
    int mini_clip_y = grp->y + MINI_PAD;
    int mini_clip_w = grp->w - MINI_PAD * 2;
    int mini_clip_h = grp->h - MINI_PAD * 2;
    if (mini_clip_w < 0)
      mini_clip_w = 0;
    if (mini_clip_h < 0)
      mini_clip_h = 0;

    cairo_save(bcr);
    rounded_rect(bcr, mini_clip_x, mini_clip_y, mini_clip_w, mini_clip_h,
                 DESKBOX_RADIUS);
    cairo_clip(bcr);

    // Overlapping (floating) windows pehle sirf array/launch-order mein
    // draw hoti thi, is liye jo app baad mein khuli hoti wahi hamesha
    // upar (on top) nazar aati thi — chahe focus kahin aur switch ho
    // chuka ho. Ab is group ke liye jo window abhi actually focused/
    // last-focused hai usay sab se aakhir mein (yani sab se upar) draw
    // karte hain, taake overview drawing bhi real focus ke mutabiq ho.
    int top_idx = -1;
    for (int i = 0; i < nclients; i++) {
      Client *c = &clients[i];
      if (c->desktop != grp->id || c->mini_w <= 0 || c->mini_h <= 0 ||
          mini_hidden[i])
        continue;
      if (c->win == active_window) {
        top_idx = i;
        break;
      }
    }
    if (top_idx < 0 && grp->id >= 0 && grp->id < MAX_DESKTOPS) {
      Window remembered = last_focused[grp->id];
      if (remembered != None) {
        for (int i = 0; i < nclients; i++) {
          Client *c = &clients[i];
          if (c->desktop != grp->id || c->mini_w <= 0 || c->mini_h <= 0 ||
              mini_hidden[i])
            continue;
          if (c->win == remembered) {
            top_idx = i;
            break;
          }
        }
      }
      if (debug_enabled)
        fprintf(stderr,
                "[wsoverview] group desk=%ld remembered=0x%lx -> top_idx=%d\n",
                grp->id, (unsigned long)remembered, top_idx);
    }

    // Agar jo window "focused/last-focused" nikli wo khud ek dialog hai
    // (polkit, file-chooser, copy-popup waghera - jab wo khuli ho to
    // active_window WM ki taraf se usi par set hota hai), to top_idx ko
    // uski OWNER app par redirect kar do. Warna neeche wala reel-mode
    // single-slide filter sirf dialog ko draw karta aur asli app (jiske
    // uper ye dialog khuli hai) ko top box se poori tarah gayab kar deta
    // - "popup ke peeche app show nahi hoti" wala bug isi se aata tha.
    if (top_idx >= 0 && clients[top_idx].is_dialog &&
        clients[top_idx].dialog_owner_idx >= 0) {
      top_idx = clients[top_idx].dialog_owner_idx;
    }

    // Reel/slide mode workspaces (tiling na floating) mein sirf ek
    // window "asal mein dikhti" hai screen par - baaki reel ki windows
    // off-screen parked hoti hain. Isliye yahan bhi sirf focused/
    // last-focused (top_idx) window dikhao, baaki group ke windows
    // skip kar do. Tiling aur floating mode (aur unknown/-1, purane
    // wm.c ke sath backward-compat ke liye) untouched rehte hain -
    // wahan jaisa pehle tha waisa hi sab windows dikhte hain.
    int reel_mode = (grp->id >= 0 && grp->id < MAX_DESKTOPS &&
                     workspace_mode[grp->id] == 0);

    // PEHLE yahan do passes chalte thay: pehle pass mein sab non-dialog
    // windows apne _NET_CLIENT_LIST (launch/mapping) order mein draw hote
    // thay, phir dusre pass mein HAR dialog zabardasti sab se aakhir (sab
    // ke upar) draw kiya jata tha — chahe real screen par woh dialog us
    // "teesri" (unrelated) app ke NEECHE hi kyun na ho. Isi wajah se agar
    // koi popup (file-manager ka copy-dialog waghera) ghaseet kar kisi
    // teesri app ke upar rakha jata, to us teesri app ka mini-box hissa
    // force-overwrite ho jata tha, aur floating mode mein beech wali app
    // popup+owner ke darmiyan chhup jati thi — kyunke draw order REAL
    // on-screen stacking (Z-order) follow nahi karta tha, sirf "dialog
    // hamesha upar" wala hardcoded rule follow karta tha.
    //
    // Ab sirf ek pass mein, is group ke visible clients ko unke REAL
    // stacking rank (client_stack_rank, XQueryTree se — load_stacking_order
    // dekhein) ke mutabiq sort karke bottom-to-top draw karte hain. Isi se
    // mini-box bilkul wahi dikhata hai jo real screen par actually stacked
    // hai — koi bhi window force-on-top ya force-hide nahi hoti.
    int draw_order[MAX_CLIENTS];
    int ndraw = 0;

    /*
     * Reel/slide mode normally shows only the current slide.  A dialog is
     * different: it can be dragged away from its owner and placed over any
     * other app.  In that case the deskbar thumbnail must contain the actual
     * visible stack: the current app, the dialog, and the app physically
     * underneath that dialog — aur sirf WOHI app, koi aur nahi.
     *
     * IMPORTANT FIX: real_x/real_y/real_w/real_h ka rect-overlap test SIRF
     * us group ke liye meaningful hai jo abhi actually current_desktop hai —
     * kyunke sirf usi ki windows real screen par wahin hoti hain jahan
     * unke reported coordinates kehte hain. Baaki (background/inactive)
     * reel workspaces ki windows sirf off-screen "parked" hoti hain (dekhein
     * switch_workspace() mein local_offset logic) — wahan real_x sirf ek
     * collision-avoidance band-offset hai, real visual stacking nahi. Agar
     * wahan bhi ye overlap-test chalaya jaye to, chunke reel ki har window
     * taqreeban poori screen-width ki hoti hai aur sirf thore se slot_w
     * offset par park hoti hai, EK dialog ka parked rect us group ki
     * KAI (ya sab) non-focused parked windows se overlap kar jata hai —
     * nateeja: background workspaces ke mini-box mein sab apps ek dusre
     * ke age-peeche/overlap hoke ghalat dikhti hain, jabke current
     * workspace theek dikhta hai (kyunke wahan coordinates real hote hain).
     *
     * Fix: overlap-test SIRF current_desktop group ke liye chalao (jahan
     * coordinates live/trustworthy hain). Jo bhi app wahan dialog ke
     * "real" neeche milti hai, us association ko turant _WM_DIALOG_OWNER
     * X property mein persist (save) kar dete hain — taake ye "asal stack"
     * (popup ab kis app ke upar hai) window-manager-level pe yaad rahe,
     * chahe workspace baad mein background/parked ho jaye. Background
     * groups ke liye is persisted owner (dialog_owner_idx) ke alawa koi
     * coordinate-guessing nahi karte — is se sirf WOHI app + uska popup
     * show hota hai jis par popup asal mein tha, baaki sab clean rehte hain.
     */
    int dialog_indices[MAX_CLIENTS];
    int ndialogs = 0;
    for (int di = 0; di < nclients; di++) {
      Client *dc = &clients[di];
      if (dc->desktop != grp->id || dc->mini_w <= 0 || dc->mini_h <= 0 ||
          mini_hidden[di] || !dc->is_dialog || dc->real_w <= 0 ||
          dc->real_h <= 0)
        continue;
      dialog_indices[ndialogs++] = di;
    }

    int is_live_group = (grp->id == current_desktop);

    if (reel_mode && is_live_group) {
      // Sirf yahan (live/current desktop) real coordinates par bharosa
      // karke dialog ke "asal neeche wali app" dhoondo, aur agar wo
      // pehle se stored dialog_owner_idx se ALAG hai (yani popup drag
      // hoke kisi doosri app par chala gaya hai), to naya owner turant
      // persist kar do taake background mein bhi ye sahi rahe.
      for (int dk = 0; dk < ndialogs; dk++) {
        Client *dc = &clients[dialog_indices[dk]];
        int dx1 = dc->real_x;
        int dy1 = dc->real_y;
        int dx2 = dc->real_x + dc->real_w;
        int dy2 = dc->real_y + dc->real_h;

        int best_idx = -1;
        int best_rank = -1;
        for (int i = 0; i < nclients; i++) {
          Client *cc = &clients[i];
          if (i == dialog_indices[dk] || cc->desktop != grp->id ||
              cc->is_dialog || cc->real_w <= 0 || cc->real_h <= 0)
            continue;
          int ax1 = cc->real_x;
          int ay1 = cc->real_y;
          int ax2 = cc->real_x + cc->real_w;
          int ay2 = cc->real_y + cc->real_h;
          if (ax1 < dx2 && ax2 > dx1 && ay1 < dy2 && ay2 > dy1) {
            // Agar ek se zyada apps overlap karein (rare), to jo real
            // Z-order mein sab se upar (dialog ke sab se qareeb) ho
            // wahi asal "neeche wali" app hai.
            if (client_stack_rank[i] > best_rank) {
              best_rank = client_stack_rank[i];
              best_idx = i;
            }
          }
        }

        if (best_idx >= 0 && best_idx != dc->dialog_owner_idx) {
          dc->dialog_owner_idx = best_idx;
          dc->dialog_owner_win = clients[best_idx].win;
          XChangeProperty(dpy, dc->win, A_WM_DIALOG_OWNER, XA_WINDOW, 32,
                          PropModeReplace,
                          (unsigned char *)&dc->dialog_owner_win, 1);
        }
      }
    }

    for (int i = 0; i < nclients; i++) {
      Client *cc = &clients[i];
      int keep_for_reel = (i == top_idx);

      if (reel_mode && !keep_for_reel) {
        if (cc->is_dialog) {
          // Dialog khud hamesha dikhti hai.
          keep_for_reel = 1;
        } else {
          // Ye app tab hi dikhegi jab kisi dialog ka (persisted, sirf
          // live-desktop overlap se update hua) owner isi app par ho —
          // koi live coordinate-guessing background groups ke liye
          // nahi hoti, is se ghalat multi-app overlap nahi banta.
          for (int dk = 0; dk < ndialogs; dk++) {
            Client *dc = &clients[dialog_indices[dk]];
            if (dc->dialog_owner_idx == i) {
              keep_for_reel = 1;
              break;
            }
          }
        }
      }

      if (reel_mode && !keep_for_reel)
        continue;

      if (cc->desktop != grp->id || cc->mini_w <= 0 || cc->mini_h <= 0 ||
          mini_hidden[i])
        continue;

      draw_order[ndraw++] = i;
    }
    // Chota insertion sort — group ke andar clients ki tadaad chhoti hoti
    // hai, isliye O(n^2) yahan koi masla nahi.
    for (int a = 1; a < ndraw; a++) {
      int key = draw_order[a];
      int b = a - 1;
      while (b >= 0 &&
             client_stack_rank[draw_order[b]] > client_stack_rank[key]) {
        draw_order[b + 1] = draw_order[b];
        b--;
      }
      draw_order[b + 1] = key;
    }

    for (int oi = 0; oi < ndraw; oi++) {
      int i = draw_order[oi];
      {
        Client *c = &clients[i];

        if (!c->is_dialog &&
            !(c->thumb_surf && c->real_w > 0 && c->real_h > 0)) {
          cairo_set_source_rgba(bcr, 0.04, 0.04, 0.07, 0.9);
          cairo_rectangle(bcr, c->mini_x, c->mini_y, c->mini_w, c->mini_h);
          cairo_fill(bcr);
        }

        if (c->thumb_surf && c->real_w > 0 && c->real_h > 0) {
          // mini_w/h ab already (non-uniform) stretched scale se calculate
          // hote hain taake box mein koi gap na aaye, isliye yahan bhi
          // thumbnail ko us hi rect mein bilkul fit (stretch) kiya jata hai
          // — dobara aspect-preserve karke letterbox gap paida nahi karna.
          double msx = (double)c->cmini_w / c->real_w;
          double msy = (double)c->cmini_h / c->real_h;
          // frame/titlebar/border wali jagah (agar hai) halke dark rang se
          // bharo, phir content upar.
          if (c->ext_l || c->ext_r || c->ext_t || c->ext_b) {
            // SIRF frame/border ki patti bharo (outer minus client rect,
            // even-odd), client area ke peeche kuch nahi — warna transparent
            // windows ke peeche dark fill aa jata aur transparency kam lagti.
            cairo_save(bcr);
            cairo_set_fill_rule(bcr, CAIRO_FILL_RULE_EVEN_ODD);
            cairo_set_source_rgba(bcr, 0.10, 0.10, 0.14, 0.35);
            cairo_rectangle(bcr, c->mini_x, c->mini_y, c->mini_w, c->mini_h);
            cairo_rectangle(bcr, c->cmini_x, c->cmini_y, c->cmini_w,
                            c->cmini_h);
            cairo_fill(bcr);
            cairo_restore(bcr);
          }
          if (DESKBOX_WIN_BACKING > 0.0) {
            cairo_set_source_rgba(bcr, 0.05, 0.05, 0.08, DESKBOX_WIN_BACKING);
            cairo_rectangle(bcr, c->cmini_x, c->cmini_y, c->cmini_w,
                            c->cmini_h);
            cairo_fill(bcr);
          }
          cairo_save(bcr);
          cairo_rectangle(bcr, c->cmini_x, c->cmini_y, c->cmini_w,
                          c->cmini_h);
          cairo_clip(bcr);
          cairo_translate(bcr, c->cmini_x, c->cmini_y);
          cairo_scale(bcr, msx, msy);
          cairo_set_source_surface(bcr, c->thumb_surf, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(bcr),
                                   CAIRO_FILTER_GOOD);
          cairo_paint(bcr);
          cairo_restore(bcr);
        } else if (c->icon) {
          int iw = cairo_image_surface_get_width(c->icon);
          int ih = cairo_image_surface_get_height(c->icon);
          double maxdim = c->mini_w < c->mini_h ? c->mini_w : c->mini_h;
          double mscale = (maxdim * 0.6) / (iw > ih ? iw : ih);
          cairo_save(bcr);
          cairo_translate(bcr,
                          c->mini_x + c->mini_w / 2.0 - (iw * mscale) / 2.0,
                          c->mini_y + c->mini_h / 2.0 - (ih * mscale) / 2.0);
          cairo_scale(bcr, mscale, mscale);
          cairo_set_source_surface(bcr, c->icon, 0, 0);
          cairo_paint(bcr);
          cairo_restore(bcr);
        }
      }
    }

    cairo_restore(bcr);

    if (use_group) {
      cairo_pop_group_to_source(bcr);
      cairo_paint_with_alpha(bcr, box_alpha);
    }

    // Border content ke baad draw hota hai (thumbnail ke upar) taake ye
    // hamesha crisp aur poori tarah visible rahe — content se dab/chhup
    // na jaye. Sab boxes par uniform thin blue border, selected ho ya na
    // ho color/width same rehta hai.
    cairo_set_source_rgba(bcr, 1, 1, 1, selected ? 0.0 : 0.22);
    cairo_set_line_width(bcr, 1.0);
    rounded_rect(bcr, grp->x + 0.5, grp->y + 0.5, grp->w - 1, grp->h - 1,
                 DESKBOX_RADIUS);
    cairo_stroke(bcr);

    // Desktop ka naam box ke neeche (KDE: "Desktop 1")
    draw_text_center(bcr, grp->name, grp->x + grp->w / 2.0,
                     grp->y + grp->h + 17, 11.5, grp->w + DESKBOX_PAD - 4,
                     selected ? 1.0 : 0.78, selected);

    // Keyboard focus abhi deskbar par ho aur ye wahi selected box ho, to
    // ek extra outer ring dikhao — taake "selected desktop" aur "keyboard
    // focus yahan hai" mein visually farq pata chale (KDE overview jese).
    if (selected && focus_area == FOCUS_AREA_DESKBAR) {
      cairo_set_source_rgba(bcr, 1.0, 1.0, 1.0, 0.95);
      cairo_set_line_width(bcr, 2.0);
      rounded_rect(bcr, grp->x - 6 + 0.5, grp->y - 6 + 0.5, grp->w + 12 - 1,
                   grp->h + 12 - 1, DESKBOX_RADIUS + 5);
      cairo_stroke(bcr);
    }
  }

  {
    double ax, ay, aw, ah;
    if (get_selection_anim_rect(&ax, &ay, &aw, &ah)) {
      cairo_save(bcr);
      cairo_set_source_rgba(bcr, 0.24, 0.56, 0.95, 1.0);
      cairo_set_line_width(bcr, 3.0);
      rounded_rect(bcr, ax - 1.5, ay - 1.5, aw + 3, ah + 3,
                   DESKBOX_RADIUS + 2);
      cairo_stroke(bcr);
      cairo_restore(bcr);
    }
  }

  // "+" tile (KDE jesa, abhi sirf visual) 
  if (ngroups > 0) {
    Group *lg = &groups[ngroups - 1];
    double bx = lg->x, by = lg->y + lg->h + DESKLABEL_H + DESKBOX_PAD;
    cairo_set_source_rgba(bcr, 1, 1, 1, 0.16);
    rounded_rect(bcr, bx, by, lg->w, lg->h, DESKBOX_RADIUS);
    cairo_fill(bcr);
    cairo_set_source_rgba(bcr, 1, 1, 1, 0.75);
    cairo_set_line_width(bcr, 5.0);
    double pcx = bx + lg->w / 2.0, pcy = by + lg->h / 2.0, pr = lg->h * 0.2;
    cairo_move_to(bcr, pcx - pr, pcy);
    cairo_line_to(bcr, pcx + pr, pcy);
    cairo_move_to(bcr, pcx, pcy - pr);
    cairo_line_to(bcr, pcx, pcy + pr);
    cairo_stroke(bcr);
  }

  double gp = get_grid_anim_progress();
  double gscale = 0.85 + 0.15 * gp;
  double galpha = gp;

  for (int i = 0; i < nclients; i++) {
    Client *c = &clients[i];
    if (c->desktop != selected_desktop || c->w <= 0 || c->h <= 0)
      continue;

    double tx, ty, tw, th;
    card_thumb_rect(c, &tx, &ty, &tw, &th);

    if (dnd_active && dnd_moved && i == dnd_client_idx) {
      cairo_save(bcr);
      cairo_set_source_rgba(bcr, 1, 1, 1, 0.22);
      double dashes[] = {6, 4};
      cairo_set_dash(bcr, dashes, 2, 0);
      cairo_set_line_width(bcr, 2.0);
      rounded_rect(bcr, tx + 1, ty + 1, tw - 2, th - 2, CARD_RADIUS);
      cairo_stroke(bcr);
      cairo_restore(bcr);
      continue;
    }

    cairo_save(bcr);
    double card_cx = c->x + c->w / 2.0, card_cy = c->y + c->h / 2.0;
    cairo_translate(bcr, card_cx, card_cy);
    cairo_scale(bcr, gscale, gscale);
    cairo_translate(bcr, -card_cx, -card_cy);

    // soft layered drop shadow — SIRF card ke bahar (even-odd clip), taake
    // transparent/translucent windows ki opacity bilkul asal jaisi rahe aur
    // shadow unke andar se na jhalke. Koi dark backing bhi nahi.
    cairo_save(bcr);
    cairo_set_fill_rule(bcr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(bcr, tx - 40, ty - 40, tw + 80, th + 90);
    rounded_rect(bcr, tx, ty, tw, th, CARD_RADIUS);
    cairo_clip(bcr);
    cairo_set_fill_rule(bcr, CAIRO_FILL_RULE_WINDING);
    for (int sh_i = 8; sh_i >= 1; sh_i--) {
      cairo_set_source_rgba(bcr, 0, 0, 0, 0.045 * galpha);
      rounded_rect(bcr, tx - sh_i, ty - sh_i + 6, tw + sh_i * 2,
                   th + sh_i * 2, CARD_RADIUS + sh_i);
      cairo_fill(bcr);
    }
    cairo_restore(bcr);

    if (c->thumb_surf && c->real_w > 0 && c->real_h > 0) {
      cairo_save(bcr);
      rounded_rect(bcr, tx, ty, tw, th, CARD_RADIUS);
      cairo_clip(bcr);
      cairo_translate(bcr, tx, ty);
      cairo_scale(bcr, tw / c->real_w, th / c->real_h);
      cairo_set_source_surface(bcr, c->thumb_surf, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_GOOD);
      cairo_paint_with_alpha(bcr, galpha);
      cairo_restore(bcr);
    } else if (c->icon) {
      int iw = cairo_image_surface_get_width(c->icon);
      int ih = cairo_image_surface_get_height(c->icon);
      double sc = 48.0 / (iw > ih ? iw : ih);
      cairo_save(bcr);
      cairo_translate(bcr, tx + tw / 2.0 - iw * sc / 2.0,
                      ty + th / 2.0 - ih * sc / 2.0);
      cairo_scale(bcr, sc, sc);
      cairo_set_source_surface(bcr, c->icon, 0, 0);
      cairo_paint_with_alpha(bcr, galpha);
      cairo_restore(bcr);
    }

    // hairline edge (halki outer + andar ki highlight line)
    cairo_set_line_width(bcr, 1.0);
    cairo_set_source_rgba(bcr, 1, 1, 1, 0.24 * galpha);
    rounded_rect(bcr, tx + 0.5, ty + 0.5, tw - 1, th - 1, CARD_RADIUS);
    cairo_stroke(bcr);

    // Selection frame card ke SAATH, icon badge se PEHLE draw hota hai, taake
    // icon hamesha frame ke upar rahe (frame ke peeche na dikhe).
    if (focus_area == FOCUS_AREA_GRID && i == focused_client_idx) {
      for (int gl = 2; gl >= 1; gl--) {
        cairo_set_source_rgba(bcr, 0.24, 0.56, 0.95, 0.10 * (3 - gl) * galpha);
        cairo_set_line_width(bcr, 3.0 + gl * 4.0);
        rounded_rect(bcr, tx - 5 + 0.5, ty - 5 + 0.5, tw + 10 - 1, th + 10 - 1,
                     CARD_RADIUS + 5);
        cairo_stroke(bcr);
      }
      cairo_set_source_rgba(bcr, 0.24, 0.56, 0.95, galpha);
      cairo_set_line_width(bcr, 3.0);
      rounded_rect(bcr, tx - 5 + 0.5, ty - 5 + 0.5, tw + 10 - 1, th + 10 - 1,
                   CARD_RADIUS + 5);
      cairo_stroke(bcr);
    }

    // app icon thumbnail ke bottom-center par, gol backdrop ke saath
    if (c->icon && c->thumb_surf) {
      double icon_sz = 30.0;
      double icx = tx + tw / 2.0, icy = ty + th;
      int iw = cairo_image_surface_get_width(c->icon);
      int ih = cairo_image_surface_get_height(c->icon);
      double sc = icon_sz / (iw > ih ? iw : ih);

      cairo_set_source_rgba(bcr, 0, 0, 0, 0.25 * galpha);
      cairo_arc(bcr, icx, icy + 2, icon_sz / 2 + 7, 0, 2 * M_PI);
      cairo_fill(bcr);
      cairo_set_source_rgba(bcr, 0.12, 0.12, 0.16, 0.92 * galpha);
      cairo_arc(bcr, icx, icy, icon_sz / 2 + 6, 0, 2 * M_PI);
      cairo_fill_preserve(bcr);
      cairo_set_source_rgba(bcr, 1, 1, 1, 0.18 * galpha);
      cairo_set_line_width(bcr, 1.0);
      cairo_stroke(bcr);

      cairo_save(bcr);
      cairo_translate(bcr, icx - iw * sc / 2.0, icy - ih * sc / 2.0);
      cairo_scale(bcr, sc, sc);
      cairo_set_source_surface(bcr, c->icon, 0, 0);
      cairo_paint_with_alpha(bcr, galpha);
      cairo_restore(bcr);
    }

    // window title icon ke neeche
    {
      int is_foc = (focus_area == FOCUS_AREA_GRID && i == focused_client_idx);
      draw_text_center(bcr, c->title, c->x + c->w / 2.0, ty + th + 38, 11.5,
                       (double)c->w + MAIN_CARD_PAD - 6,
                       (is_foc ? 1.0 : 0.82) * galpha, is_foc);
    }
    cairo_restore(bcr);
  }

  if (dnd_active && dnd_moved && dnd_client_idx >= 0 &&
      dnd_client_idx < nclients) {
    Client *dc = &clients[dnd_client_idx];
    int fw = (int)(dc->w * 0.1);
    int fh = (int)(dc->h * 0.1);
    if (fw < 4)
      fw = 4;
    if (fh < 4)
      fh = 4;
    int fx = dnd_cur_x - fw / 2;
    int fy = dnd_cur_y - fh / 2;

    int cx = fx, cy = fy, cw = fw, ch = fh;
    if (dc->thumb_surf && dc->real_w > 0 && dc->real_h > 0) {
      double sx = (double)fw / dc->real_w;
      double sy = (double)fh / dc->real_h;
      double scale = sx < sy ? sx : sy;
      double dw = dc->real_w * scale, dh = dc->real_h * scale;
      cx = (int)(fx + (fw - dw) / 2.0);
      cy = (int)(fy + (fh - dh) / 2.0);
      cw = (int)dw;
      ch = (int)dh;
    }

    cairo_save(bcr);
    cairo_set_source_rgba(bcr, 0, 0, 0, 0.35);
    cairo_rectangle(bcr, cx + 6, cy + 8, cw, ch);
    cairo_fill(bcr);

    cairo_set_source_rgba(bcr, 0.08, 0.08, 0.12, 0.95);
    cairo_rectangle(bcr, cx, cy, cw, ch);
    cairo_fill(bcr);

    if (dc->thumb_surf && dc->real_w > 0 && dc->real_h > 0) {
      cairo_save(bcr);
      cairo_rectangle(bcr, cx, cy, cw, ch);
      cairo_clip(bcr);
      cairo_translate(bcr, cx, cy);
      cairo_scale(bcr, (double)cw / dc->real_w, (double)ch / dc->real_h);
      cairo_set_source_surface(bcr, dc->thumb_surf, 0, 0);
      cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_GOOD);
      cairo_paint(bcr);
      cairo_restore(bcr);
    } else if (dc->icon) {
      int iw = cairo_image_surface_get_width(dc->icon);
      int ih = cairo_image_surface_get_height(dc->icon);
      double scale = 48.0 / (iw > ih ? iw : ih);
      cairo_save(bcr);
      cairo_translate(bcr, cx + cw / 2.0 - (iw * scale) / 2.0,
                      cy + ch / 2.0 - (ih * scale) / 2.0);
      cairo_scale(bcr, scale, scale);
      cairo_set_source_surface(bcr, dc->icon, 0, 0);
      cairo_paint(bcr);
      cairo_restore(bcr);
    }

    cairo_set_source_rgba(bcr, 0.55, 0.78, 1.0, 1.0);
    cairo_set_line_width(bcr, 2.5);
    cairo_rectangle(bcr, cx + 1, cy + 1, cw - 2, ch - 2);
    cairo_stroke(bcr);
    cairo_restore(bcr);
  }

  if (launching) {
    cairo_pop_group_to_source(bcr);
    cairo_paint_with_alpha(bcr, lalpha);
    cairo_restore(bcr);
  }

  cairo_set_source_surface(cr, bufsurf, 0, 0);
  cairo_paint(cr);
  cairo_surface_flush(csurf);
  XFlush(dpy);
}

static void send_activate_messages(Client *c) {
  XClientMessageEvent ev = {0};
  ev.type = ClientMessage;
  ev.window = c->win;
  ev.message_type = A_NET_ACTIVE_WINDOW;
  ev.format = 32;
  ev.data.l[0] = 1;
  ev.data.l[1] = CurrentTime;
  XSendEvent(dpy, root, False,
             SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&ev);
  XMapRaised(dpy, c->win);
  XSetInputFocus(dpy, c->win, RevertToParent, CurrentTime);
  XFlush(dpy);
}

// Escape/'w' se overview band hone par (bina kisi app select kiye) focus
// ko explicitly wapas usi window par le jaane ke liye flag — warna
// overview destroy hone ke baad WM ka default/focus-follows-mouse
// behavior jo bhi window cursor ke neeche ho usko focus de deta hai.
static int did_activate = 0;

static void activate(Client *c) {
  did_activate = 1;
  int switching_desktop =
      have_desktops && c->desktop >= 0 && c->desktop != current_desktop;

  if (have_desktops && c->desktop >= 0) {
    XClientMessageEvent ev = {0};
    ev.type = ClientMessage;
    ev.window = root;
    ev.message_type = A_NET_CURRENT_DESKTOP;
    ev.format = 32;
    ev.data.l[0] = c->desktop;
    ev.data.l[1] = CurrentTime;
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               (XEvent *)&ev);
    XFlush(dpy);
    // Slide-mode WMs kabhi desktop switch ko animate karte hain - us
    // animation ko thora waqt do taake wo apna restack khud kar le,
    // is se pehle ke hum window raise karein.
    if (switching_desktop)
      usleep(60000);
  }

  send_activate_messages(c);
  usleep(30000);

  // Kuch slide-mode WM apne animation/restack ke aakhir mein khud
  // apna z-order dobara likh dete hain, jis se humara upar wala raise
  // "lost" ho jata hai aur peechli app dikhti reh jati hai (jabke focus
  // sahi window par hi hota hai). Isi race ko haraane ke liye raise +
  // focus ko thori der baad dobara bhejte hain.
  send_activate_messages(c);
  usleep(30000);
}

static Client *client_at(int x, int y) {
  for (int i = 0; i < nclients; i++) {
    Client *c = &clients[i];
    if (c->w <= 0 || c->h <= 0) {
      continue;
    }
    if (x >= c->x && x <= c->x + c->w && y >= c->y && y <= c->y + c->h)
      return c;
  }
  return NULL;
}

static int desktop_box_at(int x, int y, int *hit) {
  *hit = 0;
  for (int g = 0; g < ngroups; g++) {
    Group *grp = &groups[g];
    if (x >= grp->x && x <= grp->x + grp->w && y >= grp->y &&
        y <= grp->y + grp->h) {
      *hit = 1;
      return g;
    }
  }
  return -1;
}

// Underlying WM kabhi kabhi ek tracked client window ko (desktop-move,
// resize, ya kisi aur wajah se) restack/remap kar deta hai, jis se wo
// hamare override-redirect `overview` window k UPAR aa jati hai aur
// overview aese nazar aata hai jese khula hi na ho. Isi liye har us
// jagah call karo jahan client ki MapNotify/ConfigureNotify ya
// desktop-move ho rahi ho, taake overview hamesha sab se upar rahe.
static void raise_overview(void) {
  XRaiseWindow(dpy, overview);
  XFlush(dpy);
}

static void move_client_to_desktop(Client *c, long new_desktop) {
  XClientMessageEvent ev = {0};
  ev.type = ClientMessage;
  ev.window = c->win;
  ev.message_type = A_NET_WM_DESKTOP;
  ev.format = 32;
  ev.data.l[0] = new_desktop;
  ev.data.l[1] = 2;
  XSendEvent(dpy, root, False,
             SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&ev);
  XFlush(dpy);
  c->desktop = new_desktop;
  selected_desktop = new_desktop;

  XSync(dpy, False);
  usleep(30000);
}

static void switch_real_desktop(long desktop_id) {
  did_activate = 1;
  if (!have_desktops)
    return;
  XClientMessageEvent ev = {0};
  ev.type = ClientMessage;
  ev.window = root;
  ev.message_type = A_NET_CURRENT_DESKTOP;
  ev.format = 32;
  ev.data.l[0] = desktop_id;
  ev.data.l[1] = CurrentTime;
  XSendEvent(dpy, root, False,
             SubstructureRedirectMask | SubstructureNotifyMask, (XEvent *)&ev);
  XFlush(dpy);
}

static void switch_desktop_by_delta(int delta) {
  int idx = 0;
  for (int g = 0; g < ngroups; g++) {
    if (groups[g].id == selected_desktop) {
      idx = g;
      break;
    }
  }
  idx += delta;
  if (idx < 0)
    idx = ngroups - 1;
  if (idx >= ngroups)
    idx = 0;
  set_selected_desktop(groups[idx].id);
  layout_main_grid();
  need_redraw = 1;
}

static int move_focus(KeySym ks) {
  int cand[MAX_CLIENTS], ncand = 0;
  for (int i = 0; i < nclients; i++) {
    if (clients[i].desktop == selected_desktop && clients[i].w > 0 &&
        clients[i].h > 0)
      cand[ncand++] = i;
  }
  if (ncand == 0)
    return 0;

  if (focused_client_idx < 0) {
    focused_client_idx = cand[0];
    return 1;
  }

  Client *cur = &clients[focused_client_idx];
  double fx = cur->x + cur->w / 2.0, fy = cur->y + cur->h / 2.0;

  int best = -1;
  double best_score = 1e18;
  for (int k = 0; k < ncand; k++) {
    int i = cand[k];
    if (i == focused_client_idx)
      continue;
    Client *c = &clients[i];
    double cx = c->x + c->w / 2.0, cy = c->y + c->h / 2.0;
    double dx = cx - fx, dy = cy - fy;

    int ok = 0;
    double primary = 0, perp = 0;
    if (ks == XK_Left) {
      ok = dx < -4;
      primary = -dx;
      perp = fabs(dy);
    } else if (ks == XK_Right) {
      ok = dx > 4;
      primary = dx;
      perp = fabs(dy);
    } else if (ks == XK_Up) {
      ok = dy < -4;
      primary = -dy;
      perp = fabs(dx);
    } else if (ks == XK_Down) {
      ok = dy > 4;
      primary = dy;
      perp = fabs(dx);
    }
    if (!ok)
      continue;

    double score = primary + perp * 2.0;
    if (score < best_score) {
      best_score = score;
      best = i;
    }
  }

  if (best >= 0) {
    focused_client_idx = best;
    return 1;
  }
  return 0;
}

static void acquire_compositing_manager(void) {
  if (!have_composite)
    return;

  char sel_name[32];
  snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", scr);
  Atom cm_atom = XInternAtom(dpy, sel_name, False);

  Window existing = XGetSelectionOwner(dpy, cm_atom);
  if (existing != None) {
    return;
  }

  cm_owner_win = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
  XSetSelectionOwner(dpy, cm_atom, cm_owner_win, CurrentTime);

  if (XGetSelectionOwner(dpy, cm_atom) == cm_owner_win) {
    became_cm = 1;
  } else {
    XDestroyWindow(dpy, cm_owner_win);
    cm_owner_win = None;
  }
}

static void release_compositing_manager(void) {
  if (became_cm && cm_owner_win != None) {
    XDestroyWindow(dpy, cm_owner_win);
    cm_owner_win = None;
    became_cm = 0;
  }
}

static int x_error_handler(Display *d, XErrorEvent *e) {
  char msg[256];
  XGetErrorText(d, e->error_code, msg, sizeof(msg));
  fprintf(stderr, "wsoverview: X error ignored: %s (request %d.%d)\n", msg,
          e->request_code, e->minor_code);
  return 0;
}

int main(void) {
  XSetErrorHandler(x_error_handler);

  debug_enabled = getenv("WSOVERVIEW_DEBUG") != NULL;

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "wsoverview: X display nahi khul saka\n");
    return 1;
  }
  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);
  root_visual = DefaultVisual(dpy, scr);
  sw = DisplayWidth(dpy, scr);
  sh = DisplayHeight(dpy, scr);

  int ev_base, err_base;
  have_composite = XCompositeQueryExtension(dpy, &ev_base, &err_base);
  have_damage = XDamageQueryExtension(dpy, &damage_evt_base, &damage_err_base);
  if (!have_composite) {
    fprintf(stderr, "wsoverview: XComposite extension nahi mila — "
                    "sirf icon+naam dikhega, live thumbnails nahi.\n");
  }

  acquire_compositing_manager();

  init_atoms();
  // Pehle live wallpaper window (xwinwrap wagera) dhoondo — agar mil
  // jaye to usko composite kar ke istemal karo. Warna purane
  // _XROOTPMAP_ID/ESETROOT_PMAP_ID atom wale static wallpaper par
  // fallback karo.
  if (!setup_live_wallpaper())
    wallpaper_surf = get_wallpaper_surface();
  build_wallpaper_thumbnail();
  build_blurred_background();
  if (debug_enabled)
    fprintf(stderr, "[wsoverview] wallpaper=%dx%d screen=%dx%d\n",
            wallpaper_w, wallpaper_h, sw, sh);
  for (int i = 0; i < MAX_DESKTOPS; i++)
    last_focused[i] = None;
  for (int i = 0; i < MAX_DESKTOPS; i++)
    workspace_mode[i] = -1;
  load_clients();

  if (active_window != None) {
    for (int i = 0; i < nclients; i++) {
      if (clients[i].win == active_window) {
        long d = clients[i].desktop;

        if (d >= 0 && d < MAX_DESKTOPS)
          last_focused[d] = active_window;

        break;
      }
    }
  }

  load_last_focus_from_wm();
  load_workspace_modes_from_wm();
  detect_slide_hidden();
  layout();
  XSelectInput(dpy, root, PropertyChangeMask);
  XSetWindowAttributes swa;
  swa.override_redirect = True;
  swa.background_pixel = BlackPixel(dpy, scr);
  swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                   ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                   StructureNotifyMask;

  overview =
      XCreateWindow(dpy, root, 0, 0, sw, sh, 0, DefaultDepth(dpy, scr),
                    InputOutput, DefaultVisual(dpy, scr),
                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);

  XStoreName(dpy, overview, "wsoverview");

  bufsurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
  bcr = cairo_create(bufsurf);
  init_bg_pm = XCreatePixmap(dpy, root, sw, sh, DefaultDepth(dpy, scr));
  csurf = cairo_xlib_surface_create(dpy, init_bg_pm, DefaultVisual(dpy, scr),
                                    sw, sh);
  cr = cairo_create(csurf);
  draw();
  XSetWindowBackgroundPixmap(dpy, overview, init_bg_pm);
  cairo_destroy(cr);
  cairo_surface_destroy(csurf);
  csurf =
      cairo_xlib_surface_create(dpy, overview, DefaultVisual(dpy, scr), sw, sh);
  cr = cairo_create(csurf);

  XMapRaised(dpy, overview);

  XEvent me;
  do {
    XWindowEvent(dpy, overview, StructureNotifyMask, &me);
  } while (me.type != MapNotify);
  int kb_status = -1;
  for (int tries = 0; tries < 50 && kb_status != GrabSuccess; tries++) {
    kb_status = XGrabKeyboard(dpy, overview, False, GrabModeAsync,
                              GrabModeAsync, CurrentTime);
    if (kb_status != GrabSuccess)
      usleep(2000);
  }
  if (kb_status != GrabSuccess) {
    fprintf(stderr,
            "wsoverview: keyboard grab fail (status %d) — "
            "Escape/q shayad kaam na karein\n",
            kb_status);
  }

  int ptr_status = -1;
  for (int tries = 0; tries < 50 && ptr_status != GrabSuccess; tries++) {
    ptr_status =
        XGrabPointer(dpy, overview, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    if (ptr_status != GrabSuccess)
      usleep(2000);
  }

  XSetInputFocus(dpy, overview, RevertToParent, CurrentTime);

  // Ab overview visible ho chuka hai — launch fade+scale-in aur cards ka
  // pehla pop-in yahan se shuru karo taake pehla frame hi animate ho.
  // clock_gettime(CLOCK_MONOTONIC, &launch_anim_start_ts);
  // launch_anim_active = 1;
  // grid_anim_start_ts = launch_anim_start_ts;
  // grid_anim_active = 1;
  need_redraw = 1;

  int running = 1;
  XEvent ev;
  int xfd = ConnectionNumber(dpy);
  while (running) {
    if (XPending(dpy) == 0) {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(xfd, &fds);
      int animating = any_anim_active();
      struct timeval tv = {
          0, (animating ? ANIM_FRAME_INTERVAL_MS : FRAME_INTERVAL_MS) * 1000L};
      int sr = select(xfd + 1, &fds, NULL, NULL,
                      (need_redraw || animating) ? &tv : NULL);
      if (sr == 0 && (need_redraw || animating)) {
        /* timed out waiting for events with a paced redraw still
         *           pending (e.g. the last damage burst tapered off) - flush
         *           it now instead of waiting indefinitely for the next event
         * animating hote hue kam interval par tick karte rehte hain taake
         * selection-slide/launch/grid animations smooth dikhein, khatam
         * hote hi wapas normal pacing par chale jaate hain */
        draw();
        need_redraw = any_anim_active() ? 1 : 0;
        clock_gettime(CLOCK_MONOTONIC, &last_draw_ts);
        continue;
      }
      if (XPending(dpy) == 0)
        continue;
    }
    XNextEvent(dpy, &ev);

    switch (ev.type) {

    case PropertyNotify: {

      if (ev.xproperty.window != root)
        break;

      int changed = 0;

      if (ev.xproperty.atom == A_NET_ACTIVE_WINDOW) {

        unsigned long n;
        unsigned char *data =
            get_prop(root, A_NET_ACTIVE_WINDOW, XA_WINDOW, &n);

        Window new_active = None;

        if (data && n > 0) {
          new_active = *(Window *)data;
          XFree(data);
        }

        if (new_active != active_window) {

          active_window = new_active;

          for (int i = 0; i < nclients; i++) {

            if (clients[i].win != active_window)
              continue;

            long d = clients[i].desktop;

            if (d >= 0 && d < MAX_DESKTOPS)
              last_focused[d] = active_window;

            break;
          }

          changed = 1;
        }
      } else if (ev.xproperty.atom == A_WM_LAST_FOCUS_PER_WS) {
        load_last_focus_from_wm();
        changed = 1;
      } else if (ev.xproperty.atom == A_WM_WORKSPACE_MODE) {
        load_workspace_modes_from_wm();
        changed = 1;
      } else if (ev.xproperty.atom == A_NET_CURRENT_DESKTOP) {
        unsigned long n;
        unsigned char *data =
            get_prop(root, A_NET_CURRENT_DESKTOP, XA_CARDINAL, &n);
        if (data && n > 0) {
          long new_desktop = *(long *)data;
          XFree(data);
          if (new_desktop != current_desktop) {
            current_desktop = new_desktop;
            set_selected_desktop(current_desktop);
            changed = 1;
          }
        }
      } else if (ev.xproperty.atom == A_NET_CLIENT_LIST) {
        // Naya app launch/close hone par WM yeh property update karta
        // hai. Pehle yahan koi handling nahi thi, is liye overview ko
        // naye window ka pata hi nahi chalta tha aur naya app iske
        // upar khul jata tha. Purane clients ke thumbnail resources
        // free karo (warna load_clients() dobara call karne par leak
        // ho jayenge), phir list reload karo.
        for (int i = 0; i < nclients; i++)
          free_thumbnail(&clients[i]);
        load_clients();
        changed = 1;
      }

      if (changed) {

        detect_slide_hidden();

        layout_desktop_bar();
        layout_main_grid();

        // Client list ya focus badalne par (jese naya app launch hona)
        // overview ko hamesha sabse upar rakho, warna naya window
        // isko peeche daba deta hai.
        raise_overview();

        need_redraw = 1;
      }

      break;
    }

    case Expose:
      if (ev.xexpose.count == 0)
        draw();
      break;
    case ButtonPress: {
      int hit;
      int g = desktop_box_at(ev.xbutton.x, ev.xbutton.y, &hit);
      if (hit) {
        if (groups[g].id != selected_desktop) {
          set_selected_desktop(groups[g].id);
          layout_main_grid();
          need_redraw = 1;
        }
        break;
      }
      Client *c = client_at(ev.xbutton.x, ev.xbutton.y);
      if (c) {
        dnd_active = 1;
        dnd_moved = 0;
        dnd_client_idx = (int)(c - clients);
        focused_client_idx = dnd_client_idx;
        dnd_press_x = dnd_cur_x = ev.xbutton.x;
        dnd_press_y = dnd_cur_y = ev.xbutton.y;
      }
      break;
    }
    case MotionNotify: {
      if (dnd_active) {
        dnd_cur_x = ev.xmotion.x;
        dnd_cur_y = ev.xmotion.y;
        if (!dnd_moved) {
          int mdx = dnd_cur_x - dnd_press_x;
          int mdy = dnd_cur_y - dnd_press_y;
          if (mdx * mdx + mdy * mdy > DND_THRESHOLD * DND_THRESHOLD)
            dnd_moved = 1;
        }
        need_redraw = 1;
      }
      break;
    }
    case ButtonRelease: {
      if (dnd_active) {
        if (dnd_moved && dnd_client_idx >= 0 && dnd_client_idx < nclients) {
          int hit = 0;
          int g = desktop_box_at(ev.xbutton.x, ev.xbutton.y, &hit);
          if (hit && groups[g].id != clients[dnd_client_idx].desktop) {
            move_client_to_desktop(&clients[dnd_client_idx], groups[g].id);
            // Khali/nayi desktop par drop hone par WM window ko
            // remap/restack kar sakta hai — overview ko wapas upar
            // le aao warna wo app k peeche chhup jata hai.
            raise_overview();
          }
          detect_slide_hidden();
          layout_desktop_bar();
          layout_main_grid();
          need_redraw = 1;
        } else if (dnd_client_idx >= 0 && dnd_client_idx < nclients) {
          activate(&clients[dnd_client_idx]);
          running = 0;
        }
      }
      dnd_active = 0;
      dnd_moved = 0;
      dnd_client_idx = -1;
      break;
    }
    case KeyPress: {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      if (ks == XK_Escape || ks == XK_w)
        running = 0;
      else if (ks == XK_Tab) {
        tab_held = 1;
        switch_desktop_by_delta((ev.xkey.state & ShiftMask) ? -1 : 1);
      } else if (tab_held && (ks == XK_Left || ks == XK_Right || ks == XK_Up ||
                              ks == XK_Down)) {
        switch_desktop_by_delta((ks == XK_Right || ks == XK_Down) ? 1 : -1);
      } else if (!(ev.xkey.state & Mod4Mask) &&
                 (ks == XK_Up || ks == XK_Down)) {
        // Deskbar ab LEFT column hai: us mein Up/Down workspace badalte
        // hain; grid mein Up/Down cards k darmiyan move karte hain.
        if (focus_area == FOCUS_AREA_DESKBAR)
          switch_desktop_by_delta(ks == XK_Down ? 1 : -1);
        else
          move_focus(ks);
        need_redraw = 1;
      } else if (!(ev.xkey.state & Mod4Mask) && ks == XK_Left) {
        // Grid mein ho aur left mein koi card na mile (pehle column par
        // ho) to LEFT deskbar mein "escape" kar jao.
        if (focus_area == FOCUS_AREA_DESKBAR || !move_focus(ks))
          focus_area = FOCUS_AREA_DESKBAR;
        need_redraw = 1;
      } else if (!(ev.xkey.state & Mod4Mask) && ks == XK_Right) {
        // Deskbar mein ho to grid mein "enter" kar jao; grid mein ho to
        // daayen wale card par move ho jao (agar mojood ho).
        if (focus_area == FOCUS_AREA_DESKBAR) {
          focus_area = FOCUS_AREA_GRID;
          if (focused_client_idx < 0)
            move_focus(ks);
        } else {
          move_focus(ks);
        }
        need_redraw = 1;
      } else if (ks == XK_Return || ks == XK_KP_Enter) {
        if (focus_area == FOCUS_AREA_GRID && focused_client_idx >= 0 &&
            focused_client_idx < nclients) {
          activate(&clients[focused_client_idx]);
          running = 0;
        } else {
          // Ya to focus deskbar mein hai, ya grid mein hai lekin selected
          // workspace khaali hai (koi client focused nahin) — dono
          // suraton mein koi Client nahin milta jise activate() kiya
          // ja sake, is liye WM ko seedha real desktop switch bhejo.
          switch_real_desktop(selected_desktop);
          running = 0;
        }
      } else if (/*(ev.xkey.state & Mod4Mask) &&*/ ks >= XK_1 && ks <= XK_9) {
        long target_id = (long)(ks - XK_1);
        int found = 0;
        for (int g = 0; g < ngroups; g++) {
          if (groups[g].id == target_id) {
            found = 1;
            break;
          }
        }
        if (found && target_id != selected_desktop) {
          set_selected_desktop(target_id);
          layout_main_grid();
          need_redraw = 1;
        }
      }
      break;
    }

    case KeyRelease: {
      KeySym ks = XLookupKeysym(&ev.xkey, 0);
      if (ks == XK_Tab)
        tab_held = 0;
      break;
    }

    case ConfigureNotify: {

      for (int i = 0; i < nclients; i++) {

        if (clients[i].win != ev.xconfigure.window)
          continue;

        clients[i].real_x = ev.xconfigure.x;
        clients[i].real_y = ev.xconfigure.y;
        clients[i].real_w = ev.xconfigure.width;
        clients[i].real_h = ev.xconfigure.height;
        refresh_client_pixmap(&clients[i]);
        detect_slide_hidden();
        load_stacking_order();
        layout_desktop_bar();
        layout_main_grid();
        raise_overview();
        need_redraw = 1;
        break;
      }

      break;
    }

    case MapNotify: {

      int matched = 0;

      for (int i = 0; i < nclients; i++) {

        if (clients[i].win != ev.xmap.window)
          continue;

        matched = 1;

        refresh_client_pixmap(&clients[i]);

        detect_slide_hidden();
        load_stacking_order();

        layout_desktop_bar();
        layout_main_grid();

        need_redraw = 1;

        break;
      }

      // Window abhi tak clients[] mein tracked nahi (e.g.
      // _NET_CLIENT_LIST property update MapNotify ke baad aaya) —
      // is race ke bawajood overview ko naye window ke peeche mat
      // jaane do.
      raise_overview();
      if (!matched)
        need_redraw = 1;

      break;
    }
    case UnmapNotify: {
      for (int i = 0; i < nclients; i++) {
        if (clients[i].win == ev.xunmap.window) {
          free_thumbnail(&clients[i]);
          load_stacking_order();
          detect_slide_hidden();
          layout_desktop_bar();
          layout_main_grid();
          raise_overview();
          need_redraw = 1;
          break;
        }
      }
      break;
    }
    default:
      if (have_damage && ev.type == damage_evt_base + XDamageNotify) {
        XDamageNotifyEvent *de = (XDamageNotifyEvent *)&ev;
        if (xwinwrap_damage != None && de->damage == xwinwrap_damage) {
          // Live wallpaper (xwinwrap) ne naya frame draw kiya —
          // wallpaper_surf ko dirty mark karo aur thumbnail dobara
          // bana lo taake deskbar boxes mein bhi live update dikhe.
          XDamageSubtract(dpy, xwinwrap_damage, None, None);
          if (wallpaper_surf) {
            cairo_surface_flush(wallpaper_surf);
            cairo_surface_mark_dirty(wallpaper_surf);
          }
          build_wallpaper_thumbnail();
          build_blurred_background();
          need_redraw = 1;
          break;
        }
        for (int i = 0; i < nclients; i++) {
          if (clients[i].damage == de->damage) {

            XDamageSubtract(dpy, clients[i].damage, None, None);

            refresh_thumbnail(&clients[i]);

            need_redraw = 1;

            break;
          }
        }
      }
      break;
    }

    if ((need_redraw || any_anim_active()) && XPending(dpy) == 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      long elapsed_ms = (now.tv_sec - last_draw_ts.tv_sec) * 1000L +
                        (now.tv_nsec - last_draw_ts.tv_nsec) / 1000000L;
      long min_interval =
          any_anim_active() ? ANIM_FRAME_INTERVAL_MS : FRAME_INTERVAL_MS;
      if (elapsed_ms >= min_interval) {
        draw();
        need_redraw = any_anim_active() ? 1 : 0;
        last_draw_ts = now;
      } else {
        need_redraw = 1;
      }
    }
  }

  cairo_destroy(bcr);
  cairo_surface_destroy(bufsurf);
  if (init_bg_pm != None)
    XFreePixmap(dpy, init_bg_pm);
  cairo_destroy(cr);
  cairo_surface_destroy(csurf);
  for (int i = 0; i < nclients; i++) {
    if (clients[i].icon)
      cairo_surface_destroy(clients[i].icon);
    if (clients[i].thumb_surf)
      cairo_surface_destroy(clients[i].thumb_surf);
    if (clients[i].thumb_pixmap)
      XFreePixmap(dpy, clients[i].thumb_pixmap);
    if (clients[i].damage)
      XDamageDestroy(dpy, clients[i].damage);
    if (have_composite)
      XCompositeUnredirectWindow(dpy, clients[i].win,
                                 CompositeRedirectAutomatic);
  }
  if (wallpaper_surf)
    cairo_surface_destroy(wallpaper_surf);
  if (wallpaper_thumb_surf)
    cairo_surface_destroy(wallpaper_thumb_surf);
  if (wallpaper_blur_surf)
    cairo_surface_destroy(wallpaper_blur_surf);
  if (have_damage && xwinwrap_damage != None)
    XDamageDestroy(dpy, xwinwrap_damage);
  if (have_composite && xwinwrap_win != None)
    XCompositeUnredirectWindow(dpy, xwinwrap_win, CompositeRedirectAutomatic);
  XUngrabKeyboard(dpy, CurrentTime);
  XUngrabPointer(dpy, CurrentTime);
  XDestroyWindow(dpy, overview);
  // Agar koi app explicitly activate nahi hui (Escape/'w' se band kiya),
  // to focus ko wapas usi app par le jao jispar overview khulne se pehle
  // focus tha — mouse cursor jispar ho usko focus mil jaye, ye avoid
  // karne ke liye.
  if (!did_activate && active_window != None) {
    XSetInputFocus(dpy, active_window, RevertToParent, CurrentTime);
    XFlush(dpy);
  }
  release_compositing_manager();
  XCloseDisplay(dpy);
  return 0;
}
