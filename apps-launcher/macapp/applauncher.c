/*
 * applauncher.c - glass panel application launcher (X11 / xcb / cairo)
 *
 * Layout (matches the reference screenshot):
 *   - small translucent rounded panel, opens at the bottom-left (not fullscreen)
 *   - header: "A" glyph | search field ("Applications") | "..." button
 *   - horizontally scrolling category pills
 *   - 5 x 4 icon grid, selected icon gets a rounded highlight box,
 *     labels wrap on two lines
 *
 * Build:
 *   gcc -O2 -o applauncher applauncher.c \
 *     $(pkg-config --cflags --libs gtk+-3.0 cairo cairo-xcb xcb xcb-keysyms) -lm
 *
 * Keys:
 *   type            search (switches to "All")
 *   arrows          move selection
 *   Tab/Shift+Tab   next/previous category
 *   PgUp/PgDn       scroll a page
 *   Enter           launch      Esc  close
 * Mouse:
 *   click tile = launch, click pill = category, click outside = close,
 *   wheel = scroll grid (wheel over the pills = change category),
 *   right-click + drag on the pills = scroll them freely
 */
#define _GNU_SOURCE
#include <X11/keysym.h>
#include <cairo-xcb.h>
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#define MAX_APPS 1024
#define NAME_LEN 128
#define EXEC_LEN 512
#define ICON_LEN 128
#define SEARCH_LEN 256

/* ---------------- panel geometry (panel-local pixels) ---------------- */
#define PANEL_W 570
#define PANEL_H 540
#define PANEL_R 10

/* where the window sits on screen (bottom-left) */
#define LAUNCH_MARGIN_X 0
#define LAUNCH_MARGIN_BOTTOM 0 /* leave room for a taskbar/dock */

#define HEADER_CY 38 /* vertical centre of the header row */

#define TAB_Y 66
#define TAB_H 24
#define TAB_GAP 8

#define GRID_Y 112
#define COLS 5
#define VROWS 4
#define COL_W 108
#define ROW_H 104
#define GRID_PAD ((PANEL_W - COLS * COL_W) / 2)

#define BOX 70  /* highlight box around the icon */
#define ICON 48 /* icon size */

/* ---------------- categories ---------------- */
enum {
  CAT_ALL = 0,
  CAT_DEV,
  CAT_EDU,
  CAT_GAMES,
  CAT_GRAPHICS,
  CAT_HELP,
  CAT_INTERNET,
  CAT_MULTIMEDIA,
  CAT_OFFICE,
  CAT_SYSTEM,
  CAT_UTILITIES,
  CAT_COUNT
};

static const char *CAT_NAMES[CAT_COUNT] = {
    "All",      "Development", "Education",  "Games",  "Graphics", "Help",
    "Internet", "Multimedia",  "Office",     "System", "Utilities"};

typedef struct {
  const char *key;
  int cat;
} CatMap;

static const CatMap CAT_MAP[] = {
    {"Development", CAT_DEV},
    {"IDE", CAT_DEV},
    {"Debugger", CAT_DEV},
    {"RevisionControl", CAT_DEV},
    {"WebDevelopment", CAT_DEV},
    {"GUIDesigner", CAT_DEV},
    {"Building", CAT_DEV},
    {"Profiling", CAT_DEV},
    {"Translation", CAT_DEV},

    {"Education", CAT_EDU},
    {"Science", CAT_EDU},
    {"Math", CAT_EDU},
    {"Astronomy", CAT_EDU},
    {"Chemistry", CAT_EDU},
    {"Physics", CAT_EDU},
    {"Biology", CAT_EDU},
    {"Languages", CAT_EDU},
    {"Geography", CAT_EDU},
    {"ArtificialIntelligence", CAT_EDU},

    {"Game", CAT_GAMES},
    {"ArcadeGame", CAT_GAMES},
    {"BoardGame", CAT_GAMES},
    {"CardGame", CAT_GAMES},
    {"StrategyGame", CAT_GAMES},
    {"LogicGame", CAT_GAMES},
    {"RolePlaying", CAT_GAMES},
    {"Simulation", CAT_GAMES},
    {"SportsGame", CAT_GAMES},
    {"ActionGame", CAT_GAMES},
    {"AdventureGame", CAT_GAMES},
    {"BlocksGame", CAT_GAMES},
    {"KidsGame", CAT_GAMES},

    {"Graphics", CAT_GRAPHICS},
    {"2DGraphics", CAT_GRAPHICS},
    {"3DGraphics", CAT_GRAPHICS},
    {"RasterGraphics", CAT_GRAPHICS},
    {"VectorGraphics", CAT_GRAPHICS},
    {"Photography", CAT_GRAPHICS},
    {"Scanning", CAT_GRAPHICS},
    {"OCR", CAT_GRAPHICS},

    {"Documentation", CAT_HELP},
    {"Help", CAT_HELP},

    {"Network", CAT_INTERNET},
    {"WebBrowser", CAT_INTERNET},
    {"Email", CAT_INTERNET},
    {"Chat", CAT_INTERNET},
    {"InstantMessaging", CAT_INTERNET},
    {"IRCClient", CAT_INTERNET},
    {"FileTransfer", CAT_INTERNET},
    {"P2P", CAT_INTERNET},
    {"News", CAT_INTERNET},
    {"RemoteAccess", CAT_INTERNET},
    {"Telephony", CAT_INTERNET},
    {"VideoConference", CAT_INTERNET},
    {"Feed", CAT_INTERNET},

    {"AudioVideo", CAT_MULTIMEDIA},
    {"Audio", CAT_MULTIMEDIA},
    {"Video", CAT_MULTIMEDIA},
    {"Player", CAT_MULTIMEDIA},
    {"Recorder", CAT_MULTIMEDIA},
    {"Music", CAT_MULTIMEDIA},
    {"Midi", CAT_MULTIMEDIA},
    {"Mixer", CAT_MULTIMEDIA},
    {"Sequencer", CAT_MULTIMEDIA},
    {"Tuner", CAT_MULTIMEDIA},
    {"TV", CAT_MULTIMEDIA},
    {"DiscBurning", CAT_MULTIMEDIA},
    {"AudioVideoEditing", CAT_MULTIMEDIA},

    {"Office", CAT_OFFICE},
    {"WordProcessor", CAT_OFFICE},
    {"Spreadsheet", CAT_OFFICE},
    {"Presentation", CAT_OFFICE},
    {"Calendar", CAT_OFFICE},
    {"ContactManagement", CAT_OFFICE},
    {"Database", CAT_OFFICE},
    {"Dictionary", CAT_OFFICE},
    {"Finance", CAT_OFFICE},
    {"Publishing", CAT_OFFICE},
    {"ProjectManagement", CAT_OFFICE},

    {"System", CAT_SYSTEM},
    {"Settings", CAT_SYSTEM},
    {"DesktopSettings", CAT_SYSTEM},
    {"HardwareSettings", CAT_SYSTEM},
    {"PackageManager", CAT_SYSTEM},
    {"Monitor", CAT_SYSTEM},
    {"Security", CAT_SYSTEM},
    {"Emulator", CAT_SYSTEM},
    {"FileManager", CAT_SYSTEM},
    {"FileSystem", CAT_SYSTEM},
    {"TerminalEmulator", CAT_SYSTEM},

    {"Utility", CAT_UTILITIES},
    {"Accessibility", CAT_UTILITIES},
    {"Archiving", CAT_UTILITIES},
    {"Compression", CAT_UTILITIES},
    {"Calculator", CAT_UTILITIES},
    {"Clock", CAT_UTILITIES},
    {"TextEditor", CAT_UTILITIES},
    {"Maps", CAT_UTILITIES},
    {"TextTools", CAT_UTILITIES},
    {"FileTools", CAT_UTILITIES},
};

