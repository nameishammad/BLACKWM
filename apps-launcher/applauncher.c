#include <X11/keysym.h>
#include <cairo-xcb.h>
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define TILE_W 116
#define TILE_H 116
#define TILE_GAP 16
#define SIDE_MARGIN 24
#define TOP_MARGIN 104
#define BOTTOM_MARGIN 24
#define SEARCH_BOX_Y 24
#define SEARCH_BOX_H 50

typedef struct {
  char name[NAME_LEN];
  char exec[EXEC_LEN];
  char icon[ICON_LEN];
  int terminal;
  GdkPixbuf *pixbuf; /* Cached icon. */
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

static xcb_connection_t *conn;
static xcb_screen_t *screen;
static xcb_window_t win;
static cairo_surface_t *surface;
static cairo_t *cr;
static int g_cols = 1;

/* ---------------------------------------------------------------- */
/* Helper to locate icon files                                      */
/* ---------------------------------------------------------------- */

static GdkPixbuf *load_theme_icon(const char *icon_name, int size) {
  if (!icon_name || !icon_name[0])
    return NULL;

  GdkScreen *screen = gdk_screen_get_default();
  if (!screen)
    return NULL;

  GtkIconTheme *theme = gtk_icon_theme_get_for_screen(screen);
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

  return pixbuf;
}

static void load_app_icons(void) {
  for (int i = 0; i < g_app_count; i++) {
    g_apps[i].pixbuf = NULL;
    g_apps[i].icon_loaded = 0;
  }
}

static void ensure_app_icon(AppEntry *app) {
  if (!app || app->icon_loaded || !app->icon[0])
    return;

  app->pixbuf = load_theme_icon(app->icon, 56);
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

static int parse_desktop_file(const char *path, AppEntry *out) {
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;

  char line[1024];
  char name[NAME_LEN] = {0};
  char exec_raw[EXEC_LEN] = {0};
  char icon[ICON_LEN] = {0};
  char type_buf[64] = {0};
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

  snprintf(out->name, sizeof(out->name), "%s", name);
  snprintf(out->icon, sizeof(out->icon), "%s", icon);
  out->terminal = terminal;
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
/* Filtering                                                         */
/* ---------------------------------------------------------------- */

static void recompute_filter(void) {
  g_filtered_count = 0;
  for (int i = 0; i < g_app_count; i++) {
    if (g_search_len == 0) {
      g_filtered[g_filtered_count++] = i;
      continue;
    }
    char hay[NAME_LEN];
    size_t hn = strlen(g_apps[i].name);
    for (size_t k = 0; k < hn && k < sizeof(hay) - 1; k++)
      hay[k] = (char)tolower((unsigned char)g_apps[i].name[k]);
    hay[hn < sizeof(hay) - 1 ? hn : sizeof(hay) - 1] = '\0';

    char needle[SEARCH_LEN];
    for (int k = 0; k < g_search_len; k++)
      needle[k] = (char)tolower((unsigned char)g_search[k]);
    needle[g_search_len] = '\0';

    if (strstr(hay, needle))
      g_filtered[g_filtered_count++] = i;
  }
  if (g_selected >= g_filtered_count)
    g_selected = g_filtered_count > 0 ? g_filtered_count - 1 : 0;
  if (g_selected < 0)
    g_selected = 0;
  g_scroll_row = 0;
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
      execlp("xterm", "xterm", "-e", "sh", "-c", app->exec, NULL);
    } else {
      execlp("sh", "sh", "-c", app->exec, NULL);
    }
    _exit(127);
  }
}

/* ---------------------------------------------------------------- */
/* Safe glass/compositor support                                     */
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

static void set_blur_region(void) {
  xcb_atom_t prop = XCB_ATOM_NONE;
  xcb_atom_t cardinal = XCB_ATOM_CARDINAL;

  const char *prop_name = "_KDE_NET_WM_BLUR_BEHIND_REGION";
  xcb_intern_atom_cookie_t pc =
      xcb_intern_atom(conn, 0, (uint16_t)strlen(prop_name), prop_name);

  const char *card_name = "CARDINAL";
  xcb_intern_atom_cookie_t cc =
      xcb_intern_atom(conn, 0, (uint16_t)strlen(card_name), card_name);

  xcb_intern_atom_reply_t *pr = xcb_intern_atom_reply(conn, pc, NULL);
  xcb_intern_atom_reply_t *crp = xcb_intern_atom_reply(conn, cc, NULL);

  if (pr) {
    prop = pr->atom;
    free(pr);
  }
  if (crp) {
    cardinal = crp->atom;
    free(crp);
  }

  if (prop == XCB_ATOM_NONE || cardinal == XCB_ATOM_NONE)
    return;

  int sw = screen->width_in_pixels;
  int sh = screen->height_in_pixels;
  uint32_t region[4] = {0, 0, (uint32_t)sw, (uint32_t)sh};

  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, prop, cardinal, 32, 4,
                      region);
}

