#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_WIDTH 380
#define MAIN_HEIGHT 400

#define VIEW_MAIN 0
#define VIEW_WIFI 1
#define VIEW_BLUETOOTH 2

// ================= THEME =================

#define WIN_RADIUS 18

#define COL_BG_TOP 0x111827
#define COL_BG_BOTTOM 0x030712
#define COL_TEXT 0xf8fafc
#define COL_TEXT_SOFT 0xe2e8f0
#define COL_TEXT_DIM 0x94a3b8
#define COL_BLUE 0x3b82f6
#define COL_INDIGO 0x6366f1
#define COL_PURPLE 0x8b5cf6
#define COL_GREEN 0x10b981
#define COL_RED 0xef4444

// slider track geometry (knob stays fully inside the window at 0% / 100%)
#define SL_X 23
#define SL_W 334

// ids returned by hit_test() (also used for hover highlighting)
enum {
  HIT_NONE = -1,
  HIT_ARROW0 = 4, // 4, 5 = chevron zone of Wi-Fi / Bluetooth tile
  HIT_VOL = 10,
  HIT_MIC = 11,
  HIT_LOGOUT = 20,
  HIT_RESTART = 21,
  HIT_POWEROFF = 22,
  HIT_BACK = 30,
  HIT_ITEM0 = 40
};

enum {
  ICON_WIFI, // 0..3 match the tile index
  ICON_BLUETOOTH,
  ICON_MOON,
  ICON_AIRPLANE,
  ICON_SPEAKER,
  ICON_MIC,
  ICON_LOGOUT,
  ICON_RESTART,
  ICON_POWER,
  ICON_CHEV_RIGHT,
  ICON_CHEV_LEFT
};

int current_view = VIEW_MAIN;
int hover_id = HIT_NONE;

typedef struct {
  int x, y, w, h;
  char label[32];
  char status[32];
  int active;
  int has_arrow;
  unsigned int accent;
} Tile;

Tile tiles[4] = {
  {15, 70, 170, 65, "Wi-Fi", "Checking...", 0, 1, COL_BLUE},
  {195, 70, 170, 65, "Bluetooth", "Checking...", 0, 1, COL_INDIGO},
  {15, 145, 170, 65, "Do Not Disturb", "Off", 0, 0, COL_PURPLE},
  {195, 145, 170, 65, "Airplane Mode", "Off", 0, 0, COL_GREEN}
};

typedef struct {
  char name[64];
  char info[64];
} SubItem;

SubItem sub_items[10];
int sub_item_count = 0;

int volume_val = 75;
int mic_val = 60;
int is_dragging_vol = 0;
int is_dragging_mic = 0;

Visual *visual32 = NULL;
Colormap cmap32 = 0;
int depth32 = 0;

typedef struct {
  Display *d;
  Window w;
  Pixmap pix;
  GC gc;
  XftFont *title, *bold, *regular;
} UI;

typedef struct {
  Display *d;
  cairo_t *cr;
  XftDraw *xd;
  XftFont *title, *bold, *regular;
} Canvas;

void setup_32bit_visual(Display *d, int screen) {
  XVisualInfo vinfo;
  if (XMatchVisualInfo(d, screen, 32, TrueColor, &vinfo)) {
    visual32 = vinfo.visual;
    depth32 = vinfo.depth;
    cmap32 = XCreateColormap(d, RootWindow(d, screen), visual32, AllocNone);
  } else {
    visual32 = DefaultVisual(d, screen);
    depth32 = DefaultDepth(d, screen);
    cmap32 = DefaultColormap(d, screen);
  }
}

// ================= DRAWING PRIMITIVES =================