/* ---------------- data ---------------- */
typedef struct {
  char name[NAME_LEN];
  char exec[EXEC_LEN];
  char icon[ICON_LEN];
  int terminal;
  unsigned cats; /* bitmask of CAT_* */
  GdkPixbuf *pixbuf;
  int icon_loaded;
} AppEntry;

static AppEntry g_apps[MAX_APPS];
static int g_app_count = 0;

static int g_filtered[MAX_APPS];
static int g_filtered_count = 0;

static char g_search[SEARCH_LEN] = {0};
static int g_search_len = 0;
static int g_selected = 0;   /* index into g_filtered[] */
static int g_scroll_row = 0; /* topmost visible row */
static int g_cat = CAT_ALL;

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_window_t win;
static cairo_surface_t *surface;
static cairo_t *cr;
static int g_have_alpha = 0;

static int g_px = 0, g_py = 0, g_pw = PANEL_W, g_ph = PANEL_H; /* window-local */
static int g_wx, g_wy;
static int g_dirty = 0; /* redraw needed after current event batch */

/* tab hit-test data, filled in by render() (panel-local x) */
static double g_tab_x[CAT_COUNT], g_tab_w[CAT_COUNT];
static double g_tab_off = 0;
static double g_tab_max = 0;  /* max scroll offset of the pills */
static int g_tab_follow = 1;  /* auto-scroll to keep selected pill visible */
static int g_drag = 0;        /* right-button drag on the pills */
static int g_drag_x = 0;

/* ---------------------------------------------------------------- */
/* Icons                                                             */
/* ---------------------------------------------------------------- */

static GdkPixbuf *load_theme_icon(const char *icon_name, int size) {
  if (!icon_name || !icon_name[0])
    return NULL;

  GdkScreen *gscreen = gdk_screen_get_default();
  if (!gscreen)
    return NULL;

  GtkIconTheme *theme = gtk_icon_theme_get_for_screen(gscreen);
  if (!theme)
    return NULL;

  GError *error = NULL;
  GdkPixbuf *pixbuf = NULL;

  if (icon_name[0] == '/') {
    pixbuf =
        gdk_pixbuf_new_from_file_at_scale(icon_name, size, size, TRUE, &error);
  } else {
    pixbuf = gtk_icon_theme_load_icon(theme, icon_name, size,
                                      GTK_ICON_LOOKUP_FORCE_SIZE, &error);
  }
  if (error)
    g_error_free(error);

  if (!pixbuf) {
    error = NULL;
    pixbuf = gtk_icon_theme_load_icon(theme, "application-x-executable", size,
                                      GTK_ICON_LOOKUP_FORCE_SIZE, &error);
    if (error)
      g_error_free(error);
  }
  return pixbuf;
}

static void ensure_app_icon(AppEntry *app) {
  if (!app || app->icon_loaded)
    return;
  app->pixbuf = load_theme_icon(app->icon, ICON);
  app->icon_loaded = 1;
}

static void free_app_icons(void) {
  for (int i = 0; i < g_app_count; i++) {
    if (g_apps[i].pixbuf) {
      g_object_unref(g_apps[i].pixbuf);
      g_apps[i].pixbuf = NULL;
    }
  }
}

/* ---------------------------------------------------------------- */
/* .desktop parsing                                                  */
/* ---------------------------------------------------------------- */

static void trim_newline(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                   s[n - 1] == '\t')) {
    s[--n] = '\0';
  }
}

static void clean_exec(const char *raw, char *out, size_t out_size) {
  size_t oi = 0;
  for (size_t i = 0; raw[i] != '\0' && oi + 1 < out_size; i++) {
    if (raw[i] == '%' && raw[i + 1] != '\0') {
      char c = raw[i + 1];
      if (c == '%') {
        out[oi++] = '%';
        i++;
        continue;
      }
      if (strchr("fFuUick", c)) {
        i++;
        continue;
      }
    }
    out[oi++] = raw[i];
  }
  out[oi] = '\0';
  trim_newline(out);
}

static int app_exists(const char *name) {
  for (int i = 0; i < g_app_count; i++) {
    if (strcmp(g_apps[i].name, name) == 0)
      return 1;
  }
  return 0;
}

static unsigned parse_categories(const char *raw) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", raw);
  unsigned mask = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buf, ";", &save); tok;
       tok = strtok_r(NULL, ";", &save)) {
    for (size_t i = 0; i < sizeof(CAT_MAP) / sizeof(CAT_MAP[0]); i++) {
      if (strcmp(tok, CAT_MAP[i].key) == 0) {
        mask |= 1u << CAT_MAP[i].cat;
        break;
      }
    }
  }
  return mask;
}

