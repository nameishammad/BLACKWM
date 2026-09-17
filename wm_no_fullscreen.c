#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <cairo-xcb.h>
#include <cairo.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_util.h>

#define MAX_WORKSPACES 9
#define START_WORKSPACES 2
#define MAX_WINDOWS 100

#define WSBOX_Y 75
#define WSBOX_W 150
#define WSBOX_H 95
#define WSBOX_SPACING 15
#define WSBOX_MINI_PAD 4
#define GRID_TOP (WSBOX_Y + WSBOX_H + 25)

#define BORDER_WIDTH 2
#define BORDER_NORMAL_COLOR 0x232629
#define BORDER_HOVER_COLOR 0x4886CD

typedef struct TileNode TileNode;

typedef struct {
  xcb_connection_t *conn;
  xcb_screen_t *screen;
  xcb_window_t root;
  xcb_window_t focused;
  int running;
  int current_workspace;
  int num_workspaces;
  xcb_window_t workspace_windows[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_counts[MAX_WORKSPACES];
  int workspace_win_x[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_y[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_w[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_h[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_force_float[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_slide_dx[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_slide_dy[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_win_maximized[MAX_WORKSPACES][MAX_WINDOWS];
  int16_t workspace_win_premax_x[MAX_WORKSPACES][MAX_WINDOWS];
  int16_t workspace_win_premax_y[MAX_WORKSPACES][MAX_WINDOWS];
  uint16_t workspace_win_premax_w[MAX_WORKSPACES][MAX_WINDOWS];
  uint16_t workspace_win_premax_h[MAX_WORKSPACES][MAX_WINDOWS];
  int workspace_focus_idx[MAX_WORKSPACES];
  xcb_window_t workspace_prev_focus[MAX_WORKSPACES];
  xcb_window_t workspace_last_active[MAX_WORKSPACES];
  int workspace_tiling[MAX_WORKSPACES];
  int workspace_floating[MAX_WORKSPACES];

  int workspace_reel_anchor_idx[MAX_WORKSPACES];
  TileNode *tile_root[MAX_WORKSPACES];
  int in_overview;
  xcb_window_t overview_win;
  xcb_pixmap_t overview_pixmap;
  int screen_width;
  int screen_height;
  xcb_window_t dock_windows[MAX_WINDOWS];
  int dock_count;
  xcb_window_t dialog_windows[MAX_WINDOWS];
  int dialog_owner_ws[MAX_WINDOWS];
  int16_t dialog_x[MAX_WINDOWS];
  int16_t dialog_y[MAX_WINDOWS];

  xcb_window_t dialog_prev_focus[MAX_WINDOWS];
  int dialog_count;

  int dragging;
  xcb_window_t drag_window;
  int16_t drag_start_x;
  int16_t drag_start_y;
  int16_t drag_orig_x;
  int16_t drag_orig_y;
  uint16_t drag_orig_w;
  uint16_t drag_orig_h;
  xcb_cursor_t cursor_normal;
  xcb_cursor_t cursor_move;
  xcb_cursor_t cursor_resize;
  xcb_font_t cursor_font;
  xcb_window_t preview_top;
  xcb_window_t preview_bottom;
  xcb_window_t preview_left;
  xcb_window_t preview_right;
} WM;

#define MOVE_RESIZE_MOD XCB_MOD_MASK_4
#define MIN_WIN_SIZE 50
#define WINDOW_GAP 10
#define CORNER_RADIUS 12
// #define ANIM_ENABLED 1
#define ANIM_ENABLED 2
#define ANIM_WS_STEPS 14
#define ANIM_WS_DELAY_US 6000
#define ANIM_OPEN_STEPS 10
#define ANIM_OPEN_DELAY_US 7000
WM wm;
static xcb_cursor_context_t *cursor_ctx = NULL;

void send_notify(const char *msg) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    freopen("/tmp/wm_notify.log", "a", stderr);
    execlp("notify-send", "notify-send", "WM", msg, NULL);
    fprintf(stderr, "notify-send exec failed: %s\n", strerror(errno));
    _exit(1);
  }
}

static void reap_children(int signum) {
  (void)signum;
  int saved_errno = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0) {
  }
  errno = saved_errno;
}

static double ease_out_cubic(double t) {
  double f = t - 1.0;
  return f * f * f + 1.0;
}

void grab_focus_click(xcb_window_t win);
static int window_supports_delete(xcb_window_t win);
void focus_window(xcb_window_t win);
static void publish_workspace_modes(void);
int is_window_managed(xcb_window_t win);
void handle_expose(xcb_expose_event_t *ev);
void apply_tiling_geometry(int ws);
void apply_reel_geometry_parked(int ws);
void move_window_to_workspace(xcb_window_t win, int target_ws);
static int find_window_idx(int ws, xcb_window_t win);
void compute_tiling_layout(int ws);
void compute_floating_layout(int ws);
void switch_workspace(int workspace);

xcb_atom_t A_NET_CLIENT_LIST;
xcb_atom_t A_NET_WM_DESKTOP;
xcb_atom_t A_NET_NUMBER_OF_DESKTOPS;
xcb_atom_t A_NET_CURRENT_DESKTOP;
xcb_atom_t A_NET_ACTIVE_WINDOW;
xcb_atom_t A_NET_SUPPORTED;
xcb_atom_t A_NET_WM_WINDOW_TYPE;
xcb_atom_t A_NET_WM_WINDOW_TYPE_DOCK;
xcb_atom_t A_NET_WM_WINDOW_TYPE_DIALOG;
xcb_atom_t A_WM_TRANSIENT_FOR;
xcb_atom_t A_WM_DIALOG_OWNER;
xcb_atom_t A_WM_PROTOCOLS;
xcb_atom_t A_WM_DELETE_WINDOW;
xcb_atom_t A_WM_LAST_FOCUS_PER_WS;
xcb_atom_t A_WM_WORKSPACE_MODE;

static xcb_atom_t intern_atom(const char *name) {
  xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(wm.conn, 0, strlen(name), name);
  xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(wm.conn, cookie, NULL);
  xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
  free(reply);
  return atom;
}

void init_ewmh_atoms() {
  A_NET_CLIENT_LIST = intern_atom("_NET_CLIENT_LIST");
  A_NET_WM_DESKTOP = intern_atom("_NET_WM_DESKTOP");
  A_NET_NUMBER_OF_DESKTOPS = intern_atom("_NET_NUMBER_OF_DESKTOPS");
  A_NET_CURRENT_DESKTOP = intern_atom("_NET_CURRENT_DESKTOP");
  A_NET_ACTIVE_WINDOW = intern_atom("_NET_ACTIVE_WINDOW");
  A_NET_SUPPORTED = intern_atom("_NET_SUPPORTED");
  A_NET_WM_WINDOW_TYPE = intern_atom("_NET_WM_WINDOW_TYPE");
  A_NET_WM_WINDOW_TYPE_DOCK = intern_atom("_NET_WM_WINDOW_TYPE_DOCK");
  A_NET_WM_WINDOW_TYPE_DIALOG = intern_atom("_NET_WM_WINDOW_TYPE_DIALOG");
  A_WM_TRANSIENT_FOR = intern_atom("WM_TRANSIENT_FOR");
  A_WM_DIALOG_OWNER = intern_atom("_WM_DIALOG_OWNER");
  A_WM_PROTOCOLS = intern_atom("WM_PROTOCOLS");
  A_WM_DELETE_WINDOW = intern_atom("WM_DELETE_WINDOW");
  A_WM_LAST_FOCUS_PER_WS = intern_atom("_WM_LAST_FOCUS_PER_WS");
  A_WM_WORKSPACE_MODE = intern_atom("_WM_WORKSPACE_MODE");

  uint32_t ndesk = wm.num_workspaces;
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1,
                      &ndesk);

  xcb_atom_t supported[] = {A_NET_CLIENT_LIST,        A_NET_WM_DESKTOP,
                            A_NET_NUMBER_OF_DESKTOPS, A_NET_CURRENT_DESKTOP,
                            A_NET_ACTIVE_WINDOW,
                            };
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root, A_NET_SUPPORTED,
                      XCB_ATOM_ATOM, 32,
                      sizeof(supported) / sizeof(supported[0]), supported);
}

void update_net_current_desktop() {
  uint32_t cur = wm.current_workspace;
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, &cur);
}

void update_net_client_list() {
  xcb_window_t list[MAX_WORKSPACES * MAX_WINDOWS + MAX_WINDOWS];
  int n = 0;
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    for (int j = 0; j < wm.workspace_counts[i]; j++) {
      list[n++] = wm.workspace_windows[i][j];
    }
  }
  for (int i = 0; i < wm.dialog_count; i++) {
    xcb_window_t dw = wm.dialog_windows[i];
    int already = 0;
    for (int k = 0; k < n; k++) {
      if (list[k] == dw) {
        already = 1;
        break;
      }
    }
    if (!already)
      list[n++] = dw;
  }
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_NET_CLIENT_LIST, XCB_ATOM_WINDOW, 32, n, list);
}

void set_window_desktop(xcb_window_t win, int ws) {
  uint32_t d = ws;
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, win, A_NET_WM_DESKTOP,
                      XCB_ATOM_CARDINAL, 32, 1, &d);
}

void init_workspaces() {
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    wm.workspace_counts[i] = 0;
    wm.workspace_focus_idx[i] = 0;
    wm.workspace_prev_focus[i] = XCB_WINDOW_NONE;
    wm.workspace_last_active[i] = XCB_WINDOW_NONE;
    wm.workspace_tiling[i] = 0;
    wm.workspace_floating[i] = 0;
    wm.workspace_reel_anchor_idx[i] = 0;
    wm.tile_root[i] = NULL;
    for (int j = 0; j < MAX_WINDOWS; j++) {
      wm.workspace_windows[i][j] = XCB_WINDOW_NONE;
      wm.workspace_win_w[i][j] = 0;
      wm.workspace_win_h[i][j] = 0;
      wm.workspace_win_force_float[i][j] = 0;
      wm.workspace_win_slide_dx[i][j] = 0;
      wm.workspace_win_slide_dy[i][j] = 0;
      wm.workspace_win_maximized[i][j] = 0;
      wm.workspace_win_premax_x[i][j] = 0;
      wm.workspace_win_premax_y[i][j] = 0;
      wm.workspace_win_premax_w[i][j] = 0;
      wm.workspace_win_premax_h[i][j] = 0;
    }
  }
}

int is_dock_window(xcb_window_t win) {
  xcb_get_property_cookie_t tc = xcb_get_property(
      wm.conn, 0, win, A_NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 12);
  xcb_get_property_reply_t *tr = xcb_get_property_reply(wm.conn, tc, NULL);
  if (tr && tr->type == XCB_ATOM_ATOM &&
      xcb_get_property_value_length(tr) > 0) {
    xcb_atom_t *types = (xcb_atom_t *)xcb_get_property_value(tr);
    int count = xcb_get_property_value_length(tr) / (int)sizeof(xcb_atom_t);
    for (int i = 0; i < count; i++) {
      if (types[i] == A_NET_WM_WINDOW_TYPE_DOCK) {
        free(tr);
        return 1;
      }
    }
  }
  if (tr)
    free(tr);

  xcb_get_property_cookie_t cc = xcb_get_property(
      wm.conn, 0, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 128);
  xcb_get_property_reply_t *cr = xcb_get_property_reply(wm.conn, cc, NULL);
  int matched = 0;
  if (cr && cr->type == XCB_ATOM_STRING && cr->format == 8) {
    int len = xcb_get_property_value_length(cr);
    char buf[129];
    int n = len < 128 ? len : 128;
    memcpy(buf, xcb_get_property_value(cr), n);
    buf[n] = '\0';
    for (int i = 0; i < n; i++)
      buf[i] = (char)tolower((unsigned char)buf[i]);

    const char *dock_names[] = {"dock",    "bar",    "panel", "quickshell",
                                "polybar", "waybar", "tint2"};
    for (size_t i = 0; i < sizeof(dock_names) / sizeof(dock_names[0]); i++) {
      if (strstr(buf, dock_names[i])) {
        matched = 1;
        break;
      }
    }
  }
  if (cr)
    free(cr);
  return matched;
}

int is_utility_window(xcb_window_t win) {
  xcb_get_property_cookie_t cc = xcb_get_property(
      wm.conn, 0, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 128);
  xcb_get_property_reply_t *cr = xcb_get_property_reply(wm.conn, cc, NULL);
  int matched = 0;
  if (cr && cr->type == XCB_ATOM_STRING && cr->format == 8) {
    int len = xcb_get_property_value_length(cr);
    char buf[129];
    int n = len < 128 ? len : 128;
    memcpy(buf, xcb_get_property_value(cr), n);
    buf[n] = '\0';
    for (int i = 0; i < n; i++)
      buf[i] = (char)tolower((unsigned char)buf[i]);

    // const char *utility_names[] = {"waypaper", "picom", "compton",
    // "gtk-picker",
    //                                "spectacle"};
    const char *utility_names[] = {"picom", "plank", "spectacle"};
    for (size_t i = 0; i < sizeof(utility_names) / sizeof(utility_names[0]);
         i++) {
      if (strstr(buf, utility_names[i])) {
        matched = 1;
        break;
      }
    }
  }
  if (cr)
    free(cr);
  return matched;
}

static int window_class_contains(xcb_window_t win, const char **names,
                                 int count) {
  xcb_get_property_cookie_t cc = xcb_get_property(
      wm.conn, 0, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 128);
  xcb_get_property_reply_t *cr = xcb_get_property_reply(wm.conn, cc, NULL);
  int matched = 0;
  if (cr && cr->type == XCB_ATOM_STRING && cr->format == 8) {
    int len = xcb_get_property_value_length(cr);
    char buf[129];
    int n = len < 128 ? len : 128;
    memcpy(buf, xcb_get_property_value(cr), n);
    buf[n] = '\0';
    for (int i = 0; i < n; i++)
      buf[i] = (char)tolower((unsigned char)buf[i]);

    for (int i = 0; i < count; i++) {
      if (strstr(buf, names[i])) {
        matched = 1;
        break;
      }
    }
  }
  if (cr)
    free(cr);
  return matched;
}

int is_always_float_window(xcb_window_t win) {
  const char *names[] = {"mpv", "vlc", "gwenview"};
  return window_class_contains(win, names, 1);
}

int is_dialog_window(xcb_window_t win) {
  xcb_get_property_cookie_t tc = xcb_get_property(
      wm.conn, 0, win, A_NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 0, 12);
  xcb_get_property_reply_t *tr = xcb_get_property_reply(wm.conn, tc, NULL);
  if (tr && tr->type == XCB_ATOM_ATOM &&
      xcb_get_property_value_length(tr) > 0) {
    xcb_atom_t *types = (xcb_atom_t *)xcb_get_property_value(tr);
    int count = xcb_get_property_value_length(tr) / (int)sizeof(xcb_atom_t);
    for (int i = 0; i < count; i++) {
      if (types[i] == A_NET_WM_WINDOW_TYPE_DIALOG) {
        free(tr);
        return 1;
      }
    }
  }
  if (tr)
    free(tr);

  xcb_get_property_cookie_t xc = xcb_get_property(
      wm.conn, 0, win, A_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1);
  xcb_get_property_reply_t *xr = xcb_get_property_reply(wm.conn, xc, NULL);
  int has_transient = 0;
  if (xr && xcb_get_property_value_length(xr) > 0) {
    xcb_window_t owner = *(xcb_window_t *)xcb_get_property_value(xr);
    if (owner != wm.root && owner != win && is_window_managed(owner)) {
      has_transient = 1;
    }
  }
  if (xr)
    free(xr);
  if (has_transient)
    return 1;

  xcb_get_property_cookie_t cc = xcb_get_property(
      wm.conn, 0, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 128);
  xcb_get_property_reply_t *cr = xcb_get_property_reply(wm.conn, cc, NULL);
  int matched = 0;
  if (cr && cr->type == XCB_ATOM_STRING && cr->format == 8) {
    int len = xcb_get_property_value_length(cr);
    char buf[129];
    int n = len < 128 ? len : 128;
    memcpy(buf, xcb_get_property_value(cr), n);
    buf[n] = '\0';
    for (int i = 0; i < n; i++)
      buf[i] = (char)tolower((unsigned char)buf[i]);

    const char *dialog_names[] = {
        "polkit",      "policykit",    "authentication",    "askpass",
        "pinentry",    "gcr-prompter", "authagent",         "auth-agent",
        "filechooser", "file-chooser", "xdg-desktop-portal"};
    for (size_t i = 0; i < sizeof(dialog_names) / sizeof(dialog_names[0]);
         i++) {
      if (strstr(buf, dialog_names[i])) {
        matched = 1;
        break;
      }
    }
  }
  if (cr)
    free(cr);
  if (matched)
    return 1;

  xcb_get_property_cookie_t hc = xcb_icccm_get_wm_normal_hints(wm.conn, win);
  xcb_size_hints_t hints;
  if (xcb_icccm_get_wm_normal_hints_reply(wm.conn, hc, &hints, NULL)) {
    int has_min = hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE;
    int has_max = hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    if (has_min && has_max && hints.min_width == hints.max_width &&
        hints.min_height == hints.max_height) {
      int fixed_w = hints.min_width;
      int fixed_h = hints.min_height;
      if (fixed_w > 0 && fixed_h > 0 &&
          fixed_w < (wm.screen_width * 70) / 100 &&
          fixed_h < (wm.screen_height * 70) / 100) {
        return 1;
      }
    }
  }
  return 0;
}

xcb_window_t find_dialog_owner_window(xcb_window_t dialog_win, int ws) {
  xcb_get_property_cookie_t xc = xcb_get_property(
      wm.conn, 0, dialog_win, A_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1);
  xcb_get_property_reply_t *xr = xcb_get_property_reply(wm.conn, xc, NULL);
  xcb_window_t owner = XCB_WINDOW_NONE;
  if (xr && xcb_get_property_value_length(xr) > 0) {
    xcb_window_t candidate = *(xcb_window_t *)xcb_get_property_value(xr);
    for (int j = 0; j < wm.workspace_counts[ws]; j++) {
      if (wm.workspace_windows[ws][j] == candidate) {
        owner = candidate;
        break;
      }
    }
  }
  if (xr)
    free(xr);

  if (owner == XCB_WINDOW_NONE && ws >= 0 && ws < MAX_WORKSPACES &&
      wm.workspace_counts[ws] > 0) {
    int idx = wm.workspace_focus_idx[ws];
    if (idx >= 0 && idx < wm.workspace_counts[ws])
      owner = wm.workspace_windows[ws][idx];
  }
  return owner;
}

void add_dock_window(xcb_window_t win) {
  if (wm.dock_count < MAX_WINDOWS) {
    wm.dock_windows[wm.dock_count++] = win;
  }
  uint32_t all_desktops = 0xFFFFFFFF;
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, win, A_NET_WM_DESKTOP,
                      XCB_ATOM_CARDINAL, 32, 1, &all_desktops);
}

void raise_docks() {
  for (int i = 0; i < wm.dock_count; i++) {
    uint32_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
    uint32_t values[] = {XCB_STACK_MODE_ABOVE};
    xcb_configure_window(wm.conn, wm.dock_windows[i], mask, values);
  }

}

int remove_dock_window(xcb_window_t win) {
  for (int i = 0; i < wm.dock_count; i++) {
    if (wm.dock_windows[i] == win) {
      for (int k = i; k < wm.dock_count - 1; k++) {
        wm.dock_windows[k] = wm.dock_windows[k + 1];
      }
      wm.dock_count--;
      return 1;
    }
  }
  return 0;
}

void add_dialog_window(xcb_window_t win, int owner_ws,
                       xcb_window_t prev_focus) {
  int existing = -1;
  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] == win) {
      existing = i;
      break;
    }
  }

  int16_t x = 0, y = 0;
  xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
  xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
  if (geo) {
    x = geo->x;
    y = geo->y;
    free(geo);
  }

  if (existing >= 0) {
    wm.dialog_owner_ws[existing] = owner_ws;
    wm.dialog_x[existing] = x;
    wm.dialog_y[existing] = y;
  } else if (wm.dialog_count < MAX_WINDOWS) {
    wm.dialog_windows[wm.dialog_count] = win;
    wm.dialog_owner_ws[wm.dialog_count] = owner_ws;
    wm.dialog_x[wm.dialog_count] = x;
    wm.dialog_y[wm.dialog_count] = y;
    wm.dialog_prev_focus[wm.dialog_count] =
        (prev_focus != win) ? prev_focus : XCB_WINDOW_NONE;
    wm.dialog_count++;
  }
  set_window_desktop(win, owner_ws);

  xcb_window_t owner_win = find_dialog_owner_window(win, owner_ws);
  if (owner_win != XCB_WINDOW_NONE) {
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, win, A_WM_DIALOG_OWNER,
                        XCB_ATOM_WINDOW, 32, 1, &owner_win);
  } else {
    xcb_delete_property(wm.conn, win, A_WM_DIALOG_OWNER);
  }
}

