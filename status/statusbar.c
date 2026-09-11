#include <cairo/cairo-xcb.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>

#define BAR_HEIGHT 40
#define BAR_RADIUS 14

#define CMD_LAUNCH_CC "/home/hammad/wm/status/cc/blackcc"

typedef struct {
  xcb_connection_t *conn;
  xcb_screen_t *screen;

  xcb_window_t win;
  cairo_surface_t *surface;
  cairo_t *cr;
  int width;
  int height;

  xcb_atom_t A_NET_CURRENT_DESKTOP;
  xcb_atom_t A_NET_NUMBER_OF_DESKTOPS;
  xcb_atom_t A_NET_ACTIVE_WINDOW;
  xcb_atom_t A_NET_WM_WINDOW_TYPE;
  xcb_atom_t A_NET_WM_WINDOW_TYPE_DOCK;
  xcb_atom_t A_NET_WM_STATE;
  xcb_atom_t A_NET_WM_STATE_SKIP_TASKBAR;
  xcb_atom_t A_NET_WM_STATE_SKIP_PAGER;
  xcb_atom_t A_NET_WM_STRUT;
  xcb_atom_t A_NET_WM_STRUT_PARTIAL;
  xcb_atom_t A_NET_WM_DESKTOP;
  xcb_atom_t A_NET_WM_NAME;
  xcb_atom_t A_UTF8_STRING;

  uint32_t current_desktop;
  uint32_t num_desktops;

  int hovered_settings;
  int hovered_workspace;

  // Internet Speed Tracking State
  unsigned long long last_rx_bytes;
  unsigned long long last_tx_bytes;
  time_t last_net_time;
  char net_speed_str[64];
} Bar;

static Bar bar;

static void run_command_async(const char *cmd) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  } else if (pid > 0) {
    signal(SIGCHLD, SIG_IGN);
  }
}

static void draw_rounded_rectangle(cairo_t *cr, double x, double y,
                                   double width, double height, double radius) {
  if (width <= 0 || height <= 0)
    return;
  radius = fmin(radius, fmin(width / 2.0, height / 2.0));
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI_2, 0);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI_2);
  cairo_arc(cr, x + radius, y + height - radius, radius, M_PI_2, M_PI);
  cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI_2);
  cairo_close_path(cr);
}

// Glowing Helper Function
static void draw_glowing_pill(cairo_t *cr, double x, double y, double w,
                              double h, double r, double red, double green,
                              double blue, double glow_alpha) {
  for (int i = 6; i >= 1; i--) {
    double offset = i * 1.2;
    draw_rounded_rectangle(cr, x - offset, y - offset, w + (offset * 2),
                           h + (offset * 2), r + offset);
    cairo_set_source_rgba(cr, red, green, blue, glow_alpha / (i * 2.2));
    cairo_fill(cr);
  }
}

static xcb_atom_t get_atom(const char *name) {
  xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(bar.conn, 0, strlen(name), name);
  xcb_intern_atom_reply_t *reply =
      xcb_intern_atom_reply(bar.conn, cookie, NULL);
  if (!reply)
    return XCB_ATOM_NONE;
  xcb_atom_t atom = reply->atom;
  free(reply);
  return atom;
}

static void init_atoms(void) {
  bar.A_NET_CURRENT_DESKTOP = get_atom("_NET_CURRENT_DESKTOP");
  bar.A_NET_NUMBER_OF_DESKTOPS = get_atom("_NET_NUMBER_OF_DESKTOPS");
  bar.A_NET_ACTIVE_WINDOW = get_atom("_NET_ACTIVE_WINDOW");
  bar.A_NET_WM_WINDOW_TYPE = get_atom("_NET_WM_WINDOW_TYPE");
  bar.A_NET_WM_WINDOW_TYPE_DOCK = get_atom("_NET_WM_WINDOW_TYPE_DOCK");
  bar.A_NET_WM_STATE = get_atom("_NET_WM_STATE");
  bar.A_NET_WM_STATE_SKIP_TASKBAR = get_atom("_NET_WM_STATE_SKIP_TASKBAR");
  bar.A_NET_WM_STATE_SKIP_PAGER = get_atom("_NET_WM_STATE_SKIP_PAGER");
  bar.A_NET_WM_STRUT = get_atom("_NET_WM_STRUT");
  bar.A_NET_WM_STRUT_PARTIAL = get_atom("_NET_WM_STRUT_PARTIAL");
  bar.A_NET_WM_DESKTOP = get_atom("_NET_WM_DESKTOP");
  bar.A_NET_WM_NAME = get_atom("_NET_WM_NAME");
  bar.A_UTF8_STRING = get_atom("UTF8_STRING");
}