static int parse_desktop_file(const char *path, AppEntry *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;

  char line[1024];
  char name[NAME_LEN] = {0};
  char exec_raw[EXEC_LEN] = {0};
  char icon[ICON_LEN] = {0};
  char type_buf[64] = {0};
  char cats_raw[512] = {0};
  int terminal = 0, no_display = 0, hidden = 0;
  int in_entry = 0;

  while (fgets(line, sizeof(line), f)) {
    trim_newline(line);
    if (line[0] == '\0' || line[0] == '#')
      continue;

    if (line[0] == '[') {
      in_entry = (strcmp(line, "[Desktop Entry]") == 0);
      continue;
    }
    if (!in_entry)
      continue;

    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = line;
    char *val = eq + 1;

    if (strcmp(key, "Name") == 0 && name[0] == '\0') {
      snprintf(name, sizeof(name), "%s", val);
    } else if (strcmp(key, "Exec") == 0) {
      snprintf(exec_raw, sizeof(exec_raw), "%s", val);
    } else if (strcmp(key, "Icon") == 0) {
      snprintf(icon, sizeof(icon), "%s", val);
    } else if (strcmp(key, "Categories") == 0) {
      snprintf(cats_raw, sizeof(cats_raw), "%s", val);
    } else if (strcmp(key, "Terminal") == 0) {
      terminal = (strcasecmp(val, "true") == 0);
    } else if (strcmp(key, "NoDisplay") == 0) {
      no_display = (strcasecmp(val, "true") == 0);
    } else if (strcmp(key, "Hidden") == 0) {
      hidden = (strcasecmp(val, "true") == 0);
    } else if (strcmp(key, "Type") == 0) {
      snprintf(type_buf, sizeof(type_buf), "%s", val);
    }
  }
  fclose(f);

  if (no_display || hidden)
    return 0;
  if (type_buf[0] != '\0' && strcmp(type_buf, "Application") != 0)
    return 0;
  if (name[0] == '\0' || exec_raw[0] == '\0')
    return 0;

  memset(out, 0, sizeof(*out));
  snprintf(out->name, sizeof(out->name), "%s", name);
  snprintf(out->icon, sizeof(out->icon), "%s", icon);
  out->terminal = terminal;
  out->cats = parse_categories(cats_raw);
  clean_exec(exec_raw, out->exec, sizeof(out->exec));
  return 1;
}

static void scan_dir(const char *dirpath) {
  DIR *d = opendir(dirpath);
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    size_t len = strlen(ent->d_name);
    if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop") != 0)
      continue;
    if (g_app_count >= MAX_APPS)
      break;

    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dirpath, ent->d_name);

    AppEntry tmp;
    if (parse_desktop_file(full, &tmp) && !app_exists(tmp.name)) {
      g_apps[g_app_count++] = tmp;
    }
  }
  closedir(d);
}

static int cmp_apps(const void *a, const void *b) {
  const AppEntry *aa = a, *bb = b;
  return strcasecmp(aa->name, bb->name);
}

static void discover_apps(void) {
  char path[1024];
  const char *home = getenv("HOME");
  const char *xdg_data_home = getenv("XDG_DATA_HOME");

  if (xdg_data_home && xdg_data_home[0]) {
    snprintf(path, sizeof(path), "%s/applications", xdg_data_home);
    scan_dir(path);
  } else if (home) {
    snprintf(path, sizeof(path), "%s/.local/share/applications", home);
    scan_dir(path);
  }

  const char *data_dirs = getenv("XDG_DATA_DIRS");
  if (!data_dirs || !data_dirs[0])
    data_dirs = "/usr/local/share:/usr/share";

  char dirs_copy[1024];
  snprintf(dirs_copy, sizeof(dirs_copy), "%s", data_dirs);
  char *saveptr = NULL;
  char *tok = strtok_r(dirs_copy, ":", &saveptr);
  while (tok) {
    snprintf(path, sizeof(path), "%s/applications", tok);
    scan_dir(path);
    tok = strtok_r(NULL, ":", &saveptr);
  }

  scan_dir("/usr/share/applications");
  scan_dir("/usr/local/share/applications");

  qsort(g_apps, (size_t)g_app_count, sizeof(AppEntry), cmp_apps);
}

/* ---------------------------------------------------------------- */
/* Filtering / scrolling                                             */
/* ---------------------------------------------------------------- */

static void recompute_filter(void) {
  char needle[SEARCH_LEN];
  for (int k = 0; k < g_search_len; k++)
    needle[k] = (char)tolower((unsigned char)g_search[k]);
  needle[g_search_len] = '\0';

  g_filtered_count = 0;
  for (int i = 0; i < g_app_count; i++) {
    if (g_cat != CAT_ALL && !(g_apps[i].cats & (1u << g_cat)))
      continue;

    if (g_search_len > 0) {
      char hay[NAME_LEN];
      size_t hn = strlen(g_apps[i].name);
      for (size_t k = 0; k < hn && k < sizeof(hay) - 1; k++)
        hay[k] = (char)tolower((unsigned char)g_apps[i].name[k]);
      hay[hn < sizeof(hay) - 1 ? hn : sizeof(hay) - 1] = '\0';
      if (!strstr(hay, needle))
        continue;
    }
    g_filtered[g_filtered_count++] = i;
  }
  g_selected = 0;
  g_scroll_row = 0;
}

static void clamp_scroll(void) {
  int max_row = 0;
  if (g_filtered_count > 0) {
    int total_rows = (g_filtered_count + COLS - 1) / COLS;
    max_row = total_rows - VROWS;
    if (max_row < 0)
      max_row = 0;
  }
  if (g_scroll_row < 0)
    g_scroll_row = 0;
  if (g_scroll_row > max_row)
    g_scroll_row = max_row;
}

static void ensure_selected_visible(void) {
  int sel_row = g_selected / COLS;
  if (sel_row < g_scroll_row)
    g_scroll_row = sel_row;
  else if (sel_row >= g_scroll_row + VROWS)
    g_scroll_row = sel_row - VROWS + 1;
  clamp_scroll();
}

static void set_category(int c, int wrap) {
  if (c < 0)
    c = wrap ? CAT_COUNT - 1 : 0;
  if (c >= CAT_COUNT)
    c = wrap ? 0 : CAT_COUNT - 1;
  g_cat = c;
  g_tab_follow = 1;
  recompute_filter();
}

/* ---------------------------------------------------------------- */
/* Launching                                                         */
/* ---------------------------------------------------------------- */