static void set_color(cairo_t *cr, unsigned int hex, double a) {
  cairo_set_source_rgba(cr, ((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0, (hex & 0xff) / 255.0, a);
}

static void add_stop(cairo_pattern_t *p, double off, unsigned int hex, double a) {
  cairo_pattern_add_color_stop_rgba(p, off, ((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0, (hex & 0xff) / 255.0, a);
}

static unsigned int mix_white(unsigned int hex, double t) {
  unsigned int r = (hex >> 16) & 0xff, g = (hex >> 8) & 0xff, b = hex & 0xff;
  r += (unsigned int)((255 - r) * t);
  g += (unsigned int)((255 - g) * t);
  b += (unsigned int)((255 - b) * t);
  return (r << 16) | (g << 8) | b;
}

static void rounded_path(cairo_t *cr, double x, double y, double w, double h, double r) {
  if (r > h / 2.0) r = h / 2.0;
  if (r > w / 2.0) r = w / 2.0;
  cairo_new_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2.0);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
  cairo_close_path(cr);
}

static void fill_round(cairo_t *cr, double x, double y, double w, double h, double r, unsigned int hex, double a) {
  rounded_path(cr, x, y, w, h, r);
  set_color(cr, hex, a);
  cairo_fill(cr);
}

static void fill_round_grad(cairo_t *cr, double x, double y, double w, double h, double r, int horizontal,
                            unsigned int c1, double a1, unsigned int c2, double a2) {
  if (w < 1.0 || h < 1.0) return;
  cairo_pattern_t *p = horizontal ? cairo_pattern_create_linear(x, 0, x + w, 0)
                                  : cairo_pattern_create_linear(0, y, 0, y + h);
  add_stop(p, 0.0, c1, a1);
  add_stop(p, 1.0, c2, a2);
  rounded_path(cr, x, y, w, h, r);
  cairo_set_source(cr, p);
  cairo_fill(cr);
  cairo_pattern_destroy(p);
}

static void stroke_round(cairo_t *cr, double x, double y, double w, double h, double r, unsigned int hex, double a) {
  rounded_path(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, r - 0.5);
  set_color(cr, hex, a);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);
}

static void fill_circle(cairo_t *cr, double cx, double cy, double r, unsigned int hex, double a) {
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, r, 0, 2.0 * M_PI);
  set_color(cr, hex, a);
  cairo_fill(cr);
}

// ================= TEXT HELPERS =================

static int text_width(Display *d, XftFont *f, const char *text) {
  XGlyphInfo ext;
  XftTextExtentsUtf8(d, f, (const FcChar8 *)text, strlen(text), &ext);
  return ext.xOff;
}

// baseline that vertically centres a line of text around cy
static int baseline_center(XftFont *f, double cy) {
  return (int)(cy + (f->ascent - f->descent) / 2.0 + 0.5);
}

static void text_left(Canvas *c, XftFont *f, unsigned int hex, double a, int x, int y, const char *text) {
  // Xft/Render wants premultiplied colour
  XRenderColor rc;
  rc.red = (unsigned short)(((hex >> 16) & 0xff) * 257 * a);
  rc.green = (unsigned short)(((hex >> 8) & 0xff) * 257 * a);
  rc.blue = (unsigned short)((hex & 0xff) * 257 * a);
  rc.alpha = (unsigned short)(65535 * a);

  XftColor col;
  XftColorAllocValue(c->d, visual32, cmap32, &rc, &col);
  XftDrawStringUtf8(c->xd, &col, f, x, y, (const FcChar8 *)text, strlen(text));
  XftColorFree(c->d, visual32, cmap32, &col);
}

static void text_right(Canvas *c, XftFont *f, unsigned int hex, double a, int right, int y, const char *text) {
  text_left(c, f, hex, a, right - text_width(c->d, f, text), y, text);
}

static void text_center(Canvas *c, XftFont *f, unsigned int hex, double a, int cx, int y, const char *text) {
  text_left(c, f, hex, a, cx - text_width(c->d, f, text) / 2, y, text);
}

// draws text, cutting it with an ellipsis if it is wider than max_w
static void text_fit(Canvas *c, XftFont *f, unsigned int hex, double a, int x, int y, const char *text, int max_w) {
  char buf[160];
  snprintf(buf, sizeof(buf), "%s", text);

  if (text_width(c->d, f, buf) <= max_w) {
    text_left(c, f, hex, a, x, y, buf);
    return;
  }

  size_t len = strlen(buf);
  while (len > 0) {
    len--;
    while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80) len--; // stay on a UTF-8 boundary
    char tmp[170];
    snprintf(tmp, sizeof(tmp), "%.*s\xe2\x80\xa6", (int)len, buf);
    if (text_width(c->d, f, tmp) <= max_w) {
      text_left(c, f, hex, a, x, y, tmp);
      return;
    }
  }
}

// ================= ICONS (all drawn with Cairo, centred on cx, cy) =================