int remove_dialog_window(xcb_window_t win) {
  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] == win) {
      for (int k = i; k < wm.dialog_count - 1; k++) {
        wm.dialog_windows[k] = wm.dialog_windows[k + 1];
        wm.dialog_owner_ws[k] = wm.dialog_owner_ws[k + 1];
        wm.dialog_x[k] = wm.dialog_x[k + 1];
        wm.dialog_y[k] = wm.dialog_y[k + 1];
        wm.dialog_prev_focus[k] = wm.dialog_prev_focus[k + 1];
      }
      wm.dialog_count--;
      for (int w = 0; w < MAX_WORKSPACES; w++) {
        if (wm.workspace_last_active[w] == win) {
          wm.workspace_last_active[w] = XCB_WINDOW_NONE;
        }
      }
      return 1;
    }
  }
  return 0;
}

static void update_dialog_stack_owner(xcb_window_t win) {
  int di = -1;
  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] == win) {
      di = i;
      break;
    }
  }
  if (di < 0)
    return;

  int ws = wm.dialog_owner_ws[di];
  if (ws != wm.current_workspace)
    return;

  xcb_get_geometry_cookie_t dgc = xcb_get_geometry(wm.conn, win);
  xcb_get_geometry_reply_t *dgeo = xcb_get_geometry_reply(wm.conn, dgc, NULL);
  if (!dgeo)
    return;
  int16_t dx1 = dgeo->x;
  int16_t dy1 = dgeo->y;
  int16_t dx2 = (int16_t)(dgeo->x + dgeo->width);
  int16_t dy2 = (int16_t)(dgeo->y + dgeo->height);
  free(dgeo);

  xcb_query_tree_cookie_t qc = xcb_query_tree(wm.conn, wm.root);
  xcb_query_tree_reply_t *qr = xcb_query_tree_reply(wm.conn, qc, NULL);
  xcb_window_t *kids = qr ? xcb_query_tree_children(qr) : NULL;
  int nkids = qr ? xcb_query_tree_children_length(qr) : 0;

  xcb_window_t best_win = XCB_WINDOW_NONE;
  int best_rank = -1;

  for (int j = 0; j < wm.workspace_counts[ws]; j++) {
    xcb_window_t cand = wm.workspace_windows[ws][j];
    if (cand == win)
      continue;

    xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, cand);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
    if (!geo)
      continue;
    int16_t ax1 = geo->x;
    int16_t ay1 = geo->y;
    int16_t ax2 = (int16_t)(geo->x + geo->width);
    int16_t ay2 = (int16_t)(geo->y + geo->height);
    free(geo);

    if (ax1 < dx2 && ax2 > dx1 && ay1 < dy2 && ay2 > dy1) {
      int rank = -1;
      for (int k = 0; k < nkids; k++) {
        if (kids[k] == cand) {
          rank = k;
          break;
        }
      }
      if (rank > best_rank) {
        best_rank = rank;
        best_win = cand;
      }
    }
  }
  if (qr)
    free(qr);

  if (best_win != XCB_WINDOW_NONE) {
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, win, A_WM_DIALOG_OWNER,
                        XCB_ATOM_WINDOW, 32, 1, &best_win);
  }
}

static int window_is_alive(xcb_window_t win) {
  if (win == XCB_WINDOW_NONE)
    return 0;
  xcb_get_window_attributes_cookie_t c =
      xcb_get_window_attributes(wm.conn, win);
  xcb_get_window_attributes_reply_t *r =
      xcb_get_window_attributes_reply(wm.conn, c, NULL);
  int alive = r != NULL;
  if (r)
    free(r);
  return alive;
}

void get_usable_area(int16_t *out_x, int16_t *out_y, uint16_t *out_w,
                     uint16_t *out_h) {
  int x = 0, y = 0;
  int w = wm.screen_width;
  int h = wm.screen_height;

  for (int i = 0; i < wm.dock_count; i++) {
    xcb_get_geometry_cookie_t gc =
        xcb_get_geometry(wm.conn, wm.dock_windows[i]);
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(wm.conn, gc, NULL);
    if (!g)
      continue;

    int dock_x = g->x, dock_y = g->y;
    int dock_w = g->width, dock_h = g->height;
    free(g);

    if (dock_w >= (wm.screen_width * 6) / 10) {
      if (dock_y <= wm.screen_height / 2) {
        int bottom_edge = dock_y + dock_h;
        if (bottom_edge > y) {
          h -= (bottom_edge - y);
          y = bottom_edge;
        }
      } else {
        int top_edge = dock_y;
        if (top_edge < y + h) {
          h -= ((y + h) - top_edge);
        }
      }
    } else if (dock_h >= (wm.screen_height * 6) / 10) {
      if (dock_x <= wm.screen_width / 2) {
        int right_edge = dock_x + dock_w;
        if (right_edge > x) {
          w -= (right_edge - x);
          x = right_edge;
        }
      } else {
        int left_edge = dock_x;
        if (left_edge < x + w) {
          w -= ((x + w) - left_edge);
        }
      }
    }
  }

  if (w < MIN_WIN_SIZE)
    w = MIN_WIN_SIZE;
  if (h < MIN_WIN_SIZE)
    h = MIN_WIN_SIZE;

  *out_x = (int16_t)x;
  *out_y = (int16_t)y;
  *out_w = (uint16_t)w;
  *out_h = (uint16_t)h;
}

void get_window_target(int16_t *target_x, int16_t *target_y,
                           uint16_t *target_w, uint16_t *target_h) {
  int16_t area_x, area_y;
  uint16_t area_w, area_h;
  get_usable_area(&area_x, &area_y, &area_w, &area_h);

  uint16_t gap2 = (uint16_t)(WINDOW_GAP * 2);

  *target_x = (int16_t)(area_x + WINDOW_GAP);
  *target_y = (int16_t)(area_y + WINDOW_GAP);
  *target_w = (area_w > gap2) ? (uint16_t)(area_w - gap2) : area_w;
  *target_h = (area_h > gap2) ? (uint16_t)(area_h - gap2) : area_h;
}

struct TileNode {
  int is_leaf;
  xcb_window_t window;
  int split_vertical;
  TileNode *a;
  TileNode *b;
};

static TileNode *tile_new_leaf(xcb_window_t win) {
  TileNode *n = malloc(sizeof(TileNode));
  n->is_leaf = 1;
  n->window = win;
  n->split_vertical = 0;
  n->a = NULL;
  n->b = NULL;
  return n;
}

void tile_free(TileNode *n) {
  if (!n)
    return;
  if (!n->is_leaf) {
    tile_free(n->a);
    tile_free(n->b);
  }
  free(n);
}

static TileNode **tile_find_leaf_slot(TileNode **link, xcb_window_t win) {
  if (!link || !*link)
    return NULL;
  if ((*link)->is_leaf)
    return ((*link)->window == win) ? link : NULL;
  TileNode **found = tile_find_leaf_slot(&(*link)->a, win);
  if (found)
    return found;
  return tile_find_leaf_slot(&(*link)->b, win);
}

static TileNode **tile_find_parent_slot(TileNode **link, xcb_window_t win,
                                        int *is_a) {
  if (!link || !*link || (*link)->is_leaf)
    return NULL;
  TileNode *n = *link;
  if (n->a && n->a->is_leaf && n->a->window == win) {
    *is_a = 1;
    return link;
  }
  if (n->b && n->b->is_leaf && n->b->window == win) {
    *is_a = 0;
    return link;
  }
  TileNode **found = tile_find_parent_slot(&n->a, win, is_a);
  if (found)
    return found;
  return tile_find_parent_slot(&n->b, win, is_a);
}

void tile_remove(TileNode **root, xcb_window_t win) {
  if (!root || !*root)
    return;

  if ((*root)->is_leaf) {
    if ((*root)->window == win) {
      free(*root);
      *root = NULL;
    }
    return;
  }

  int is_a = 0;
  TileNode **pslot = tile_find_parent_slot(root, win, &is_a);
  if (!pslot)
    return;

  TileNode *parent = *pslot;
  TileNode *leaf = is_a ? parent->a : parent->b;
  TileNode *sibling = is_a ? parent->b : parent->a;

  *pslot = sibling;
  free(leaf);
  free(parent);
}

void tile_insert_split(TileNode **root, xcb_window_t target_win,
                       xcb_window_t new_win, int zone) {
  if (!root)
    return;
  if (!*root) {
    *root = tile_new_leaf(new_win);
    return;
  }

  TileNode **slot = tile_find_leaf_slot(root, target_win);
  if (!slot) {
    TileNode *old_root = *root;
    TileNode *new_leaf = tile_new_leaf(new_win);
    TileNode *split = malloc(sizeof(TileNode));
    split->is_leaf = 0;
    split->split_vertical = (zone == 0 || zone == 1);
    if (zone == 0 || zone == 2) {
      split->a = new_leaf;
      split->b = old_root;
    } else {
      split->a = old_root;
      split->b = new_leaf;
    }
    *root = split;
    return;
  }

  TileNode *old_leaf = *slot;
  TileNode *new_leaf = tile_new_leaf(new_win);
  TileNode *split = malloc(sizeof(TileNode));
  split->is_leaf = 0;
  split->split_vertical = (zone == 0 || zone == 1);
  if (zone == 0 || zone == 2) {
    split->a = new_leaf;
    split->b = old_leaf;
  } else {
    split->a = old_leaf;
    split->b = new_leaf;
  }
  *slot = split;
}

void tile_add_default(TileNode **root, xcb_window_t win) {
  if (!root)
    return;
  if (!*root) {
    *root = tile_new_leaf(win);
    return;
  }

  TileNode *n = *root;
  int depth = 0;
  while (!n->is_leaf) {
    depth++;
    n = n->b;
  }
  int zone = (depth % 2 == 0) ? 1 : 3;
  tile_insert_split(root, n->window, win, zone);
}

static int find_window_idx(int ws, xcb_window_t win) {
  for (int i = 0; i < wm.workspace_counts[ws]; i++) {
    if (wm.workspace_windows[ws][i] == win)
      return i;
  }
  return -1;
}

void tile_layout(int ws, TileNode *node, int16_t x, int16_t y, uint16_t w,
                 uint16_t h) {
  if (!node)
    return;

  if (node->is_leaf) {
    int idx = find_window_idx(ws, node->window);
    if (idx < 0)
      return;
    wm.workspace_win_x[ws][idx] = x;
    wm.workspace_win_y[ws][idx] = y;
    // wm.workspace_win_w[ws][idx] = w;
    // wm.workspace_win_h[ws][idx] = h;
    wm.workspace_win_w[ws][idx] = (w > BORDER_WIDTH) ? w - BORDER_WIDTH : w;
    wm.workspace_win_h[ws][idx] = (h > BORDER_WIDTH) ? h - BORDER_WIDTH : h;
    return;
  }

  if (node->split_vertical) {
    uint16_t w1 = (w > WINDOW_GAP) ? (uint16_t)((w - WINDOW_GAP) / 2) : w;
    uint16_t w2 = (w > w1 + WINDOW_GAP) ? (uint16_t)(w - w1 - WINDOW_GAP) : w1;
    tile_layout(ws, node->a, x, y, w1, h);
    tile_layout(ws, node->b, (int16_t)(x + w1 + WINDOW_GAP), y, w2, h);
  } else {
    uint16_t h1 = (h > WINDOW_GAP) ? (uint16_t)((h - WINDOW_GAP) / 2) : h;
    uint16_t h2 = (h > h1 + WINDOW_GAP) ? (uint16_t)(h - h1 - WINDOW_GAP) : h1;
    tile_layout(ws, node->a, x, y, w, h1);
    tile_layout(ws, node->b, x, (int16_t)(y + h1 + WINDOW_GAP), w, h2);
  }
}

void compute_tiling_layout(int ws) {
  int count = wm.workspace_counts[ws];
  if (count == 0)
    return;

  int16_t area_x, area_y;
  uint16_t area_w, area_h;
  get_usable_area(&area_x, &area_y, &area_w, &area_h);
  uint16_t gap2 = (uint16_t)(WINDOW_GAP * 2);
  int16_t inset_x = (int16_t)(area_x + WINDOW_GAP);
  int16_t inset_y = (int16_t)(area_y + WINDOW_GAP);
  uint16_t inset_w = (area_w > gap2) ? (uint16_t)(area_w - gap2) : area_w;
  uint16_t inset_h = (area_h > gap2) ? (uint16_t)(area_h - gap2) : area_h;

  tile_layout(ws, wm.tile_root[ws], inset_x, inset_y, inset_w, inset_h);
}

void apply_tiling_geometry(int ws) {
  if (ws < 0 || ws >= MAX_WORKSPACES || !wm.workspace_tiling[ws])
    return;

  int count = wm.workspace_counts[ws];
  for (int i = 0; i < count; i++) {
    if (wm.workspace_win_force_float[ws][i])
      continue;

    xcb_window_t win = wm.workspace_windows[ws][i];
    if (win == XCB_WINDOW_NONE)
      continue;

    uint16_t w = (wm.workspace_win_w[ws][i] > 0)
                     ? (uint16_t)wm.workspace_win_w[ws][i]
                     : 1;
    uint16_t h = (wm.workspace_win_h[ws][i] > 0)
                     ? (uint16_t)wm.workspace_win_h[ws][i]
                     : 1;
    uint32_t values[] = {(uint32_t)(int32_t)wm.workspace_win_x[ws][i],
                         (uint32_t)(int32_t)wm.workspace_win_y[ws][i], w, h};

    xcb_configure_window(wm.conn, win,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    uint32_t border_w = BORDER_WIDTH;
    uint32_t border_color =
        (win == wm.focused) ? BORDER_HOVER_COLOR : BORDER_NORMAL_COLOR;
    xcb_change_window_attributes(wm.conn, win, XCB_CW_BORDER_PIXEL,
                                 (uint32_t[]){border_color});
    xcb_configure_window(wm.conn, win, XCB_CONFIG_WINDOW_BORDER_WIDTH,
                         &border_w);
  }
}

void compute_floating_layout(int ws) {
  int count = wm.workspace_counts[ws];
  if (count == 0)
    return;

  int16_t area_x, area_y;
  uint16_t area_w, area_h;
  get_usable_area(&area_x, &area_y, &area_w, &area_h);

  int16_t center_x = area_x + (area_w / 2);
  int16_t center_y = area_y + (area_h / 2);

  for (int i = 0; i < count; i++) {
    xcb_window_t win = wm.workspace_windows[ws][i];

    int is_new =
        (wm.workspace_win_x[ws][i] == 0 && wm.workspace_win_y[ws][i] == 0 &&
         wm.workspace_win_w[ws][i] == 0 && wm.workspace_win_h[ws][i] == 0);

    if (is_new) {
      xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
      xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);

      // Keep every newly-created floating window centered.
      if (geo) {
        wm.workspace_win_w[ws][i] = geo->width;
        wm.workspace_win_h[ws][i] = geo->height;
        wm.workspace_win_x[ws][i] = center_x - (geo->width / 2);
        wm.workspace_win_y[ws][i] = center_y - (geo->height / 2);
        free(geo);
      } else {
        uint16_t def_w = (uint16_t)(area_w * 0.6);
        uint16_t def_h = (uint16_t)(area_h * 0.6);
        wm.workspace_win_w[ws][i] = def_w;
        wm.workspace_win_h[ws][i] = def_h;
        wm.workspace_win_x[ws][i] = center_x - (def_w / 2);
        wm.workspace_win_y[ws][i] = center_y - (def_h / 2);
      }
    }
  }
}

void float_single_window(int ws, int idx) {
  xcb_window_t win = wm.workspace_windows[ws][idx];

  int is_new =
      (wm.workspace_win_x[ws][idx] == 0 && wm.workspace_win_y[ws][idx] == 0 &&
       wm.workspace_win_w[ws][idx] == 0 && wm.workspace_win_h[ws][idx] == 0);
  if (!is_new)
    return;

  int16_t area_x, area_y;
  uint16_t area_w, area_h;
  get_usable_area(&area_x, &area_y, &area_w, &area_h);
  int16_t center_x = area_x + (area_w / 2);
  int16_t center_y = area_y + (area_h / 2);

  xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
  xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
  if (geo) {
    wm.workspace_win_w[ws][idx] = geo->width;
    wm.workspace_win_h[ws][idx] = geo->height;
    wm.workspace_win_x[ws][idx] = center_x - (geo->width / 2);
    wm.workspace_win_y[ws][idx] = center_y - (geo->height / 2);
    free(geo);
  } else {
    uint16_t def_w = (uint16_t)(area_w * 0.5);
    uint16_t def_h = (uint16_t)(area_h * 0.5);
    wm.workspace_win_w[ws][idx] = def_w;
    wm.workspace_win_h[ws][idx] = def_h;
    wm.workspace_win_x[ws][idx] = center_x - (def_w / 2);
    wm.workspace_win_y[ws][idx] = center_y - (def_h / 2);
  }
}

void compute_reel_layout(int ws) {
  int count = wm.workspace_counts[ws];
  if (count == 0)
    return;

  if (wm.workspace_tiling[ws]) {
    compute_tiling_layout(ws);
    return;
  }

  if (wm.workspace_floating[ws]) {
    compute_floating_layout(ws);
    return;
  }

  int16_t target_x, target_y;
  uint16_t target_w, target_h;
  get_window_target(&target_x, &target_y, &target_w, &target_h);
  int step = target_w + WINDOW_GAP;

  int focus_idx = wm.workspace_focus_idx[ws];
  if (focus_idx < 0)
    focus_idx = 0;
  if (focus_idx >= count)
    focus_idx = count - 1;

  for (int i = 0; i < count; i++) {
    xcb_window_t win = wm.workspace_windows[ws][i];

    if (wm.workspace_win_force_float[ws][i] && is_always_float_window(win)) {
      float_single_window(ws, i);
      continue;
    }

    int32_t slot_x = target_x + (i - focus_idx) * step - BORDER_WIDTH;
    int32_t slot_y = target_y;
    if (wm.workspace_win_force_float[ws][i]) {
      wm.workspace_win_x[ws][i] = slot_x + wm.workspace_win_slide_dx[ws][i];
      wm.workspace_win_y[ws][i] = slot_y + wm.workspace_win_slide_dy[ws][i];
    } else {
      wm.workspace_win_x[ws][i] = slot_x;
      wm.workspace_win_y[ws][i] = slot_y;
    }
  }

  wm.workspace_reel_anchor_idx[ws] = focus_idx;
}

void apply_reel_geometry(int ws) {
  int16_t target_x, target_y;
  uint16_t target_w, target_h;
  get_window_target(&target_x, &target_y, &target_w, &target_h);
  (void)target_x;
  (void)target_y;

  int count = wm.workspace_counts[ws];
  for (int i = 0; i < count; i++) {
    xcb_window_t win = wm.workspace_windows[ws][i];
    uint16_t w = (wm.workspace_win_w[ws][i] > 0)
                     ? (uint16_t)wm.workspace_win_w[ws][i]
                     : target_w;
    uint16_t h = (wm.workspace_win_h[ws][i] > 0)
                     ? (uint16_t)wm.workspace_win_h[ws][i]
                     : target_h;
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

    uint32_t values[] = {(uint32_t)(int32_t)wm.workspace_win_x[ws][i],
                         (uint32_t)(int32_t)wm.workspace_win_y[ws][i], w, h};

    xcb_configure_window(wm.conn, win, mask, values);
    uint32_t border_w = BORDER_WIDTH;
    uint32_t border_color =
        (win == wm.focused) ? BORDER_HOVER_COLOR : BORDER_NORMAL_COLOR;
    xcb_change_window_attributes(wm.conn, win, XCB_CW_BORDER_PIXEL,
                                 (uint32_t[]){border_color});
    xcb_configure_window(wm.conn, win, XCB_CONFIG_WINDOW_BORDER_WIDTH,
                         &border_w);
  }
  xcb_flush(wm.conn);
}