static void launch_app(const AppEntry *app) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (app->terminal) {
      execlp("wezterm", "wezterm", "start", "--", "sh", "-c", app->exec, NULL);
      execlp("konsole", "konsole", "-e", "sh", "-c", app->exec, NULL);
      execlp("xterm", "xterm", "-e", "sh", "-c", app->exec, NULL);
    } else {
      execlp("sh", "sh", "-c", app->exec, NULL);
    }
    _exit(127);
  }
}

/* ---------------------------------------------------------------- */
/* Window / compositor support                                       */
/* ---------------------------------------------------------------- */

static xcb_visualtype_t *find_argb_visual(xcb_depth_t **out_depth) {
  xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);

  for (; di.rem; xcb_depth_next(&di)) {
    if (di.data->depth != 32)
      continue;

    xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
    for (; vi.rem; xcb_visualtype_next(&vi)) {
      if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR ||
          vi.data->_class == XCB_VISUAL_CLASS_DIRECT_COLOR) {
        if (out_depth)
          *out_depth = di.data;
        return vi.data;
      }
    }
  }

  if (out_depth)
    *out_depth = NULL;
  return NULL;
}

/* Blur only the panel, with rounded corners (built from 1px-high rects). */
static void set_blur_region(void) {
  const char *prop_name = "_KDE_NET_WM_BLUR_BEHIND_REGION";
  xcb_intern_atom_cookie_t pc =
      xcb_intern_atom(conn, 0, (uint16_t)strlen(prop_name), prop_name);
  xcb_intern_atom_reply_t *pr = xcb_intern_atom_reply(conn, pc, NULL);
  if (!pr)
    return;
  xcb_atom_t prop = pr->atom;
  free(pr);

  int r = PANEL_R;
  int nrects = 2 * r + 1;
  uint32_t *reg = malloc(sizeof(uint32_t) * 4 * (size_t)nrects);
  if (!reg)
    return;

  int n = 0;
  for (int i = 0; i < r; i++) {
    double dy = r - i - 0.5;
    int inset = (int)lround(r - sqrt((double)r * r - dy * dy));
    /* top row i */
    reg[n * 4 + 0] = (uint32_t)(g_px + inset);
    reg[n * 4 + 1] = (uint32_t)(g_py + i);
    reg[n * 4 + 2] = (uint32_t)(g_pw - 2 * inset);
    reg[n * 4 + 3] = 1;
    n++;
    /* mirrored bottom row */
    reg[n * 4 + 0] = (uint32_t)(g_px + inset);
    reg[n * 4 + 1] = (uint32_t)(g_py + g_ph - 1 - i);
    reg[n * 4 + 2] = (uint32_t)(g_pw - 2 * inset);
    reg[n * 4 + 3] = 1;
    n++;
  }
  reg[n * 4 + 0] = (uint32_t)g_px;
  reg[n * 4 + 1] = (uint32_t)(g_py + r);
  reg[n * 4 + 2] = (uint32_t)g_pw;
  reg[n * 4 + 3] = (uint32_t)(g_ph - 2 * r);
  n++;

  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop, XCB_ATOM_CARDINAL,
                      32, (uint32_t)(n * 4), reg);
  free(reg);
}

// static void compute_layout(void) {
//   int sh = screen->height_in_pixels;
//   g_px = 0;
//   g_py = 0;
//   g_wx = LAUNCH_MARGIN_X;
//   g_wy = sh - g_ph - LAUNCH_MARGIN_BOTTOM;
//   if (g_wy < 0)
//     g_wy = 0;
// }
// static void compute_layout(void) {
//   int sw = screen->width_in_pixels;
//   int sh = screen->height_in_pixels;
//
//   g_px = 0;
//   g_py = 0;
//
//   g_wx = (sw - g_pw) / 2;
//   g_wy = sh - g_ph - 40;
//
//   if (g_wy < 0)
//     g_wy = 0;
// }
static void compute_layout(void) {
  int sw = screen->width_in_pixels;
  int sh = screen->height_in_pixels;

  g_px = 0;
  g_py = 0;

  g_wx = (sw - g_pw) / 2;
  g_wy = (sh - g_ph) / 2;
}

/* ---------------------------------------------------------------- */
/* Drawing helpers                                                   */
/* ---------------------------------------------------------------- */

static void rounded_rect(cairo_t *c, double x, double y, double w, double h,
                         double r) {
  cairo_new_sub_path(c);
  cairo_arc(c, x + w - r, y + r, r, -M_PI_2, 0);
  cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI_2);
  cairo_arc(c, x + r, y + h - r, r, M_PI_2, M_PI);
  cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI_2);
  cairo_close_path(c);
}

static void set_font(cairo_t *c, double size, int bold) {
  cairo_select_font_face(c, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         bold ? CAIRO_FONT_WEIGHT_BOLD
                              : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(c, size);
}

static void name_to_color(const char *name, double *r, double *g, double *b) {
  unsigned long hash = 5381;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++)
    hash = hash * 33 + *p;
  double hue = (double)(hash % 360);
  double s = 0.55, v = 0.75;
  double c = v * s;
  double x = c * (1 - fabs(fmod(hue / 60.0, 2) - 1));
  double m = v - c;
  double rr, gg, bb;
  if (hue < 60) {
    rr = c; gg = x; bb = 0;
  } else if (hue < 120) {
    rr = x; gg = c; bb = 0;
  } else if (hue < 180) {
    rr = 0; gg = c; bb = x;
  } else if (hue < 240) {
    rr = 0; gg = x; bb = c;
  } else if (hue < 300) {
    rr = x; gg = 0; bb = c;
  } else {
    rr = c; gg = 0; bb = x;
  }
  *r = rr + m;
  *g = gg + m;
  *b = bb + m;
}

/* Copy `text` into buf, trimming with "..." until it fits max_w. */
static void ellipsize_fit(cairo_t *c, const char *text, double max_w, char *buf,
                          size_t bufsz) {
  snprintf(buf, bufsz, "%s", text);
  cairo_text_extents_t e;
  cairo_text_extents(c, buf, &e);
  if (e.x_advance <= max_w)
    return;

  size_t len = strlen(buf);
  while (len > 1) {
    len--;
    while (len > 1 && ((unsigned char)buf[len] & 0xC0) == 0x80)
      len--; /* don't cut inside a UTF-8 sequence */
    char tmp[NAME_LEN + 4];
    snprintf(tmp, sizeof(tmp), "%.*s...", (int)len, text);
    cairo_text_extents(c, tmp, &e);
    if (e.x_advance <= max_w || len == 1) {
      snprintf(buf, bufsz, "%s", tmp);
      return;
    }
  }
}