static void draw_icon(cairo_t *cr, int kind, double cx, double cy, double s, unsigned int hex, double a, int level) {
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_scale(cr, s, s);
  set_color(cr, hex, a);
  cairo_set_line_width(cr, 2.0);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_new_path(cr);

  switch (kind) {
  case ICON_WIFI:
    for (int i = 1; i <= 3; i++) {
      cairo_new_path(cr);
      cairo_arc(cr, 0, 5.5, i * 4.2, -3.0 * M_PI / 4.0, -M_PI / 4.0);
      cairo_stroke(cr);
    }
    cairo_new_path(cr);
    cairo_arc(cr, 0, 5.5, 1.5, 0, 2.0 * M_PI);
    cairo_fill(cr);
    break;

  case ICON_BLUETOOTH:
    cairo_move_to(cr, -4.2, -4.2);
    cairo_line_to(cr, 4.2, 4.2);
    cairo_line_to(cr, 0, 8);
    cairo_line_to(cr, 0, -8);
    cairo_line_to(cr, 4.2, -4.2);
    cairo_line_to(cr, -4.2, 4.2);
    cairo_stroke(cr);
    break;

  case ICON_MOON:
    // crescent = big disc with an offset disc cut out of it
    cairo_arc(cr, 3.6, -3.2, 6.4, 0, 2.0 * M_PI);
    cairo_rectangle(cr, -20, -20, 40, 40);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0, 7.5, 0, 2.0 * M_PI);
    cairo_fill(cr);
    break;

  case ICON_AIRPLANE: {
    static const double pts[][2] = {
      {0, -10}, {1.6, -6.2}, {1.6, -2.4}, {10, 2.8}, {10, 5.2}, {1.6, 2.6}, {1.6, 6.6}, {4.4, 8.8}, {4.4, 10.4},
      {0, 9.2}, {-4.4, 10.4}, {-4.4, 8.8}, {-1.6, 6.6}, {-1.6, 2.6}, {-10, 5.2}, {-10, 2.8}, {-1.6, -2.4}, {-1.6, -6.2}
    };
    cairo_rotate(cr, M_PI / 4.0);
    cairo_scale(cr, 0.85, 0.85);
    cairo_move_to(cr, pts[0][0], pts[0][1]);
    for (size_t i = 1; i < sizeof(pts) / sizeof(pts[0]); i++) cairo_line_to(cr, pts[i][0], pts[i][1]);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
    break;
  }

  case ICON_SPEAKER:
    cairo_translate(cr, -1.0, 0);
    cairo_move_to(cr, -7, -2.8);
    cairo_line_to(cr, -3.5, -2.8);
    cairo_line_to(cr, 0.5, -6.5);
    cairo_line_to(cr, 0.5, 6.5);
    cairo_line_to(cr, -3.5, 2.8);
    cairo_line_to(cr, -7, 2.8);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.8);
    if (level <= 0) {
      cairo_move_to(cr, 3.5, -3);
      cairo_line_to(cr, 9, 3);
      cairo_move_to(cr, 9, -3);
      cairo_line_to(cr, 3.5, 3);
      cairo_stroke(cr);
    } else {
      for (int i = 1; i <= (level < 50 ? 1 : 2); i++) {
        cairo_new_path(cr);
        cairo_arc(cr, 0.5, 0, 3.2 + i * 3.0, -M_PI / 4.0, M_PI / 4.0);
        cairo_stroke(cr);
      }
    }
    break;

  case ICON_MIC:
    rounded_path(cr, -2.8, -8, 5.6, 10.5, 2.8);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1.8);
    cairo_new_path(cr);
    cairo_arc(cr, 0, 0.5, 5.5, 0, M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 0, 6);
    cairo_line_to(cr, 0, 9);
    cairo_move_to(cr, -3.5, 9);
    cairo_line_to(cr, 3.5, 9);
    cairo_stroke(cr);
    break;

  case ICON_LOGOUT:
    cairo_move_to(cr, -1, -7);
    cairo_line_to(cr, -7, -7);
    cairo_line_to(cr, -7, 7);
    cairo_line_to(cr, -1, 7);
    cairo_stroke(cr);
    cairo_move_to(cr, -3.5, 0);
    cairo_line_to(cr, 7.5, 0);
    cairo_stroke(cr);
    cairo_move_to(cr, 3.5, -4);
    cairo_line_to(cr, 7.5, 0);
    cairo_line_to(cr, 3.5, 4);
    cairo_stroke(cr);
    break;

  case ICON_RESTART: {
    double r = 6.5, th = 1.5 * M_PI - 0.35;
    cairo_arc(cr, 0, 0, r, -M_PI / 2.0 + 0.55, th);
    cairo_stroke(cr);
    // arrow head at the end of the arc
    double ex = r * cos(th), ey = r * sin(th);
    double tx = -sin(th), ty = cos(th), nx = -ty, ny = tx;
    cairo_new_path(cr);
    cairo_move_to(cr, ex + tx * 4.0, ey + ty * 4.0);
    cairo_line_to(cr, ex + nx * 3.4, ey + ny * 3.4);
    cairo_line_to(cr, ex - nx * 3.4, ey - ny * 3.4);
    cairo_close_path(cr);
    cairo_fill(cr);
    break;
  }

  case ICON_POWER:
    cairo_arc(cr, 0, 1, 6.8, -M_PI / 2.0 + 0.65, 1.5 * M_PI - 0.65);
    cairo_stroke(cr);
    cairo_move_to(cr, 0, -8);
    cairo_line_to(cr, 0, -0.5);
    cairo_stroke(cr);
    break;

  case ICON_CHEV_RIGHT:
    cairo_set_line_width(cr, 1.8);
    cairo_move_to(cr, -2.5, -5);
    cairo_line_to(cr, 2.5, 0);
    cairo_line_to(cr, -2.5, 5);
    cairo_stroke(cr);
    break;

  case ICON_CHEV_LEFT:
    cairo_set_line_width(cr, 1.8);
    cairo_move_to(cr, 2.5, -5);
    cairo_line_to(cr, -2.5, 0);
    cairo_line_to(cr, 2.5, 5);
    cairo_stroke(cr);
    break;
  }

  cairo_restore(cr);
}

// ================= SYSTEM HARDWARE LOGIC =================