void apply_reel_geometry_parked(int ws) {
  if (ws < 0 || ws >= MAX_WORKSPACES)
    return;

  int16_t target_x, target_y;
  uint16_t target_w, target_h;
  get_window_target(&target_x, &target_y, &target_w, &target_h);
  (void)target_x;
  (void)target_y;

  int32_t offset = wm.screen_width * (ws + 2);
  int count = wm.workspace_counts[ws];
  for (int i = 0; i < count; i++) {
    xcb_window_t win = wm.workspace_windows[ws][i];
    if (win == XCB_WINDOW_NONE)
      continue;

    uint16_t w = (wm.workspace_win_w[ws][i] > 0)
                     ? (uint16_t)wm.workspace_win_w[ws][i]
                     : target_w;
    uint16_t h = (wm.workspace_win_h[ws][i] > 0)
                     ? (uint16_t)wm.workspace_win_h[ws][i]
                     : target_h;

    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    uint32_t values[] = {
        (uint32_t)(int32_t)(wm.workspace_win_x[ws][i] + offset),
        (uint32_t)(int32_t)wm.workspace_win_y[ws][i], w, h};
    xcb_configure_window(wm.conn, win, mask, values);
  }
  xcb_flush(wm.conn);
}

void swap_reel_windows(int ws, int idx_a, int idx_b) {
  int count = wm.workspace_counts[ws];
  if (idx_a < 0 || idx_b < 0 || idx_a >= count || idx_b >= count)
    return;
  if (idx_a == idx_b)
    return;

  xcb_window_t tmp_win = wm.workspace_windows[ws][idx_a];
  wm.workspace_windows[ws][idx_a] = wm.workspace_windows[ws][idx_b];
  wm.workspace_windows[ws][idx_b] = tmp_win;

  int tmp_w = wm.workspace_win_w[ws][idx_a];
  wm.workspace_win_w[ws][idx_a] = wm.workspace_win_w[ws][idx_b];
  wm.workspace_win_w[ws][idx_b] = tmp_w;

  int tmp_h = wm.workspace_win_h[ws][idx_a];
  wm.workspace_win_h[ws][idx_a] = wm.workspace_win_h[ws][idx_b];
  wm.workspace_win_h[ws][idx_b] = tmp_h;

  int tmp_ff = wm.workspace_win_force_float[ws][idx_a];
  wm.workspace_win_force_float[ws][idx_a] =
      wm.workspace_win_force_float[ws][idx_b];
  wm.workspace_win_force_float[ws][idx_b] = tmp_ff;

  int tmp_dx = wm.workspace_win_slide_dx[ws][idx_a];
  wm.workspace_win_slide_dx[ws][idx_a] = wm.workspace_win_slide_dx[ws][idx_b];
  wm.workspace_win_slide_dx[ws][idx_b] = tmp_dx;
  int tmp_dy = wm.workspace_win_slide_dy[ws][idx_a];
  wm.workspace_win_slide_dy[ws][idx_a] = wm.workspace_win_slide_dy[ws][idx_b];
  wm.workspace_win_slide_dy[ws][idx_b] = tmp_dy;

  // Focus us window ke sath jaye jo hila hai (jispe hum khade the).
  wm.workspace_focus_idx[ws] = idx_b;

  compute_reel_layout(ws);
  apply_reel_geometry(ws);
  focus_window(wm.workspace_windows[ws][idx_b]);
}

void jump_reel_focus(int ws, int new_idx) {
  int count = wm.workspace_counts[ws];
  if (count == 0)
    return;
  if (new_idx < 0)
    new_idx = 0;
  if (new_idx >= count)
    new_idx = count - 1;

  int old_idx = wm.workspace_focus_idx[ws];
  if (old_idx < 0)
    old_idx = 0;
  if (old_idx >= count)
    old_idx = count - 1;

  if (new_idx == old_idx) {
    focus_window(wm.workspace_windows[ws][new_idx]);
    return;
  }

  if (wm.workspace_tiling[ws] || wm.workspace_floating[ws]) {
    wm.workspace_focus_idx[ws] = new_idx;
    focus_window(wm.workspace_windows[ws][new_idx]);
    return;
  }

  int16_t target_x, target_y;
  uint16_t target_w, target_h;
  get_window_target(&target_x, &target_y, &target_w, &target_h);
  int step = target_w + WINDOW_GAP;

  int32_t old_x[MAX_WINDOWS];
  int32_t new_x[MAX_WINDOWS];
  int32_t old_y[MAX_WINDOWS];
  int32_t new_y[MAX_WINDOWS];

  for (int i = 0; i < count; i++) {
    int32_t old_slot = target_x + (i - old_idx) * step - BORDER_WIDTH;
    int32_t new_slot = target_x + (i - new_idx) * step - BORDER_WIDTH;

    if (wm.workspace_win_force_float[ws][i]) {
      int32_t custom_x = wm.workspace_win_x[ws][i];
      int32_t custom_y = wm.workspace_win_y[ws][i];

      xcb_get_geometry_cookie_t gc =
          xcb_get_geometry(wm.conn, wm.workspace_windows[ws][i]);
      xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
      if (geo) {
        custom_x = geo->x;
        custom_y = geo->y;
        free(geo);
      }

      int32_t dx = custom_x - old_slot;
      int32_t dy = custom_y - target_y;

      old_x[i] = custom_x;
      new_x[i] = new_slot + dx;
      old_y[i] = custom_y;
      new_y[i] = target_y + dy;

      wm.workspace_win_slide_dx[ws][i] = dx;
      wm.workspace_win_slide_dy[ws][i] = dy;
    } else {
      old_x[i] = old_slot;
      new_x[i] = new_slot;
      old_y[i] = target_y;
      new_y[i] = target_y;
    }
  }

#if ANIM_ENABLED
  for (int s = 1; s <= ANIM_WS_STEPS; s++) {
    double t = ease_out_cubic((double)s / ANIM_WS_STEPS);
    for (int i = 0; i < count; i++) {
      xcb_window_t win = wm.workspace_windows[ws][i];
      int16_t x = (int16_t)(old_x[i] + (new_x[i] - old_x[i]) * t);
      int16_t y = (int16_t)(old_y[i] + (new_y[i] - old_y[i]) * t);
      uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t values[] = {x, y};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }
    xcb_flush(wm.conn);
    usleep(ANIM_WS_DELAY_US);
  }
#endif

  for (int i = 0; i < count; i++) {
    xcb_window_t win = wm.workspace_windows[ws][i];
    int16_t y = (int16_t)new_y[i];
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    int32_t values[] = {new_x[i], y};
    xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);

    wm.workspace_win_x[ws][i] = new_x[i];
    wm.workspace_win_y[ws][i] = new_y[i];
  }

  wm.workspace_focus_idx[ws] = new_idx;
  wm.workspace_reel_anchor_idx[ws] = new_idx;
  xcb_flush(wm.conn);
  focus_window(wm.workspace_windows[ws][new_idx]);
}

void slide_reel_focus(int ws, int direction) {
  int count = wm.workspace_counts[ws];
  if (count < 2)
    return;

  int idx = wm.workspace_focus_idx[ws];
  if (idx < 0)
    idx = 0;
  if (idx >= count)
    idx = count - 1;

  jump_reel_focus(ws, idx + direction);
}






void maximize_focused_window() {
  int ws = wm.current_workspace;
  if (wm.workspace_tiling[ws])
    return;

  int idx = find_window_idx(ws, wm.focused);
  if (idx < 0)
    return;

  xcb_window_t win = wm.workspace_windows[ws][idx];
  if (win == XCB_WINDOW_NONE)
    return;

  if (wm.workspace_win_maximized[ws][idx])
    return; // pehle hi maximized hai

  xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
  xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
  if (geo) {
    wm.workspace_win_premax_x[ws][idx] = geo->x;
    wm.workspace_win_premax_y[ws][idx] = geo->y;
    wm.workspace_win_premax_w[ws][idx] = geo->width;
    wm.workspace_win_premax_h[ws][idx] = geo->height;
    free(geo);
  } else {
    wm.workspace_win_premax_x[ws][idx] = (int16_t)wm.workspace_win_x[ws][idx];
    wm.workspace_win_premax_y[ws][idx] = (int16_t)wm.workspace_win_y[ws][idx];
    wm.workspace_win_premax_w[ws][idx] = (uint16_t)wm.workspace_win_w[ws][idx];
    wm.workspace_win_premax_h[ws][idx] = (uint16_t)wm.workspace_win_h[ws][idx];
  }

  int16_t target_x, target_y;
  uint16_t target_w, target_h;
  get_window_target(&target_x, &target_y, &target_w, &target_h);

  wm.workspace_win_x[ws][idx] = target_x;
  wm.workspace_win_y[ws][idx] = target_y;
  wm.workspace_win_w[ws][idx] = target_w;
  wm.workspace_win_h[ws][idx] = target_h;
  wm.workspace_win_maximized[ws][idx] = 1;

  uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
  uint32_t values[] = {(uint32_t)(int32_t)target_x, (uint32_t)(int32_t)target_y,
                       target_w, target_h};
  xcb_configure_window(wm.conn, win, mask, values);
  xcb_flush(wm.conn);

  send_notify("Window Maximized");
}

void unmaximize_focused_window() {
  int ws = wm.current_workspace;
  if (wm.workspace_tiling[ws])
    return;

  int idx = find_window_idx(ws, wm.focused);
  if (idx < 0)
    return;

  xcb_window_t win = wm.workspace_windows[ws][idx];
  if (win == XCB_WINDOW_NONE)
    return;

  if (!wm.workspace_win_maximized[ws][idx])
    return; // maximized nahi thi, kuch nahi karna

  wm.workspace_win_x[ws][idx] = wm.workspace_win_premax_x[ws][idx];
  wm.workspace_win_y[ws][idx] = wm.workspace_win_premax_y[ws][idx];
  wm.workspace_win_w[ws][idx] = wm.workspace_win_premax_w[ws][idx];
  wm.workspace_win_h[ws][idx] = wm.workspace_win_premax_h[ws][idx];
  wm.workspace_win_maximized[ws][idx] = 0;

  uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
  uint32_t values[] = {(uint32_t)(int32_t)wm.workspace_win_x[ws][idx],
                       (uint32_t)(int32_t)wm.workspace_win_y[ws][idx],
                       (uint32_t)wm.workspace_win_w[ws][idx],
                       (uint32_t)wm.workspace_win_h[ws][idx]};
  xcb_configure_window(wm.conn, win, mask, values);
  xcb_flush(wm.conn);

  send_notify("Window Unmaximized");
}

void cycle_focus(int ws, int direction) {

  int reel_count = wm.workspace_counts[ws];

  xcb_window_t combined[MAX_WINDOWS * 2];
  int total = 0;
  for (int i = 0; i < reel_count; i++) {
    combined[total++] = wm.workspace_windows[ws][i];
  }
  int dialog_start = total;
  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_owner_ws[i] == ws) {
      combined[total++] = wm.dialog_windows[i];
    }
  }

  if (total == 0)
    return;

  int cur_idx = -1;
  for (int i = 0; i < total; i++) {
    if (combined[i] == wm.focused) {
      cur_idx = i;
      break;
    }
  }

  int next_idx = (cur_idx < 0) ? 0 : (cur_idx + direction + total) % total;
  xcb_window_t target = combined[next_idx];

  if (next_idx < dialog_start) {
    jump_reel_focus(ws, next_idx);
  } else {
    uint32_t stack_mask = XCB_CONFIG_WINDOW_STACK_MODE;
    uint32_t stack_values[] = {XCB_STACK_MODE_ABOVE};
    xcb_configure_window(wm.conn, target, stack_mask, stack_values);
    focus_window(target);
    send_notify("Focus changed");
  }
}

static void revalidate_force_float_geometry(int ws) {
  if (ws < 0 || ws >= MAX_WORKSPACES)
    return;

  int16_t area_x, area_y;
  uint16_t area_w, area_h;
  get_usable_area(&area_x, &area_y, &area_w, &area_h);

  int count = wm.workspace_counts[ws];
  for (int i = 0; i < count; i++) {
    if (!wm.workspace_win_force_float[ws][i])
      continue;

    int32_t wx = wm.workspace_win_x[ws][i];
    int32_t wy = wm.workspace_win_y[ws][i];
    int32_t ww = wm.workspace_win_w[ws][i];
    int32_t wh = wm.workspace_win_h[ws][i];

    int off_screen = (wx + ww <= area_x) || (wx >= area_x + area_w) ||
                     (wy + wh <= area_y) || (wy >= area_y + area_h);

    if (off_screen) {
      wm.workspace_win_x[ws][i] = 0;
      wm.workspace_win_y[ws][i] = 0;
      wm.workspace_win_w[ws][i] = 0;
      wm.workspace_win_h[ws][i] = 0;
    }
  }
}

static void clear_temporary_force_float_pins(int ws) {
  if (ws < 0 || ws >= MAX_WORKSPACES)
    return;

  int count = wm.workspace_counts[ws];
  for (int i = 0; i < count; i++) {
    if (!wm.workspace_win_force_float[ws][i])
      continue;

    xcb_window_t win = wm.workspace_windows[ws][i];
    if (win == XCB_WINDOW_NONE)
      continue;

    if (!is_always_float_window(win)) {
      wm.workspace_win_force_float[ws][i] = 0;
    }
  }
}

void toggle_tiling_mode() {
  int ws = wm.current_workspace;
  if (ws < 0 || ws >= MAX_WORKSPACES)
    return;

  wm.workspace_tiling[ws] = !wm.workspace_tiling[ws];
  if (wm.workspace_tiling[ws]) {
    wm.workspace_floating[ws] = 0;
  }
  printf("Tiling mode %s for workspace %d\n",
         wm.workspace_tiling[ws] ? "ON" : "OFF", ws + 1);

  //===============3==================================
  send_notify(wm.workspace_tiling[ws] ? "Tiling ON" : "Tiling OFF");
  publish_workspace_modes();

  int count = wm.workspace_counts[ws];
  if (count == 0)
    return;

  clear_temporary_force_float_pins(ws);

  if (wm.workspace_tiling[ws]) {
    revalidate_force_float_geometry(ws);
  }

  for (int i = 0; i < count; i++) {
    if (wm.workspace_win_force_float[ws][i])
      continue;
    wm.workspace_win_x[ws][i] = 0;
    wm.workspace_win_y[ws][i] = 0;
    wm.workspace_win_w[ws][i] = 0;
    wm.workspace_win_h[ws][i] = 0;
    wm.workspace_win_maximized[ws][i] = 0;
  }

  if (wm.workspace_tiling[ws]) {
    tile_free(wm.tile_root[ws]);
    wm.tile_root[ws] = NULL;
    for (int i = 0; i < count; i++) {
      if (wm.workspace_win_force_float[ws][i])
        continue;
      tile_add_default(&wm.tile_root[ws], wm.workspace_windows[ws][i]);
    }
  }

  compute_reel_layout(ws);
  apply_reel_geometry(ws);
  focus_window(wm.workspace_windows[ws][wm.workspace_focus_idx[ws]]);
}

void toggle_floating_mode() {
  int ws = wm.current_workspace;
  if (ws < 0 || ws >= MAX_WORKSPACES)
    return;

  wm.workspace_floating[ws] = !wm.workspace_floating[ws];
  if (wm.workspace_floating[ws]) {
    wm.workspace_tiling[ws] = 0;
  }
  printf("Floating mode %s for workspace %d\n",
         wm.workspace_floating[ws] ? "ON" : "OFF", ws + 1);

  //============4==========================
  send_notify(wm.workspace_floating[ws] ? "Floating ON" : "Floating OFF");
  publish_workspace_modes();

  int count = wm.workspace_counts[ws];
  if (count == 0)
    return;

  clear_temporary_force_float_pins(ws);

  if (wm.workspace_floating[ws]) {
    revalidate_force_float_geometry(ws);
  }

  for (int i = 0; i < count; i++) {
    if (wm.workspace_win_force_float[ws][i])
      continue;
    wm.workspace_win_x[ws][i] = 0;
    wm.workspace_win_y[ws][i] = 0;
    wm.workspace_win_w[ws][i] = 0;
    wm.workspace_win_h[ws][i] = 0;
  }

  compute_reel_layout(ws);
  apply_reel_geometry(ws);
  focus_window(wm.workspace_windows[ws][wm.workspace_focus_idx[ws]]);
}

static void init_cursors_fallback() {
  /* Fallback: old core-font cursors (does NOT follow Xcursor theme) */
  wm.cursor_font = xcb_generate_id(wm.conn);
  xcb_open_font(wm.conn, wm.cursor_font, strlen("cursor"), "cursor");
  wm.cursor_normal = xcb_generate_id(wm.conn);
  xcb_create_glyph_cursor(wm.conn, wm.cursor_normal, wm.cursor_font,
                          wm.cursor_font, XC_left_ptr, XC_left_ptr + 1, 0, 0, 0,
                          0xffff, 0xffff, 0xffff);

  wm.cursor_move = xcb_generate_id(wm.conn);
  xcb_create_glyph_cursor(wm.conn, wm.cursor_move, wm.cursor_font,
                          wm.cursor_font, XC_fleur, XC_fleur + 1, 0, 0, 0,
                          0xffff, 0xffff, 0xffff);

  wm.cursor_resize = xcb_generate_id(wm.conn);
  xcb_create_glyph_cursor(wm.conn, wm.cursor_resize, wm.cursor_font,
                          wm.cursor_font, XC_bottom_right_corner,
                          XC_bottom_right_corner + 1, 0, 0, 0, 0xffff, 0xffff,
                          0xffff);
}

void init_cursors() {
  if (xcb_cursor_context_new(wm.conn, wm.screen, &cursor_ctx) < 0) {
    fprintf(stderr,
            "wm: xcb_cursor_context_new failed, using fallback cursors\n");
    init_cursors_fallback();
  } else {
    wm.cursor_normal = xcb_cursor_load_cursor(cursor_ctx, "left_ptr");
    wm.cursor_move = xcb_cursor_load_cursor(cursor_ctx, "fleur");
    if (wm.cursor_move == XCB_CURSOR_NONE)
      wm.cursor_move = xcb_cursor_load_cursor(cursor_ctx, "move");
    wm.cursor_resize =
        xcb_cursor_load_cursor(cursor_ctx, "bottom_right_corner");
    if (wm.cursor_resize == XCB_CURSOR_NONE)
      wm.cursor_resize = xcb_cursor_load_cursor(cursor_ctx, "se-resize");

    if (wm.cursor_normal == XCB_CURSOR_NONE ||
        wm.cursor_move == XCB_CURSOR_NONE ||
        wm.cursor_resize == XCB_CURSOR_NONE) {
      fprintf(stderr,
              "wm: theme missing some cursors, using fallback for those\n");
      xcb_cursor_context_free(cursor_ctx);
      cursor_ctx = NULL;
      init_cursors_fallback();
    }
  }

  uint32_t values[] = {wm.cursor_normal};
  xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_CURSOR, values);
  xcb_flush(wm.conn);
}

#define PREVIEW_ACCENT_COLOR 0x3584e4
#define PREVIEW_THICKNESS 5

static xcb_window_t create_preview_strip() {
  xcb_window_t win = xcb_generate_id(wm.conn);
  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT;
  uint32_t values[] = {PREVIEW_ACCENT_COLOR, 1};
  xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, win, wm.root, 0, 0, 1, 1, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual, mask,
                    values);
  return win;
}

void init_drop_preview() {
  wm.preview_top = create_preview_strip();
  wm.preview_bottom = create_preview_strip();
  wm.preview_left = create_preview_strip();
  wm.preview_right = create_preview_strip();
}

void show_drop_preview(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  uint16_t thick = PREVIEW_THICKNESS;
  if (w > 0 && thick * 3 > w)
    thick = w / 3 > 0 ? w / 3 : 1;
  if (h > 0 && thick * 3 > h)
    thick = (h / 3 < thick) ? (h / 3 > 0 ? h / 3 : 1) : thick;

  uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                  XCB_CONFIG_WINDOW_STACK_MODE;

  uint32_t vtop[5] = {(uint32_t)x, (uint32_t)y, w, thick, XCB_STACK_MODE_ABOVE};
  xcb_configure_window(wm.conn, wm.preview_top, mask, vtop);

  uint32_t vbot[5] = {(uint32_t)x, (uint32_t)(y + h - thick), w, thick,
                      XCB_STACK_MODE_ABOVE};
  xcb_configure_window(wm.conn, wm.preview_bottom, mask, vbot);

  uint32_t vleft[5] = {(uint32_t)x, (uint32_t)y, thick, h,
                       XCB_STACK_MODE_ABOVE};
  xcb_configure_window(wm.conn, wm.preview_left, mask, vleft);

  uint32_t vright[5] = {(uint32_t)(x + w - thick), (uint32_t)y, thick, h,
                        XCB_STACK_MODE_ABOVE};
  xcb_configure_window(wm.conn, wm.preview_right, mask, vright);

  xcb_map_window(wm.conn, wm.preview_top);
  xcb_map_window(wm.conn, wm.preview_bottom);
  xcb_map_window(wm.conn, wm.preview_left);
  xcb_map_window(wm.conn, wm.preview_right);
  xcb_flush(wm.conn);
}

