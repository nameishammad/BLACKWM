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
#define DESKBAR_Y 20
#define DESKBAR_H 150
#define DESKBOX_PAD 16
#define MINI_PAD 0
#define MINI_TITLE_H 16
#define MAIN_CARD_W 200
#define MAIN_CARD_H 180
#define MAIN_CARD_PAD 24
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
  int mini_x, mini_y, mini_w, mini_h;
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
// Keyboard focus do jaghon mein se kisi ek "area" mein hoti hai: ya to
// upar wale deskbar boxes mein, ya neeche skippy-xd wale grid section
// mein. Up arrow se deskbar par jao, Down arrow se wapas grid mein.
// Jab focus deskbar mein ho to Left/Right workspace switch karte hain,
// aur jab focus grid mein ho to Left/Right cards k darmiyan move karte
// hain (KDE overview jese).
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
    A_WM_WORKSPACE_MODE;

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
  return surf;
}

// xwinwrap apni "live wallpaper" window root ke seedhe neeche, poori
// screen ke barabar size ki, aur override-redirect bana kar chhod deta
// hai (taake WM usko decorate/manage na kare) — bilkul root pixmap ki
// tarah "dikhta" hai lekin XQueryTree se pakda ja sakta hai.
// XQueryTree bottom-to-top stacking order mein windows deta hai, aur
// xwinwrap sabse peeche (root ke bilkul upar) hoti hai, isliye pehli
// match wali full-screen override-redirect window le lete hain.
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

// xwinwrap (ya jo bhi live wallpaper window mile) ko composite redirect
// karke uska pixmap seedha wallpaper_surf ke taur par use karte hain,
// bilkul waisi hi jaise client windows ke thumbnails liye jate hain
// (get_window_thumbnail dekhein). Damage extension se har naya frame
// pakड़ ke overview ko bhi live update kar dete hain.
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

  const int target_w =
      480; // itni resolution kaafi hai chhote box previews ke liye
  int target_h = (int)((double)sh * target_w / (double)sw);
  if (target_h < 1)
    target_h = 1;

  if (sw <= target_w) {
    // wallpaper already chhota hai, seedha snapshot le lo
    wallpaper_thumb_surf =
        downscale_image_surface(wallpaper_surf, sw, sh, sw, sh);
    wallpaper_thumb_w = sw;
    wallpaper_thumb_h = sh;
    return;
  }

  cairo_surface_t *cur = wallpaper_surf;
  int cur_w = sw, cur_h = sh, owns_cur = 0;

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
  }

  int n = ngroups > 0 ? ngroups : 1;
  double aspect = (double)sw / (double)sh;
  int box_h = DESKBAR_H;
  int box_w = (int)(box_h * aspect + 0.2);

  int total_w = n * box_w + (n + 1) * DESKBOX_PAD;
  if (total_w > sw) {
    double scale = (double)(sw - (n + 1) * DESKBOX_PAD) / (double)(n * box_w);
    if (scale < 0.05)
      scale = 0.05;
    box_h = (int)(box_h * scale + 0.5);
    box_w = (int)(box_w * scale + 0.5);
  }
  if (box_h < 40)
    box_h = 40;
  if (box_w < 60)
    box_w = 60;
  deskbar_box_h = box_h;

  total_w = n * box_w + (n - 1) * DESKBOX_PAD;
  int gx = (sw - total_w) / 2;
  if (gx < DESKBOX_PAD)
    gx = DESKBOX_PAD;

  for (int i = 0; i < nclients; i++) {
    clients[i].mini_w = 0;
    clients[i].mini_h = 0;
  }

  for (int g = 0; g < ngroups; g++) {
    groups[g].x = gx;
    groups[g].y = DESKBAR_Y;
    groups[g].w = box_w;
    groups[g].h = box_h;
    gx += box_w + DESKBOX_PAD;

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
    double scale_y = (double)inner_h / (double)span_h;

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
      }

      c->mini_x = preview_x + (int)(local_x * scale_x);
      c->mini_y = preview_y + (int)(local_y * scale_y);
      c->mini_w = (int)(c->real_w * scale_x);
      c->mini_h = (int)(c->real_h * scale_y);

      if (c->mini_w < 2)
        c->mini_w = 2;

      if (c->mini_h < 2)
        c->mini_h = 2;

      if (c->mini_w < 2)
        c->mini_w = 2;

      if (c->mini_h < 2)
        c->mini_h = 2;
    }
  }
}