/* ---------------------------------------------------------------- */
/* Rendering (Double Buffered)                                       */
/* ---------------------------------------------------------------- */

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
    rr = c;
    gg = x;
    bb = 0;
  } else if (hue < 120) {
    rr = x;
    gg = c;
    bb = 0;
  } else if (hue < 180) {
    rr = 0;
    gg = c;
    bb = x;
  } else if (hue < 240) {
    rr = 0;
    gg = x;
    bb = c;
  } else if (hue < 300) {
    rr = x;
    gg = 0;
    bb = c;
  } else {
    rr = c;
    gg = 0;
    bb = x;
  }
  *r = rr + m;
  *g = gg + m;
  *b = bb + m;
}

static void rounded_rect(cairo_t *c, double x, double y, double w, double h,
                         double r) {
  cairo_new_sub_path(c);
  cairo_arc(c, x + w - r, y + r, r, -M_PI_2, 0);
  cairo_arc(c, x + w - r, y + h - r, r, 0, M_PI_2);
  cairo_arc(c, x + r, y + h - r, r, M_PI_2, M_PI);
  cairo_arc(c, x + r, y + r, r, M_PI, 3 * M_PI_2);
  cairo_close_path(c);
}

static void draw_ellipsized(cairo_t *c, const char *text, double max_w) {
  char buf[NAME_LEN];
  snprintf(buf, sizeof(buf), "%s", text);
  cairo_text_extents_t ext;
  cairo_text_extents(c, buf, &ext);

  if (ext.width <= max_w) {
    cairo_show_text(c, buf);
    return;
  }

  size_t len = strlen(buf);
  while (len > 1 && ext.width > max_w) {
    len--;
    buf[len] = '\0';

    char tmp[NAME_LEN + 4];
    snprintf(tmp, sizeof(tmp), "%s...", buf);
    cairo_text_extents(c, tmp, &ext);

    if (ext.width <= max_w || len == 1) {
      cairo_show_text(c, tmp);
      return;
    }
  }
}

static void clamp_scroll(int visible_rows) {
  if (visible_rows < 1)
    visible_rows = 1;

  int max_row = 0;
  if (g_filtered_count > 0) {
    int total_rows = (g_filtered_count + g_cols - 1) / g_cols;
    max_row = total_rows - visible_rows;
    if (max_row < 0)
      max_row = 0;
  }

  if (g_scroll_row < 0)
    g_scroll_row = 0;
  if (g_scroll_row > max_row)
    g_scroll_row = max_row;
}