static void update_wm_info(void) {
  xcb_get_property_cookie_t c_ndesk =
      xcb_get_property(bar.conn, 0, bar.screen->root,
                       bar.A_NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 0, 1);
  xcb_get_property_reply_t *r_ndesk =
      xcb_get_property_reply(bar.conn, c_ndesk, NULL);
  if (r_ndesk && xcb_get_property_value_length(r_ndesk) > 0) {
    bar.num_desktops = *(uint32_t *)xcb_get_property_value(r_ndesk);
  } else {
    bar.num_desktops = 4;
  }
  free(r_ndesk);

  xcb_get_property_cookie_t c_curdesk =
      xcb_get_property(bar.conn, 0, bar.screen->root, bar.A_NET_CURRENT_DESKTOP,
                       XCB_ATOM_CARDINAL, 0, 1);
  xcb_get_property_reply_t *r_curdesk =
      xcb_get_property_reply(bar.conn, c_curdesk, NULL);
  if (r_curdesk && xcb_get_property_value_length(r_curdesk) > 0) {
    bar.current_desktop = *(uint32_t *)xcb_get_property_value(r_curdesk);
  }
  free(r_curdesk);
}

static void format_speed(double bytes_per_sec, char *buf, size_t buf_size) {
  if (bytes_per_sec >= 1024.0 * 1024.0) {
    snprintf(buf, buf_size, "%.1f MB/s", bytes_per_sec / (1024.0 * 1024.0));
  } else {
    snprintf(buf, buf_size, "%.0f KB/s", bytes_per_sec / 1024.0);
  }
}

static void update_net_speed(void) {
  FILE *fp = fopen("/proc/net/dev", "r");
  if (!fp) {
    snprintf(bar.net_speed_str, sizeof(bar.net_speed_str), "Offline");
    return;
  }

  char line[256];
  unsigned long long total_rx = 0, total_tx = 0;

  fgets(line, sizeof(line), fp);
  fgets(line, sizeof(line), fp);

  while (fgets(line, sizeof(line), fp)) {
    char iface[32];
    unsigned long long rx = 0, tx = 0;
    if (sscanf(line, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", iface,
               &rx, &tx) == 3) {
      if (strcmp(iface, "lo") != 0) {
        total_rx += rx;
        total_tx += tx;
      }
    }
  }
  fclose(fp);

  time_t now = time(NULL);
  double diff = difftime(now, bar.last_net_time);

  if (diff > 0 && bar.last_net_time > 0) {
    double rx_speed = (total_rx - bar.last_rx_bytes) / diff;
    double tx_speed = (total_tx - bar.last_tx_bytes) / diff;

    char rx_str[16], tx_str[16];
    format_speed(rx_speed, rx_str, sizeof(rx_str));
    format_speed(tx_speed, tx_str, sizeof(tx_str));

    snprintf(bar.net_speed_str, sizeof(bar.net_speed_str), " %s  %s",
             rx_str, tx_str);
  } else if (bar.last_net_time == 0) {
    snprintf(bar.net_speed_str, sizeof(bar.net_speed_str),
             " 0 KB/s  0 KB/s");
  }

  bar.last_rx_bytes = total_rx;
  bar.last_tx_bytes = total_tx;
  bar.last_net_time = now;
}

static void switch_workspace(uint32_t desktop) {
  xcb_client_message_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.response_type = XCB_CLIENT_MESSAGE;
  ev.format = 32;
  ev.window = bar.screen->root;
  ev.type = bar.A_NET_CURRENT_DESKTOP;
  ev.data.data32[0] = desktop;
  ev.data.data32[1] = XCB_CURRENT_TIME;

  xcb_send_event(bar.conn, 0, bar.screen->root,
                 XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                     XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                 (const char *)&ev);
  xcb_flush(bar.conn);
}