void hide_drop_preview() {
  xcb_unmap_window(wm.conn, wm.preview_top);
  xcb_unmap_window(wm.conn, wm.preview_bottom);
  xcb_unmap_window(wm.conn, wm.preview_left);
  xcb_unmap_window(wm.conn, wm.preview_right);
  xcb_flush(wm.conn);
}

int compute_tiling_drop(int ws, xcb_window_t drag_win, int16_t px, int16_t py,
                        int *target_idx_out, xcb_window_t *target_win_out,
                        int *zone_out, int16_t *prev_x_out, int16_t *prev_y_out,
                        uint16_t *prev_w_out, uint16_t *prev_h_out) {
  int target_idx = -1;
  xcb_window_t target_win = XCB_WINDOW_NONE;
  int16_t tx = 0, ty = 0;
  uint16_t tw = 0, th = 0;

  for (int i = 0; i < wm.workspace_counts[ws]; i++) {
    xcb_window_t w = wm.workspace_windows[ws][i];
    if (w == drag_win)
      continue;
    int16_t x = (int16_t)wm.workspace_win_x[ws][i];
    int16_t y = (int16_t)wm.workspace_win_y[ws][i];
    uint16_t ww = (uint16_t)wm.workspace_win_w[ws][i];
    uint16_t hh = (uint16_t)wm.workspace_win_h[ws][i];
    if (px >= x && px <= x + ww && py >= y && py <= y + hh) {
      target_idx = i;
      target_win = w;
      tx = x;
      ty = y;
      tw = ww;
      th = hh;
      break;
    }
  }

  if (target_idx < 0 || tw == 0 || th == 0)
    return 0;

  int dist_left = px - tx;
  int dist_right = (tx + tw) - px;
  int dist_top = py - ty;
  int dist_bottom = (ty + th) - py;

  int zone = 0;
  int min_dist = dist_left;
  if (dist_right < min_dist) {
    min_dist = dist_right;
    zone = 1;
  }
  if (dist_top < min_dist) {
    min_dist = dist_top;
    zone = 2;
  }
  if (dist_bottom < min_dist) {
    min_dist = dist_bottom;
    zone = 3;
  }

  int16_t px_, py_;
  uint16_t pw_, ph_;
  switch (zone) {
  case 0: // left half
    px_ = tx;
    py_ = ty;
    pw_ = (uint16_t)(tw / 2);
    ph_ = th;
    break;
  case 1: // right half
    px_ = (int16_t)(tx + tw / 2);
    py_ = ty;
    pw_ = (uint16_t)(tw - tw / 2);
    ph_ = th;
    break;
  case 2: // top half
    px_ = tx;
    py_ = ty;
    pw_ = tw;
    ph_ = (uint16_t)(th / 2);
    break;
  default: // bottom half
    px_ = tx;
    py_ = (int16_t)(ty + th / 2);
    pw_ = tw;
    ph_ = (uint16_t)(th - th / 2);
    break;
  }

  if (target_idx_out)
    *target_idx_out = target_idx;
  if (target_win_out)
    *target_win_out = target_win;
  if (zone_out)
    *zone_out = zone;
  if (prev_x_out)
    *prev_x_out = px_;
  if (prev_y_out)
    *prev_y_out = py_;
  if (prev_w_out)
    *prev_w_out = pw_;
  if (prev_h_out)
    *prev_h_out = ph_;
  return 1;
}

void maybe_grow_workspaces(int ws) {
  if (ws == wm.num_workspaces - 1 && wm.num_workspaces < MAX_WORKSPACES) {
    wm.num_workspaces++;
    uint32_t ndesk = wm.num_workspaces;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        A_NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1,
                        &ndesk);
  }
}

int is_window_managed(xcb_window_t win) {
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    for (int j = 0; j < wm.workspace_counts[i]; j++) {
      if (wm.workspace_windows[i][j] == win) {
        return 1;
      }
    }
  }
  return 0;
}

void add_window_to_workspace_ext(xcb_window_t win, int ws, int force_float) {
  if (is_window_managed(win)) {
    printf("Window 0x%08x already managed, skipping duplicate add\n", win);
    return;
  }
  if (wm.workspace_counts[ws] < MAX_WINDOWS) {
    int idx = wm.workspace_counts[ws];
    wm.workspace_windows[ws][idx] = win;
    wm.workspace_win_x[ws][idx] = 0;
    wm.workspace_win_y[ws][idx] = 0;
    wm.workspace_win_w[ws][idx] = 0;
    wm.workspace_win_h[ws][idx] = 0;
    wm.workspace_win_force_float[ws][idx] = force_float;
    wm.workspace_win_slide_dx[ws][idx] = 0;
    wm.workspace_win_slide_dy[ws][idx] = 0;
    wm.workspace_win_maximized[ws][idx] = 0;
    wm.workspace_counts[ws]++;
    if (wm.workspace_tiling[ws] && !force_float) {
      tile_add_default(&wm.tile_root[ws], win);
    }
    printf("Window 0x%08x added to workspace %d\n", win, ws + 1);
    set_window_desktop(win, ws);
    update_net_client_list();
    maybe_grow_workspaces(ws);
  }
}

void add_window_to_workspace(xcb_window_t win) {
  add_window_to_workspace_ext(win, wm.current_workspace, 0);
}

int remove_window_from_workspace(xcb_window_t win) {
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    for (int j = 0; j < wm.workspace_counts[i]; j++) {
      if (wm.workspace_windows[i][j] == win) {
        tile_remove(&wm.tile_root[i], win);
        for (int k = j; k < wm.workspace_counts[i] - 1; k++) {
          wm.workspace_windows[i][k] = wm.workspace_windows[i][k + 1];
          wm.workspace_win_x[i][k] = wm.workspace_win_x[i][k + 1];
          wm.workspace_win_y[i][k] = wm.workspace_win_y[i][k + 1];
          wm.workspace_win_w[i][k] = wm.workspace_win_w[i][k + 1];
          wm.workspace_win_h[i][k] = wm.workspace_win_h[i][k + 1];
          wm.workspace_win_force_float[i][k] =
              wm.workspace_win_force_float[i][k + 1];
          wm.workspace_win_slide_dx[i][k] = wm.workspace_win_slide_dx[i][k + 1];
          wm.workspace_win_slide_dy[i][k] = wm.workspace_win_slide_dy[i][k + 1];
          wm.workspace_win_maximized[i][k] =
              wm.workspace_win_maximized[i][k + 1];
          wm.workspace_win_premax_x[i][k] = wm.workspace_win_premax_x[i][k + 1];
          wm.workspace_win_premax_y[i][k] = wm.workspace_win_premax_y[i][k + 1];
          wm.workspace_win_premax_w[i][k] = wm.workspace_win_premax_w[i][k + 1];
          wm.workspace_win_premax_h[i][k] = wm.workspace_win_premax_h[i][k + 1];
        }
        wm.workspace_counts[i]--;
        wm.workspace_win_maximized[i][wm.workspace_counts[i]] = 0;
        if (wm.workspace_prev_focus[i] == win) {
          wm.workspace_prev_focus[i] = XCB_WINDOW_NONE;
        }
        if (wm.workspace_last_active[i] == win) {
          wm.workspace_last_active[i] = XCB_WINDOW_NONE;
        }

        int restored = 0;
        if (win == wm.focused) {
          xcb_window_t prev = wm.workspace_prev_focus[i];
          if (prev != XCB_WINDOW_NONE) {
            for (int p = 0; p < wm.workspace_counts[i]; p++) {
              if (wm.workspace_windows[i][p] == prev) {
                wm.workspace_focus_idx[i] = p;
                restored = 1;
                break;
              }
            }
          }
          wm.workspace_prev_focus[i] = XCB_WINDOW_NONE;
        } else if (wm.focused != XCB_WINDOW_NONE) {
          int real_idx = find_window_idx(i, wm.focused);
          if (real_idx >= 0) {
            wm.workspace_focus_idx[i] = real_idx;
            restored = 1;
          }
        }
        wm.workspace_reel_anchor_idx[i] = wm.workspace_focus_idx[i];

        if (!restored && wm.workspace_focus_idx[i] >= wm.workspace_counts[i]) {
          wm.workspace_focus_idx[i] = wm.workspace_counts[i] - 1;
        }
        update_net_client_list();
        return i;
      }
    }
  }
  return -1;
}

void compact_workspaces(int removed_ws) {
  if (removed_ws < 0 || removed_ws >= wm.num_workspaces)
    return;
  if (wm.workspace_counts[removed_ws] != 0)
    return; /* sirf khaali workspace hi compact hoga */
  if (removed_ws >= wm.num_workspaces - 1)
    return; /* aakhri (spare) workspace ko chhor do */
  if (wm.num_workspaces <= START_WORKSPACES)
    return; /* minimum workspaces se neeche kabhi na jayein */

  for (int i = removed_ws; i < wm.num_workspaces - 1; i++) {
    memcpy(wm.workspace_windows[i], wm.workspace_windows[i + 1],
           sizeof(wm.workspace_windows[i]));
    memcpy(wm.workspace_win_x[i], wm.workspace_win_x[i + 1],
           sizeof(wm.workspace_win_x[i]));
    memcpy(wm.workspace_win_y[i], wm.workspace_win_y[i + 1],
           sizeof(wm.workspace_win_y[i]));
    memcpy(wm.workspace_win_w[i], wm.workspace_win_w[i + 1],
           sizeof(wm.workspace_win_w[i]));
    memcpy(wm.workspace_win_h[i], wm.workspace_win_h[i + 1],
           sizeof(wm.workspace_win_h[i]));
    memcpy(wm.workspace_win_force_float[i], wm.workspace_win_force_float[i + 1],
           sizeof(wm.workspace_win_force_float[i]));
    memcpy(wm.workspace_win_slide_dx[i], wm.workspace_win_slide_dx[i + 1],
           sizeof(wm.workspace_win_slide_dx[i]));
    memcpy(wm.workspace_win_slide_dy[i], wm.workspace_win_slide_dy[i + 1],
           sizeof(wm.workspace_win_slide_dy[i]));
    wm.workspace_counts[i] = wm.workspace_counts[i + 1];
    wm.workspace_focus_idx[i] = wm.workspace_focus_idx[i + 1];
    wm.workspace_reel_anchor_idx[i] = wm.workspace_reel_anchor_idx[i + 1];
    wm.workspace_tiling[i] = wm.workspace_tiling[i + 1];
    wm.workspace_floating[i] = wm.workspace_floating[i + 1];
    wm.tile_root[i] = wm.tile_root[i + 1];

    for (int j = 0; j < wm.workspace_counts[i]; j++) {
      set_window_desktop(wm.workspace_windows[i][j], i);
    }
  }

  int last = wm.num_workspaces - 1;
  wm.workspace_counts[last] = 0;
  wm.workspace_focus_idx[last] = 0;
  wm.workspace_reel_anchor_idx[last] = 0;
  wm.workspace_tiling[last] = 0;
  wm.workspace_floating[last] = 0;
  wm.tile_root[last] = NULL;
  for (int j = 0; j < MAX_WINDOWS; j++) {
    wm.workspace_windows[last][j] = XCB_WINDOW_NONE;
    wm.workspace_win_w[last][j] = 0;
    wm.workspace_win_h[last][j] = 0;
    wm.workspace_win_force_float[last][j] = 0;
    wm.workspace_win_slide_dx[last][j] = 0;
    wm.workspace_win_slide_dy[last][j] = 0;
  }

  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_owner_ws[i] > removed_ws) {
      wm.dialog_owner_ws[i]--;
    }
  }

  wm.num_workspaces--;
  uint32_t ndesk = wm.num_workspaces;
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1,
                      &ndesk);

  if (wm.current_workspace > removed_ws) {
    wm.current_workspace--;
  }
  if (wm.current_workspace >= wm.num_workspaces) {
    wm.current_workspace = wm.num_workspaces - 1;
  }

  for (int i = 0; i < wm.dialog_count; i++) {
    xcb_window_t dwin = wm.dialog_windows[i];
    int dws = wm.dialog_owner_ws[i];
    set_window_desktop(dwin, dws);
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    if (dws == wm.current_workspace) {
      int32_t values[] = {wm.dialog_x[i], wm.dialog_y[i]};
      xcb_configure_window(wm.conn, dwin, mask, (uint32_t *)values);
    } else {
      int32_t park_x = wm.dialog_x[i] + wm.screen_width * (dws + 2);
      int32_t values[] = {park_x, wm.dialog_y[i]};
      xcb_configure_window(wm.conn, dwin, mask, (uint32_t *)values);
    }
  }

  for (int i = removed_ws; i < wm.num_workspaces; i++) {
    if (wm.workspace_counts[i] == 0)
      continue;
    compute_reel_layout(i);
    if (i == wm.current_workspace) {
      apply_reel_geometry(i);
    } else {
      apply_reel_geometry_parked(i);
    }
  }

  update_net_current_desktop();
  update_net_client_list();
  publish_workspace_modes();
}

void switch_workspace(int workspace) {
  if (workspace < 0 || workspace >= wm.num_workspaces)
    return;
  if (workspace == wm.current_workspace)
    return;

  int from_ws = wm.current_workspace;
  int to_ws = workspace;
  int dir = (to_ws > from_ws) ? 1 : -1;
  int slide = wm.screen_width;

  compute_reel_layout(to_ws);

  int from_count = wm.workspace_counts[from_ws];
  int to_count = wm.workspace_counts[to_ws];

  int16_t from_x[MAX_WINDOWS], from_y[MAX_WINDOWS];
  int16_t from_w[MAX_WINDOWS];

  for (int i = 0; i < from_count; i++) {
    if (!wm.workspace_tiling[from_ws] && !wm.workspace_floating[from_ws]) {
      from_x[i] = (int16_t)wm.workspace_win_x[from_ws][i];
      from_y[i] = (int16_t)wm.workspace_win_y[from_ws][i];
      from_w[i] = (int16_t)wm.workspace_win_w[from_ws][i];
    } else {
      xcb_window_t win = wm.workspace_windows[from_ws][i];
      xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
      xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);

      if (geo) {
        from_x[i] = geo->x;
        from_y[i] = geo->y;
        from_w[i] = geo->width;

        wm.workspace_win_x[from_ws][i] = geo->x;
        wm.workspace_win_y[from_ws][i] = geo->y;
        wm.workspace_win_w[from_ws][i] = geo->width;
        wm.workspace_win_h[from_ws][i] = geo->height;
        free(geo);
      } else {
        from_x[i] = (int16_t)wm.workspace_win_x[from_ws][i];
        from_y[i] = (int16_t)wm.workspace_win_y[from_ws][i];
        from_w[i] = (int16_t)wm.workspace_win_w[from_ws][i];
      }
    }
  }

  int16_t to_target_x[MAX_WINDOWS], to_target_y[MAX_WINDOWS];
  for (int i = 0; i < to_count; i++) {
    xcb_window_t win = wm.workspace_windows[to_ws][i];
    to_target_x[i] = wm.workspace_win_x[to_ws][i];
    to_target_y[i] = wm.workspace_win_y[to_ws][i];

    xcb_map_window(wm.conn, win);
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                    XCB_CONFIG_WINDOW_STACK_MODE;
    int32_t start_values[] = {to_target_x[i] + dir * slide, to_target_y[i],
                              XCB_STACK_MODE_ABOVE};
    xcb_configure_window(wm.conn, win, mask, (uint32_t *)start_values);
  }
  xcb_flush(wm.conn);

#if ANIM_ENABLED
  for (int step = 1; step <= ANIM_WS_STEPS; step++) {
    double t = ease_out_cubic((double)step / ANIM_WS_STEPS);

    for (int i = 0; i < from_count; i++) {
      xcb_window_t win = wm.workspace_windows[from_ws][i];
      int32_t x = from_x[i] - (int32_t)(dir * slide * t);
      uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t values[] = {x, from_y[i]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }
    for (int i = 0; i < to_count; i++) {
      xcb_window_t win = wm.workspace_windows[to_ws][i];
      int32_t x = to_target_x[i] + (int32_t)(dir * slide * (1.0 - t));
      uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t values[] = {x, to_target_y[i]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }
    xcb_flush(wm.conn);
    usleep(ANIM_WS_DELAY_US);

    xcb_generic_event_t *aev;
    while ((aev = xcb_poll_for_event(wm.conn)) != NULL) {
      int atype = aev->response_type & ~0x80;
      if (atype == XCB_EXPOSE) {
        handle_expose((xcb_expose_event_t *)aev);
      }
      free(aev);
    }
  }
#endif

  for (int i = 0; i < from_count; i++) {
    xcb_window_t win = wm.workspace_windows[from_ws][i];

    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    int32_t local_offset;
    if (wm.workspace_tiling[from_ws] || wm.workspace_floating[from_ws]) {
      local_offset = from_x[i];
      if (local_offset < 0)
        local_offset = 0;
      if (local_offset >= wm.screen_width)
        local_offset = wm.screen_width - 1;
    } else {
      int focus_idx = wm.workspace_focus_idx[from_ws];
      if (focus_idx < 0)
        focus_idx = 0;
      if (focus_idx >= from_count)
        focus_idx = from_count - 1;

      if (i == focus_idx) {
        local_offset = from_x[i];
        if (local_offset < 0)
          local_offset = 0;
        int32_t w = from_w[i] > 0 ? from_w[i] : wm.screen_width;
        int32_t max_offset = wm.screen_width - w;
        if (max_offset < 0)
          max_offset = 0;
        if (local_offset > max_offset)
          local_offset = max_offset;
      } else if (from_count > 1) {
        int32_t slot_w = wm.screen_width / from_count;
        local_offset = i * slot_w;
      } else {
        local_offset = 0;
      }
    }
    int32_t park_x = wm.screen_width * (from_ws + 2) + local_offset;
    int32_t values[] = {park_x, from_y[i]};
    xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
  }
  for (int i = 0; i < to_count; i++) {
    xcb_window_t win = wm.workspace_windows[to_ws][i];
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    int32_t values[] = {to_target_x[i], to_target_y[i]};
    xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    xcb_map_window(wm.conn, win);
  }

  if (wm.workspace_tiling[to_ws]) {
    apply_tiling_geometry(to_ws);
  }

  for (int i = 0; i < wm.dialog_count; i++) {
    xcb_window_t win = wm.dialog_windows[i];
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    if (wm.dialog_owner_ws[i] == to_ws) {
      int32_t values[] = {wm.dialog_x[i], wm.dialog_y[i]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    } else if (wm.dialog_owner_ws[i] == from_ws) {
      xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
      xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
      if (geo && geo->x < wm.screen_width && geo->y < wm.screen_height) {
        wm.dialog_x[i] = geo->x;
        wm.dialog_y[i] = geo->y;
      }
      if (geo)
        free(geo);
      int32_t park_x = wm.dialog_x[i] + wm.screen_width * (from_ws + 2);
      int32_t values[] = {park_x, wm.dialog_y[i]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }
  }

  wm.current_workspace = to_ws;

  if (wm.workspace_counts[to_ws] > 0 && !wm.workspace_tiling[to_ws] &&
      !wm.workspace_floating[to_ws]) {
    compute_reel_layout(to_ws);
    apply_reel_geometry(to_ws);
  }

  if (wm.workspace_counts[to_ws] > 0) {
    int idx = wm.workspace_focus_idx[to_ws];

    if (idx < 0)
      idx = 0;

    if (idx >= wm.workspace_counts[to_ws])
      idx = wm.workspace_counts[to_ws] - 1;

    wm.workspace_focus_idx[to_ws] = idx;

    xcb_window_t win = wm.workspace_windows[to_ws][idx];

    xcb_window_t last_active = wm.workspace_last_active[to_ws];
    if (last_active != XCB_WINDOW_NONE && last_active != win &&
        window_is_alive(last_active)) {
      int belongs = 0;
      for (int k = 0; k < wm.workspace_counts[to_ws]; k++) {
        if (wm.workspace_windows[to_ws][k] == last_active) {
          belongs = 1;
          break;
        }
      }
      if (!belongs) {
        for (int k = 0; k < wm.dialog_count; k++) {
          if (wm.dialog_windows[k] == last_active &&
              wm.dialog_owner_ws[k] == to_ws) {
            belongs = 1;
            break;
          }
        }
      }
      if (belongs) {
        win = last_active;
      }
    }

    if (win != XCB_WINDOW_NONE) {
      focus_window(win);
    } else {
      xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, wm.root,
                          XCB_CURRENT_TIME);
      xcb_delete_property(wm.conn, wm.root, A_NET_ACTIVE_WINDOW);
      wm.focused = XCB_WINDOW_NONE;
    }
  } else {
    xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, wm.root,
                        XCB_CURRENT_TIME);
    xcb_delete_property(wm.conn, wm.root, A_NET_ACTIVE_WINDOW);
    wm.focused = XCB_WINDOW_NONE;
  }
  update_net_current_desktop();
  xcb_flush(wm.conn);
}

void launch_kitty() {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execlp("wezterm", "wezterm", NULL);
    execlp("xterm", "xterm", NULL);
    exit(1);
  } else if (pid > 0) {
    usleep(100000);
  }
}