static void show_centered(cairo_t *c, const char *s, double cx, double y,
                          double alpha) {
  cairo_text_extents_t e;
  cairo_text_extents(c, s, &e);
  double x = cx - e.x_advance / 2.0;

  cairo_set_source_rgba(c, 0, 0, 0, 0.35 * alpha); /* soft shadow */
  cairo_move_to(c, x + 0.5, y + 1.0);
  cairo_show_text(c, s);

  cairo_set_source_rgba(c, 1, 1, 1, alpha);
  cairo_move_to(c, x, y);
  cairo_show_text(c, s);
}

/* Two-line wrapped label, centered on cx. */
static void draw_label(cairo_t *c, const char *name, double cx, double y1) {
  const double maxw = COL_W - 2;
  char buf[NAME_LEN + 4];
  cairo_text_extents_t e;

  cairo_text_extents(c, name, &e);
  if (e.x_advance <= maxw) {
    show_centered(c, name, cx, y1 + 6, 0.92);
    return;
  }

  /* longest word-boundary prefix that fits on line 1 */
  int best = -1;
  size_t n = strlen(name);
  for (size_t i = 1; i < n; i++) {
    if (name[i] != ' ')
      continue;
    char tmp[NAME_LEN];
    snprintf(tmp, sizeof(tmp), "%.*s", (int)i, name);
    cairo_text_extents(c, tmp, &e);
    if (e.x_advance <= maxw)
      best = (int)i;
    else
      break;
  }

  if (best <= 0) { /* a single long word */
    ellipsize_fit(c, name, maxw, buf, sizeof(buf));
    show_centered(c, buf, cx, y1 + 6, 0.92);
    return;
  }

  char l1[NAME_LEN];
  snprintf(l1, sizeof(l1), "%.*s", best, name);
  const char *rest = name + best;
  while (*rest == ' ')
    rest++;

  ellipsize_fit(c, rest, maxw, buf, sizeof(buf));
  show_centered(c, l1, cx, y1, 0.92);
  show_centered(c, buf, cx, y1 + 14, 0.92);
}

/* ---------------------------------------------------------------- */
/* Panel drawing (panel-local coordinates)                           */
/* ---------------------------------------------------------------- */

static void draw_header(cairo_t *c) {
  const double W = g_pw;

  /* Arch Linux Nerd Font glyph (U+F303), replacing the App Store "A".
   * Requires a Nerd Font with the Arch Linux glyph, e.g. Symbols Nerd Font. */
  cairo_save(c);
  cairo_set_source_rgba(c, 0.35, 0.72, 1.0, 0.96);
  PangoLayout *arch_layout = pango_cairo_create_layout(c);
  PangoFontDescription *arch_font =
      pango_font_description_from_string("Symbols Nerd Font 25");
  pango_layout_set_font_description(arch_layout, arch_font);
  pango_layout_set_text(arch_layout, "\xef\x8c\x83", -1); /*  */
  PangoRectangle arch_logical;
  pango_layout_get_pixel_extents(arch_layout, NULL, &arch_logical);
  cairo_move_to(c, 27 - arch_logical.width / 2.0 - arch_logical.x,
                HEADER_CY - arch_logical.height / 2.0 - arch_logical.y);
  pango_cairo_show_layout(c, arch_layout);
  pango_font_description_free(arch_font);
  g_object_unref(arch_layout);
  cairo_restore(c);

  /* search text */
  double tx = 64, avail = W - tx - 62; /* extra gap after Arch Linux logo */
  set_font(c, 21, 0);

  cairo_save(c);
  cairo_rectangle(c, tx - 8, HEADER_CY - 20, avail + 8, 40);
  cairo_clip(c);

  double base = HEADER_CY + 7;
  if (g_search_len == 0) {
    /* caret then placeholder */
    cairo_set_source_rgba(c, 1, 1, 1, 0.92);
    cairo_rectangle(c, tx - 6, HEADER_CY - 12, 1.8, 24);
    cairo_fill(c);

    cairo_set_source_rgba(c, 1, 1, 1, 0.62);
    cairo_move_to(c, tx, base);
    cairo_show_text(c, "Applications");
  } else {
    cairo_text_extents_t e;
    cairo_text_extents(c, g_search, &e);
    double shift = e.x_advance > avail - 6 ? avail - 6 - e.x_advance : 0;

    cairo_set_source_rgba(c, 1, 1, 1, 0.95);
    cairo_move_to(c, tx + shift, base);
    cairo_show_text(c, g_search);

    cairo_rectangle(c, tx + shift + e.x_advance + 2, HEADER_CY - 12, 1.8, 24);
    cairo_fill(c);
  }
  cairo_restore(c);

  /* "..." button */
  double bx = W - 28, by = HEADER_CY;
  cairo_save(c);
  cairo_set_source_rgba(c, 1, 1, 1, 0.70);
  cairo_set_line_width(c, 1.5);
  cairo_new_path(c);
  cairo_arc(c, bx, by, 10, 0, 2 * M_PI);
  cairo_stroke(c);
  for (int i = -1; i <= 1; i++) {
    cairo_new_path(c);
    cairo_arc(c, bx + i * 5.0, by, 1.4, 0, 2 * M_PI);
    cairo_fill(c);
  }
  cairo_restore(c);
}