void get_wifi_name(char *out_name, int max_len, int *is_active) {
  FILE *fp_radio = popen("timeout 1s nmcli radio wifi 2>/dev/null", "r");
  char radio_status[16] = {0};
  if (fp_radio) {
    if (fgets(radio_status, sizeof(radio_status), fp_radio) != NULL) {
      radio_status[strcspn(radio_status, "\r\n")] = 0;
    }
    pclose(fp_radio);
  }

  if (strstr(radio_status, "disabled") != NULL) {
    snprintf(out_name, max_len, "Off");
    *is_active = 0;
    return;
  }

  FILE *fp = popen("timeout 1s nmcli -t -f GENERAL.CONNECTION dev show 2>/dev/null | head -n 1 | cut -d':' -f2", "r");
  if (fp) {
    if (fgets(out_name, max_len, fp) != NULL) {
      out_name[strcspn(out_name, "\r\n")] = 0;
      if (strlen(out_name) > 0 && strcmp(out_name, "--") != 0) {
        *is_active = 1;
        pclose(fp);
        return;
      }
    }
    pclose(fp);
  }
  snprintf(out_name, max_len, "Disconnected");
  *is_active = 1;
}

void toggle_wifi(int current_state) {
  if (current_state) {
    system("timeout 1s nmcli radio wifi off 2>/dev/null &");
  } else {
    system("timeout 1s nmcli radio wifi on 2>/dev/null &");
  }
}

void get_bluetooth_name(char *out_name, int max_len, int *is_active) {
  FILE *fp = popen("timeout 0.2s bluetoothctl info 2>/dev/null | grep 'Name:' | cut -d' ' -f2-", "r");
  if (fp) {
    if (fgets(out_name, max_len, fp) != NULL) {
      out_name[strcspn(out_name, "\r\n")] = 0;
      if (strlen(out_name) > 0) {
        *is_active = 1;
        pclose(fp);
        return;
      }
    }
    pclose(fp);
  }
  snprintf(out_name, max_len, "Not connected");
  *is_active = 0;
}

void toggle_bluetooth(int current_state) {
  if (current_state) {
    system("timeout 1s bluetoothctl power off 2>/dev/null &");
  } else {
    system("timeout 1s bluetoothctl power on 2>/dev/null &");
  }
}

void get_dnd_status(char *out_name, int max_len, int *is_active) {
  FILE *fp = popen("timeout 1s dunstctl is-paused 2>/dev/null", "r");
  if (fp) {
    char buf[16] = {0};
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      if (strstr(buf, "true") != NULL) {
        snprintf(out_name, max_len, "On");
        *is_active = 1;
        pclose(fp);
        return;
      }
    }
    pclose(fp);
  }
  snprintf(out_name, max_len, "Off");
  *is_active = 0;
}

void toggle_dnd() { system("dunstctl set-paused toggle 2>/dev/null"); }

void get_airplane_status(char *out_name, int max_len, int *is_active) {
  FILE *fp = popen("timeout 1s rfkill list 2>/dev/null | grep -i 'Soft blocked: no'", "r");
  if (fp) {
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), fp) == NULL) {
      snprintf(out_name, max_len, "On");
      *is_active = 1;
      pclose(fp);
      return;
    }
    pclose(fp);
  }
  snprintf(out_name, max_len, "Off");
  *is_active = 0;
}

void toggle_airplane(int current_state) {
  if (current_state) {
    system("timeout 1s rfkill unblock all 2>/dev/null &");
    system("timeout 1s nmcli radio all on 2>/dev/null &");
  } else {
    system("timeout 1s rfkill block all 2>/dev/null &");
  }
}

int get_system_volume() {
  int vol = 75;
  FILE *fp = popen("timeout 0.2s pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      char *pct = strchr(buf, '%');
      if (pct) {
        char *start = pct;
        while (start > buf && *(start - 1) >= '0' && *(start - 1) <= '9') start--;
        sscanf(start, "%d", &vol);
      }
    }
    pclose(fp);
  }
  return (vol < 0) ? 0 : (vol > 100) ? 100 : vol;
}

int get_system_mic() {
  int mic = 60;
  FILE *fp = popen("timeout 0.2s pactl get-source-volume @DEFAULT_SOURCE@ 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      char *pct = strchr(buf, '%');
      if (pct) {
        char *start = pct;
        while (start > buf && *(start - 1) >= '0' && *(start - 1) <= '9') start--;
        sscanf(start, "%d", &mic);
      }
    }
    pclose(fp);
  }
  return (mic < 0) ? 0 : (mic > 100) ? 100 : mic;
}

void set_system_volume(int val) {
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null &", val);
  system(cmd);
}

void set_system_mic(int val) {
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "pactl set-source-volume @DEFAULT_SOURCE@ %d%% 2>/dev/null &", val);
  system(cmd);
}

void update_device_statuses() {
  get_wifi_name(tiles[0].status, sizeof(tiles[0].status), &tiles[0].active);
  get_bluetooth_name(tiles[1].status, sizeof(tiles[1].status), &tiles[1].active);
  get_dnd_status(tiles[2].status, sizeof(tiles[2].status), &tiles[2].active);
  get_airplane_status(tiles[3].status, sizeof(tiles[3].status), &tiles[3].active);
}