void launch_autostart_process(const char *path, char *const argv[]) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execvp(path, argv);
    fprintf(stderr, "autostart: failed to launch %s\n", path);
    _exit(1);
  }
}

static void spawnstartup(void) {
  if (fork() == 0) {
    setsid();
    execl("/bin/sh", "sh", "-c",
          // "dunst & "
          // "picom & "
          // "waypaper --restore & "
          // // "./.config/polybar/launch.sh --cuts &"
          // //"/home/hammad/wm/tested-overview/overview &"
          // "./wm/status/statusbar &"
          // "my_dock &"
          // "livepaper-gui --restore &"
          // // "/home/hammad/bin/live.sh &",
          // "/usr/lib/polkit-gnome/polkit-gnome-authentication-agent-1",
          "/home/hammad/.bin/autostart.sh &", (char *)NULL);
    _exit(1);
  }
}

void force_close_window(xcb_window_t win) {
  if (win == XCB_WINDOW_NONE)
    return;

  if (window_supports_delete(win)) {
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = A_WM_PROTOCOLS;
    ev.data.data32[0] = A_WM_DELETE_WINDOW;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(wm.conn, 0, win, XCB_EVENT_MASK_NO_EVENT, (char *)&ev);
    usleep(100000); // Give it a moment to respond
  }

  xcb_kill_client(wm.conn, win);
  //========================5===============
  send_notify("Window Closed");

  xcb_flush(wm.conn);
}

void cleanup_stuck_windows() {
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    for (int i = 0; i < wm.workspace_counts[ws]; i++) {
      xcb_window_t win = wm.workspace_windows[ws][i];
      xcb_get_window_attributes_cookie_t attr_cookie =
          xcb_get_window_attributes(wm.conn, win);
      xcb_get_window_attributes_reply_t *attr =
          xcb_get_window_attributes_reply(wm.conn, attr_cookie, NULL);

      if (attr) {
        if (attr->map_state == XCB_MAP_STATE_VIEWABLE) {
        }
        free(attr);
      }
    }
  }
}

static void publish_workspace_modes(void) {
  uint32_t arr[MAX_WORKSPACES];
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    if (wm.workspace_tiling[ws])
      arr[ws] = 1;
    else if (wm.workspace_floating[ws])
      arr[ws] = 2;
    else
      arr[ws] = 0;
  }
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_WM_WORKSPACE_MODE, XCB_ATOM_CARDINAL, 32,
                      MAX_WORKSPACES, arr);
}

static void publish_last_focus_per_workspace(void) {
  xcb_window_t arr[MAX_WORKSPACES];
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    int count = wm.workspace_counts[ws];
    int idx = wm.workspace_focus_idx[ws];
    if (count <= 0) {
      arr[ws] = XCB_WINDOW_NONE;
      continue;
    }
    if (idx < 0)
      idx = 0;
    if (idx >= count)
      idx = count - 1;
    arr[ws] = wm.workspace_windows[ws][idx];
  }
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_WM_LAST_FOCUS_PER_WS, XCB_ATOM_WINDOW, 32,
                      MAX_WORKSPACES, arr);
}

void focus_window(xcb_window_t win) {
  if (win == XCB_WINDOW_NONE)
    return;

  if (wm.focused != XCB_WINDOW_NONE && wm.focused != win) {
    xcb_change_window_attributes(wm.conn, wm.focused, XCB_CW_BORDER_PIXEL,
                                 (uint32_t[]){BORDER_NORMAL_COLOR});
    xcb_configure_window(wm.conn, wm.focused, XCB_CONFIG_WINDOW_BORDER_WIDTH,
                         (uint32_t[]){BORDER_WIDTH});
    xcb_flush(wm.conn);
    wm.workspace_prev_focus[wm.current_workspace] = wm.focused;
  }

  xcb_change_window_attributes(wm.conn, win, XCB_CW_BORDER_PIXEL,
                               (uint32_t[]){BORDER_HOVER_COLOR});
  xcb_configure_window(
      wm.conn, win, XCB_CONFIG_WINDOW_BORDER_WIDTH,
      (uint32_t[]){BORDER_WIDTH});
  xcb_flush(wm.conn);

  xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, win,
                      XCB_CURRENT_TIME);
  xcb_configure_window(wm.conn, win, XCB_CONFIG_WINDOW_STACK_MODE,
                       (uint32_t[]){XCB_STACK_MODE_ABOVE});

  {
    int cws = wm.current_workspace;
    for (int i = 0; i < wm.workspace_counts[cws]; i++) {
      xcb_window_t ffwin = wm.workspace_windows[cws][i];
      if (ffwin == win || ffwin == XCB_WINDOW_NONE)
        continue;
      if (wm.workspace_win_force_float[cws][i] &&
          is_always_float_window(ffwin)) {
        xcb_configure_window(wm.conn, ffwin, XCB_CONFIG_WINDOW_STACK_MODE,
                             (uint32_t[]){XCB_STACK_MODE_ABOVE});
      }
    }
  }

  wm.focused = win;
  wm.workspace_last_active[wm.current_workspace] = win;
  xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                      A_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 32, 1, &win);
  publish_last_focus_per_workspace();
  raise_docks();
  xcb_flush(wm.conn);

  int win_is_dialog = 0;
  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] == win) {
      win_is_dialog = 1;
      break;
    }
  }
  if (!win_is_dialog) {
    for (int i = 0; i < wm.dialog_count; i++) {
      if (wm.dialog_owner_ws[i] == wm.current_workspace)
        update_dialog_stack_owner(wm.dialog_windows[i]);
    }
  }
}

static int window_supports_delete(xcb_window_t win) {
  xcb_get_property_cookie_t c =
      xcb_get_property(wm.conn, 0, win, A_WM_PROTOCOLS, XCB_ATOM_ATOM, 0, 32);
  xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn, c, NULL);
  int supports = 0;
  if (r && xcb_get_property_value_length(r) > 0) {
    xcb_atom_t *atoms = (xcb_atom_t *)xcb_get_property_value(r);
    int count = xcb_get_property_value_length(r) / (int)sizeof(xcb_atom_t);
    for (int i = 0; i < count; i++) {
      if (atoms[i] == A_WM_DELETE_WINDOW) {
        supports = 1;
        break;
      }
    }
  }
  if (r)
    free(r);
  return supports;
}

void close_focused_window() {
  xcb_window_t win = wm.focused;
  if (win == XCB_WINDOW_NONE) {
    return;
  }

  if (window_supports_delete(win)) {
    xcb_client_message_event_t ev = {0};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = win;
    ev.type = A_WM_PROTOCOLS;
    ev.data.data32[0] = A_WM_DELETE_WINDOW;
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(wm.conn, 0, win, XCB_EVENT_MASK_NO_EVENT, (char *)&ev);
  } else {
    xcb_kill_client(wm.conn, win);
  }
  xcb_flush(wm.conn);
}

void get_window_title(xcb_window_t win, char *buf, int bufsize) {
  xcb_get_property_cookie_t cookie = xcb_get_property(
      wm.conn, 0, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, 256);
  xcb_get_property_reply_t *reply =
      xcb_get_property_reply(wm.conn, cookie, NULL);

  if (reply && reply->type == XCB_ATOM_STRING && reply->format == 8) {
    int len = (reply->value_len < (uint32_t)(bufsize - 1))
                  ? (int)reply->value_len
                  : bufsize - 1;
    memcpy(buf, xcb_get_property_value(reply), len);
    buf[len] = '\0';
  } else {
    snprintf(buf, bufsize, "Window 0x%08x", win);
  }
  free(reply);
}

void capture_thumbnail(xcb_window_t win, xcb_pixmap_t *pixmap, int *w, int *h) {
  xcb_get_geometry_cookie_t geo_cookie = xcb_get_geometry(wm.conn, win);
  xcb_get_geometry_reply_t *geo =
      xcb_get_geometry_reply(wm.conn, geo_cookie, NULL);

  if (!geo) {
    *pixmap = 0;
    *w = 100;
    *h = 100;
    return;
  }

  *w = geo->width;
  *h = geo->height;
  free(geo);

  if (*w < 10 || *h < 10) {
    *w = 100;
    *h = 100;
  }

  uint32_t raise_mask = XCB_CONFIG_WINDOW_STACK_MODE;
  uint32_t raise_values[] = {XCB_STACK_MODE_ABOVE};
  xcb_configure_window(wm.conn, win, raise_mask, raise_values);
  xcb_flush(wm.conn);
  usleep(15000);

  *pixmap = xcb_generate_id(wm.conn);
  xcb_create_pixmap(wm.conn, wm.screen->root_depth, *pixmap, win, *w, *h);

  xcb_gcontext_t gc = xcb_generate_id(wm.conn);
  xcb_create_gc(wm.conn, gc, *pixmap, 0, NULL);

  xcb_copy_area(wm.conn, win, *pixmap, gc, 0, 0, 0, 0, *w, *h);
  xcb_free_gc(wm.conn, gc);

  if (wm.overview_win) {
    uint32_t lower_mask =
        XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
    uint32_t lower_values[] = {wm.overview_win, XCB_STACK_MODE_BELOW};
    xcb_configure_window(wm.conn, win, lower_mask, lower_values);
  }
  xcb_flush(wm.conn);
}

void reclassify_stray_windows() {
  int touched[MAX_WORKSPACES] = {0};

  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    int i = 0;
    while (i < wm.workspace_counts[ws]) {
      xcb_window_t win = wm.workspace_windows[ws][i];
      int is_dock = is_dock_window(win);
      int is_util = !is_dock && is_utility_window(win);

      if (!is_dock && !is_util) {
        i++;
        continue;
      }

      remove_window_from_workspace(win);
      touched[ws] = 1;

      if (is_dock) {
        add_dock_window(win);
      }
    }
  }

  int any_dock = 0;
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    if (touched[ws]) {
      any_dock = 1;
      if (wm.workspace_counts[ws] > 0) {
        compute_reel_layout(ws);
        if (ws == wm.current_workspace) {
          apply_reel_geometry(ws);
          focus_window(wm.workspace_windows[ws][wm.workspace_focus_idx[ws]]);
        }
      } else if (ws == wm.current_workspace) {
        wm.focused = XCB_WINDOW_NONE;
      }
    }
  }
  if (any_dock) {
    raise_docks();
  }
}

void render_overview() {
  reclassify_stray_windows();

  if (!wm.overview_win)
    return;
  xcb_visualtype_t *visual = NULL;
  xcb_depth_iterator_t depth_iter =
      xcb_screen_allowed_depths_iterator(wm.screen);
  while (depth_iter.rem) {
    xcb_depth_t *depth = depth_iter.data;
    xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth);
    while (visual_iter.rem) {
      visual = visual_iter.data;
      break;
    }
    if (visual)
      break;
    xcb_depth_next(&depth_iter);
  }

  xcb_clear_area(wm.conn, 0, wm.overview_win, 0, 0, wm.screen_width,
                 wm.screen_height);

  cairo_surface_t *surface = cairo_xcb_surface_create(
      wm.conn, wm.overview_win, visual, wm.screen_width, wm.screen_height);
  cairo_t *cr = cairo_create(surface);

  cairo_set_source_rgb(cr, 0.05, 0.05, 0.08);
  cairo_rectangle(cr, 0, 0, wm.screen_width, wm.screen_height);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 22);
  cairo_move_to(cr, 30, 50);
  cairo_show_text(cr, "Overview - All Workspaces");

  int margin = 20;
  int gap = 8;
  int available = wm.screen_width - (margin * 2);
  int ws_box_w =
      (available - (wm.num_workspaces - 1) * gap) / wm.num_workspaces;
  if (ws_box_w < 50)
    ws_box_w = 50;
  int ws_box_h = 75;
  int ws_box_y = 70;

  int total_width =
      wm.num_workspaces * ws_box_w + (wm.num_workspaces - 1) * gap;
  int start_x = (wm.screen_width - total_width) / 2;

  int grid_top = ws_box_y + ws_box_h + 20;

  fprintf(stderr,
          "[overview] screen=%dx%d ws_box_w=%d ws_box_h=%d start_x=%d "
          "total_width=%d\n",
          wm.screen_width, wm.screen_height, ws_box_w, ws_box_h, start_x,
          total_width);

  for (int i = 0; i < wm.num_workspaces; i++) {
    int bx = start_x + i * (ws_box_w + gap);
    int by = ws_box_y;

    fprintf(stderr, "[overview] box %d: x=%d y=%d w=%d h=%d\n", i, bx, by,
            ws_box_w, ws_box_h);

    if (i == wm.current_workspace) {
      cairo_set_source_rgb(cr, 0.15, 0.35, 0.5);
    } else {
      cairo_set_source_rgb(cr, 0.12, 0.12, 0.16);
    }
    cairo_rectangle(cr, bx, by, ws_box_w, ws_box_h);
    cairo_fill(cr);

    if (i == wm.current_workspace) {
      cairo_set_source_rgb(cr, 0.3, 0.8, 1.0);
      cairo_set_line_width(cr, 2);
    } else {
      cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
      cairo_set_line_width(cr, 1);
    }
    cairo_rectangle(cr, bx, by, ws_box_w, ws_box_h);
    cairo_stroke(cr);

    char label[16];
    snprintf(label, sizeof(label), "WS %d", i + 1);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    int font_size = (ws_box_w < 70) ? 9 : 11;
    cairo_set_font_size(cr, font_size);
    cairo_move_to(cr, bx + 4, by + 14);
    cairo_show_text(cr, label);

    char count_label[16];
    snprintf(count_label, sizeof(count_label), "(%d)", wm.workspace_counts[i]);
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_set_font_size(cr, 8);
    cairo_move_to(cr, bx + 4, by + 26);
    cairo_show_text(cr, count_label);

    int count = wm.workspace_counts[i];
    if (count > 0) {
      int mini_top = by + 32;
      int mini_area_h = ws_box_h - 36;
      int is_reel_mode = !wm.workspace_tiling[i] && !wm.workspace_floating[i];

      if (is_reel_mode) {
        int fidx = wm.workspace_focus_idx[i];
        if (fidx < 0)
          fidx = 0;
        if (fidx >= count)
          fidx = count - 1;

        int mx = bx + 8;
        int my = mini_top + 4;
        int mw = ws_box_w - 16;
        int mh = mini_area_h - 8;
        if (mw < 3)
          mw = 3;
        if (mh < 3)
          mh = 3;

        float r = 0.15 + (fidx * 0.08);
        float g = 0.20 + (fidx * 0.06);
        float b = 0.35 + (fidx * 0.05);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, mx, my, mw, mh);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.6);
        cairo_set_line_width(cr, 0.5);
        cairo_rectangle(cr, mx, my, mw, mh);
        cairo_stroke(cr);
      } else {
        int mini_cols = (count < 3) ? count : 3;
        int mini_rows = (count + mini_cols - 1) / mini_cols;
        int mini_w = (ws_box_w - 8 * (mini_cols + 1)) / mini_cols;
        int mini_h = (mini_area_h - 8 * (mini_rows + 1)) / mini_rows;
        if (mini_w < 3)
          mini_w = 3;
        if (mini_h < 3)
          mini_h = 3;

        for (int j = 0; j < count && j < MAX_WINDOWS; j++) {
          int mcol = j % mini_cols;
          int mrow = j / mini_cols;
          int mx = bx + 8 + mcol * (mini_w + 8);
          int my = mini_top + 8 + mrow * (mini_h + 8);

          float r = 0.15 + (j * 0.08);
          float g = 0.20 + (j * 0.06);
          float b = 0.35 + (j * 0.05);
          cairo_set_source_rgb(cr, r, g, b);
          cairo_rectangle(cr, mx, my, mini_w, mini_h);
          cairo_fill(cr);
          cairo_set_source_rgb(cr, 0.5, 0.5, 0.6);
          cairo_set_line_width(cr, 0.5);
          cairo_rectangle(cr, mx, my, mini_w, mini_h);
          cairo_stroke(cr);
        }
      }
    } else {
      cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
      cairo_set_font_size(cr, 8);
      cairo_move_to(cr, bx + 10, by + ws_box_h / 2 + 3);
      cairo_show_text(cr, "empty");
    }
  }
  int total_windows = 0;
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    total_windows += wm.workspace_counts[i];
  }

  if (total_windows == 0) {
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_set_font_size(cr, 18);
    cairo_move_to(cr, 40, grid_top + 30);
    cairo_show_text(cr, "No windows open. Press Ctrl+K to launch Kitty.");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_flush(wm.conn);
    return;
  }

  // Grid layout
  int cols = (total_windows < 3) ? total_windows : 3;
  int rows = (total_windows + cols - 1) / cols;
  int grid_margin = 30;
  int grid_gap = 15;

  int thumb_width =
      (wm.screen_width - grid_margin * 2 - grid_gap * (cols - 1)) / cols;
  int thumb_height =
      (wm.screen_height - grid_top - grid_margin * 2 - grid_gap * (rows - 1)) /
      rows;

  if (thumb_height < 50)
    thumb_height = 50;
  if (thumb_width < 50)
    thumb_width = 50;

  int idx = 0;
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    for (int j = 0; j < wm.workspace_counts[ws]; j++) {
      xcb_window_t win = wm.workspace_windows[ws][j];

      int col = idx % cols;
      int row = idx / cols;
      int x = grid_margin + col * (thumb_width + grid_gap);
      int y = grid_top + row * (thumb_height + grid_gap);
      char title[256];
      get_window_title(win, title, sizeof(title));
      cairo_set_source_rgb(cr, 0.12, 0.12, 0.16);
      cairo_rectangle(cr, x, y, thumb_width, thumb_height);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 0.3, 0.5, 0.8);
      cairo_set_line_width(cr, 1.5);
      cairo_rectangle(cr, x, y, thumb_width, thumb_height);
      cairo_stroke(cr);
      xcb_pixmap_t pixmap = 0;
      int w = 0, h = 0;
      capture_thumbnail(win, &pixmap, &w, &h);

      if (pixmap && w > 0 && h > 0) {
        cairo_surface_t *thumb_surface =
            cairo_xcb_surface_create(wm.conn, pixmap, visual, w, h);
        if (thumb_surface && !cairo_surface_status(thumb_surface)) {
          cairo_save(cr);
          cairo_rectangle(cr, x + 4, y + 22, thumb_width - 8,
                          thumb_height - 30);
          cairo_clip(cr);

          double scale_x = (double)(thumb_width - 8) / w;
          double scale_y = (double)(thumb_height - 30) / h;
          double scale = (scale_x < scale_y) ? scale_x : scale_y;

          cairo_translate(cr, x + 4 + (thumb_width - 8 - w * scale) / 2,
                          y + 22 + (thumb_height - 30 - h * scale) / 2);
          cairo_scale(cr, scale, scale);
          cairo_set_source_surface(cr, thumb_surface, 0, 0);
          cairo_paint(cr);
          cairo_restore(cr);
          cairo_surface_destroy(thumb_surface);
        }
        xcb_free_pixmap(wm.conn, pixmap);
      } else {
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.35);
        cairo_rectangle(cr, x + 4, y + 22, thumb_width - 8, thumb_height - 30);
        cairo_fill(cr);
        char win_id[16];
        snprintf(win_id, sizeof(win_id), "0x%08x", win);
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.6);
        cairo_set_font_size(cr, 9);
        cairo_move_to(cr, x + 10, y + 22 + (thumb_height - 30) / 2 + 3);
        cairo_show_text(cr, win_id);
      }
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 10);
      char display_title[64];
      strncpy(display_title, title, 60);
      display_title[60] = '\0';
      cairo_move_to(cr, x + 6, y + 16);
      cairo_show_text(cr, display_title);
      char ws_badge[10];
      snprintf(ws_badge, sizeof(ws_badge), "WS%d", ws + 1);
      cairo_set_source_rgb(cr, 0.2, 0.6, 1.0);
      cairo_rectangle(cr, x + thumb_width - 38, y + 4, 34, 15);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_set_font_size(cr, 8);
      cairo_move_to(cr, x + thumb_width - 32, y + 16);
      cairo_show_text(cr, ws_badge);

      idx++;
    }
  }

  cairo_set_source_rgb(cr, 0.4, 0.4, 0.4);
  cairo_set_font_size(cr, 11);
  cairo_move_to(cr, 30, wm.screen_height - 18);
  cairo_show_text(
      cr, "ESC: Exit | Click: Focus window | Alt+1..5: Switch workspace");

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  xcb_flush(wm.conn);
}