static void draw_tabs(cairo_t *c) {
  const double W = g_pw;
  set_font(c, 11.5, 0);

  double xs[CAT_COUNT], ws[CAT_COUNT], total = 0;
  for (int i = 0; i < CAT_COUNT; i++) {
    cairo_text_extents_t e;
    cairo_text_extents(c, CAT_NAMES[i], &e);
    ws[i] = e.x_advance + 26;
    xs[i] = total;
    total += ws[i] + TAB_GAP;
  }
  total -= TAB_GAP;

  /* keep selected pill in view, with a peek of its neighbours */
  double view = W - 44;
  double left = xs[g_cat], right = xs[g_cat] + ws[g_cat];
  double peek = 36;
  if (g_tab_follow) {
    if (right + (g_cat < CAT_COUNT - 1 ? peek : 0) - g_tab_off > view)
      g_tab_off = right + (g_cat < CAT_COUNT - 1 ? peek : 0) - view;
    if (left - (g_cat > 0 ? peek : 0) - g_tab_off < 0)
      g_tab_off = left - (g_cat > 0 ? peek : 0);
    g_tab_follow = 0;
  }
  double max_off = total > view ? total - view : 0;
  g_tab_max = max_off;
  if (g_tab_off > max_off)
    g_tab_off = max_off;
  if (g_tab_off < 0)
    g_tab_off = 0;

  cairo_save(c);
  cairo_rectangle(c, 10, TAB_Y - 2, W - 20, TAB_H + 4);
  cairo_clip(c);

  for (int i = 0; i < CAT_COUNT; i++) {
    double x = 22 + xs[i] - g_tab_off;
    g_tab_x[i] = x;
    g_tab_w[i] = ws[i];
    if (x + ws[i] < 0 || x > W)
      continue;

    int sel = (i == g_cat);
    rounded_rect(c, x, TAB_Y, ws[i], TAB_H, TAB_H / 2.0);
    if (sel)
      cairo_set_source_rgba(c, 0.78, 0.84, 0.89, 0.80);
    else
      cairo_set_source_rgba(c, 0.07, 0.15, 0.23, 0.42);
    cairo_fill(c);

    cairo_text_extents_t e;
    cairo_text_extents(c, CAT_NAMES[i], &e);
    if (sel)
      cairo_set_source_rgba(c, 0.07, 0.11, 0.17, 0.95);
    else
      cairo_set_source_rgba(c, 1, 1, 1, 0.78);
    cairo_move_to(c, x + (ws[i] - e.x_advance) / 2.0, TAB_Y + TAB_H / 2.0 + 4);
    cairo_show_text(c, CAT_NAMES[i]);
  }
  cairo_restore(c);
}

static void draw_grid(cairo_t *c) {
  if (g_filtered_count == 0) {
    set_font(c, 17, 0);
    cairo_set_source_rgba(c, 1, 1, 1, 0.65);
    cairo_text_extents_t e;
    const char *msg = "No matching apps";
    cairo_text_extents(c, msg, &e);
    cairo_move_to(c, (g_pw - e.x_advance) / 2.0, GRID_Y + 60);
    cairo_show_text(c, msg);
    return;
  }

  int start = g_scroll_row * COLS;
  int end = start + VROWS * COLS;
  if (end > g_filtered_count)
    end = g_filtered_count;

  for (int fi = start; fi < end; fi++) {
    int rel = fi - start;
    int col = rel % COLS;
    int row = rel / COLS;

    double cx = GRID_PAD + col * COL_W + COL_W / 2.0;
    double y = GRID_Y + row * ROW_H;

    AppEntry *app = &g_apps[g_filtered[fi]];
    ensure_app_icon(app);

    if (fi == g_selected) {
      rounded_rect(c, cx - BOX / 2.0 + 0.5, y + 0.5, BOX - 1, BOX - 1, 14);
      cairo_set_source_rgba(c, 1, 1, 1, 0.10);
      cairo_fill_preserve(c);
      cairo_set_source_rgba(c, 1, 1, 1, 0.38);
      cairo_set_line_width(c, 1.2);
      cairo_stroke(c);
    }

    double ix = cx - ICON / 2.0;
    double iy = y + (BOX - ICON) / 2.0;

    if (app->pixbuf) {
      cairo_save(c);
      gdk_cairo_set_source_pixbuf(c, app->pixbuf, ix, iy);
      cairo_rectangle(c, ix, iy, ICON, ICON);
      cairo_fill(c);
      cairo_restore(c);
    } else {
      double r, g, b;
      name_to_color(app->name, &r, &g, &b);
      rounded_rect(c, ix, iy, ICON, ICON, 12);
      cairo_set_source_rgba(c, r, g, b, 0.92);
      cairo_fill(c);

      char letter[2] = {(char)toupper((unsigned char)app->name[0]), 0};
      set_font(c, 22, 1);
      cairo_text_extents_t le;
      cairo_text_extents(c, letter, &le);
      cairo_set_source_rgba(c, 1, 1, 1, 0.95);
      cairo_move_to(c, ix + (ICON - le.width) / 2 - le.x_bearing,
                    iy + (ICON - le.height) / 2 - le.y_bearing);
      cairo_show_text(c, letter);
    }

    set_font(c, 11.5, 0);
    draw_label(c, app->name, cx, y + BOX + 13);
  }
}

static void draw_panel(cairo_t *c) {
  const double W = g_pw, H = g_ph;

  cairo_set_operator(c, CAIRO_OPERATOR_CLEAR);
  cairo_paint(c);
  cairo_set_operator(c, CAIRO_OPERATOR_OVER);

  /* glass body */
  rounded_rect(c, 0.5, 0.5, W - 1, H - 1, PANEL_R);
  cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, H);
  cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.0, 0.0, 0.00, 0.50);
  cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.0, 0.0, 0.0, 0.60);
  cairo_set_source(c, grad);
  cairo_fill_preserve(c);
  cairo_pattern_destroy(grad);
  cairo_set_source_rgba(c, 1, 1, 1, 0.20);
  cairo_set_line_width(c, 1.0);
  cairo_stroke(c);

  draw_header(c);
  draw_tabs(c);
  draw_grid(c);
}

static cairo_surface_t *g_bs;
static cairo_t *g_bb;

static void render(int full) {
  (void)full;
  if (!g_bs) { /* persistent back buffer: allocated once */
    g_bs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g_pw, g_ph);
    g_bb = cairo_create(g_bs);
  }
  draw_panel(g_bb);

  if (!g_have_alpha) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0.04, 0.06, 0.09);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  } else {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  }
  cairo_set_source_surface(cr, g_bs, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  cairo_surface_flush(surface);
  xcb_flush(conn);
}

/* ---------------------------------------------------------------- */
/* Input helpers                                                     */
/* ---------------------------------------------------------------- */

static int inside_panel(int x, int y) {
  return x >= g_px && x < g_px + g_pw && y >= g_py && y < g_py + g_ph;
}