static void setup_nerd_font(cairo_t *cr, double size) {
  const char *font_names[] = {"JetBrainsMono Nerd Font", "FiraCode Nerd Font",
                              "Symbols Nerd Font", "Sans", NULL};

  for (int i = 0; font_names[i] != NULL; i++) {
    cairo_select_font_face(cr, font_names[i], CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, size);

    cairo_text_extents_t test;
    cairo_text_extents(cr, "󰕒", &test);
    if (test.width > 0)
      break;
  }
}

static void render_bar(void) {
  cairo_t *cr = bar.cr;

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);

  const double panel_x = 6.0;
  const double panel_y = 2.0;
  const double panel_w = bar.width - 12.0;
  const double panel_h = bar.height - 4.0;
  const double panel_r = BAR_RADIUS;

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  // Modern Glass Panel Background
  draw_rounded_rectangle(cr, panel_x, panel_y, panel_w, panel_h, panel_r);
  cairo_clip(cr);
  cairo_pattern_t *bg =
      cairo_pattern_create_linear(0, panel_y, 0, panel_y + panel_h);
  cairo_pattern_add_color_stop_rgba(bg, 0.00, 0.05, 0.07, 0.11, 0.94);
  cairo_pattern_add_color_stop_rgba(bg, 1.00, 0.02, 0.03, 0.05, 0.96);
  cairo_set_source(cr, bg);
  cairo_fill(cr);
  cairo_reset_clip(cr);

  // Rim Highlight
  draw_rounded_rectangle(cr, panel_x + 0.5, panel_y + 0.5,
                         panel_w - 1.5, panel_h - 1.0,
                         panel_r - 1.5);
  cairo_set_source_rgba(cr, 0.435, 0.694, 1.0, 1.0);
  cairo_set_line_width(cr, 2.0);
  cairo_stroke(cr);;

  // WORKSPACES — Nerd Font Numbers
  static const char *ws_num_icons[] = {
      // "󰆍",  // Workspace 0: Terminal
      // "󰖟",  // Workspace 1: Browser
      // "󰎆",  // Workspace 2: Music
      // "󰆉",  // Workspace 3: Settings
      // "󰊤",  // Workspace 4: Documents
      // "󰉋",  // Workspace 5: Folder
      // "󰌽",  // Workspace 6: Chat
      // "󰙱",  // Workspace 7: Games
      // "󰀼",  // Workspace 8: Code 󰀻   󰛨   󰊠   󰎈   󰕮   󰗼
      // 󰚩   󰘦   󰆍
      // "󰗡",  // Workspace 9: Files 󰖟   󰆍   󰈹   󰙯   󰉋   󰓓
      // 󰋩   󰒱   󰇧
      "", "", "", "", "󰌽", "", "  ", " "};
  static const int ws_num_count =
      sizeof(ws_num_icons) / sizeof(ws_num_icons[0]);

  double x = 14.0;
  const int ws_h = 30;
  const int ws_y = (BAR_HEIGHT - ws_h) / 2;

  for (uint32_t i = 0; i < bar.num_desktops; i++) {
    int is_active = (i == bar.current_desktop);
    int is_hover = (i == (uint32_t)bar.hovered_workspace);

    int ws_w = is_active ? 38 : 32;
    int ws_radius = 8;

    draw_rounded_rectangle(cr, x, ws_y, ws_w, ws_h, ws_radius);

    if (is_active) {
      cairo_pattern_t *active_bg =
          cairo_pattern_create_linear(x, ws_y, x + ws_w, ws_y);
      cairo_pattern_add_color_stop_rgba(active_bg, 0.0, 0.00, 0.80, 0.98, 1.0);
      cairo_pattern_add_color_stop_rgba(active_bg, 1.0, 0.55, 0.35, 0.98, 1.0);
      cairo_set_source(cr, active_bg);
      cairo_fill(cr);
      cairo_pattern_destroy(active_bg);
    } else if (is_hover) {
      cairo_set_source_rgba(cr, 0.25, 0.32, 0.45, 0.70);
      cairo_fill(cr);
    } else {
      cairo_set_source_rgba(cr, 0.12, 0.16, 0.24, 0.50);
      cairo_fill(cr);
    }

    char ws_str[16];
    if ((int)i < ws_num_count) {
      snprintf(ws_str, sizeof(ws_str), "%s", ws_num_icons[i]);
    } else {
      snprintf(ws_str, sizeof(ws_str), "%u", i + 1);
    }

    setup_nerd_font(cr, is_active ? 20.0 : 20.0);

    if (is_active) {
      cairo_set_source_rgba(cr, 0.02, 0.02, 0.05, 1.0);
    } else if (is_hover) {
      cairo_set_source_rgba(cr, 0.95, 0.98, 1.0, 1.0);
    } else {
      cairo_set_source_rgba(cr, 0.75, 0.82, 0.92, 0.90);
    }

    cairo_text_extents_t ext;
    cairo_text_extents(cr, ws_str, &ext);
    cairo_move_to(cr, x + (ws_w - ext.width) / 2.0 - ext.x_bearing,
                  ws_y + (ws_h - ext.height) / 2.0 - ext.y_bearing);
    cairo_show_text(cr, ws_str);

    x += ws_w + 6.0;
  }

  // CENTER CLOCK Capsule (WITH GLOW)
  time_t now = time(NULL);
  struct tm *tm_info = localtime(&now);
  char datetime_str[64];
  strftime(datetime_str, sizeof(datetime_str), "%a, %b %d   %I:%M %p", tm_info);

  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 11.0);

  cairo_text_extents_t dt_ext;
  cairo_text_extents(cr, datetime_str, &dt_ext);

  const double time_w = dt_ext.width + 30.0;
  const double time_h = 26.0;
  const double time_x = (bar.width - time_w) / 2.0;
  const double time_y = (BAR_HEIGHT - time_h) / 2.0;

  // 1. Clock Outer Glow (Cyan/Electric Blue)
  draw_glowing_pill(cr, time_x, time_y, time_w, time_h, 8.0, 0.00, 0.80, 0.98,
                    0.40);

  // 2. Base Fill & Glowing Border
  draw_rounded_rectangle(cr, time_x, time_y, time_w, time_h, 8.0);
  cairo_set_source_rgba(cr, 0.08, 0.12, 0.22, 0.85);
  cairo_fill_preserve(cr);

  cairo_set_source_rgba(cr, 0.00, 0.80, 0.98, 0.50);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  // 3. Text
  cairo_set_source_rgba(cr, 0.95, 0.98, 1.0, 1.0);
  cairo_move_to(cr, time_x + (time_w - dt_ext.width) / 2.0 - dt_ext.x_bearing,
                time_y + (time_h - dt_ext.height) / 2.0 - dt_ext.y_bearing);
  cairo_show_text(cr, datetime_str);

  // RIGHT SIDE CONTROLS
  double right_x = bar.width - 14.0;
  const int btn_w = 34;
  const int btn_h = 26;
  const int btn_y = (BAR_HEIGHT - btn_h) / 2;

  right_x -= btn_w;
  draw_rounded_rectangle(cr, right_x, btn_y, btn_w, btn_h, 8.0);

  if (bar.hovered_settings) {
    cairo_set_source_rgba(cr, 0.25, 0.35, 0.55, 0.80);
  } else {
    cairo_set_source_rgba(cr, 0.10, 0.13, 0.20, 0.50);
  }
  cairo_fill_preserve(cr);

  cairo_set_source_rgba(cr, 0.35, 0.45, 0.65, 0.25);
  cairo_set_line_width(cr, 0.8);
  cairo_stroke(cr);

  setup_nerd_font(cr, 13.0);
  cairo_set_source_rgba(cr, 0.92, 0.95, 1.0, 0.95);
  cairo_move_to(cr, right_x + 10, btn_y + 18);
  cairo_show_text(cr, "󰍜");

  right_x -= 8.0;

  // NETWORK SPEED PILL (FIXED SIZE & GLOW)
  setup_nerd_font(cr, 10.0);

  const double net_w = 135.0;
  const double net_h = 26.0;
  const double net_x = right_x - net_w;
  const double net_y = (BAR_HEIGHT - net_h) / 2.0;

  // 1. Net Speed Outer Glow (Purple/Magenta)
  draw_glowing_pill(cr, net_x, net_y, net_w, net_h, 8.0, 0.60, 0.35, 0.98,
                    0.35);

  // 2. Base Fill & Glowing Border
  draw_rounded_rectangle(cr, net_x, net_y, net_w, net_h, 8.0);
  cairo_set_source_rgba(cr, 0.10, 0.12, 0.22, 0.85);
  cairo_fill_preserve(cr);

  cairo_set_source_rgba(cr, 0.60, 0.35, 0.98, 0.50);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  // 3. Text Center Align
  cairo_text_extents_t net_ext;
  cairo_text_extents(cr, bar.net_speed_str, &net_ext);

  cairo_set_source_rgba(cr, 0.95, 0.98, 1.0, 1.0);
  cairo_move_to(cr, net_x + (net_w - net_ext.width) / 2.0 - net_ext.x_bearing,
                net_y + (net_h - net_ext.height) / 2.0 - net_ext.y_bearing);
  cairo_show_text(cr, bar.net_speed_str);

  cairo_surface_flush(bar.surface);
  xcb_flush(bar.conn);
}