void setup_overview() {
  if (wm.overview_win) {
    xcb_destroy_window(wm.conn, wm.overview_win);
  }

  wm.overview_win = xcb_generate_id(wm.conn);
  uint32_t mask =
      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_OVERRIDE_REDIRECT;
  uint32_t values[] = {wm.screen->black_pixel,
                       XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS |
                           XCB_EVENT_MASK_EXPOSURE,
                       1};

  xcb_create_window(wm.conn, wm.screen->root_depth, wm.overview_win, wm.root, 0,
                    0, wm.screen_width, wm.screen_height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual, mask,
                    values);

  xcb_map_window(wm.conn, wm.overview_win);
  xcb_flush(wm.conn);
}

void toggle_overview() {
  wm.in_overview = !wm.in_overview;

  if (wm.in_overview) {
    printf("=== Opening Overview: Mapping all windows ===\n");
    for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
      for (int i = 0; i < wm.workspace_counts[ws]; i++) {
        xcb_window_t win = wm.workspace_windows[ws][i];

        if (ws != wm.current_workspace) {
          int32_t restore_values[] = {wm.workspace_win_x[ws][i],
                                      wm.workspace_win_y[ws][i]};
          xcb_configure_window(wm.conn, win,
                               XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                               (uint32_t *)restore_values);
        }

        xcb_map_window(wm.conn, win);
        printf("  Mapped window 0x%08x from workspace %d\n", win, ws + 1);
      }
    }
    xcb_flush(wm.conn);
    usleep(100000);

    setup_overview();
    render_overview();
    for (int i = 0; i < wm.dialog_count; i++) {
      uint32_t stack_mask =
          XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE;
      uint32_t stack_values[] = {wm.overview_win, XCB_STACK_MODE_BELOW};
      xcb_configure_window(wm.conn, wm.dialog_windows[i], stack_mask,
                           stack_values);
    }
    xcb_flush(wm.conn);
  } else {
    printf("=== Closing Overview: Restoring workspaces ===\n");
    for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
      for (int i = 0; i < wm.workspace_counts[ws]; i++) {
        xcb_window_t win = wm.workspace_windows[ws][i];
        uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        if (ws == wm.current_workspace) {
          int32_t values[] = {wm.workspace_win_x[ws][i],
                              wm.workspace_win_y[ws][i]};
          xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
          xcb_map_window(wm.conn, win);
          printf("  Kept window 0x%08x in workspace %d\n", win, ws + 1);
        } else {
          xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
          xcb_get_geometry_reply_t *geo =
              xcb_get_geometry_reply(wm.conn, gc, NULL);
          if (geo && geo->x < wm.screen_width && geo->y < wm.screen_height) {
            wm.workspace_win_x[ws][i] = geo->x;
            wm.workspace_win_y[ws][i] = geo->y;
          }
          if (geo)
            free(geo);

          int32_t values[] = {wm.workspace_win_x[ws][i] +
                                  wm.screen_width * (ws + 2),
                              wm.workspace_win_y[ws][i]};
          xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
          printf("  Moved window 0x%08x off-screen (workspace %d)\n", win,
                 ws + 1);
        }
      }
    }
    if (wm.overview_win) {
      xcb_destroy_window(wm.conn, wm.overview_win);
      wm.overview_win = 0;
      xcb_flush(wm.conn);
    }
  }
}

void map_all_windows() {
  xcb_query_tree_cookie_t cookie = xcb_query_tree(wm.conn, wm.root);
  xcb_query_tree_reply_t *reply = xcb_query_tree_reply(wm.conn, cookie, NULL);

  if (!reply)
    return;

  xcb_window_t *children = xcb_query_tree_children(reply);
  int num_children = xcb_query_tree_children_length(reply);

  for (int i = 0; i < num_children; i++) {
    xcb_window_t win = children[i];

    xcb_get_window_attributes_cookie_t attr_cookie =
        xcb_get_window_attributes(wm.conn, win);
    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(wm.conn, attr_cookie, NULL);

    if (attr) {
      if (attr->override_redirect) {
        free(attr);
        continue;
      }
      if (attr->map_state == XCB_MAP_STATE_UNMAPPED ||
          attr->map_state == XCB_MAP_STATE_VIEWABLE) {

        if (is_dock_window(win)) {
          xcb_map_window(wm.conn, win);
          add_dock_window(win);
          raise_docks();
        } else if (is_utility_window(win)) {
          xcb_map_window(wm.conn, win);
        } else if (is_dialog_window(win)) {
          xcb_map_window(wm.conn, win);
          uint32_t dmask = XCB_CW_EVENT_MASK;
          uint32_t dvalues[] = {XCB_EVENT_MASK_STRUCTURE_NOTIFY};
          xcb_change_window_attributes(wm.conn, win, dmask, dvalues);
          add_dialog_window(win, wm.current_workspace, wm.focused);
          update_dialog_stack_owner(win);
          update_net_client_list();
        } else {
          xcb_map_window(wm.conn, win);

          uint32_t mask = XCB_CW_EVENT_MASK;
          uint32_t values[] = {
              XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE |
              XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW |
              XCB_EVENT_MASK_STRUCTURE_NOTIFY};
          xcb_change_window_attributes(wm.conn, win, mask, values);
          grab_focus_click(win);

          add_window_to_workspace(win);
        }
      }
      free(attr);
    }
  }

  free(reply);

  int ws = wm.current_workspace;
  int count = wm.workspace_counts[ws];
  if (count > 0) {
    wm.workspace_focus_idx[ws] = count - 1;
    compute_reel_layout(ws);
    apply_reel_geometry(ws);
    focus_window(wm.workspace_windows[ws][wm.workspace_focus_idx[ws]]);
  }

  xcb_flush(wm.conn);
}

void animate_window_open(xcb_window_t win, int16_t target_x, int16_t target_y,
                         uint16_t target_w, uint16_t target_h) {
#if ANIM_ENABLED
  uint16_t start_w = (uint16_t)(target_w * 0.55);
  uint16_t start_h = (uint16_t)(target_h * 0.55);
  if (start_w < MIN_WIN_SIZE)
    start_w = MIN_WIN_SIZE;
  if (start_h < MIN_WIN_SIZE)
    start_h = MIN_WIN_SIZE;
  int16_t start_x = target_x + (int16_t)((target_w - start_w) / 2);
  int16_t start_y = target_y + (int16_t)((target_h - start_h) / 2);

  uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
  uint32_t values0[4] = {(uint32_t)start_x, (uint32_t)start_y, start_w,
                         start_h};
  xcb_configure_window(wm.conn, win, mask, values0);
  xcb_flush(wm.conn);

  for (int step = 1; step <= ANIM_OPEN_STEPS; step++) {
    double t = ease_out_cubic((double)step / ANIM_OPEN_STEPS);

    int16_t x = (int16_t)(start_x + (target_x - start_x) * t);
    int16_t y = (int16_t)(start_y + (target_y - start_y) * t);
    uint16_t w = (uint16_t)(start_w + (target_w - start_w) * t);
    uint16_t h = (uint16_t)(start_h + (target_h - start_h) * t);

    uint32_t values[4] = {(uint32_t)x, (uint32_t)y, w, h};
    xcb_configure_window(wm.conn, win, mask, values);
    xcb_flush(wm.conn);
    usleep(ANIM_OPEN_DELAY_US);
  }
#endif
  uint32_t mask2 = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                   XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
  uint32_t values2[4] = {(uint32_t)target_x, (uint32_t)target_y, target_w,
                         target_h};
  xcb_configure_window(wm.conn, win, mask2, values2);

  xcb_flush(wm.conn);
}

void handle_map_request(xcb_map_request_event_t *ev) {
  xcb_window_t win = ev->window;

  xcb_get_window_attributes_cookie_t or_cookie =
      xcb_get_window_attributes(wm.conn, win);
  xcb_get_window_attributes_reply_t *or_attr =
      xcb_get_window_attributes_reply(wm.conn, or_cookie, NULL);
  if (or_attr) {
    int override = or_attr->override_redirect;
    free(or_attr);
    if (override) {
      xcb_map_window(wm.conn, win);
      xcb_flush(wm.conn);
      return;
    }
  }

  reclassify_stray_windows();

  if (is_dock_window(win)) {
    xcb_map_window(wm.conn, win);
    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = {XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                         XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_change_window_attributes(wm.conn, win, mask, values);
    add_dock_window(win);
    raise_docks();

    for (int i = 0; i < MAX_WORKSPACES; i++) {
      if (wm.workspace_counts[i] > 0) {
        compute_reel_layout(i);
      }
    }
    if (wm.workspace_counts[wm.current_workspace] > 0) {
      apply_reel_geometry(wm.current_workspace);
    }

    xcb_flush(wm.conn);
    return;
  }

  if (is_utility_window(win)) {
    xcb_map_window(wm.conn, win);
    uint32_t stack_mask = XCB_CONFIG_WINDOW_STACK_MODE;
    uint32_t stack_values[] = {XCB_STACK_MODE_ABOVE};
    xcb_configure_window(wm.conn, win, stack_mask, stack_values);
    xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, win,
                        XCB_CURRENT_TIME);
    wm.focused = win;
    xcb_flush(wm.conn);
    return;
  }

  int win_is_dialog = is_dialog_window(win);
  if (!win_is_dialog) {
    xcb_flush(wm.conn);
    usleep(30000);
    win_is_dialog = is_dialog_window(win);
  }

  if (win_is_dialog) {
    xcb_map_window(wm.conn, win);
    uint32_t dmask = XCB_CW_EVENT_MASK;
    uint32_t dvalues[] = {XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_change_window_attributes(wm.conn, win, dmask, dvalues);
    xcb_window_t prev_focus = wm.focused;

    if (prev_focus != XCB_WINDOW_NONE) {
      xcb_get_geometry_cookie_t ngc = xcb_get_geometry(wm.conn, win);
      xcb_get_geometry_reply_t *ngeo =
          xcb_get_geometry_reply(wm.conn, ngc, NULL);
      xcb_get_geometry_cookie_t ogc = xcb_get_geometry(wm.conn, prev_focus);
      xcb_get_geometry_reply_t *ogeo =
          xcb_get_geometry_reply(wm.conn, ogc, NULL);

      if (ngeo && ogeo) {
        uint16_t nw = ngeo->width, nh = ngeo->height;

        /* Center every dialog on the usable desktop area instead of
         * cascading it relative to the previously focused window. */
        int16_t area_x, area_y;
        uint16_t area_w, area_h;
        get_usable_area(&area_x, &area_y, &area_w, &area_h);

        int16_t center_x =
            area_x + (int16_t)((area_w - nw) / 2);
        int16_t center_y =
            area_y + (int16_t)((area_h - nh) / 2);

        if (center_x < area_x)
          center_x = area_x;
        if (center_y < area_y)
          center_y = area_y;

        uint32_t pmask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        uint32_t pvalues[] = {
            (uint32_t)(int32_t)center_x,
            (uint32_t)(int32_t)center_y
        };
        xcb_configure_window(wm.conn, win, pmask, pvalues);
      }
      if (ngeo)
        free(ngeo);
      if (ogeo)
        free(ogeo);
    }

    focus_window(win);
    add_dialog_window(win, wm.current_workspace, prev_focus);
    update_dialog_stack_owner(win);
    update_net_client_list();
    xcb_flush(wm.conn);
    return;
  }

  xcb_map_window(wm.conn, win);

  if (is_window_managed(win)) {
    focus_window(win);
    xcb_flush(wm.conn);
    return;
  }

  uint32_t mask = XCB_CW_EVENT_MASK;
  uint32_t values[] = {XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE |
                       XCB_EVENT_MASK_PROPERTY_CHANGE |
                       XCB_EVENT_MASK_ENTER_WINDOW |
                       XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_change_window_attributes(wm.conn, win, mask, values);
  grab_focus_click(win);

  int target_ws = wm.current_workspace;
  int force_float = is_always_float_window(win);

  add_window_to_workspace_ext(win, target_ws, force_float);
  int ws = target_ws;
  int idx = wm.workspace_counts[ws] - 1;
  wm.workspace_focus_idx[ws] = idx;
  wm.workspace_reel_anchor_idx[ws] = idx;

  xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, win);
  xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);

  if (geo && (wm.workspace_floating[ws] || force_float)) {
    // Floating mode mein window ki actual size use karein
    wm.workspace_win_w[ws][idx] = geo->width;
    wm.workspace_win_h[ws][idx] = geo->height;

    // Center position calculate karein
    int16_t area_x, area_y;
    uint16_t area_w, area_h;
    get_usable_area(&area_x, &area_y, &area_w, &area_h);
    int16_t center_x = area_x + (area_w / 2);
    int16_t center_y = area_y + (area_h / 2);

    wm.workspace_win_x[ws][idx] = center_x - (geo->width / 2);
    wm.workspace_win_y[ws][idx] = center_y - (geo->height / 2);
  }
  if (geo) {
    free(geo);
  }

  compute_reel_layout(ws);

  if (ws != wm.current_workspace) {
    int16_t px = (int16_t)wm.workspace_win_x[ws][idx];
    int16_t py = (int16_t)wm.workspace_win_y[ws][idx];
    uint32_t pmask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    int32_t park_x = px + wm.screen_width * (ws + 2);
    int32_t pvalues[] = {park_x, py};
    xcb_configure_window(wm.conn, win, pmask, (uint32_t *)pvalues);
  } else if (wm.workspace_tiling[ws]) {
    apply_reel_geometry(ws);
    focus_window(win);
  } else {
    for (int i = 0; i < idx; i++) {
      xcb_window_t other = wm.workspace_windows[ws][i];
      uint32_t omask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t ovalues[] = {(int32_t)wm.workspace_win_x[ws][i],
                           (int32_t)wm.workspace_win_y[ws][i]};
      xcb_configure_window(wm.conn, other, omask, (uint32_t *)ovalues);
    }

    int16_t target_x = (int16_t)wm.workspace_win_x[ws][idx];
    int16_t target_y = (int16_t)wm.workspace_win_y[ws][idx];
    uint16_t target_w, target_h;
    if (wm.workspace_floating[ws] || force_float) {
      target_w = (uint16_t)wm.workspace_win_w[ws][idx];
      target_h = (uint16_t)wm.workspace_win_h[ws][idx];
    } else {
      int16_t fx, fy;
      get_window_target(&fx, &fy, &target_w, &target_h);
    }

    focus_window(win);
    animate_window_open(win, target_x, target_y, target_w, target_h);
  }

  if (wm.in_overview) {
    render_overview();
  }

  xcb_flush(wm.conn);
}

void move_focused_window_to_workspace(int target_ws) {
  if (target_ws < 0 || target_ws >= wm.num_workspaces)
    return;
  if (target_ws == wm.current_workspace)
    return;

  xcb_window_t win = wm.focused;
  if (win == XCB_WINDOW_NONE)
    return;

  int from_ws = wm.current_workspace;

  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] != win || wm.dialog_owner_ws[i] != from_ws)
      continue;

    wm.dialog_owner_ws[i] = target_ws;
    set_window_desktop(win, target_ws);

    int32_t park_x = wm.dialog_x[i] + wm.screen_width * (target_ws + 2);
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    int32_t values[] = {park_x, wm.dialog_y[i]};
    xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);

    if (wm.workspace_counts[from_ws] > 0) {
      focus_window(
          wm.workspace_windows[from_ws][wm.workspace_focus_idx[from_ws]]);
    } else {
      wm.focused = XCB_WINDOW_NONE;
    }

    if (wm.in_overview) {
      render_overview();
    }
    xcb_flush(wm.conn);
    return;
  }

  int from_idx = -1;
  for (int i = 0; i < wm.workspace_counts[from_ws]; i++) {
    if (wm.workspace_windows[from_ws][i] == win) {
      from_idx = i;
      break;
    }
  }
  if (from_idx < 0)
    return;
  int saved_w = wm.workspace_win_w[from_ws][from_idx];
  int saved_h = wm.workspace_win_h[from_ws][from_idx];
  int saved_force_float = wm.workspace_win_force_float[from_ws][from_idx];
  int saved_slide_dx = wm.workspace_win_slide_dx[from_ws][from_idx];
  int saved_slide_dy = wm.workspace_win_slide_dy[from_ws][from_idx];

  remove_window_from_workspace(win);

  if (wm.workspace_counts[target_ws] < MAX_WINDOWS) {
    int new_idx = wm.workspace_counts[target_ws];
    wm.workspace_windows[target_ws][new_idx] = win;
    wm.workspace_win_w[target_ws][new_idx] = saved_w;
    wm.workspace_win_h[target_ws][new_idx] = saved_h;
    wm.workspace_win_force_float[target_ws][new_idx] = saved_force_float;
    wm.workspace_win_slide_dx[target_ws][new_idx] = saved_slide_dx;
    wm.workspace_win_slide_dy[target_ws][new_idx] = saved_slide_dy;
    wm.workspace_counts[target_ws]++;
    if (wm.workspace_tiling[target_ws] && !saved_force_float) {
      tile_add_default(&wm.tile_root[target_ws], win);
    }
    set_window_desktop(win, target_ws);
    update_net_client_list();

    wm.workspace_focus_idx[target_ws] = new_idx;

    compute_reel_layout(target_ws);

    if (wm.in_overview) {
      apply_reel_geometry(target_ws);
    } else if (wm.workspace_tiling[target_ws]) {
      apply_reel_geometry_parked(target_ws);
    } else {
      uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t values[] = {wm.workspace_win_x[target_ws][new_idx] +
                              wm.screen_width * (target_ws + 2),
                          wm.workspace_win_y[target_ws][new_idx]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }
    maybe_grow_workspaces(target_ws);
  }

  if (wm.workspace_counts[from_ws] > 0) {
    compute_reel_layout(from_ws);
    apply_reel_geometry(from_ws);
    focus_window(
        wm.workspace_windows[from_ws][wm.workspace_focus_idx[from_ws]]);
  } else {
    compact_workspaces(from_ws);
    if (wm.workspace_counts[wm.current_workspace] > 0) {
      compute_reel_layout(wm.current_workspace);
      apply_reel_geometry(wm.current_workspace);
      focus_window(
          wm.workspace_windows[wm.current_workspace]
                              [wm.workspace_focus_idx[wm.current_workspace]]);
    } else {
      wm.focused = XCB_WINDOW_NONE;
    }
  }

  if (wm.in_overview) {
    render_overview();
  }

  xcb_flush(wm.conn);
}