static int tab_at(int px, int py) {
  int lx = px - g_px, ly = py - g_py;
  if (ly < TAB_Y || ly > TAB_Y + TAB_H)
    return -1;
  if (lx < 10 || lx > g_pw - 10)
    return -1;
  for (int i = 0; i < CAT_COUNT; i++) {
    if (lx >= g_tab_x[i] && lx < g_tab_x[i] + g_tab_w[i])
      return i;
  }
  return -1;
}

static int over_tab_bar(int px, int py) {
  int lx = px - g_px, ly = py - g_py;
  return lx >= 0 && lx < g_pw && ly >= TAB_Y - 4 && ly <= TAB_Y + TAB_H + 4;
}

static int tile_at(int px, int py) {
  int lx = px - g_px - GRID_PAD, ly = py - g_py - GRID_Y;
  if (lx < 0 || ly < 0)
    return -1;
  int c = lx / COL_W, r = ly / ROW_H;
  if (c >= COLS || r >= VROWS)
    return -1;
  if (ly % ROW_H > BOX + 30)
    return -1;
  int fi = (g_scroll_row + r) * COLS + c;
  if (fi < 0 || fi >= g_filtered_count)
    return -1;
  return fi;
}

static void scroll_rows(int delta) {
  g_scroll_row += delta;
  clamp_scroll();

  int sel_row = g_selected / COLS;
  int sel_col = g_selected % COLS;
  if (sel_row < g_scroll_row)
    g_selected = g_scroll_row * COLS + sel_col;
  else if (sel_row >= g_scroll_row + VROWS)
    g_selected = (g_scroll_row + VROWS - 1) * COLS + sel_col;

  if (g_selected >= g_filtered_count)
    g_selected = g_filtered_count > 0 ? g_filtered_count - 1 : 0;

  g_dirty = 1;
}

/* ---------------------------------------------------------------- */
/* Main                                                              */
/* ---------------------------------------------------------------- */