void fetch_wifi_networks() {
  sub_item_count = 0;
  system("timeout 2s nmcli device wifi rescan 2>/dev/null &");
  FILE *fp = popen("timeout 2s nmcli -t -f ACTIVE,SSID dev wifi 2>/dev/null", "r");
  if (fp) {
    char buf[128];
    while (fgets(buf, sizeof(buf), fp) && sub_item_count < 6) {
      buf[strcspn(buf, "\r\n")] = 0;
      char *active = strtok(buf, ":");
      char *ssid = strtok(NULL, ":");
      if (ssid && strlen(ssid) > 0 && strcmp(ssid, "--") != 0) {
        snprintf(sub_items[sub_item_count].name, sizeof(sub_items[sub_item_count].name), "%s", ssid);
        snprintf(sub_items[sub_item_count].info, sizeof(sub_items[sub_item_count].info), "%s", (active && strcmp(active, "yes") == 0) ? "Connected" : "Available");
        sub_item_count++;
      }
    }
    pclose(fp);
  }
}

void fetch_bluetooth_devices() {
  sub_item_count = 0;
  FILE *fp = popen("timeout 1s bluetoothctl devices 2>/dev/null", "r");
  if (fp) {
    char buf[128];
    while (fgets(buf, sizeof(buf), fp) && sub_item_count < 6) {
      buf[strcspn(buf, "\r\n")] = 0;
      if (strncmp(buf, "Device ", 7) == 0) {
        char *mac = strtok(buf + 7, " ");
        char *name = strtok(NULL, "");
        if (name) {
          snprintf(sub_items[sub_item_count].name, sizeof(sub_items[sub_item_count].name), "%s", name);
          snprintf(sub_items[sub_item_count].info, sizeof(sub_items[sub_item_count].info), "%s", mac);
          sub_item_count++;
        }
      }
    }
    pclose(fp);
  }
}

// ================= HIT TESTING =================

static int in_rect(int mx, int my, int x, int y, int w, int h) {
  return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

static int slider_from_x(int mx) {
  int v = ((mx - SL_X) * 100) / SL_W;
  return (v < 0) ? 0 : (v > 100) ? 100 : v;
}

int hit_test(int mx, int my) {
  if (current_view == VIEW_MAIN) {
    for (int i = 0; i < 4; i++) {
      if (in_rect(mx, my, tiles[i].x, tiles[i].y, tiles[i].w, tiles[i].h)) {
        if (tiles[i].has_arrow && mx >= tiles[i].x + tiles[i].w - 30) return HIT_ARROW0 + i;
        return i;
      }
    }
    if (in_rect(mx, my, 15, 245, 350, 30)) return HIT_VOL;
    if (in_rect(mx, my, 15, 295, 350, 30)) return HIT_MIC;
    for (int b = 0; b < 3; b++) {
      if (in_rect(mx, my, 15 + b * 120, 338, 110, 42)) return HIT_LOGOUT + b;
    }
  } else {
    if (in_rect(mx, my, 15, 50, 80, 30)) return HIT_BACK;
    for (int i = 0; i < sub_item_count; i++) {
      if (in_rect(mx, my, 15, 90 + i * 45, 350, 40)) return HIT_ITEM0 + i;
    }
  }
  return HIT_NONE;
}

// ================= RENDER ENGINE =================

static void draw_background(cairo_t *cr) {
  // start fully transparent, then paint a rounded glass panel
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  fill_round_grad(cr, 0, 0, MAIN_WIDTH, MAIN_HEIGHT, WIN_RADIUS, 0, COL_BG_TOP, 0.92, COL_BG_BOTTOM, 0.95);

  // soft highlight at the top edge
  cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, 150);
  add_stop(p, 0.0, 0xffffff, 0.07);
  add_stop(p, 1.0, 0xffffff, 0.0);
  rounded_path(cr, 0, 0, MAIN_WIDTH, MAIN_HEIGHT, WIN_RADIUS);
  cairo_set_source(cr, p);
  cairo_fill(cr);
  cairo_pattern_destroy(p);

  stroke_round(cr, 0, 0, MAIN_WIDTH, MAIN_HEIGHT, WIN_RADIUS, 0xffffff, 0.12);
}