void move_window_to_workspace(xcb_window_t win, int target_ws) {
  if (win == XCB_WINDOW_NONE)
    return;
  if (target_ws < 0 || target_ws >= MAX_WORKSPACES)
    return;

  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] != win)
      continue;

    int dialog_ws = wm.dialog_owner_ws[i];
    if (dialog_ws == target_ws)
      return; /* already wahin hai */

    wm.dialog_owner_ws[i] = target_ws;
    set_window_desktop(win, target_ws);

    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    if (target_ws == wm.current_workspace) {
      int32_t values[] = {wm.dialog_x[i], wm.dialog_y[i]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    } else {
      int32_t park_x = wm.dialog_x[i] + wm.screen_width * (target_ws + 2);
      int32_t values[] = {park_x, wm.dialog_y[i]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }

    if (wm.in_overview) {
      render_overview();
    }
    xcb_flush(wm.conn);
    return;
  }

  int from_ws = -1, from_idx = -1;
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    for (int j = 0; j < wm.workspace_counts[i]; j++) {
      if (wm.workspace_windows[i][j] == win) {
        from_ws = i;
        from_idx = j;
        break;
      }
    }
    if (from_ws >= 0)
      break;
  }
  if (from_ws < 0 || from_ws == target_ws)
    return;

  int saved_w = wm.workspace_win_w[from_ws][from_idx];
  int saved_h = wm.workspace_win_h[from_ws][from_idx];
  int saved_force_float = wm.workspace_win_force_float[from_ws][from_idx];
  int saved_slide_dx = wm.workspace_win_slide_dx[from_ws][from_idx];
  int saved_slide_dy = wm.workspace_win_slide_dy[from_ws][from_idx];

  remove_window_from_workspace(win);

  if (wm.workspace_counts[target_ws] < MAX_WINDOWS) {
    int new_idx = wm.workspace_counts[target_ws];
    wm.workspace_windows[target_ws][new_idx] = win;
    wm.workspace_win_w[target_ws][new_idx] = saved_w;
    wm.workspace_win_h[target_ws][new_idx] = saved_h;
    wm.workspace_win_force_float[target_ws][new_idx] = saved_force_float;
    wm.workspace_win_slide_dx[target_ws][new_idx] = saved_slide_dx;
    wm.workspace_win_slide_dy[target_ws][new_idx] = saved_slide_dy;
    wm.workspace_counts[target_ws]++;
    if (wm.workspace_tiling[target_ws] && !saved_force_float) {
      tile_add_default(&wm.tile_root[target_ws], win);
    }
    set_window_desktop(win, target_ws);
    update_net_client_list();

    wm.workspace_focus_idx[target_ws] = new_idx;
    compute_reel_layout(target_ws);

    if (target_ws == wm.current_workspace) {
      apply_reel_geometry(target_ws);
      focus_window(win);
    } else if (wm.workspace_tiling[target_ws]) {
      apply_reel_geometry_parked(target_ws);
    } else {
      uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t values[] = {wm.workspace_win_x[target_ws][new_idx] +
                              wm.screen_width * (target_ws + 2),
                          wm.workspace_win_y[target_ws][new_idx]};
      xcb_configure_window(wm.conn, win, mask, (uint32_t *)values);
    }
    maybe_grow_workspaces(target_ws);
  }

  if (from_ws == wm.current_workspace) {
    if (wm.workspace_counts[from_ws] > 0) {
      compute_reel_layout(from_ws);
      apply_reel_geometry(from_ws);
      focus_window(
          wm.workspace_windows[from_ws][wm.workspace_focus_idx[from_ws]]);
    } else {
      compact_workspaces(from_ws);
      if (wm.workspace_counts[wm.current_workspace] > 0) {
        compute_reel_layout(wm.current_workspace);
        apply_reel_geometry(wm.current_workspace);
        focus_window(
            wm.workspace_windows[wm.current_workspace]
                                [wm.workspace_focus_idx[wm.current_workspace]]);
      } else {
        wm.focused = XCB_WINDOW_NONE;
      }
    }
  } else if (wm.workspace_counts[from_ws] > 0) {
    compute_reel_layout(from_ws);

    apply_reel_geometry_parked(from_ws);
  } else {
    compact_workspaces(from_ws);
  }

  if (wm.in_overview) {
    render_overview();
  }

  xcb_flush(wm.conn);
}

void handle_client_message(xcb_client_message_event_t *ev) {
  if (ev->type == A_NET_ACTIVE_WINDOW) {
    xcb_window_t win = ev->window;

    int ws = -1;
    int win_idx = -1;
    for (int i = 0; i < MAX_WORKSPACES && ws < 0; i++) {
      for (int j = 0; j < wm.workspace_counts[i]; j++) {
        if (wm.workspace_windows[i][j] == win) {
          ws = i;
          win_idx = j;
          break;
        }
      }
    }
    int is_dialog = 0;
    if (ws < 0) {
      for (int i = 0; i < wm.dialog_count; i++) {
        if (wm.dialog_windows[i] == win) {
          ws = wm.dialog_owner_ws[i];
          is_dialog = 1;
          break;
        }
      }
    }

    if (ws >= 0 && ws != wm.current_workspace) {
      switch_workspace(ws);
    }

    if (!is_dialog && win_idx >= 0) {
      jump_reel_focus(ws, win_idx);
    } else {
      focus_window(win);
    }
    xcb_flush(wm.conn);
    return;
  }

  if (ev->type == A_NET_WM_DESKTOP) {
    int target_ws = (int)ev->data.data32[0];
    move_window_to_workspace(ev->window, target_ws);
    return;
  }

  if (ev->type == A_NET_CURRENT_DESKTOP) {
    int target_ws = (int)ev->data.data32[0];
    switch_workspace(target_ws);
    xcb_flush(wm.conn);
    return;
  }
}

void handle_key_press(xcb_key_press_event_t *ev) {
  xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(wm.conn);
  xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, ev->detail, 0);

  if (wm.in_overview && sym == XK_Escape) {
    toggle_overview();
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && (ev->state & XCB_MOD_MASK_SHIFT) &&
      sym == XK_f) {
    toggle_floating_mode();
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && (ev->state & XCB_MOD_MASK_SHIFT) &&
      sym == XK_t) {
    toggle_tiling_mode();
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && (ev->state & XCB_MOD_MASK_SHIFT) &&
      sym >= XK_1 && sym <= XK_5) {
    int workspace = sym - XK_1;
    if (workspace < wm.num_workspaces) {
      move_focused_window_to_workspace(workspace);
      if (wm.in_overview) {
        render_overview();
      }
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym >= XK_1 && sym <= XK_9) {
    int workspace = sym - XK_1;
    if (workspace < wm.num_workspaces) {
      switch_workspace(workspace);
      if (wm.in_overview) {
        render_overview();
      }
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_d) {
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      execlp("/home/hammad/wm/apps-launcher/apps-launcher.sh",
             "/home/hammad/wm/apps-launcher/apps-launcher.sh", NULL);
      exit(1);
    }
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }
  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_a) {
    pid_t pid = fork();

    if (pid == 0) {
      setsid();

      execlp("rofi", "rofi", "-show", "drun", NULL);

      perror("rofi");
      _exit(127);
    }

    if (pid < 0) {
      perror("fork: rofi");
    }

    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_w) {
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      execlp("/home/hammad/wm/wsoverview.sh", "/home/hammad/wm/wsoverview.sh",
             NULL);
      exit(1);
    }
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_Tab) {
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      execlp("overview", "overview", NULL);
      exit(1);
    }
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_e) {
    pid_t pid = fork();
    if (pid == 0) {
      setsid();
      execlp("gtkfm", "gtkfm", NULL);
      exit(1);
    }
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_o) {
    toggle_overview();
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_q) {
    close_focused_window();
    xcb_key_symbols_free(keysyms);
    return;
  }
  //====================7===================
  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_n) {
    send_notify("Test Notification");
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_j) {
    cycle_focus(wm.current_workspace, 1);
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_k) {
    cycle_focus(wm.current_workspace, -1);
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && (ev->state & XCB_MOD_MASK_SHIFT) &&
      (sym == XK_Left || sym == XK_Right)) {
    int direction = (sym == XK_Right) ? 1 : -1;
    int ws = wm.current_workspace;
    int idx = wm.workspace_focus_idx[ws];
    if (idx < 0)
      idx = 0;
    if (idx >= wm.workspace_counts[ws])
      idx = wm.workspace_counts[ws] - 1;
    swap_reel_windows(ws, idx, idx + direction);
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && (sym == XK_Left || sym == XK_Right)) {
    int direction = (sym == XK_Right) ? 1 : -1;
    slide_reel_focus(wm.current_workspace, direction);
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  // Mod+Up: focused window maximize (sirf floating ya reel/slide mode
  // mein, tiling mode mein ignore).
  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_Up) {
    maximize_focused_window();
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_Down) {
    unmaximize_focused_window();
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) &&
      (sym == XK_Page_Up || sym == XK_Page_Down)) {
    int direction = (sym == XK_Page_Down) ? 1 : -1;
    int target = wm.current_workspace + direction;
    switch_workspace(target);
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  if ((ev->state & XCB_MOD_MASK_4) && sym == XK_Return) {
    launch_kitty();
    if (wm.in_overview) {
      render_overview();
    }
    xcb_key_symbols_free(keysyms);
    return;
  }

  xcb_key_symbols_free(keysyms);
}

void handle_button_press(xcb_button_press_event_t *ev) {
  if (!wm.in_overview && (ev->state & MOVE_RESIZE_MOD) &&
      ev->child != XCB_WINDOW_NONE &&
      (ev->detail == XCB_BUTTON_INDEX_1 || ev->detail == XCB_BUTTON_INDEX_3)) {

    xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, ev->child);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);
    if (geo) {
      wm.dragging = (ev->detail == XCB_BUTTON_INDEX_1) ? 1 : 2;
      wm.drag_window = ev->child;
      wm.drag_start_x = ev->root_x;
      wm.drag_start_y = ev->root_y;
      wm.drag_orig_x = geo->x;
      wm.drag_orig_y = geo->y;
      wm.drag_orig_w = geo->width;
      wm.drag_orig_h = geo->height;
      free(geo);

      {
        int fws = wm.current_workspace;
        int fidx = find_window_idx(fws, wm.drag_window);
        if (fidx >= 0) {
          wm.workspace_focus_idx[fws] = fidx;
          wm.workspace_reel_anchor_idx[fws] = fidx;
        }
      }
      focus_window(wm.drag_window);

      xcb_change_active_pointer_grab(
          wm.conn,
          (ev->detail == XCB_BUTTON_INDEX_1) ? wm.cursor_move
                                             : wm.cursor_resize,
          XCB_CURRENT_TIME,
          XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_BUTTON_MOTION);

      if (ev->detail == XCB_BUTTON_INDEX_3) {
        xcb_warp_pointer(wm.conn, XCB_NONE, wm.drag_window, 0, 0, 0, 0,
                         wm.drag_orig_w, wm.drag_orig_h);

        wm.drag_start_x = wm.drag_orig_x + wm.drag_orig_w;
        wm.drag_start_y = wm.drag_orig_y + wm.drag_orig_h;
      }
      xcb_flush(wm.conn);
    }
    return;
  }

  if (!wm.in_overview && ev->event != wm.root) {
    {
      int fws = wm.current_workspace;
      int fidx = find_window_idx(fws, ev->event);
      if (fidx >= 0) {
        wm.workspace_focus_idx[fws] = fidx;
        wm.workspace_reel_anchor_idx[fws] = fidx;
      }
    }
    focus_window(ev->event);
    xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
    xcb_flush(wm.conn);
    return;
  }

  if (!wm.in_overview)
    return;

  int cols = 3;
  int margin = 40;
  int spacing = 20;

  int total_windows = 0;
  for (int i = 0; i < MAX_WORKSPACES; i++) {
    total_windows += wm.workspace_counts[i];
  }

  int rows = (total_windows + cols - 1) / cols;
  int thumb_width =
      (wm.screen_width - margin * 2 - spacing * (cols - 1)) / cols;
  int thumb_height =
      (wm.screen_height - GRID_TOP - margin * 2 - spacing * (rows - 1)) / rows;

  if (thumb_height < 100)
    thumb_height = 100;
  if (thumb_width < 100)
    thumb_width = 100;

  int idx = 0;
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    for (int j = 0; j < wm.workspace_counts[ws]; j++) {
      xcb_window_t win = wm.workspace_windows[ws][j];

      int col = idx % cols;
      int row = idx / cols;
      int x = margin + col * (thumb_width + spacing);
      int y = GRID_TOP + row * (thumb_height + spacing);

      if (ev->event_x >= x && ev->event_x <= x + thumb_width &&
          ev->event_y >= y && ev->event_y <= y + thumb_height) {

        if (ws != wm.current_workspace) {
          switch_workspace(ws);
        }

        focus_window(win);
        toggle_overview();
        return;
      }
      idx++;
    }
  }
}

void handle_motion_notify(xcb_motion_notify_event_t *ev) {
  if (!wm.dragging || wm.drag_window == XCB_WINDOW_NONE)
    return;

  xcb_query_pointer_cookie_t pc = xcb_query_pointer(wm.conn, wm.root);
  xcb_query_pointer_reply_t *pr = xcb_query_pointer_reply(wm.conn, pc, NULL);
  int16_t root_x = pr ? pr->root_x : ev->root_x;
  int16_t root_y = pr ? pr->root_y : ev->root_y;
  if (pr)
    free(pr);

  int dx = root_x - wm.drag_start_x;
  int dy = root_y - wm.drag_start_y;

  if (wm.dragging == 1) {
    int32_t values[2];
    values[0] = wm.drag_orig_x + dx;
    values[1] = wm.drag_orig_y + dy;
    xcb_configure_window(wm.conn, wm.drag_window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                         (uint32_t *)values);

    update_dialog_stack_owner(wm.drag_window);

    if (wm.workspace_tiling[wm.current_workspace]) {
      int16_t prev_x, prev_y;
      uint16_t prev_w, prev_h;
      int found = compute_tiling_drop(wm.current_workspace, wm.drag_window,
                                      root_x, root_y, NULL, NULL, NULL, &prev_x,
                                      &prev_y, &prev_w, &prev_h);
      if (found) {
        show_drop_preview(prev_x, prev_y, prev_w, prev_h);
      } else {
        hide_drop_preview();
      }
    }
  } else if (wm.dragging == 2) {
    int new_w = wm.drag_orig_w + dx;
    int new_h = wm.drag_orig_h + dy;
    if (new_w < MIN_WIN_SIZE)
      new_w = MIN_WIN_SIZE;
    if (new_h < MIN_WIN_SIZE)
      new_h = MIN_WIN_SIZE;
    uint32_t values[2] = {(uint32_t)new_w, (uint32_t)new_h};
    xcb_configure_window(wm.conn, wm.drag_window,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         values);
  }
  xcb_flush(wm.conn);
}

void handle_button_release(xcb_button_release_event_t *ev) {
  (void)ev;

  hide_drop_preview();

  if (wm.dragging) {
    if (wm.dragging == 1 && wm.drag_window != XCB_WINDOW_NONE &&
        wm.workspace_tiling[wm.current_workspace]) {
      int ws = wm.current_workspace;
      xcb_query_pointer_cookie_t pc = xcb_query_pointer(wm.conn, wm.root);
      xcb_query_pointer_reply_t *pr =
          xcb_query_pointer_reply(wm.conn, pc, NULL);

      if (pr) {
        int16_t px = pr->root_x;
        int16_t py = pr->root_y;
        free(pr);

        int target_idx = -1;
        xcb_window_t target_win = XCB_WINDOW_NONE;
        int zone = 0;
        int found =
            compute_tiling_drop(ws, wm.drag_window, px, py, &target_idx,
                                &target_win, &zone, NULL, NULL, NULL, NULL);

        if (found && target_win != wm.drag_window) {
          tile_remove(&wm.tile_root[ws], wm.drag_window);
          tile_insert_split(&wm.tile_root[ws], target_win, wm.drag_window,
                            zone);
        }

        int drag_idx = find_window_idx(ws, wm.drag_window);
        if (drag_idx >= 0) {
          wm.workspace_focus_idx[ws] = drag_idx;
        }
      }

      compute_reel_layout(ws);
      apply_reel_geometry(ws);
      focus_window(wm.drag_window);
    } else if (wm.dragging == 1 && wm.drag_window != XCB_WINDOW_NONE &&
               !wm.workspace_tiling[wm.current_workspace]) {
      int ws = wm.current_workspace;
      int idx = find_window_idx(ws, wm.drag_window);
      int is_force_float =
          (idx >= 0) ? wm.workspace_win_force_float[ws][idx] : 0;

      if (wm.workspace_floating[ws] || is_force_float) {
        xcb_get_geometry_cookie_t gc =
            xcb_get_geometry(wm.conn, wm.drag_window);
        xcb_get_geometry_reply_t *geo =
            xcb_get_geometry_reply(wm.conn, gc, NULL);

        if (geo) {
          if (idx >= 0) {
            wm.workspace_win_x[ws][idx] = geo->x;
            wm.workspace_win_y[ws][idx] = geo->y;

            if (!wm.workspace_floating[ws]) {
              int16_t tx, ty;
              uint16_t tw, th;
              get_window_target(&tx, &ty, &tw, &th);

              int step = (int)tw + WINDOW_GAP;
              int focus_idx = wm.workspace_focus_idx[ws];

              if (focus_idx < 0)
                focus_idx = 0;
              if (focus_idx >= wm.workspace_counts[ws])
                focus_idx = wm.workspace_counts[ws] - 1;

              int32_t slot_x = tx + (idx - focus_idx) * step - BORDER_WIDTH;

              wm.workspace_win_slide_dx[ws][idx] = (int)geo->x - (int)slot_x;
              wm.workspace_win_slide_dy[ws][idx] = (int)geo->y - (int)ty;
            }
          }
          free(geo);
        }
      } else if (idx >= 0) {
        xcb_get_geometry_cookie_t gc =
            xcb_get_geometry(wm.conn, wm.drag_window);
        xcb_get_geometry_reply_t *geo =
            xcb_get_geometry_reply(wm.conn, gc, NULL);
        if (geo) {
          wm.workspace_win_x[ws][idx] = geo->x;
          wm.workspace_win_y[ws][idx] = geo->y;
          wm.workspace_win_w[ws][idx] = geo->width;
          wm.workspace_win_h[ws][idx] = geo->height;
          wm.workspace_win_force_float[ws][idx] = 1;

          int16_t tx, ty;
          uint16_t tw, th;
          get_window_target(&tx, &ty, &tw, &th);
          int step = (int)tw + WINDOW_GAP;
          int focus_idx = wm.workspace_focus_idx[ws];
          if (focus_idx < 0)
            focus_idx = 0;
          if (focus_idx >= wm.workspace_counts[ws])
            focus_idx = wm.workspace_counts[ws] - 1;
          int32_t slot_x = tx + (idx - focus_idx) * step - BORDER_WIDTH;
          wm.workspace_win_slide_dx[ws][idx] = geo->x - slot_x;
          wm.workspace_win_slide_dy[ws][idx] = geo->y - ty;
          free(geo);
        }
      }
      focus_window(wm.drag_window);
    }

    if (wm.dragging == 2 && wm.drag_window != XCB_WINDOW_NONE) {
      xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, wm.drag_window);
      xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);

      if (geo) {
        for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
          int i = find_window_idx(ws, wm.drag_window);
          if (i < 0)
            continue;

          wm.workspace_win_w[ws][i] = geo->width;
          wm.workspace_win_h[ws][i] = geo->height;

          if (wm.workspace_floating[ws]) {
            wm.workspace_win_x[ws][i] = geo->x;
            wm.workspace_win_y[ws][i] = geo->y;
          }

          free(geo);
          geo = NULL;
          break;
        }

        if (geo)
          free(geo);
      }

      int ws = wm.current_workspace;
      if (ws >= 0 && ws < MAX_WORKSPACES && !wm.workspace_tiling[ws] &&
          !wm.workspace_floating[ws]) {
        compute_reel_layout(ws);
        apply_reel_geometry(ws);
      }
    }

    wm.dragging = 0;
    wm.drag_window = XCB_WINDOW_NONE;

    xcb_change_active_pointer_grab(wm.conn, wm.cursor_normal, XCB_CURRENT_TIME,
                                   XCB_EVENT_MASK_BUTTON_RELEASE |
                                       XCB_EVENT_MASK_BUTTON_MOTION);
    xcb_flush(wm.conn);
  }
}

static void grab_key_mod4_variants(xcb_keycode_t keycode, uint16_t mods) {
  const uint16_t variants[] = {
      mods, (uint16_t)(mods | XCB_MOD_MASK_LOCK),
      (uint16_t)(mods | XCB_MOD_MASK_2),
      (uint16_t)(mods | XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)};

  for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
    xcb_grab_key(wm.conn, 1, wm.root, variants[i], keycode, XCB_GRAB_MODE_ASYNC,
                 XCB_GRAB_MODE_ASYNC);
  }
}