static void render(void) {
  int sw = screen->width_in_pixels;
  int sh = screen->height_in_pixels;

  cairo_surface_t *bb_surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
  cairo_t *bb = cairo_create(bb_surface);

  /* FIX 3: Dynamic Grid Margins (Left/Right Gap Reduction) */
  int custom_side_margin = SIDE_MARGIN;
  int avail_w = sw - 2 * custom_side_margin;
  g_cols = (avail_w + TILE_GAP) / (TILE_W + TILE_GAP);
  if (g_cols < 1)
    g_cols = 1;

  /* Fully transparent canvas: let the compositor show the desktop/blur behind it. */
  cairo_set_operator(bb, CAIRO_OPERATOR_CLEAR);
  cairo_paint(bb);
  cairo_set_operator(bb, CAIRO_OPERATOR_OVER);

  /* Search Box Setup */
  double box_w = sw - 48 > 760 ? 760 : sw - 48;
  double box_x = (sw - box_w) / 2.0;
  double box_y = SEARCH_BOX_Y;

  rounded_rect(bb, box_x, box_y, box_w, SEARCH_BOX_H, 26);
  cairo_set_source_rgba(bb, 1.0, 1.0, 1.0, 0.12);
  cairo_fill(bb);

  cairo_select_font_face(bb, "Sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(bb, 16);

  /* FIX 2: Search Bar Text Clipping (Prevents Overflow) */
  cairo_save(bb);
  cairo_rectangle(bb, box_x + 18, box_y, box_w - 58, SEARCH_BOX_H);
  cairo_clip(bb);

  if (g_search_len == 0) {
    cairo_set_source_rgba(bb, 1, 1, 1, 0.50);
    cairo_move_to(bb, box_x + 20, box_y + SEARCH_BOX_H / 2 + 6);
    cairo_show_text(bb, "All Applications...");
  } else {
    cairo_set_source_rgba(bb, 1, 1, 1, 0.95);
    cairo_move_to(bb, box_x + 20, box_y + SEARCH_BOX_H / 2 + 6);
    cairo_show_text(bb, g_search);
  }
  cairo_restore(bb);

  /* Search Glyph */
  double sx = box_x + box_w - 25;
  double sy = box_y + SEARCH_BOX_H / 2.0 - 3;

  cairo_save(bb);
  cairo_new_path(bb);
  cairo_set_source_rgba(bb, 1, 1, 1, 0.72);
  cairo_set_line_width(bb, 2.0);
  cairo_arc(bb, sx - 3, sy - 2, 7, 0, 2 * M_PI);
  cairo_stroke(bb);

  cairo_new_path(bb);
  cairo_move_to(bb, sx + 2, sy + 3);
  cairo_line_to(bb, sx + 8, sy + 9);
  cairo_stroke(bb);
  cairo_restore(bb);

  /* FIX 3: Grid Bottom & Top Gap Reduction */
  int grid_top = TOP_MARGIN;
  int grid_bottom = sh - BOTTOM_MARGIN;
  int usable_h = grid_bottom - grid_top;
  int visible_rows = usable_h / (TILE_H + TILE_GAP);
  if (visible_rows < 1)
    visible_rows = 1;

  clamp_scroll(visible_rows);

  int grid_w = g_cols * TILE_W + (g_cols - 1) * TILE_GAP;
  int grid_x0 = (sw - grid_w) / 2;

  if (g_filtered_count == 0) {
    cairo_set_source_rgba(bb, 1, 1, 1, 0.65);
    cairo_set_font_size(bb, 18);
    cairo_text_extents_t ext;
    const char *msg = "No matching apps";
    cairo_text_extents(bb, msg, &ext);
    cairo_move_to(bb, (sw - ext.width) / 2, grid_top + 45);
    cairo_show_text(bb, msg);
  } else {
    int start_idx = g_scroll_row * g_cols;
    int end_idx = start_idx + visible_rows * g_cols;
    if (end_idx > g_filtered_count)
      end_idx = g_filtered_count;

    for (int fi = start_idx; fi < end_idx; fi++) {
      int rel = fi - start_idx;
      int col = rel % g_cols;
      int row = rel / g_cols;
      double x = grid_x0 + col * (TILE_W + TILE_GAP);
      double y = grid_top + row * (TILE_H + TILE_GAP);

      AppEntry *app = &g_apps[g_filtered[fi]];
      int selected = (fi == g_selected);
      ensure_app_icon(app);

      rounded_rect(bb, x, y, TILE_W, TILE_H, 18);
      cairo_set_source_rgba(bb, 1, 1, 1, selected ? 0.16 : 0.065);
      cairo_fill_preserve(bb);

      cairo_set_source_rgba(bb, 1, 1, 1, selected ? 0.70 : 0.13);
      cairo_set_line_width(bb, selected ? 2.0 : 1.0);
      cairo_stroke(bb);

      if (selected) {
        cairo_save(bb);
        cairo_set_source_rgba(bb, 0.0, 0.0, 0.0, 0.20);
        cairo_arc(bb, x + TILE_W / 2.0, y + 41, 38, 0, 2 * M_PI);
        cairo_fill(bb);
        cairo_restore(bb);
      }

      double icon_size = 54;
      double icon_x = x + (TILE_W - icon_size) / 2;
      double icon_y = y + 9;

      if (app->pixbuf) {
        cairo_save(bb);
        gdk_cairo_set_source_pixbuf(bb, app->pixbuf, icon_x, icon_y);
        cairo_rectangle(bb, icon_x, icon_y, icon_size, icon_size);
        cairo_fill(bb);
        cairo_restore(bb);
      } else {
        double r, g, b;
        name_to_color(app->name, &r, &g, &b);
        rounded_rect(bb, icon_x, icon_y, icon_size, icon_size, 14);
        cairo_set_source_rgba(bb, r, g, b, 0.90);
        cairo_fill(bb);

        char letter[2] = {0};
        letter[0] = (char)toupper((unsigned char)app->name[0]);
        cairo_set_source_rgba(bb, 1, 1, 1, 0.95);
        cairo_select_font_face(bb, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(bb, 24);
        cairo_text_extents_t lext;
        cairo_text_extents(bb, letter, &lext);
        cairo_move_to(bb,
                      icon_x + (icon_size - lext.width) / 2 - lext.x_bearing,
                      icon_y + (icon_size - lext.height) / 2 - lext.y_bearing);
        cairo_show_text(bb, letter);
      }

      cairo_select_font_face(bb, "Sans", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(bb, 12);
      cairo_set_source_rgba(bb, 1, 1, 1, 0.84);

      cairo_text_extents_t next;
      cairo_text_extents(bb, app->name, &next);
      double label_y = y + TILE_H - 9;
      cairo_move_to(
          bb,
          x + (TILE_W - (next.width > TILE_W - 12 ? TILE_W - 12 : next.width)) /
                  2.0,
          label_y);
      draw_ellipsized(bb, app->name, TILE_W - 12);
    }
  }

  /* Bottom Help Bar */
  cairo_set_source_rgba(bb, 1, 1, 1, 0.40);
  cairo_set_font_size(bb, 12);
  cairo_move_to(bb, 22, sh - 10);
  cairo_show_text(
      bb, "Type to search   \xE2\x86\x91\xE2\x86\x93\xE2\x86\x90\xE2\x86\x92 "
          "Navigate   Enter Launch   Esc Close");

  /* Double-Buffer Blitting */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, bb_surface, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  cairo_destroy(bb);
  cairo_surface_destroy(bb_surface);

  cairo_surface_flush(surface);
  xcb_flush(conn);
}

static void scroll_rows(int delta) {
  int sh = screen->height_in_pixels;
  int visible_rows = (sh - TOP_MARGIN - BOTTOM_MARGIN) / (TILE_H + TILE_GAP);
  if (visible_rows < 1)
    visible_rows = 1;

  g_scroll_row += delta;
  clamp_scroll(visible_rows);

  int sel_row = g_selected / g_cols;
  int sel_col = g_selected % g_cols;

  if (sel_row < g_scroll_row) {
    g_selected = g_scroll_row * g_cols + sel_col;
  } else if (sel_row >= g_scroll_row + visible_rows) {
    g_selected = (g_scroll_row + visible_rows - 1) * g_cols + sel_col;
  }

  if (g_selected >= g_filtered_count) {
    g_selected = g_filtered_count > 0 ? g_filtered_count - 1 : 0;
  }

  render();
}

static int tile_at(int px, int py) {
  int sw = screen->width_in_pixels;
  int avail_w = sw - 2 * SIDE_MARGIN;
  int cols = (avail_w + TILE_GAP) / (TILE_W + TILE_GAP);
  if (cols < 1)
    cols = 1;
  int grid_w = cols * TILE_W + (cols - 1) * TILE_GAP;
  int grid_x0 = (sw - grid_w) / 2;

  if (py < TOP_MARGIN)
    return -1;
  int rel_y = py - TOP_MARGIN;
  int row_in_view = rel_y / (TILE_H + TILE_GAP);
  if (rel_y % (TILE_H + TILE_GAP) > TILE_H)
    return -1;

  int col = -1;
  for (int c = 0; c < cols; c++) {
    int tx = grid_x0 + c * (TILE_W + TILE_GAP);
    if (px >= tx && px < tx + TILE_W) {
      col = c;
      break;
    }
  }
  if (col < 0)
    return -1;

  int start_idx = g_scroll_row * cols;
  int fi = start_idx + row_in_view * cols + col;
  if (fi < 0 || fi >= g_filtered_count)
    return -1;
  return fi;
}

/* ---------------------------------------------------------------- */
/* Main Loop                                                         */
/* ---------------------------------------------------------------- */

int main(int argc, char **argv) {
  if (!gdk_init_check(&argc, &argv)) {
    fprintf(stderr, "applauncher: cannot initialize GDK\n");
    return 1;
  }

  discover_apps();
  load_app_icons();
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

  xcb_depth_t *argb_depth = NULL;
  xcb_visualtype_t *argb_vis = find_argb_visual(&argb_depth);
  xcb_visualtype_t *vis = NULL;
  uint8_t depth = screen->root_depth;
  xcb_colormap_t cmap = XCB_COLORMAP_NONE;
  int have_alpha = 0;

  if (argb_vis && argb_depth && argb_depth->depth == 32) {
    vis = argb_vis;
    depth = argb_depth->depth;
    have_alpha = 1;

    cmap = xcb_generate_id(conn);
    xcb_create_colormap(conn, XCB_COLORMAP_ALLOC_NONE, cmap, screen->root,
                        vis->visual_id);
  } else {
    depth = screen->root_depth;
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

    fprintf(
        stderr,
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

  uint32_t mask = 0;
  uint32_t values[5];
  int idx = 0;

  if (have_alpha) {
    mask |= XCB_CW_BACK_PIXEL;
    values[idx++] = 0;

    mask |= XCB_CW_BORDER_PIXEL;
    values[idx++] = 0;

    mask |= XCB_CW_OVERRIDE_REDIRECT;
    values[idx++] = 1;

    mask |= XCB_CW_EVENT_MASK;
    values[idx++] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
                    XCB_EVENT_MASK_BUTTON_PRESS;

    mask |= XCB_CW_COLORMAP;
    values[idx++] = cmap;
  } else {
    mask |= XCB_CW_BACK_PIXEL;
    values[idx++] = screen->black_pixel;

    mask |= XCB_CW_OVERRIDE_REDIRECT;
    values[idx++] = 1;

    mask |= XCB_CW_EVENT_MASK;
    values[idx++] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS |
                    XCB_EVENT_MASK_BUTTON_PRESS;
  }

  xcb_void_cookie_t create_cookie = xcb_create_window_checked(
      conn, depth, win, screen->root, 0, 0, (uint16_t)screen->width_in_pixels,
      (uint16_t)screen->height_in_pixels, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
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

  if (have_alpha)
    set_blur_region();

  xcb_flush(conn);

  surface = cairo_xcb_surface_create(conn, win, vis, screen->width_in_pixels,
                                     screen->height_in_pixels);

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "applauncher: cairo surface creation failed\n");
    if (cmap != XCB_COLORMAP_NONE)
      xcb_free_colormap(conn, cmap);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 1;
  }

  cr = cairo_create(surface);
  if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "applauncher: cairo context creation failed\n");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    if (cmap != XCB_COLORMAP_NONE)
      xcb_free_colormap(conn, cmap);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 1;
  }

  xcb_key_symbols_t *keysyms = xcb_key_symbols_alloc(conn);
  if (!keysyms) {
    fprintf(stderr, "applauncher: cannot initialize keyboard symbols\n");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    if (cmap != XCB_COLORMAP_NONE)
      xcb_free_colormap(conn, cmap);
    xcb_destroy_window(conn, win);
    xcb_disconnect(conn);
    return 1;
  }

  render();

  int running = 1;
  xcb_generic_event_t *ev;

  while (running && (ev = xcb_wait_for_event(conn))) {
    switch (ev->response_type & ~0x80) {
    case XCB_EXPOSE:
      render();
      break;

    case XCB_BUTTON_PRESS: {
      xcb_button_press_event_t *be = (xcb_button_press_event_t *)ev;

      if (be->detail == 4) {
        scroll_rows(-1);
        break;
      }
      if (be->detail == 5) {
        scroll_rows(1);
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
      xcb_keysym_t sym =
      xcb_key_symbols_get_keysym(keysyms, ke->detail, col_idx);

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
          recompute_filter();
          render();
        }
      } else if (sym == XK_Left) {
        if (g_selected > 0) {
          g_selected--;

          int visible_rows =
          (screen->height_in_pixels - TOP_MARGIN - BOTTOM_MARGIN) /
          (TILE_H + TILE_GAP);
          if (visible_rows < 1)
            visible_rows = 1;

          int sel_row = g_selected / g_cols;
          if (sel_row < g_scroll_row)
            g_scroll_row = sel_row;

          clamp_scroll(visible_rows);
          render();
        }
      } else if (sym == XK_Right) {
        if (g_selected + 1 < g_filtered_count) {
          g_selected++;

          int visible_rows =
          (screen->height_in_pixels - TOP_MARGIN - BOTTOM_MARGIN) /
          (TILE_H + TILE_GAP);
          if (visible_rows < 1)
            visible_rows = 1;

          int sel_row = g_selected / g_cols;
          if (sel_row >= g_scroll_row + visible_rows)
            g_scroll_row = sel_row - visible_rows + 1;

          clamp_scroll(visible_rows);
          render();
        }
      } else if (sym == XK_Up) {
        if (g_selected - g_cols >= 0) {
          g_selected -= g_cols;

          int visible_rows =
          (screen->height_in_pixels - TOP_MARGIN - BOTTOM_MARGIN) /
          (TILE_H + TILE_GAP);
          if (visible_rows < 1)
            visible_rows = 1;

          int sel_row = g_selected / g_cols;
          if (sel_row < g_scroll_row)
            g_scroll_row = sel_row;

          clamp_scroll(visible_rows);
          render();
        }
      } else if (sym == XK_Down) {
        if (g_selected + g_cols < g_filtered_count) {
          g_selected += g_cols;

          int visible_rows =
          (screen->height_in_pixels - TOP_MARGIN - BOTTOM_MARGIN) /
          (TILE_H + TILE_GAP);
          if (visible_rows < 1)
            visible_rows = 1;

          int sel_row = g_selected / g_cols;
          if (sel_row >= g_scroll_row + visible_rows)
            g_scroll_row = sel_row - visible_rows + 1;

          clamp_scroll(visible_rows);
          render();
        }
      } else if (sym >= 0x20 && sym <= 0x7e) {
        if (g_search_len + 1 < SEARCH_LEN) {
          g_search[g_search_len++] = (char)sym;
          g_search[g_search_len] = '\0';
          recompute_filter();
          render();
        }
      }
      break;
    }

    default:
      break;
    }

    free(ev);
  }

  xcb_key_symbols_free(keysyms);
  xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);

  free_app_icons();

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  xcb_destroy_window(conn, win);

  if (cmap != XCB_COLORMAP_NONE)
    xcb_free_colormap(conn, cmap);

  xcb_flush(conn);
  xcb_disconnect(conn);
  return 0;
}