static void draw_tile(Canvas *c, int i) {
  cairo_t *cr = c->cr;
  Tile *t = &tiles[i];
  double x = t->x, y = t->y, w = t->w, h = t->h;
  int hov = (hover_id == i) || (hover_id == HIT_ARROW0 + i);
  int arrow_hov = (hover_id == HIT_ARROW0 + i);

  if (t->active) {
    fill_round_grad(cr, x, y, w, h, 16, 0, t->accent, 0.38, t->accent, 0.16);
    stroke_round(cr, x, y, w, h, 16, t->accent, 0.55);
  } else {
    fill_round(cr, x, y, w, h, 16, 0xffffff, 0.06);
    stroke_round(cr, x, y, w, h, 16, 0xffffff, 0.08);
  }
  if (hov) fill_round(cr, x, y, w, h, 16, 0xffffff, 0.06);

  // icon badge
  double bcx = x + 29, bcy = y + h / 2.0;
  fill_circle(cr, bcx, bcy, 17, t->active ? t->accent : 0xffffff, t->active ? 1.0 : 0.10);
  draw_icon(cr, i, bcx, bcy, 1.0, 0xffffff, t->active ? 1.0 : 0.65, 0);

  int tx = (int)x + 54;
  int max_w = (int)w - 54 - (t->has_arrow ? 34 : 12);
  unsigned int sub_col = t->active ? mix_white(t->accent, 0.65) : COL_TEXT_DIM;
  text_fit(c, c->bold, COL_TEXT, 1.0, tx, (int)y + 29, t->label, max_w);
  text_fit(c, c->regular, sub_col, 1.0, tx, (int)y + 47, t->status, max_w);

  if (t->has_arrow) {
    cairo_new_path(cr);
    cairo_move_to(cr, x + w - 30 + 0.5, y + 14);
    cairo_line_to(cr, x + w - 30 + 0.5, y + h - 14);
    set_color(cr, 0xffffff, 0.12);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    draw_icon(cr, ICON_CHEV_RIGHT, x + w - 15, y + h / 2.0, 1.0, 0xffffff, arrow_hov ? 1.0 : 0.6, 0);
  }
}

static void draw_slider(Canvas *c, int base_y, const char *label, int val, unsigned int col_a, unsigned int col_b,
                        int icon, int highlighted) {
  cairo_t *cr = c->cr;

  draw_icon(cr, icon, 23, base_y - 4, 1.0, 0xcbd5e1, 1.0, val);
  text_left(c, c->bold, COL_TEXT_SOFT, 1.0, 40, base_y, label);

  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", val);
  text_right(c, c->regular, COL_TEXT_DIM, 1.0, 365, base_y, pct);

  double ty = base_y + 14, th = 6;
  double kx = SL_X + SL_W * val / 100.0, ky = ty + th / 2.0;

  fill_round(cr, SL_X, ty, SL_W, th, 3, 0xffffff, 0.12);
  fill_round_grad(cr, SL_X, ty, kx - SL_X, th, 3, 1, col_a, 0.95, col_b, 1.0);

  fill_circle(cr, kx, ky + 1.5, 9.5, 0x000000, 0.30); // shadow
  if (highlighted) fill_circle(cr, kx, ky, 13, col_b, 0.28); // halo
  fill_circle(cr, kx, ky, 8, 0xffffff, 1.0);
  fill_circle(cr, kx, ky, 3, col_b, 1.0);
}

static void draw_footer_button(Canvas *c, int idx) {
  static const char *labels[3] = {"Logout", "Restart", "Power Off"};
  static const int icons[3] = {ICON_LOGOUT, ICON_RESTART, ICON_POWER};

  cairo_t *cr = c->cr;
  int bx = 15 + idx * 120, by = 338, bw = 110, bh = 42;
  int hov = (hover_id == HIT_LOGOUT + idx);
  int danger = (idx == 2);

  if (danger) {
    fill_round(cr, bx, by, bw, bh, 12, COL_RED, hov ? 0.32 : 0.16);
    stroke_round(cr, bx, by, bw, bh, 12, COL_RED, 0.40);
  } else {
    fill_round(cr, bx, by, bw, bh, 12, 0xffffff, hov ? 0.14 : 0.07);
    stroke_round(cr, bx, by, bw, bh, 12, 0xffffff, 0.09);
  }

  unsigned int fg = danger ? 0xfca5a5 : COL_TEXT_SOFT;
  int tw = text_width(c->d, c->bold, labels[idx]);
  int total = 16 + 7 + tw;
  int sx = bx + (bw - total) / 2;
  double cy = by + bh / 2.0;

  draw_icon(cr, icons[idx], sx + 8, cy, 1.0, fg, 1.0, 0);
  text_left(c, c->bold, fg, 1.0, sx + 23, baseline_center(c->bold, cy), labels[idx]);
}

static void draw_main_view(Canvas *c) {
  text_left(c, c->title, COL_TEXT, 1.0, 20, 34, "System Control");

  for (int i = 0; i < 4; i++) draw_tile(c, i);

  draw_slider(c, 240, "Volume", volume_val, COL_BLUE, 0x60a5fa, ICON_SPEAKER, is_dragging_vol || hover_id == HIT_VOL);
  draw_slider(c, 292, "Microphone", mic_val, COL_GREEN, 0x34d399, ICON_MIC, is_dragging_mic || hover_id == HIT_MIC);

  for (int b = 0; b < 3; b++) draw_footer_button(c, b);
}