static void layout_main_grid(void) {
  int area_x = MAIN_TOP_MARGIN;
  int area_y = DESKBAR_Y + deskbar_box_h + MAIN_TOP_MARGIN;
  int area_w = sw - MAIN_TOP_MARGIN * 2;
  int area_h = sh - area_y - MAIN_TOP_MARGIN;
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

    // 60% area use karein, center mein
    double max_w = area_w * 0.6;
    double max_h = area_h * 0.6;

    // Aspect ratio ke hisaab se size calculate karein
    double w = max_w;
    double h = w / aspect_ratio;
    if (h > max_h) {
      h = max_h;
      w = h * aspect_ratio;
    }

    // Minimum size limit
    if (w < 100)
      w = 100;
    if (h < 100)
      h = 100;

    // Center position
    c->w = (int)w;
    c->h = (int)h;
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
      double w = nat_w[k] * s, h = nat_h[k] * s;
      if (!started) {
        row_w = w;
        row_h = h;
        started = 1;
      } else if (row_w + MAIN_CARD_PAD + w <= area_w) {
        row_w += MAIN_CARD_PAD + w;
        if (h > row_h)
          row_h = h;
      } else {
        total_h += row_h + MAIN_CARD_PAD;
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
  double max_scale = 1.2; // 1.8 se kam karein
  if (scale > max_scale)
    scale = max_scale;
  if (scale < 0.12)
    scale = 0.12;

  int row_start[MAX_CLIENTS + 1];
  double row_h_arr[MAX_CLIENTS];
  int nrows = 0;
  {
    double row_w = 0, row_h = 0;
    int started = 0;
    row_start[0] = 0;
    for (int k = 0; k < count; k++) {
      double w = nat_w[k] * scale, h = nat_h[k] * scale;
      if (!started) {
        row_w = w;
        row_h = h;
        started = 1;
      } else if (row_w + MAIN_CARD_PAD + w <= area_w) {
        row_w += MAIN_CARD_PAD + w;
        if (h > row_h)
          row_h = h;
      } else {
        row_h_arr[nrows] = row_h;
        nrows++;
        row_start[nrows] = k;
        row_w = w;
        row_h = h;
      }
    }
    row_h_arr[nrows] = row_h;
    nrows++;
  }
  row_start[nrows] = count;

  double grid_h = 0;
  for (int r = 0; r < nrows; r++)
    grid_h += row_h_arr[r];
  grid_h += (nrows - 1) * MAIN_CARD_PAD;

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
        row_w += MAIN_CARD_PAD;
    }
    double cx = area_x + (area_w - row_w) / 2.0;
    for (int k = first; k < last; k++) {
      Client *c = &clients[idxs[k]];
      double w = nat_w[k] * scale, h = nat_h[k] * scale;
      c->x = (int)cx;
      c->y = (int)(cy + (row_h_arr[r] - h) / 2.0);
      c->w = (int)w;
      c->h = (int)h;
      cx += w + MAIN_CARD_PAD;
    }
    cy += row_h_arr[r] + MAIN_CARD_PAD;
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

  if (wallpaper_surf) {
    cairo_save(bcr);
    cairo_set_source_surface(bcr, wallpaper_surf, 0, 0);
    cairo_paint(bcr);
    cairo_set_source_rgba(bcr, 0, 0, 0, 0.45);
    cairo_paint(bcr);
    cairo_restore(bcr);
  } else {
    cairo_set_source_rgba(bcr, 0, 0, 0, 0.85);
    cairo_paint(bcr);
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
        cairo_rectangle(bcr, grp->x + 1.5, grp->y + 1.5, grp->w - 3,
                        grp->h - 3);
        cairo_stroke(bcr);
      }
    }

    if (wallpaper_thumb_surf) {
      int inner_x = grp->x + MINI_PAD;
      int inner_y = grp->y + MINI_PAD;
      int inner_w = grp->w - MINI_PAD * 2;
      int inner_h = grp->h - MINI_PAD * 2;
      if (inner_w > 0 && inner_h > 0) {
        double sxw = (double)inner_w / (double)wallpaper_thumb_w;
        double syh = (double)inner_h / (double)wallpaper_thumb_h;
        cairo_save(bcr);
        cairo_rectangle(bcr, inner_x, inner_y, inner_w, inner_h);
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
    cairo_rectangle(bcr, mini_clip_x, mini_clip_y, mini_clip_w, mini_clip_h);
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
          double msx = (double)c->mini_w / c->real_w;
          double msy = (double)c->mini_h / c->real_h;
          cairo_save(bcr);
          cairo_rectangle(bcr, c->mini_x, c->mini_y, c->mini_w, c->mini_h);
          cairo_clip(bcr);
          cairo_translate(bcr, c->mini_x, c->mini_y);
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

    // Border content ke baad draw hota hai (thumbnail ke upar) taake ye
    // hamesha crisp aur poori tarah visible rahe — content se dab/chhup
    // na jaye. Sab boxes par uniform thin blue border, selected ho ya na
    // ho color/width same rehta hai.
    cairo_set_source_rgba(bcr, 0.35, 0.6, 1.0, 1);
    cairo_set_line_width(bcr, 2.5); // ← 1.0 se 2.5 kiya
    cairo_set_line_width(bcr, 3.0);
    cairo_rectangle(bcr, grp->x + 0.5, grp->y + 0.5, grp->w - 1, grp->h - 1);
    cairo_stroke(bcr);

    // Keyboard focus abhi deskbar par ho aur ye wahi selected box ho, to
    // ek extra outer ring dikhao — taake "selected desktop" aur "keyboard
    // focus yahan hai" mein visually farq pata chale (KDE overview jese).
    if (selected && focus_area == FOCUS_AREA_DESKBAR) {
      cairo_set_source_rgba(bcr, 1.0, 1.0, 1.0, 0.95);
      cairo_set_line_width(bcr, 2.0);
      cairo_rectangle(bcr, grp->x - 3 + 0.5, grp->y - 3 + 0.5, grp->w + 6 - 1,
                      grp->h + 6 - 1);
      cairo_stroke(bcr);
    }
  }

  {
    double ax, ay, aw, ah;
    if (get_selection_anim_rect(&ax, &ay, &aw, &ah)) {
      cairo_save(bcr);
      cairo_set_source_rgba(bcr, 0.55, 0.72, 1.0, 0.95);
      cairo_set_line_width(bcr, 3.0);
      cairo_rectangle(bcr, ax + 1.5, ay + 1.5, aw - 3, ah - 3);
      cairo_stroke(bcr);
      cairo_restore(bcr);
    }
  }

  double gp = get_grid_anim_progress();
  double gscale = 0.85 + 0.15 * gp;
  double galpha = gp;

  for (int i = 0; i < nclients; i++) {
    Client *c = &clients[i];
    if (c->desktop != selected_desktop || c->w <= 0 || c->h <= 0)
      continue;

    if (dnd_active && dnd_moved && i == dnd_client_idx) {
      cairo_save(bcr);
      cairo_set_source_rgba(bcr, 1, 1, 1, 0.18);
      double dashes[] = {6, 4};
      cairo_set_dash(bcr, dashes, 2, 0);
      cairo_set_line_width(bcr, 2.0);
      cairo_rectangle(bcr, c->x + 2, c->y + 2, c->w - 4, c->h - 4);
      cairo_stroke(bcr);
      cairo_restore(bcr);
      continue;
    }

    int prev_x = c->x + 2;
    int prev_y = c->y + 2;
    int prev_w = c->w - 4;
    int prev_h = c->h - 4;

    if (c->thumb_surf && c->real_w > 0 && c->real_h > 0) {
      double sx = (double)prev_w / c->real_w;
      double sy = (double)prev_h / c->real_h;
      double scale = sx < sy ? sx : sy;

      double dw = c->real_w * scale, dh = c->real_h * scale;
      double dx = prev_x + (prev_w - dw) / 2.0;
      double dy = prev_y + (prev_h - dh) / 2.0;

      cairo_save(bcr);
      double card_cx = c->x + c->w / 2.0, card_cy = c->y + c->h / 2.0;
      cairo_translate(bcr, card_cx, card_cy);
      cairo_scale(bcr, gscale, gscale);
      cairo_translate(bcr, -card_cx, -card_cy);
      cairo_rectangle(bcr, prev_x, prev_y, prev_w, prev_h);
      cairo_clip(bcr);
      cairo_translate(bcr, dx, dy);
      cairo_scale(bcr, scale, scale);
      cairo_set_source_surface(bcr, c->thumb_surf, 0, 0);

      cairo_pattern_set_filter(cairo_get_source(bcr), CAIRO_FILTER_GOOD);

      cairo_paint_with_alpha(bcr, galpha);
      cairo_restore(bcr);
    } else if (c->icon) {
      int iw = cairo_image_surface_get_width(c->icon);
      int ih = cairo_image_surface_get_height(c->icon);
      double scale = 48.0 / (iw > ih ? iw : ih);
      cairo_save(bcr);
      double card_cx = c->x + c->w / 2.0, card_cy = c->y + c->h / 2.0;
      cairo_translate(bcr, card_cx, card_cy);
      cairo_scale(bcr, gscale, gscale);
      cairo_translate(bcr, -card_cx, -card_cy);
      cairo_translate(bcr, c->x + c->w / 2.0 - (iw * scale) / 2.0,
                      prev_y + prev_h / 2.0 - (ih * scale) / 2.0);
      cairo_scale(bcr, scale, scale);
      cairo_set_source_surface(bcr, c->icon, 0, 0);
      cairo_paint_with_alpha(bcr, galpha);
      cairo_restore(bcr);
    }
    // 🔥 GRID CARD BORDER - YEH NAYA CODE ADD KARO 🔥
    // cairo_set_source_rgba(bcr, 0.45, 0.70, 1.0, 0.7);
    // cairo_set_line_width(bcr, 1.8);
    // cairo_rectangle(bcr, c->x + 1.5, c->y + 1.5, c->w - 3, c->h - 3);
    // cairo_stroke(bcr);
    // BLUE BORDER - AUTO-FIT APP THUMBNAIL KE EXACT AROUND
    if (c->thumb_surf && c->real_w > 0 && c->real_h > 0) {
      int prev_x = c->x + 2;
      int prev_y = c->y + 2;
      int prev_w = c->w - 4;
      int prev_h = c->h - 4;

      double sx = (double)prev_w / (double)c->real_w;
      double sy = (double)prev_h / (double)c->real_h;
      double scale = (sx < sy) ? sx : sy;

      int border_w = (int)(c->real_w * scale);
      int border_h = (int)(c->real_h * scale);
      int border_x = (int)(prev_x + (prev_w - border_w) / 2.0);
      int border_y = (int)(prev_y + (prev_h - border_h) / 2.0);

      cairo_set_source_rgba(bcr, 0.20, 0.60, 1.0, 0.95);
      cairo_set_line_width(bcr, 2.5);

      cairo_rectangle(bcr, border_x - 3 + 0.5, border_y - 3 + 0.5,
                      border_w + 6 - 1, border_h + 6 - 1);

      cairo_stroke(bcr);
    }
  }

  if (focus_area == FOCUS_AREA_GRID && focused_client_idx >= 0 &&
      focused_client_idx < nclients &&
      !(dnd_active && dnd_moved && dnd_client_idx == focused_client_idx)) {
    Client *fc = &clients[focused_client_idx];
    if (fc->desktop == selected_desktop && fc->w > 0 && fc->h > 0) {
      int hb_x = fc->x, hb_y = fc->y, hb_w = fc->w, hb_h = fc->h;
      if (fc->thumb_surf && fc->real_w > 0 && fc->real_h > 0) {
        int prev_x = fc->x + 2, prev_y = fc->y + 2;
        int prev_w = fc->w - 4, prev_h = fc->h - 4;
        double sx = (double)prev_w / fc->real_w;
        double sy = (double)prev_h / fc->real_h;
        double scale = sx < sy ? sx : sy;
        double dw = fc->real_w * scale, dh = fc->real_h * scale;
        hb_x = (int)(prev_x + (prev_w - dw) / 2.0);
        hb_y = (int)(prev_y + (prev_h - dh) / 2.0);
        hb_w = (int)dw;
        hb_h = (int)dh;
      }
      cairo_set_source_rgba(bcr, 0.45, 0.70, 1.0, 0.95);
      cairo_set_line_width(bcr, 2.5);
      cairo_rectangle(bcr, hb_x - 3 + 0.5, hb_y - 3 + 0.5, hb_w + 6 - 1,
                      hb_h + 6 - 1);
      cairo_stroke(bcr);
    }
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

  // FIX: WM (wm.c) ye ClientMessage ASYNC (alag process mein) handle
  // karta hai - ye turant hi c->win ko naye workspace ki "parked" X
  // position par nahi le jata. Agar hum turant (isi ButtonRelease
  // frame mein) layout_desktop_bar() chala dein to woh
  // XGetWindowAttributes se ABHI TAK PURANI (old workspace wali)
  // real_x utha lega, jab ke origin/offset calculation upar c->desktop
  // ko already NAYE desktop par set kar chuke hain is liye NAYE
  // desktop ke hisaab se ho rahi hogi. Ye mismatch thumbnail ki
  // scaled/computed position ko dono boxes ke bahar/ghalat jaga bhej
  // deta tha ("dono boxes khali/hide"). Overview band-khol karne par
  // WM tab tak apna kaam kar chuka hota isliye theek dikhta tha.
  //
  // NOTE: pehle yahan XCheckTypedWindowEvent() se khud WM ka
  // ConfigureNotify "chura" kar consume kar liya jata tha - is se
  // asal event kabhi neeche wale normal event-loop (jo pixmap,
  // stacking order, DONO layouts poori tarah refresh karta hai) tak
  // pohnchta hi nahi tha, aur agar mera apna adhoora manual update
  // WM ke asal update se pehle hi 80ms timeout ho jata to client
  // ek ajeeb "in-between" state mein phas kar dono boxes mein khali
  // dikhta tha. Ab hum event ko chhente/consume nahi karte - sirf WM
  // ko apna kaam poora karne ke liye thora sa waqt dete hain, taake
  // agli layout_desktop_bar() call tak server-side position update ho
  // chuki ho. Asal ConfigureNotify apne aap neeche wale normal
  // event-loop mein pohanch kar poora (sahi) refresh kar dega.
  XSync(dpy, False);
  usleep(30000);
}

// Deskbar/grid mein koi client select na ho (e.g. empty workspace) tab
// bhi Enter/click par ASAL desktop switch ho jaye, is liye ye function
// seedha _NET_CURRENT_DESKTOP WM ko bhejta hai. Pehle sirf activate(c)
// ye kaam karta tha, jo sirf tab chalta jab koi client focused ho —
// isi wajah se empty workspace par Enter kuch nahi karta tha.
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

// Agar koi compositing manager pehle se register nahi, to hum khud
// temporarily _NET_WM_CM_Sn selection le lete hain. Iske bina X server
// un windows ke liye off-screen pixmap guarantee nahi karta jo screen par
// already unobscured/visible hain (CompositeRedirectAutomatic ka yehi
// behavior hai) — isi wajah se aik window (jaise wezterm) jab bilkul
// usi size ki kisi doosri window ke upar hoti hai (hamesha unobscured
// rehti hai), uska thumbnail khali/transparent aata tha.
static void acquire_compositing_manager(void) {
  if (!have_composite)
    return;

  char sel_name[32];
  snprintf(sel_name, sizeof(sel_name), "_NET_WM_CM_S%d", scr);
  Atom cm_atom = XInternAtom(dpy, sel_name, False);

  Window existing = XGetSelectionOwner(dpy, cm_atom);
  if (existing != None) {
    // Pehle se koi compositor chal raha hai — us se pixmaps already
    // reliable milne chahiye, hum ownership nahi cheente.
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
      } else if (!(ev.xkey.state & Mod4Mask) && ks == XK_Up) {
        // Grid mein ho aur upar koi card na mile (top row par ho) to
        // deskbar mein "escape" kar jao — warna row k andar hi upar
        // wale card par move ho jao.
        if (focus_area == FOCUS_AREA_DESKBAR || !move_focus(ks))
          focus_area = FOCUS_AREA_DESKBAR;
        need_redraw = 1;
      } else if (!(ev.xkey.state & Mod4Mask) && ks == XK_Down) {
        // Deskbar mein ho to grid mein "enter" kar jao; grid mein ho to
        // neeche wale row k card par move ho jao (agar mojood ho).
        if (focus_area == FOCUS_AREA_DESKBAR) {
          focus_area = FOCUS_AREA_GRID;
          if (focused_client_idx < 0)
            move_focus(ks);
        } else {
          move_focus(ks);
        }
        need_redraw = 1;
      } else if (!(ev.xkey.state & Mod4Mask) &&
                 (ks == XK_Left || ks == XK_Right)) {
        if (focus_area == FOCUS_AREA_DESKBAR) {
          switch_desktop_by_delta(ks == XK_Right ? 1 : -1);
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