static void apply_rounded_window_shape(void) {
  const int x0 = 6;
  const int y0 = 2;
  const int w = bar.width - 12;
  const int h = bar.height - 4;
  const int r = BAR_RADIUS;

  if (w <= 0 || h <= 0)
    return;

  xcb_rectangle_t rects[BAR_HEIGHT];
  int n = 0;

  for (int row = 0; row < h; row++) {
    double dy = 0.0;
    if (row < r)
      dy = (double)r - row - 0.5;
    else if (row >= h - r)
      dy = (double)row - (h - r) + 0.5;

    int inset = 0;
    if (dy > 0.0) {
      double inside = (double)r * r - dy * dy;
      if (inside > 0.0)
        inset = (int)floor((double)r - sqrt(inside));
    }

    xcb_rectangle_t rr = {.x = (int16_t)(x0 + inset),
                          .y = (int16_t)(y0 + row),
                          .width = (uint16_t)(w - 2 * inset),
                          .height = 1};

    if (rr.width > 0)
      rects[n++] = rr;
  }

  xcb_shape_rectangles(bar.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING,
                       XCB_CLIP_ORDERING_UNSORTED, bar.win, 0, 0, n, rects);
  xcb_flush(bar.conn);
}

static void setup_window(void) {
  bar.width = bar.screen->width_in_pixels;
  bar.height = BAR_HEIGHT;

  bar.win = xcb_generate_id(bar.conn);

  uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  uint32_t values[] = {
      bar.screen->black_pixel,
      XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_PROPERTY_CHANGE |
          XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_ENTER_WINDOW |
          XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION};

  xcb_create_window(bar.conn, XCB_COPY_FROM_PARENT, bar.win, bar.screen->root,
                    0, 0, bar.width, bar.height, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, bar.screen->root_visual,
                    mask, values);

  init_atoms();

  uint32_t root_event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(bar.conn, bar.screen->root, XCB_CW_EVENT_MASK,
                               &root_event_mask);

  xcb_change_property(bar.conn, XCB_PROP_MODE_REPLACE, bar.win,
                      bar.A_NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 32, 1,
                      &bar.A_NET_WM_WINDOW_TYPE_DOCK);

  xcb_atom_t skip_states[2] = {bar.A_NET_WM_STATE_SKIP_TASKBAR,
                               bar.A_NET_WM_STATE_SKIP_PAGER};
  xcb_change_property(bar.conn, XCB_PROP_MODE_REPLACE, bar.win,
                      bar.A_NET_WM_STATE, XCB_ATOM_ATOM, 32, 2, skip_states);

  uint32_t strut[4] = {0, 0, BAR_HEIGHT, 0};
  xcb_change_property(bar.conn, XCB_PROP_MODE_REPLACE, bar.win,
                      bar.A_NET_WM_STRUT, XCB_ATOM_CARDINAL, 32, 4, strut);

  uint32_t strut_partial[12] = {
      0, 0, BAR_HEIGHT, 0, 0, 0, 0, 0, 0, (uint32_t)bar.width - 1, 0, 0};
  xcb_change_property(bar.conn, XCB_PROP_MODE_REPLACE, bar.win,
                      bar.A_NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 32, 12,
                      strut_partial);

  uint32_t all_desktops = 0xFFFFFFFF;
  xcb_change_property(bar.conn, XCB_PROP_MODE_REPLACE, bar.win,
                      bar.A_NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1,
                      &all_desktops);

  xcb_visualtype_t *visual =
      xcb_aux_find_visual_by_id(bar.screen, bar.screen->root_visual);
  bar.surface = cairo_xcb_surface_create(bar.conn, bar.win, visual, bar.width,
                                         bar.height);
  bar.cr = cairo_create(bar.surface);

  apply_rounded_window_shape();
  xcb_map_window(bar.conn, bar.win);

  bar.hovered_settings = 0;
  bar.hovered_workspace = -1;
  bar.last_rx_bytes = 0;
  bar.last_tx_bytes = 0;
  bar.last_net_time = 0;
  bar.current_desktop = 0;

  xcb_flush(bar.conn);
}