static void draw_sub_view(Canvas *c) {
  cairo_t *cr = c->cr;
  int wifi = (current_view == VIEW_WIFI);
  unsigned int accent = wifi ? COL_BLUE : COL_INDIGO;

  text_left(c, c->title, COL_TEXT, 1.0, 20, 34, wifi ? "Wi-Fi Networks" : "Bluetooth Devices");

  // back button
  int back_hov = (hover_id == HIT_BACK);
  fill_round(cr, 15, 50, 80, 30, 15, 0xffffff, back_hov ? 0.14 : 0.08);
  stroke_round(cr, 15, 50, 80, 30, 15, 0xffffff, 0.10);
  draw_icon(cr, ICON_CHEV_LEFT, 32, 65, 1.0, 0xffffff, 0.9, 0);
  text_left(c, c->bold, COL_TEXT, 1.0, 44, baseline_center(c->bold, 65), "Back");

  // list
  int start_y = 90;
  for (int i = 0; i < sub_item_count; i++) {
    int ry = start_y + i * 45;
    double cy = ry + 20;
    int connected = wifi && strcmp(sub_items[i].info, "Connected") == 0;

    if (connected) {
      fill_round_grad(cr, 15, ry, 350, 40, 12, 0, accent, 0.32, accent, 0.12);
      stroke_round(cr, 15, ry, 350, 40, 12, accent, 0.50);
    } else {
      fill_round(cr, 15, ry, 350, 40, 12, 0xffffff, 0.06);
      stroke_round(cr, 15, ry, 350, 40, 12, 0xffffff, 0.07);
    }
    if (hover_id == HIT_ITEM0 + i) fill_round(cr, 15, ry, 350, 40, 12, 0xffffff, 0.06);

    fill_circle(cr, 38, cy, 12, connected ? accent : 0xffffff, connected ? 1.0 : 0.10);
    draw_icon(cr, wifi ? ICON_WIFI : ICON_BLUETOOTH, 38, cy, 0.7, 0xffffff, connected ? 1.0 : 0.7, 0);

    int info_w;
    if (connected) {
      info_w = text_width(c->d, c->regular, "Connected") + 18;
      fill_round(cr, 355 - info_w, ry + 10, info_w, 20, 10, COL_GREEN, 0.22);
      text_left(c, c->regular, 0x6ee7b7, 1.0, 355 - info_w + 9, baseline_center(c->regular, cy), "Connected");
    } else {
      info_w = text_width(c->d, c->regular, sub_items[i].info);
      text_right(c, c->regular, COL_TEXT_DIM, 1.0, 355, baseline_center(c->regular, cy), sub_items[i].info);
    }

    text_fit(c, c->bold, COL_TEXT, 1.0, 60, baseline_center(c->bold, cy), sub_items[i].name, 355 - info_w - 70);
  }

  if (sub_item_count == 0) {
    draw_icon(cr, wifi ? ICON_WIFI : ICON_BLUETOOTH, MAIN_WIDTH / 2.0, 185, 1.8, 0xffffff, 0.22, 0);
    text_center(c, c->regular, COL_TEXT_DIM, 1.0, MAIN_WIDTH / 2, 240, wifi ? "No networks found / Wi-Fi unavailable" : "No paired devices / Bluetooth unavailable");
  }
}

void draw_ui(UI *ui) {
  Display *d = ui->d;
  cairo_surface_t *surface = cairo_xlib_surface_create(d, ui->pix, visual32, MAIN_WIDTH, MAIN_HEIGHT);
  cairo_t *cr = cairo_create(surface);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

  Canvas cv;
  cv.d = d;
  cv.cr = cr;
  cv.xd = XftDrawCreate(d, ui->pix, visual32, cmap32);
  cv.title = ui->title;
  cv.bold = ui->bold;
  cv.regular = ui->regular;

  draw_background(cr);
  if (current_view == VIEW_MAIN) {
    draw_main_view(&cv);
  } else {
    draw_sub_view(&cv);
  }

  XftDrawDestroy(cv.xd);
  cairo_surface_flush(surface);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  XCopyArea(d, ui->pix, ui->w, ui->gc, 0, 0, MAIN_WIDTH, MAIN_HEIGHT, 0, 0);
  XFlush(d);
}

// ================= MAIN EVENT LOOP =================