int main(int argc, char **argv) {
  if (!gdk_init_check(&argc, &argv)) {
    fprintf(stderr, "applauncher: cannot initialize GDK\n");
    return 1;
  }

  discover_apps();
  recompute_filter();

  conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(conn)) {
    fprintf(stderr, "applauncher: cannot connect to X server\n");
    xcb_disconnect(conn);
    return 1;
  }

  screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
  if (!screen) {
    fprintf(stderr, "applauncher: cannot find X screen\n");
    xcb_disconnect(conn);
    return 1;
  }
  compute_layout();

  xcb_depth_t *argb_depth = NULL;
  xcb_visualtype_t *argb_vis = find_argb_visual(&argb_depth);
  xcb_visualtype_t *vis = NULL;
  uint8_t depth = screen->root_depth;
  xcb_colormap_t cmap = XCB_COLORMAP_NONE;

  if (argb_vis && argb_depth && argb_depth->depth == 32) {
    vis = argb_vis;
    depth = argb_depth->depth;
    g_have_alpha = 1;

    cmap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, cmap, screen->root,
                        vis->visual_id);
  } else {
    xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
    for (; di.rem; xcb_depth_next(&di)) {
      xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
      for (; vi.rem; xcb_visualtype_next(&vi)) {
        if (vi.data->visual_id == screen->root_visual) {
          vis = vi.data;
          break;
        }
      }
      if (vis)
        break;
    }
    fprintf(stderr,
            "applauncher: ARGB visual unavailable; using root visual fallback\n");
  }

  if (!vis) {
    fprintf(stderr, "applauncher: cannot find a compatible X visual\n");
    if (cmap != XCB_COLORMAP_NONE)
      xcb_free_colormap(conn, cmap);
    xcb_disconnect(conn);
    return 1;
  }

  win = xcb_generate_id(conn);

  const uint32_t ev_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
                           XCB_EVENT_MASK_BUTTON_PRESS |
                           XCB_EVENT_MASK_BUTTON_RELEASE |
                           XCB_EVENT_MASK_POINTER_MOTION;
  uint32_t mask = 0;
  uint32_t values[5];
  int idx = 0;

  if (g_have_alpha) {
    mask |= XCB_CW_BACK_PIXEL;
    values[idx++] = 0;
    mask |= XCB_CW_BORDER_PIXEL;
    values[idx++] = 0;
    mask |= XCB_CW_OVERRIDE_REDIRECT;
    values[idx++] = 1;
    mask |= XCB_CW_EVENT_MASK;
    values[idx++] = ev_mask;
    mask |= XCB_CW_COLORMAP;
    values[idx++] = cmap;
  } else {
    mask |= XCB_CW_BACK_PIXEL;
    values[idx++] = screen->black_pixel;
    mask |= XCB_CW_OVERRIDE_REDIRECT;
    values[idx++] = 1;
    mask |= XCB_CW_EVENT_MASK;
    values[idx++] = ev_mask;
  }

  xcb_void_cookie_t create_cookie = xcb_create_window_checked(
      conn, depth, win, screen->root, (int16_t)g_wx, (int16_t)g_wy,
      (uint16_t)g_pw, (uint16_t)g_ph, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
      vis->visual_id, mask, values);

  xcb_generic_error_t *create_error = xcb_request_check(conn, create_cookie);
  if (create_error) {
    fprintf(stderr,
            "applauncher: X could not create launcher window (error %u)\n",
            create_error->error_code);
    free(create_error);
    if (cmap != XCB_COLORMAP_NONE)
      xcb_free_colormap(conn, cmap);
    xcb_disconnect(conn);
    return 1;
  }

  xcb_map_window(conn, win);
  xcb_flush(conn);

  xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, win,
                      XCB_CURRENT_TIME);

  xcb_grab_keyboard_cookie_t grab_cookie =
      xcb_grab_keyboard(conn, 1, screen->root, XCB_CURRENT_TIME,
                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
  xcb_grab_keyboard_reply_t *grab_reply =
      xcb_grab_keyboard_reply(conn, grab_cookie, NULL);
  if (!grab_reply || grab_reply->status != XCB_GRAB_STATUS_SUCCESS) {
    fprintf(stderr, "applauncher: keyboard grab unavailable; continuing "
                    "without global grab\n");
  }
  free(grab_reply);

  /* grab the pointer: clicks outside our small window still reach us */
  xcb_grab_pointer_cookie_t pg = xcb_grab_pointer(
      conn, 1, win,
      XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
          XCB_EVENT_MASK_POINTER_MOTION,
      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
      XCB_CURRENT_TIME);
  free(xcb_grab_pointer_reply(conn, pg, NULL));

  if (g_have_alpha)
    set_blur_region();
  xcb_flush(conn);

  surface = cairo_xcb_surface_create(conn, win, vis, g_pw, g_ph);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "applauncher: cairo surface creation failed\n");
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 1;
  }

  cr = cairo_create(surface);
  if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "applauncher: cairo context creation failed\n");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 1;
  }

  xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(conn);
  if (!keysyms) {
    fprintf(stderr, "applauncher: cannot initialize keyboard symbols\n");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 1;
  }

  render(1);

  int running = 1;
  xcb_generic_event_t *ev;

  while (running && (ev = xcb_wait_for_event(conn))) {
   do {
    switch (ev->response_type & ~0x80) {
    case XCB_EXPOSE:
      g_dirty = 1;
      break;

    case XCB_MOTION_NOTIFY: {
      xcb_motion_notify_event_t *me = (xcb_motion_notify_event_t *)ev;
      if (g_drag) {
        g_tab_off -= me->event_x - g_drag_x;
        g_drag_x = me->event_x;
        if (g_tab_off < 0)
          g_tab_off = 0;
        if (g_tab_off > g_tab_max)
          g_tab_off = g_tab_max;
        g_dirty = 1;
        break;
      }
      int fi = tile_at(me->event_x, me->event_y);
      if (fi >= 0 && fi != g_selected) {
        g_selected = fi;
        g_dirty = 1;
      }
      break;
    }

    case XCB_BUTTON_RELEASE: {
      xcb_button_release_event_t *re = (xcb_button_release_event_t *)ev;
      if (re->detail == 3)
        g_drag = 0;
      break;
    }

    case XCB_BUTTON_PRESS: {
      xcb_button_press_event_t *be = (xcb_button_press_event_t *)ev;

      /* wheel: 4 up, 5 down, 6 left, 7 right */
      if (be->detail >= 4 && be->detail <= 7) {
        int dir = (be->detail == 4 || be->detail == 6) ? -1 : 1;
        if (be->detail >= 6 || over_tab_bar(be->event_x, be->event_y)) {
          int nc = g_cat + dir;
          if (nc >= 0 && nc < CAT_COUNT) {
            set_category(nc, 0);
            g_dirty = 1;
          }
        } else {
          scroll_rows(dir);
        }
        break;
      }

      if (be->detail == 3) { /* right click + drag scrolls the pills */
        if (over_tab_bar(be->event_x, be->event_y)) {
          g_drag = 1;
          g_drag_x = be->event_x;
        }
        break;
      }

      if (be->detail != 1)
        break;

      if (!inside_panel(be->event_x, be->event_y)) {
        running = 0; /* click outside closes */
        break;
      }

      int ti = tab_at(be->event_x, be->event_y);
      if (ti >= 0) {
        set_category(ti, 0);
        g_dirty = 1;
        break;
      }

      int fi = tile_at(be->event_x, be->event_y);
      if (fi >= 0) {
        launch_app(&g_apps[g_filtered[fi]]);
        running = 0;
      }
      break;
    }

    case XCB_KEY_PRESS: {
      xcb_key_press_event_t *ke = (xcb_key_press_event_t *)ev;
      int col_idx = (ke->state & XCB_MOD_MASK_SHIFT) ? 1 : 0;
      xcb_keysym_t sym = xcb_key_symbols_get_keysym(keysyms, ke->detail, col_idx);

      if (sym == XK_Escape) {
        running = 0;
      } else if (sym == XK_Return || sym == XK_KP_Enter) {
        if (g_filtered_count > 0) {
          launch_app(&g_apps[g_filtered[g_selected]]);
          running = 0;
        }
      } else if (sym == XK_BackSpace) {
        if (g_search_len > 0) {
          g_search[--g_search_len] = '\0';
          g_cat = CAT_ALL;
          recompute_filter();
          g_dirty = 1;
        }
      } else if (sym == XK_Tab) {
        set_category(g_cat + 1, 1);
        g_dirty = 1;
      } else if (sym == XK_ISO_Left_Tab) {
        set_category(g_cat - 1, 1);
        g_dirty = 1;
      } else if (sym == XK_Left) {
        if (g_selected > 0) {
          g_selected--;
          ensure_selected_visible();
          g_dirty = 1;
        }
      } else if (sym == XK_Right) {
        if (g_selected + 1 < g_filtered_count) {
          g_selected++;
          ensure_selected_visible();
          g_dirty = 1;
        }
      } else if (sym == XK_Up) {
        if (g_selected - COLS >= 0) {
          g_selected -= COLS;
          ensure_selected_visible();
          g_dirty = 1;
        }
      } else if (sym == XK_Down) {
        if (g_selected + COLS < g_filtered_count) {
          g_selected += COLS;
          ensure_selected_visible();
          g_dirty = 1;
        } else if (g_selected / COLS < (g_filtered_count - 1) / COLS) {
          g_selected = g_filtered_count - 1; /* jump to last partial row */
          ensure_selected_visible();
          g_dirty = 1;
        }
      } else if (sym == XK_Prior) {
        scroll_rows(-VROWS);
      } else if (sym == XK_Next) {
        scroll_rows(VROWS);
      } else if (sym >= 0x20 && sym <= 0x7e &&
                 !(ke->state & XCB_MOD_MASK_CONTROL)) {
        if (g_search_len + 1 < SEARCH_LEN) {
          g_search[g_search_len++] = (char)sym;
          g_search[g_search_len] = '\0';
          g_cat = CAT_ALL; /* search is global */
          recompute_filter();
          g_dirty = 1;
        }
      }
      break;
    }

    default:
      break;
    }

    free(ev);
   } while (running && (ev = xcb_poll_for_event(conn)));

    if (running && g_dirty) { /* one redraw per batch of events */
      render(0);
      g_dirty = 0;
    }
  }

  xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
  xcb_key_symbols_free(keysyms);
  xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);

  free_app_icons();

  if (g_bb)
    cairo_destroy(g_bb);
  if (g_bs)
    cairo_surface_destroy(g_bs);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  xcb_destroy_window(conn, win);
  if (cmap != XCB_COLORMAP_NONE)
    xcb_free_colormap(conn, cmap);

  xcb_flush(conn);
  xcb_disconnect(conn);
  return 0;
}