int main(void) {
  bar.conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(bar.conn)) {
    fprintf(stderr, "Error: Failed to connect to X server\n");
    return 1;
  }

  bar.screen = xcb_setup_roots_iterator(xcb_get_setup(bar.conn)).data;
  setup_window();

  update_wm_info();
  update_net_speed();
  render_bar();

  int xcb_fd = xcb_get_file_descriptor(bar.conn);

  while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(xcb_fd, &fds);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int ret = select(xcb_fd + 1, &fds, NULL, NULL, &tv);

    if (ret > 0) {
      xcb_generic_event_t *event;
      while ((event = xcb_poll_for_event(bar.conn))) {
        uint8_t response_type = event->response_type & ~0x80;

        if (response_type == XCB_PROPERTY_NOTIFY) {
          update_wm_info();
          render_bar();
        } else if (response_type == XCB_EXPOSE) {
          render_bar();
        } else if (response_type == XCB_LEAVE_NOTIFY) {
          bar.hovered_settings = 0;
          bar.hovered_workspace = -1;
          render_bar();
        } else if (response_type == XCB_BUTTON_PRESS) {
          xcb_button_press_event_t *bp = (xcb_button_press_event_t *)event;

          if (bp->detail != XCB_BUTTON_INDEX_1) {
            free(event);
            continue;
          }

          int btn_x = bar.width - 48;
          int btn_w = 34;

          if (bp->event_x >= btn_x && bp->event_x <= btn_x + btn_w) {
            run_command_async(CMD_LAUNCH_CC);
          } else {
            double x = 14.0;
            for (uint32_t i = 0; i < bar.num_desktops; i++) {
              int is_active = (i == bar.current_desktop);
              int ws_w = is_active ? 38 : 32;
              int ws_h = 30;
              int ws_y = (BAR_HEIGHT - ws_h) / 2;

              if (bp->event_x >= x && bp->event_x <= x + ws_w &&
                  bp->event_y >= ws_y && bp->event_y <= ws_y + ws_h) {
                bar.current_desktop = i;
                switch_workspace(i);
                render_bar();
                break;
              }
              x += ws_w + 6.0;
            }
          }
        } else if (response_type == XCB_MOTION_NOTIFY) {
          xcb_motion_notify_event_t *mn = (xcb_motion_notify_event_t *)event;

          int btn_x = bar.width - 48;
          int btn_w = 34;

          int is_settings_hover =
              (mn->event_x >= btn_x && mn->event_x <= btn_x + btn_w);
          int new_ws_hover = -1;

          double x = 14.0;
          for (uint32_t i = 0; i < bar.num_desktops; i++) {
            int is_active = (i == bar.current_desktop);
            int ws_w = is_active ? 38 : 32;
            int ws_h = 30;
            int ws_y = (BAR_HEIGHT - ws_h) / 2;

            if (mn->event_x >= x && mn->event_x <= x + ws_w &&
                mn->event_y >= ws_y && mn->event_y <= ws_y + ws_h) {
              new_ws_hover = i;
              break;
            }
            x += ws_w + 6.0;
          }

          if (is_settings_hover != bar.hovered_settings ||
              new_ws_hover != bar.hovered_workspace) {
            bar.hovered_settings = is_settings_hover;
            bar.hovered_workspace = new_ws_hover;
            render_bar();
          }
        }
        free(event);
      }
    } else {
      update_net_speed();
      render_bar();
    }
  }

  cairo_destroy(bar.cr);
  cairo_surface_destroy(bar.surface);
  xcb_disconnect(bar.conn);
  return 0;
}