int main() {
  Display *d = XOpenDisplay(NULL);
  if (!d) {
    fprintf(stderr, "Error: Cannot open X Display\n");
    return 1;
  }

  int s = DefaultScreen(d);
  Window root = RootWindow(d, s);

  setup_32bit_visual(d, s);

  UI ui;
  memset(&ui, 0, sizeof(ui));
  ui.d = d;
  // Inter if installed, otherwise any Sans. rgba=none avoids colour fringes on the translucent background.
  ui.title = XftFontOpenName(d, s, "Inter,Sans:pixelsize=16:weight=bold:rgba=none");
  ui.bold = XftFontOpenName(d, s, "Inter,Sans:pixelsize=13:weight=bold:rgba=none");
  ui.regular = XftFontOpenName(d, s, "Inter,Sans:pixelsize=11:rgba=none");

  volume_val = get_system_volume();
  mic_val = get_system_mic();
  update_device_statuses();

  int pos_x = DisplayWidth(d, s) - MAIN_WIDTH - 10;
  int pos_y = DisplayHeight(d, s) - MAIN_HEIGHT - 60;

  XSetWindowAttributes attr;
  attr.override_redirect = True;
  attr.colormap = cmap32;
  attr.background_pixel = 0;
  attr.border_pixel = 0;

  Window w = XCreateWindow(d, root, pos_x, pos_y, MAIN_WIDTH, MAIN_HEIGHT, 0,
                           depth32, InputOutput, visual32,
                           CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel, &attr);

  XSelectInput(d, w, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask);
  XMapWindow(d, w);

  XGrabKeyboard(d, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  XGrabPointer(d, w, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

  ui.w = w;
  ui.gc = XCreateGC(d, w, 0, NULL);
  ui.pix = XCreatePixmap(d, w, MAIN_WIDTH, MAIN_HEIGHT, depth32);

  XEvent e;
  while (1) {
    XNextEvent(d, &e);

    if (e.type == Expose) {
      draw_ui(&ui);
    }
    if (e.type == KeyPress) {
      break;
    }
    if (e.type == ButtonPress) {
      if (e.xbutton.button == 4 || e.xbutton.button == 5) continue;

      int mx = e.xbutton.x, my = e.xbutton.y;
      int id = hit_test(mx, my);
      if (id == HIT_NONE) break; // click outside any control closes the panel

      if (current_view == VIEW_MAIN) {
        if (id >= 0 && id < 4) {
          if (id == 0) toggle_wifi(tiles[0].active);
          else if (id == 1) toggle_bluetooth(tiles[1].active);
          else if (id == 2) toggle_dnd();
          else if (id == 3) toggle_airplane(tiles[3].active);

          update_device_statuses();
          draw_ui(&ui);
        } else if (id == HIT_ARROW0 || id == HIT_ARROW0 + 1) {
          if (id == HIT_ARROW0) {
            current_view = VIEW_WIFI;
            fetch_wifi_networks();
          } else {
            current_view = VIEW_BLUETOOTH;
            fetch_bluetooth_devices();
          }
          hover_id = HIT_NONE;
          draw_ui(&ui);
        } else if (id == HIT_VOL) {
          is_dragging_vol = 1;
          volume_val = slider_from_x(mx);
          set_system_volume(volume_val);
          draw_ui(&ui);
        } else if (id == HIT_MIC) {
          is_dragging_mic = 1;
          mic_val = slider_from_x(mx);
          set_system_mic(mic_val);
          draw_ui(&ui);
        } else if (id == HIT_LOGOUT) {
          system("pkill .");
          break;
        } else if (id == HIT_RESTART) {
          system("systemctl reboot || reboot");
          break;
        } else if (id == HIT_POWEROFF) {
          system("systemctl poweroff || poweroff");
          break;
        }
      } else {
        // Sub-view interaction
        if (id == HIT_BACK) {
          current_view = VIEW_MAIN;
          hover_id = hit_test(mx, my);
          draw_ui(&ui);
        } else if (id >= HIT_ITEM0) {
          int i = id - HIT_ITEM0;
          char cmd[128];
          if (current_view == VIEW_WIFI) {
            snprintf(cmd, sizeof(cmd), "nmcli device wifi connect \"%s\" &", sub_items[i].name);
            system(cmd);
          }
          current_view = VIEW_MAIN;
          hover_id = hit_test(mx, my);
          update_device_statuses();
          draw_ui(&ui);
        }
      }
    }

    if (e.type == MotionNotify) {
      int mx = e.xmotion.x, my = e.xmotion.y;

      if (is_dragging_vol) {
        int new_val = slider_from_x(mx);
        if (new_val != volume_val) {
          volume_val = new_val;
          set_system_volume(volume_val);
          draw_ui(&ui);
        }
      } else if (is_dragging_mic) {
        int new_val = slider_from_x(mx);
        if (new_val != mic_val) {
          mic_val = new_val;
          set_system_mic(mic_val);
          draw_ui(&ui);
        }
      } else {
        int id = hit_test(mx, my);
        if (id != hover_id) {
          hover_id = id;
          draw_ui(&ui);
        }
      }
    }

    if (e.type == ButtonRelease) {
      if (is_dragging_vol || is_dragging_mic) {
        is_dragging_vol = 0;
        is_dragging_mic = 0;
        draw_ui(&ui);
      }
    }
  }

  XUngrabPointer(d, CurrentTime);
  XUngrabKeyboard(d, CurrentTime);

  XFreePixmap(d, ui.pix);
  XFreeGC(d, ui.gc);
  XftFontClose(d, ui.title);
  XftFontClose(d, ui.bold);
  XftFontClose(d, ui.regular);
  XCloseDisplay(d);
  return 0;
}