void setup_key_grabs() {
  xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(wm.conn);

  xcb_keycode_t *k_keycode = xcb_key_symbols_get_keycode(keysyms, XK_k);
  if (k_keycode) {
    grab_key_mod4_variants(*k_keycode, XCB_MOD_MASK_4);
    free(k_keycode);
  }

  for (int i = XK_1; i <= XK_9; i++) {
    xcb_keycode_t *keycode = xcb_key_symbols_get_keycode(keysyms, i);
    if (keycode) {
      grab_key_mod4_variants(*keycode, XCB_MOD_MASK_4);
      free(keycode);
    }
  }

  for (int i = XK_1; i <= XK_5; i++) {
    xcb_keycode_t *keycode = xcb_key_symbols_get_keycode(keysyms, i);
    if (keycode) {
      grab_key_mod4_variants(*keycode, XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT);
      free(keycode);
    }
  }

  xcb_keycode_t *pgup_keycode =
      xcb_key_symbols_get_keycode(keysyms, XK_Page_Up);
  if (pgup_keycode) {
    grab_key_mod4_variants(*pgup_keycode, XCB_MOD_MASK_4);
    free(pgup_keycode);
  }

  xcb_keycode_t *pgdown_keycode =
      xcb_key_symbols_get_keycode(keysyms, XK_Page_Down);
  if (pgdown_keycode) {
    grab_key_mod4_variants(*pgdown_keycode, XCB_MOD_MASK_4);
    free(pgdown_keycode);
  }

  xcb_keycode_t *left_keycode = xcb_key_symbols_get_keycode(keysyms, XK_Left);
  if (left_keycode) {
    grab_key_mod4_variants(*left_keycode, XCB_MOD_MASK_4);
    free(left_keycode);
  }

  xcb_keycode_t *right_keycode = xcb_key_symbols_get_keycode(keysyms, XK_Right);
  if (right_keycode) {
    grab_key_mod4_variants(*right_keycode, XCB_MOD_MASK_4);
    free(right_keycode);
  }

  xcb_keycode_t *shift_left_keycode =
      xcb_key_symbols_get_keycode(keysyms, XK_Left);
  if (shift_left_keycode) {
    grab_key_mod4_variants(*shift_left_keycode,
                           XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT);
    free(shift_left_keycode);
  }

  xcb_keycode_t *shift_right_keycode =
      xcb_key_symbols_get_keycode(keysyms, XK_Right);
  if (shift_right_keycode) {
    grab_key_mod4_variants(*shift_right_keycode,
                           XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT);
    free(shift_right_keycode);
  }

  // Mod+Up -> maximize, Mod+Down -> unmaximize (floating/reel/slide mode)
  xcb_keycode_t *up_keycode = xcb_key_symbols_get_keycode(keysyms, XK_Up);
  if (up_keycode) {
    grab_key_mod4_variants(*up_keycode, XCB_MOD_MASK_4);
    free(up_keycode);
  }

  xcb_keycode_t *down_keycode = xcb_key_symbols_get_keycode(keysyms, XK_Down);
  if (down_keycode) {
    grab_key_mod4_variants(*down_keycode, XCB_MOD_MASK_4);
    free(down_keycode);
  }

  xcb_keycode_t *o_keycode = xcb_key_symbols_get_keycode(keysyms, XK_Tab);
  if (o_keycode) {
    grab_key_mod4_variants(*o_keycode, XCB_MOD_MASK_4);
    free(o_keycode);
  }

  xcb_keycode_t *ret_keycode = xcb_key_symbols_get_keycode(keysyms, XK_Return);
  if (ret_keycode) {
    grab_key_mod4_variants(*ret_keycode, XCB_MOD_MASK_4);
    free(ret_keycode);
  }

  xcb_keycode_t *a_keycode = xcb_key_symbols_get_keycode(keysyms, XK_a);
  if (a_keycode) {
    grab_key_mod4_variants(*a_keycode, XCB_MOD_MASK_4);
    free(a_keycode);
  }

  xcb_keycode_t *w_keycode = xcb_key_symbols_get_keycode(keysyms, XK_w);
  if (w_keycode) {
    grab_key_mod4_variants(*w_keycode, XCB_MOD_MASK_4);
    free(w_keycode);
  }

  xcb_keycode_t *q_keycode = xcb_key_symbols_get_keycode(keysyms, XK_q);
  if (q_keycode) {
    grab_key_mod4_variants(*q_keycode, XCB_MOD_MASK_4);
    free(q_keycode);
  }

  xcb_keycode_t *j_keycode = xcb_key_symbols_get_keycode(keysyms, XK_j);
  if (j_keycode) {
    grab_key_mod4_variants(*j_keycode, XCB_MOD_MASK_4);
    free(j_keycode);
  }

  xcb_keycode_t *k_keycode2 = xcb_key_symbols_get_keycode(keysyms, XK_k);
  if (k_keycode2) {
    grab_key_mod4_variants(*k_keycode2, XCB_MOD_MASK_4);
    free(k_keycode2);
  }

  xcb_keysym_t extra_mod4_keys[] = {XK_d, XK_e, XK_o, XK_n};
  for (size_t i = 0; i < sizeof(extra_mod4_keys) / sizeof(extra_mod4_keys[0]);
       i++) {
    xcb_keycode_t *kc =
        xcb_key_symbols_get_keycode(keysyms, extra_mod4_keys[i]);
    if (kc) {
      grab_key_mod4_variants(*kc, XCB_MOD_MASK_4);
      free(kc);
    }
  }

  xcb_keycode_t *t_keycode = xcb_key_symbols_get_keycode(keysyms, XK_t);
  if (t_keycode) {
    grab_key_mod4_variants(*t_keycode, XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT);
    free(t_keycode);
  }

  xcb_keycode_t *f_keycode = xcb_key_symbols_get_keycode(keysyms, XK_f);
  if (f_keycode) {
    grab_key_mod4_variants(*f_keycode, XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT);
    free(f_keycode);
  }

  xcb_key_symbols_free(keysyms);
  xcb_flush(wm.conn);
}

void setup_move_resize_grabs() {
  uint16_t grab_mask = XCB_EVENT_MASK_BUTTON_PRESS |
                       XCB_EVENT_MASK_BUTTON_RELEASE |
                       XCB_EVENT_MASK_BUTTON_MOTION;

  xcb_grab_button(wm.conn, 0, wm.root, grab_mask, XCB_GRAB_MODE_ASYNC,
                  XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1,
                  MOVE_RESIZE_MOD);

  xcb_grab_button(wm.conn, 0, wm.root, grab_mask, XCB_GRAB_MODE_ASYNC,
                  XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_3,
                  MOVE_RESIZE_MOD);

  xcb_flush(wm.conn);
}

void handle_configure_request(xcb_configure_request_event_t *ev) {
  for (int ws = 0; ws < MAX_WORKSPACES; ws++) {
    int idx = find_window_idx(ws, ev->window);
    if (idx < 0)
      continue;

    int floating_mode = wm.workspace_floating[ws];
    int reel_mode = !wm.workspace_tiling[ws] && !floating_mode;

    if (reel_mode) {
      uint32_t mask = 0;
      uint32_t values[7];
      int n = 0;

      /* Never forward client X/Y in reel mode. */
      if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
        mask |= XCB_CONFIG_WINDOW_WIDTH;
        values[n++] = ev->width;
        wm.workspace_win_w[ws][idx] = ev->width;
      }
      if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
        mask |= XCB_CONFIG_WINDOW_HEIGHT;
        values[n++] = ev->height;
        wm.workspace_win_h[ws][idx] = ev->height;
      }
      if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
        mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
        values[n++] = ev->border_width;
      }
      if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
        mask |= XCB_CONFIG_WINDOW_SIBLING;
        values[n++] = ev->sibling;
      }
      if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
        mask |= XCB_CONFIG_WINDOW_STACK_MODE;
        values[n++] = ev->stack_mode;
      }

      if (mask)
        xcb_configure_window(wm.conn, ev->window, mask, values);

      if (ws != wm.current_workspace) {
        /* Keep an inactive reel window parked on its own workspace. */
        compute_reel_layout(ws);
        int32_t park_x =
            wm.workspace_win_x[ws][idx] + wm.screen_width * (ws + 2);
        int32_t park_y = wm.workspace_win_y[ws][idx];
        uint32_t pmask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        int32_t pvalues[] = {park_x, park_y};
        xcb_configure_window(wm.conn, ev->window, pmask, (uint32_t *)pvalues);
      } else {
        compute_reel_layout(ws);
        apply_reel_geometry(ws);
      }

      xcb_flush(wm.conn);

      if (ev->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))
        update_dialog_stack_owner(ev->window);
      return;
    }

    uint32_t mask = 0;
    uint32_t values[7];
    int n = 0;

    int inactive_floating = floating_mode && ws != wm.current_workspace;

    if (!inactive_floating && (ev->value_mask & XCB_CONFIG_WINDOW_X)) {
      mask |= XCB_CONFIG_WINDOW_X;
      values[n++] = (uint32_t)(int32_t)ev->x;
    }

    if (!inactive_floating && (ev->value_mask & XCB_CONFIG_WINDOW_Y)) {
      mask |= XCB_CONFIG_WINDOW_Y;
      values[n++] = (uint32_t)(int32_t)ev->y;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
      mask |= XCB_CONFIG_WINDOW_WIDTH;
      values[n++] = ev->width;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
      mask |= XCB_CONFIG_WINDOW_HEIGHT;
      values[n++] = ev->height;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
      mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
      values[n++] = ev->border_width;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
      mask |= XCB_CONFIG_WINDOW_SIBLING;
      values[n++] = ev->sibling;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
      mask |= XCB_CONFIG_WINDOW_STACK_MODE;
      values[n++] = ev->stack_mode;
    }

    if (mask)
      xcb_configure_window(wm.conn, ev->window, mask, values);
    xcb_flush(wm.conn);

    xcb_get_geometry_cookie_t gc = xcb_get_geometry(wm.conn, ev->window);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(wm.conn, gc, NULL);

    if (geo) {
      wm.workspace_win_w[ws][idx] = geo->width;
      wm.workspace_win_h[ws][idx] = geo->height;

      if (!inactive_floating) {
        wm.workspace_win_x[ws][idx] = geo->x;
        wm.workspace_win_y[ws][idx] = geo->y;
      }

      free(geo);
    }

    if (inactive_floating) {
      int32_t park_x = wm.workspace_win_x[ws][idx] + wm.screen_width * (ws + 2);
      int32_t park_y = wm.workspace_win_y[ws][idx];
      uint32_t pmask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      int32_t pvalues[] = {park_x, park_y};

      xcb_configure_window(wm.conn, ev->window, pmask, (uint32_t *)pvalues);
      xcb_flush(wm.conn);
    }

    if (ev->value_mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                          XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT))
      update_dialog_stack_owner(ev->window);
    return;
  }

  /* Unmanaged window: forward the request unchanged. */
  uint32_t mask = 0;
  uint32_t values[7];
  int n = 0;
  if (ev->value_mask & XCB_CONFIG_WINDOW_X) {
    mask |= XCB_CONFIG_WINDOW_X;
    values[n++] = (uint32_t)(int32_t)ev->x;
  }
  if (ev->value_mask & XCB_CONFIG_WINDOW_Y) {
    mask |= XCB_CONFIG_WINDOW_Y;
    values[n++] = (uint32_t)(int32_t)ev->y;
  }
  if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
    mask |= XCB_CONFIG_WINDOW_WIDTH;
    values[n++] = ev->width;
  }
  if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
    mask |= XCB_CONFIG_WINDOW_HEIGHT;
    values[n++] = ev->height;
  }
  if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
    mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
    values[n++] = ev->border_width;
  }
  if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
    mask |= XCB_CONFIG_WINDOW_SIBLING;
    values[n++] = ev->sibling;
  }
  if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
    mask |= XCB_CONFIG_WINDOW_STACK_MODE;
    values[n++] = ev->stack_mode;
  }
  if (mask)
    xcb_configure_window(wm.conn, ev->window, mask, values);
  xcb_flush(wm.conn);
}

void grab_focus_click(xcb_window_t win) {
  uint32_t cursor_value[] = {wm.cursor_normal};

  xcb_change_window_attributes(wm.conn, win, XCB_CW_CURSOR, cursor_value);
  uint8_t buttons[] = {XCB_BUTTON_INDEX_1, XCB_BUTTON_INDEX_2,
                       XCB_BUTTON_INDEX_3};
  for (int i = 0; i < 3; i++) {
    xcb_grab_button(wm.conn, 1, win, XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
                    buttons[i], 0);
  }
}

void handle_enter_notify(xcb_enter_notify_event_t *ev) { (void)ev; }

void handle_focus_in(xcb_focus_in_event_t *ev) {
  if (ev->mode == XCB_NOTIFY_MODE_NORMAL) {
    wm.focused = ev->event;
  }
}

static void cleanup_closed_window(xcb_window_t win) {
  int dialog_owner_ws_idx = -1;
  xcb_window_t dialog_prev_focus_win = XCB_WINDOW_NONE;
  for (int i = 0; i < wm.dialog_count; i++) {
    if (wm.dialog_windows[i] == win) {
      dialog_owner_ws_idx = wm.dialog_owner_ws[i];
      dialog_prev_focus_win = wm.dialog_prev_focus[i];
      break;
    }
  }

  if (remove_dialog_window(win)) {
    update_net_client_list();
  }
  int was_dock = remove_dock_window(win);
  int ws = remove_window_from_workspace(win);
  if (win == wm.focused) {
    wm.focused = XCB_WINDOW_NONE;
  }

  if (ws < 0 && dialog_owner_ws_idx >= 0 &&
      wm.workspace_counts[dialog_owner_ws_idx] > 0) {
    xcb_window_t restore_target = XCB_WINDOW_NONE;
    if (dialog_prev_focus_win != XCB_WINDOW_NONE &&
        dialog_prev_focus_win != win &&
        window_is_alive(dialog_prev_focus_win)) {
      restore_target = dialog_prev_focus_win;
    } else {
      restore_target =
          wm.workspace_windows[dialog_owner_ws_idx]
                              [wm.workspace_focus_idx[dialog_owner_ws_idx]];
    }

    if (dialog_owner_ws_idx == wm.current_workspace) {
      focus_window(restore_target);
    }
  }

  if (ws >= 0) {
    if (wm.workspace_counts[ws] > 0) {
      compute_reel_layout(ws);
      if (ws == wm.current_workspace) {
        apply_reel_geometry(ws);
        focus_window(wm.workspace_windows[ws][wm.workspace_focus_idx[ws]]);
      }
    } else {
      compact_workspaces(ws);
      if (wm.workspace_counts[wm.current_workspace] > 0) {
        compute_reel_layout(wm.current_workspace);
        apply_reel_geometry(wm.current_workspace);
        focus_window(
            wm.workspace_windows[wm.current_workspace]
                                [wm.workspace_focus_idx[wm.current_workspace]]);
      } else {
        wm.focused = XCB_WINDOW_NONE;
      }
    }
  }

  if (was_dock) {
    for (int i = 0; i < MAX_WORKSPACES; i++) {
      if (wm.workspace_counts[i] > 0) {
        compute_reel_layout(i);
      }
    }
    if (wm.workspace_counts[wm.current_workspace] > 0) {
      apply_reel_geometry(wm.current_workspace);
    }
  }

  if (wm.in_overview) {
    render_overview();
  }
}

void handle_unmap_notify(xcb_unmap_notify_event_t *ev) {
  cleanup_closed_window(ev->window);
}

void handle_destroy_notify(xcb_destroy_notify_event_t *ev) {
  cleanup_closed_window(ev->window);
}

void handle_expose(xcb_expose_event_t *ev) {
  if (wm.in_overview && ev->window == wm.overview_win) {
    render_overview();
  }
}

void handle_property_notify(xcb_property_notify_event_t *ev) {

  if (ev->atom == A_WM_TRANSIENT_FOR) {
    if (is_window_managed(ev->window) && is_dialog_window(ev->window)) {
      int ws = remove_window_from_workspace(ev->window);
      if (ws >= 0) {
        add_dialog_window(ev->window, ws, wm.focused);
        update_dialog_stack_owner(ev->window);
        update_net_client_list();
        focus_window(ev->window);

        if (wm.workspace_counts[ws] > 0) {
          compute_reel_layout(ws);
          if (ws == wm.current_workspace) {
            apply_reel_geometry(ws);
          }
        } else if (ws == wm.current_workspace) {
          wm.focused = ev->window;
        }

        if (wm.in_overview) {
          render_overview();
        }
      }
    }
    return;
  }

  if (ev->atom != XCB_ATOM_WM_CLASS && ev->atom != A_NET_WM_WINDOW_TYPE) {
    return;
  }

  int is_dock = is_dock_window(ev->window);
  int is_util = !is_dock && is_utility_window(ev->window);
  if (!is_dock && !is_util) {
    return;
  }

  int ws = remove_window_from_workspace(ev->window);
  if (ws < 0) {
    return;
  }

  if (is_dock) {
    add_dock_window(ev->window);
    raise_docks();
  }

  if (wm.workspace_counts[ws] > 0) {
    compute_reel_layout(ws);
    if (ws == wm.current_workspace) {
      apply_reel_geometry(ws);
      focus_window(wm.workspace_windows[ws][wm.workspace_focus_idx[ws]]);
    }
  } else if (ev->window == wm.focused) {
    wm.focused = XCB_WINDOW_NONE;
  }

  if (wm.in_overview) {
    render_overview();
  }
}

int main() {
  wm.conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(wm.conn)) {
    fprintf(stderr, "Failed to connect to X server\n");
    return 1;
  }

  wm.screen = xcb_setup_roots_iterator(xcb_get_setup(wm.conn)).data;
  wm.root = wm.screen->root;
  wm.focused = XCB_WINDOW_NONE;
  wm.running = 1;
  wm.current_workspace = 0;
  wm.num_workspaces = START_WORKSPACES;
  wm.in_overview = 0;
  wm.overview_win = 0;
  wm.screen_width = wm.screen->width_in_pixels;
  wm.screen_height = wm.screen->height_in_pixels;
  wm.dragging = 0;
  wm.drag_window = XCB_WINDOW_NONE;
  wm.dock_count = 0;
  wm.dialog_count = 0;

  struct sigaction sa;
  sa.sa_handler = reap_children;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, NULL);

  init_workspaces();
  init_ewmh_atoms();
  init_cursors();
  init_drop_preview();
  update_net_current_desktop();
  publish_workspace_modes();

  uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                  XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                  XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_KEY_PRESS;
  xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_EVENT_MASK, &mask);

  setup_key_grabs();
  setup_move_resize_grabs();

  spawnstartup();

  map_all_windows();
  cleanup_stuck_windows();

  usleep(200000);
  reclassify_stray_windows();

  xcb_generic_event_t *ev;
  while (wm.running && (ev = xcb_wait_for_event(wm.conn))) {
    int type = ev->response_type & ~0x80;

    switch (type) {
    case XCB_KEY_PRESS:
      handle_key_press((xcb_key_press_event_t *)ev);
      break;

    case XCB_BUTTON_PRESS:
      handle_button_press((xcb_button_press_event_t *)ev);
      break;

    case XCB_BUTTON_RELEASE:
      handle_button_release((xcb_button_release_event_t *)ev);
      break;

    case XCB_MOTION_NOTIFY:
      handle_motion_notify((xcb_motion_notify_event_t *)ev);
      break;

    case XCB_MAP_REQUEST:
      handle_map_request((xcb_map_request_event_t *)ev);
      break;

    case XCB_CONFIGURE_REQUEST:
      handle_configure_request((xcb_configure_request_event_t *)ev);
      break;

    case XCB_UNMAP_NOTIFY:
      handle_unmap_notify((xcb_unmap_notify_event_t *)ev);
      break;

    case XCB_DESTROY_NOTIFY:
      handle_destroy_notify((xcb_destroy_notify_event_t *)ev);
      break;

    case XCB_ENTER_NOTIFY:
      handle_enter_notify((xcb_enter_notify_event_t *)ev);
      break;

    case XCB_FOCUS_IN:
      handle_focus_in((xcb_focus_in_event_t *)ev);
      break;

    case XCB_EXPOSE:
      handle_expose((xcb_expose_event_t *)ev);
      break;

    case XCB_PROPERTY_NOTIFY:
      handle_property_notify((xcb_property_notify_event_t *)ev);
      break;

    case XCB_CLIENT_MESSAGE:
      handle_client_message((xcb_client_message_event_t *)ev);
      break;

    default:
      break;
    }
    free(ev);
    xcb_flush(wm.conn);
  }

  xcb_disconnect(wm.conn);
  printf("WM stopped\n");
  return 0;
}
