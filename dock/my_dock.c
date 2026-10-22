#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <librsvg/rsvg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>

#define DOCK_HEIGHT 52
#define ICON_SIZE 34
#define PADDING 9
#define MAX_GROUPS 12
#define MAX_WINDOWS_PER_GROUP 10
#define BOTTOM_MARGIN 12      // gap between dock and screen edge
#define CORNER_RADIUS 18      // dock outer corner radius
#define ICON_RADIUS 9         // per-icon corner radius

typedef struct {
    char *class_name;
    char *icon_path;
    Window windows[MAX_WINDOWS_PER_GROUP];
    int window_count;
} AppGroup;

// Global desktop cache
static GHashTable *desktop_cache = NULL;
// Cache of resolved icon paths per class_name
static GHashTable *icon_resolve_cache = NULL;

// Popup Window state
static Window popup_win = None;
static int hovered_group_idx = -1;
static int current_window_cycle_index[MAX_GROUPS] = {0};
static int popup_visible = 0;
static int popup_geo_x = 0, popup_geo_y = 0, popup_geo_w = 0, popup_geo_h = 0;

// Bouncing animation variables
static int bouncing_group_idx = -1;
static double bounce_progress = 0.0;
static const double BOUNCE_DURATION = 0.35; // duration in seconds
static const double BOUNCE_HEIGHT = 12.0;   // max bounce height in pixels

// Live dock window geometry
static Window dock_win = None;
static cairo_surface_t *dock_surface = NULL;
static cairo_t *dock_cr = NULL;
static int g_dock_x = 0, g_dock_y = 0, g_dock_w = 0;
static int g_screen_width = 0, g_screen_height = 0, g_screen = 0;
static Display *g_display = NULL;
static XVisualInfo g_vinfo;

void trigger_icon_bounce(int group_idx) {
    bouncing_group_idx = group_idx;
    bounce_progress = 0.0;
}

// Returns 1 if the given root-relative point lies inside the popup's rect
int point_in_popup(int root_x, int root_y) {
    if (!popup_visible) return 0;
    return root_x >= popup_geo_x && root_x <= popup_geo_x + popup_geo_w &&
    root_y >= popup_geo_y && root_y <= popup_geo_y + popup_geo_h;
}

// Draws a rounded-rectangle path
void rounded_rect_path(cairo_t *cr, double x, double y, double w, double h, double r) {
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

int compute_dock_width(int group_count) {
    int apps_start = PADDING;
    int content_end;
    if (group_count > 0) {
        content_end = apps_start + group_count * (ICON_SIZE + PADDING);
    } else {
        content_end = PADDING;
    }
    // Rofi icon ke baad sirf ek PADDING ki zaroorat hai right side par
    return content_end + ICON_SIZE + PADDING;
}

void draw_app_grid_icon(cairo_t *cr, double x, double y, double size, double r, double g, double b, double a) {
    int cols = 3, rows = 3;
    double margin = size * 0.16;
    double gap = size * 0.10;
    double cell = (size - margin * 2 - gap * (cols - 1)) / cols;
    double dot_r = cell / 2.0;

    cairo_set_source_rgba(cr, r, g, b, a);
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            double cx = x + margin + dot_r + col * (cell + gap);
            double cy = y + margin + dot_r + row * (cell + gap);
            cairo_arc(cr, cx, cy, dot_r, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
}

void redraw_dock(void);
void hide_popup(Display *display);

static int is_dock_hidden = 0;

void check_auto_hide(Display *display, Window root, Window win) {
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;

    XQueryPointer(display, root, &root_ret, &child_ret, &root_x, &root_y, &win_x, &win_y, &mask);

    // int trigger_zone_y = g_screen_height - 8;
    int over_dock = (root_x >= g_dock_x && root_x <= g_dock_x + g_dock_w &&
    root_y >= g_dock_y && root_y <= g_dock_y + DOCK_HEIGHT);

    // if (root_y >= trigger_zone_y || over_dock) {
    if (over_dock) {
        if (is_dock_hidden) {
            XMapRaised(display, win);
            is_dock_hidden = 0;
            redraw_dock();
        }
    } else {
        if (!is_dock_hidden && !point_in_popup(root_x, root_y)) {
            XUnmapWindow(display, win);
            hide_popup(display);
            is_dock_hidden = 1;
        }
    }
}

void draw_icon(cairo_t *cr, const char *icon_path, double x, double y, double width, double height);
void draw_grouped_apps(cairo_t *cr, int start_x, AppGroup *groups, int count);
AppGroup* get_grouped_apps(Display *display, int *group_count);
char* get_rofi_icon_path();

void activate_window(Display *display, Window win) {
    Atom net_active = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    XEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.type = ClientMessage;
    xev.xclient.window = win;
    xev.xclient.message_type = net_active;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 2;
    xev.xclient.data.l[1] = CurrentTime;

    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XRaiseWindow(display, win);
    XSetInputFocus(display, win, RevertToParent, CurrentTime);

    if (dock_win != None) {
        XRaiseWindow(display, dock_win);
    }

    if (popup_visible && popup_win != None) {
        XRaiseWindow(display, popup_win);
    }

    XFlush(display);
}

char* get_window_title(Display *display, Window win) {
    Atom net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(display, win, net_wm_name, 0, 1024, False,
        XInternAtom(display, "UTF8_STRING", False),
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop) {
        char *title = strdup((char *)prop);
    XFree(prop);
    return title;
                           }

                           char *wm_name = NULL;
                           if (XFetchName(display, win, &wm_name) && wm_name) {
                               char *title = strdup(wm_name);
                               XFree(wm_name);
                               return title;
                           }

                           return strdup("Window");
}

char* get_window_class(Display *display, Window window) {
    XClassHint *class_hint = XAllocClassHint();
    char *class_name = NULL;

    if (XGetClassHint(display, window, class_hint)) {
        if (class_hint->res_class) {
            class_name = strdup(class_hint->res_class);
        }
    }
    XFree(class_hint);
    return class_name;
}

int file_exists(const char *path) {
    return path && g_file_test(path, G_FILE_TEST_EXISTS);
}

void show_popup(Display *display, int screen, int abs_dock_x, int abs_dock_y, AppGroup *group) {
    if (!group || group->window_count == 0) return;

    int popup_width = 220;
    int item_height = 30;
    int row_gap = 3;
    int popup_height = group->window_count * item_height + 10;

    int popup_x = abs_dock_x - (popup_width / 2) + (ICON_SIZE / 2);
    int popup_y = abs_dock_y - popup_height - 10;

    if (popup_x < 0) popup_x = 4;
    if (popup_x + popup_width > g_screen_width) popup_x = g_screen_width - popup_width - 4;

    popup_geo_x = popup_x;
    popup_geo_y = popup_y;
    popup_geo_w = popup_width;
    popup_geo_h = popup_height;
    popup_visible = 1;

    if (!popup_win) {
        XVisualInfo vinfo;
        XMatchVisualInfo(display, screen, 32, TrueColor, &vinfo);

        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        attrs.background_pixel = 0;
        attrs.border_pixel = 0;
        attrs.colormap = XCreateColormap(display, DefaultRootWindow(display), vinfo.visual, AllocNone);

        popup_win = XCreateWindow(
            display, DefaultRootWindow(display),
                                  popup_x, popup_y, popup_width, popup_height, 0,
                                  vinfo.depth, InputOutput, vinfo.visual,
                                  CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap, &attrs
        );

        Atom wm_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        Atom wm_tooltip = XInternAtom(display, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
        XChangeProperty(display, popup_win, wm_type, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&wm_tooltip, 1);

        Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
        Atom state_above = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
        XChangeProperty(display, popup_win, net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&state_above, 1);

        XSelectInput(display, popup_win, ExposureMask | ButtonPressMask | LeaveWindowMask);
    } else {
        XMoveResizeWindow(display, popup_win, popup_x, popup_y, popup_width, popup_height);
    }

    XMapRaised(display, popup_win);
    XRaiseWindow(display, popup_win);

    cairo_surface_t *surface = cairo_xlib_surface_create(
        display, popup_win, g_vinfo.visual, popup_width, popup_height
    );

    cairo_t *cr = cairo_create(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    rounded_rect_path(cr, 0.5, 0.5, popup_width - 1, popup_height - 1, 12);
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);

    for (int i = 0; i < group->window_count; i++) {
        char *title = get_window_title(display, group->windows[i]);
        double row_y = 5 + (i * item_height);

        rounded_rect_path(cr, 5, row_y, popup_width - 10, item_height - row_gap, 8);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.06);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.35, 0.75, 1.0, 0.95);
        cairo_arc(cr, 16, row_y + (item_height - row_gap) / 2.0, 2.5, 0, 2 * M_PI);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
        cairo_move_to(cr, 26, row_y + (item_height - row_gap) / 2.0 + 4);

        if (strlen(title) > 26) title[26] = '\0';
        cairo_show_text(cr, title);
        free(title);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    XRaiseWindow(display, popup_win);
    XFlush(display);
}

void hide_popup(Display *display) {
    if (popup_win) {
        XUnmapWindow(display, popup_win);
        hovered_group_idx = -1;
        popup_visible = 0;
    }
}

char* find_icon_recursive(const char *root_dir, const char *icon_name, int depth) {
    if (!root_dir || depth > 6) return NULL;

    DIR *dir = opendir(root_dir);
    if (!dir) return NULL;

    struct dirent *entry;
    char *result = NULL;
    char *subdirs[256];
    int subdir_count = 0;

    const char *extensions[] = {".svg", ".png", ".xpm"};

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *full_path = g_build_filename(root_dir, entry->d_name, NULL);
        struct stat st;
        if (stat(full_path, &st) != 0) {
            g_free(full_path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (subdir_count < 256) {
                subdirs[subdir_count++] = full_path;
            } else {
                g_free(full_path);
            }
            continue;
        }

        for (size_t e = 0; e < sizeof(extensions)/sizeof(extensions[0]); e++) {
            char *target = g_strdup_printf("%s%s", icon_name, extensions[e]);
            if (strcmp(entry->d_name, target) == 0) {
                g_free(target);
                result = full_path;
                break;
            }
            g_free(target);
        }

        if (result) break;
        g_free(full_path);
    }
    closedir(dir);

    if (!result) {
        for (int i = 0; i < subdir_count; i++) {
            if (!result) {
                result = find_icon_recursive(subdirs[i], icon_name, depth + 1);
            }
            g_free(subdirs[i]);
        }
    } else {
        for (int i = 0; i < subdir_count; i++) g_free(subdirs[i]);
    }

    return result;
}

char* find_icon_in_system(const char *icon_name) {
    if (!icon_name) return NULL;

    if (icon_name[0] == '/' && file_exists(icon_name)) {
        return strdup(icon_name);
    }

    if (strstr(icon_name, ".")) {
        if (file_exists(icon_name)) {
            return strdup(icon_name);
        }
    }

    const char *extensions[] = {".svg", ".png", ".xpm", ""};

    const char *dirs[] = {
        "/usr/local/share/icons/hicolor/scalable/apps",
        "/usr/local/share/icons/hicolor/48x48/apps",
        "/usr/local/share/icons/hicolor/256x256/apps",
        "/usr/local/share/icons/hicolor/128x128/apps",
        "/usr/local/share/icons/hicolor/64x64/apps",
        "/usr/local/share/icons/hicolor/32x32/apps",
        "/usr/local/share/icons/hicolor/24x24/apps",
        "/usr/local/share/icons/hicolor/22x22/apps",
        "/usr/local/share/icons/hicolor/16x16/apps",
        "/usr/local/share/icons",
        "/usr/local/share/pixmaps",
        "/usr/local/pixmaps",
        "/usr/local/icons",
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/256x256/apps",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/32x32/apps",
        "/usr/share/icons/hicolor/24x24/apps",
        "/usr/share/icons/hicolor/22x22/apps",
        "/usr/share/icons/hicolor/16x16/apps",
        "/usr/share/icons/Adwaita/scalable/apps",
        "/usr/share/icons/Papirus/scalable/apps",
        "/usr/share/pixmaps",
        "/usr/share/icons",
        "/opt/icons",
        "/opt/share/icons",
        "/opt/share/pixmaps",
        "/opt/share/apps/icons"
    };

    char *local_icon_dir = g_build_filename(g_get_home_dir(), ".local/share/icons", NULL);
    char *local_apps_dir = g_build_filename(g_get_home_dir(), ".local/share/applications", NULL);
    char *local_pixmaps = g_build_filename(g_get_home_dir(), ".local/share/pixmaps", NULL);
    char *local_icons = g_build_filename(g_get_home_dir(), ".icons", NULL);

    const char *user_dirs[] = {
        local_icon_dir,
        local_apps_dir,
        local_pixmaps,
        local_icons,
        NULL
    };

    for (size_t d = 0; user_dirs[d]; d++) {
        for (size_t e = 0; e < sizeof(extensions)/sizeof(extensions[0]); e++) {
            char *full_path = g_strdup_printf("%s/%s%s", user_dirs[d], icon_name, extensions[e]);
            if (file_exists(full_path)) {
                g_free(local_icon_dir);
                g_free(local_apps_dir);
                g_free(local_pixmaps);
                g_free(local_icons);
                return full_path;
            }
            g_free(full_path);
        }
    }

    g_free(local_icon_dir);
    g_free(local_apps_dir);
    g_free(local_pixmaps);
    g_free(local_icons);

    for (size_t d = 0; d < sizeof(dirs)/sizeof(dirs[0]); d++) {
        for (size_t e = 0; e < sizeof(extensions)/sizeof(extensions[0]); e++) {
            char *full_path = g_strdup_printf("%s/%s%s", dirs[d], icon_name, extensions[e]);
            if (file_exists(full_path)) {
                return full_path;
            }
            g_free(full_path);
        }
    }

    const char *recursive_roots[] = {
        "/usr/share/icons",
        "/usr/local/share/icons",
        "/usr/share/pixmaps",
        "/usr/local/share/pixmaps",
        "/opt/icons",
        "/opt/share/icons"
    };
    char *user_icons_1 = g_build_filename(g_get_home_dir(), ".local/share/icons", NULL);
    char *user_icons_2 = g_build_filename(g_get_home_dir(), ".icons", NULL);

    char *found = NULL;
    for (size_t d = 0; d < sizeof(recursive_roots)/sizeof(recursive_roots[0]); d++) {
        found = find_icon_recursive(recursive_roots[d], icon_name, 0);
        if (found) break;
    }
    if (!found) found = find_icon_recursive(user_icons_1, icon_name, 0);
    if (!found) found = find_icon_recursive(user_icons_2, icon_name, 0);

    g_free(user_icons_1);
    g_free(user_icons_2);
    if (found) return found;

    return NULL;
}

void scan_desktop_directory(const char *dir_path, GHashTable *cache) {
    if (!dir_path) return;

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char *filename = entry->d_name;
        if (!g_str_has_suffix(filename, ".desktop")) continue;

        char *full_path = g_build_filename(dir_path, filename, NULL);
        GKeyFile *key_file = g_key_file_new();

        if (g_key_file_load_from_file(key_file, full_path, G_KEY_FILE_NONE, NULL)) {
            char *exec = g_key_file_get_string(key_file, "Desktop Entry", "Exec", NULL);
            char *icon = g_key_file_get_string(key_file, "Desktop Entry", "Icon", NULL);
            char *name = g_key_file_get_string(key_file, "Desktop Entry", "Name", NULL);
            char *wm_class = g_key_file_get_string(key_file, "Desktop Entry", "StartupWMClass", NULL);

            if (wm_class && icon) {
                g_hash_table_insert(cache, g_strdup(wm_class), g_strdup(icon));
                char *wm_class_lower = g_utf8_strdown(wm_class, -1);
                g_hash_table_insert(cache, wm_class_lower, g_strdup(icon));
            }
            g_free(wm_class);

            if (exec && icon) {
                char *exec_first = strdup(exec);
                char *space = strchr(exec_first, ' ');
                if (space) *space = '\0';

                char *basename = g_path_get_basename(filename);
                char *dot = strrchr(basename, '.');
                if (dot) *dot = '\0';

                g_hash_table_insert(cache, g_strdup(exec_first), g_strdup(icon));
                g_hash_table_insert(cache, g_strdup(basename), g_strdup(icon));

                char *exec_lower = g_utf8_strdown(exec_first, -1);
                g_hash_table_insert(cache, exec_lower, g_strdup(icon));

                char *base_lower = g_utf8_strdown(basename, -1);
                g_hash_table_insert(cache, base_lower, g_strdup(icon));

                if (name) {
                    char *name_lower = g_utf8_strdown(name, -1);
                    g_hash_table_insert(cache, name_lower, g_strdup(icon));
                    g_hash_table_insert(cache, g_strdup(name), g_strdup(icon));
                    g_free(name);
                }

                g_free(exec_first);
                g_free(basename);
            }

            g_free(exec);
            g_free(icon);
            g_key_file_free(key_file);
        }
        g_free(full_path);
    }
    closedir(dir);
}

void build_desktop_cache() {
    if (desktop_cache) {
        g_hash_table_destroy(desktop_cache);
    }

    desktop_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    char *local_desktop_dir = g_build_filename(g_get_home_dir(), ".local/share/applications", NULL);
    scan_desktop_directory(local_desktop_dir, desktop_cache);
    g_free(local_desktop_dir);

    const char *desktop_dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        "/usr/local/applications",
        "/opt/share/applications",
        "/opt/applications",
        "/usr/share/apps",
        "/usr/local/share/apps",
        "/var/lib/snapd/desktop/applications",
        NULL
    };

    for (size_t i = 0; desktop_dirs[i]; i++) {
        scan_desktop_directory(desktop_dirs[i], desktop_cache);
    }
}

char* get_icon_from_desktop(const char *class_name) {
    if (!class_name || !desktop_cache) return NULL;

    char *icon = g_hash_table_lookup(desktop_cache, class_name);
    if (icon) return g_strdup(icon);

    char *lower = g_utf8_strdown(class_name, -1);
    icon = g_hash_table_lookup(desktop_cache, lower);
    g_free(lower);
    if (icon) return g_strdup(icon);

    char *variations[6];
    int count = 0;

    char *no_suffix = g_strdup(class_name);
    char *suffixes[] = {"-bin", "-main", "-app", "-client", "-server", NULL};
    for (int i = 0; suffixes[i]; i++) {
        char *found = strstr(no_suffix, suffixes[i]);
        if (found) {
            *found = '\0';
            break;
        }
    }
    variations[count++] = no_suffix;

    char *no_prefix = g_strdup(class_name);
    char *prefixes[] = {"lib", "org.", "com.", "net.", "io.", NULL};
    for (int i = 0; prefixes[i]; i++) {
        if (g_str_has_prefix(no_prefix, prefixes[i])) {
            char *new = g_strdup(no_prefix + strlen(prefixes[i]));
            g_free(no_prefix);
            no_prefix = new;
            break;
        }
    }
    variations[count++] = no_prefix;

    char *with_spaces = g_strdup(class_name);
    for (char *p = with_spaces; *p; p++) {
        if (*p == '_' || *p == '-') *p = ' ';
    }
    variations[count++] = with_spaces;

    for (int i = 0; i < count; i++) {
        if (variations[i]) {
            char *var_lower = g_utf8_strdown(variations[i], -1);
            icon = g_hash_table_lookup(desktop_cache, var_lower);
            if (icon) {
                g_free(var_lower);
                for (int j = 0; j < count; j++) g_free(variations[j]);
                return g_strdup(icon);
            }
            g_free(var_lower);

            char *capitalized = g_strdup(variations[i]);
            if (capitalized[0]) capitalized[0] = toupper(capitalized[0]);
            icon = g_hash_table_lookup(desktop_cache, capitalized);
            g_free(capitalized);
            if (icon) {
                for (int j = 0; j < count; j++) g_free(variations[j]);
                return g_strdup(icon);
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (variations[i]) g_free(variations[i]);
    }

    return NULL;
}

static char* resolve_icon_path_for_class_uncached(const char *class_name) {
    char *path = NULL;

    if (!desktop_cache) {
        build_desktop_cache();
    }

    char *desktop_icon = get_icon_from_desktop(class_name);
    if (desktop_icon) {
        if (desktop_icon[0] == '/') {
            if (file_exists(desktop_icon)) {
                return desktop_icon;
            }
        }

        path = find_icon_in_system(desktop_icon);
        g_free(desktop_icon);
        if (path) return path;
    }

    path = find_icon_in_system(class_name);
    if (path) return path;

    char *lower_class = g_utf8_strdown(class_name, -1);
    path = find_icon_in_system(lower_class);
    if (path) {
        g_free(lower_class);
        return path;
    }
    g_free(lower_class);

    char *variations[10];
    int var_count = 0;

    variations[var_count++] = g_strdup_printf("%s-icon", class_name);
    variations[var_count++] = g_strdup_printf("%s-logo", class_name);
    variations[var_count++] = g_strdup_printf("%s-symbolic", class_name);

    char *no_sep = g_strdup(class_name);
    for (char *p = no_sep; *p; p++) {
        if (*p == ' ' || *p == '-' || *p == '_') {
            memmove(p, p+1, strlen(p));
            p--;
        }
    }
    variations[var_count++] = no_sep;

    for (int i = 0; i < var_count; i++) {
        if (variations[i]) {
            char *lower_var = g_utf8_strdown(variations[i], -1);
            path = find_icon_in_system(lower_var);
            g_free(lower_var);
            if (path) {
                for (int j = 0; j < var_count; j++) g_free(variations[j]);
                return path;
            }
        }
    }

    for (int i = 0; i < var_count; i++) {
        if (variations[i]) g_free(variations[i]);
    }

    const char *special_cases[][2] = {
        {"wezterm", "org.wezfurlong.wezterm"},
        {"kitty", "kitty"},
        {"code", "code"},
        {"discord", "discord"},
        {"firefox", "firefox"},
        {"chrome", "google-chrome"},
        {"chromium", "chromium"},
        {"brave", "brave-browser"},
        {"thunderbird", "thunderbird"},
        {"vlc", "vlc"},
        {"spotify", "spotify"},
        {"steam", "steam"},
        {"obs", "obs-studio"},
        {"gimp", "gimp"},
        {"inkscape", "inkscape"},
        {"libreoffice", "libreoffice"},
        {"terminal", "terminal"},
        {"alacritty", "Alacritty"},
        {"foot", "foot"},
        {"st", "st"},
        {"nvim", "neovim"},
        {"vim", "gvim"},
        {"emacs", "emacs"},
        {"geany", "geany"},
        {"gedit", "gedit"},
        {"nano", "nano"},
        {"htop", "htop"}
    };

    char *lower_check = g_utf8_strdown(class_name, -1);

    for (size_t i = 0; i < sizeof(special_cases)/sizeof(special_cases[0]); i++) {
        if (strstr(lower_check, special_cases[i][0])) {
            path = find_icon_in_system(special_cases[i][1]);
            if (path) {
                g_free(lower_check);
                return path;
            }
        }
    }
    g_free(lower_check);

    const char *icon_themes[] = {
        "Papirus", "Adwaita", "oxygen", "gnome", "breeze",
        "elementary", "Numix", "Mint-X", "Mint-Y", "hicolor"
    };

    for (size_t i = 0; i < sizeof(icon_themes)/sizeof(icon_themes[0]); i++) {
        char *theme_path = g_strdup_printf("/usr/share/icons/%s/scalable/apps/%s.svg",
                                           icon_themes[i], class_name);
        if (file_exists(theme_path)) {
            return theme_path;
        }
        g_free(theme_path);

        theme_path = g_strdup_printf("/usr/share/icons/%s/48x48/apps/%s.png",
                                     icon_themes[i], class_name);
        if (file_exists(theme_path)) {
            return theme_path;
        }
        g_free(theme_path);

        char *lower_name = g_utf8_strdown(class_name, -1);
        theme_path = g_strdup_printf("/usr/share/icons/%s/scalable/apps/%s.svg",
                                     icon_themes[i], lower_name);
        g_free(lower_name);
        if (file_exists(theme_path)) {
            return theme_path;
        }
        g_free(theme_path);
    }

    return NULL;
}

char* get_icon_path_for_class(const char *class_name) {
    if (!class_name) return NULL;

    if (!icon_resolve_cache) {
        icon_resolve_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    }

    gpointer cached = g_hash_table_lookup(icon_resolve_cache, class_name);
    if (cached) {
        const char *cached_str = (const char *)cached;
        return cached_str[0] ? g_strdup(cached_str) : NULL;
    }

    char *resolved = resolve_icon_path_for_class_uncached(class_name);

    g_hash_table_insert(icon_resolve_cache, g_strdup(class_name),
                        g_strdup(resolved ? resolved : ""));

    return resolved;
}

char* get_rofi_icon_path() {
    const char *rofi_names[] = {
        "view-app-grid-symbolic",
        "view-grid-symbolic",
        "org.gnome.Software",
        "gnome-panel-launcher",
        "system-search-symbolic",
        "system-search",
        "launcher-program",
        "preferences-system-search",
        "rofi",
        "rofi-symbolic",
        "search",
        "search-symbolic",
        "application-x-executable",
        "applications-other",
        "applications",
        "start-here"
    };

    for (size_t i = 0; i < sizeof(rofi_names)/sizeof(rofi_names[0]); i++) {
        char *path = find_icon_in_system(rofi_names[i]);
        if (path) return path;
    }
    return NULL;
}

AppGroup* get_grouped_apps(Display *display, int *group_count) {
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
    Window root = DefaultRootWindow(display);

    *group_count = 0;
    if (client_list == None) return NULL;

    if (XGetWindowProperty(display, root, client_list, 0, 1024, False,
        XA_WINDOW, &actual_type, &actual_format,
        &nitems, &bytes_after, &prop) != Success || !prop) {
        return NULL;
        }

        Window *windows = (Window *)prop;
    AppGroup *groups = calloc(MAX_GROUPS, sizeof(AppGroup));
    int g_count = 0;

    for (unsigned long i = 0; i < nitems; i++) {
        XWindowAttributes attrs;
        if (!XGetWindowAttributes(display, windows[i], &attrs)) continue;
        if (attrs.map_state != IsViewable) continue;
        if (attrs.width < 50 || attrs.height < 50) continue;

        char *class_name = get_window_class(display, windows[i]);
        if (!class_name) continue;

        int found_idx = -1;
        for (int j = 0; j < g_count; j++) {
            if (strcasecmp(groups[j].class_name, class_name) == 0) {
                found_idx = j;
                break;
            }
        }

        if (found_idx != -1) {
            if (groups[found_idx].window_count < MAX_WINDOWS_PER_GROUP) {
                groups[found_idx].windows[groups[found_idx].window_count++] = windows[i];
            }
            free(class_name);
        } else if (g_count < MAX_GROUPS) {
            groups[g_count].class_name = class_name;
            groups[g_count].icon_path = get_icon_path_for_class(class_name);
            groups[g_count].windows[0] = windows[i];
            groups[g_count].window_count = 1;
            g_count++;
        } else {
            free(class_name);
        }
    }

    XFree(prop);
    *group_count = g_count;
    return groups;
}

void redraw_dock(void) {
    int group_count = 0;
    AppGroup *groups = get_grouped_apps(g_display, &group_count);

    int new_width = compute_dock_width(group_count);
    if (new_width < PADDING * 2 + ICON_SIZE) new_width = PADDING * 2 + ICON_SIZE;
    if (new_width > g_screen_width - 40) new_width = g_screen_width - 40;

    int new_x = (g_screen_width - new_width) / 2;
    int new_y = g_screen_height - DOCK_HEIGHT - BOTTOM_MARGIN;

    if (new_width != g_dock_w || dock_surface == NULL) {
        XMoveResizeWindow(g_display, dock_win, new_x, new_y, new_width, DOCK_HEIGHT);

        Atom wm_strut = XInternAtom(g_display, "_NET_WM_STRUT", False);
        unsigned long strut[4] = {0, 0, 0, DOCK_HEIGHT + BOTTOM_MARGIN};
        XChangeProperty(g_display, dock_win, wm_strut, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)strut, 4);

        if (dock_cr) cairo_destroy(dock_cr);
        if (dock_surface) cairo_surface_destroy(dock_surface);

        dock_surface = cairo_xlib_surface_create(
            g_display, dock_win, g_vinfo.visual, new_width, DOCK_HEIGHT
        );

        dock_cr = cairo_create(dock_surface);
    } else if (new_x != g_dock_x) {
        XMoveWindow(g_display, dock_win, new_x, new_y);
    }

    g_dock_x = new_x;
    g_dock_y = new_y;
    g_dock_w = new_width;

    cairo_surface_t *buffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g_dock_w, DOCK_HEIGHT);
    cairo_t *bcr = cairo_create(buffer);

    cairo_set_operator(bcr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(bcr);
    cairo_set_operator(bcr, CAIRO_OPERATOR_OVER);

    rounded_rect_path(bcr, 0.5, 0.5, g_dock_w - 1, DOCK_HEIGHT - 1, CORNER_RADIUS);
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, DOCK_HEIGHT);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, 0.13, 0.13, 0.15, 0.92);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.07, 0.07, 0.09, 0.92);
    cairo_set_source(bcr, bg);
    cairo_fill_preserve(bcr);
    cairo_pattern_destroy(bg);

    cairo_set_source_rgba(bcr, 1, 1, 1, 0.08);
    cairo_set_line_width(bcr, 1.0);
    cairo_stroke(bcr);

    // 1. Grouped Apps ko Left side par draw karein
    draw_grouped_apps(bcr, PADDING, groups, group_count);

    // 2. Rofi Launcher Icon ko Right side par draw karein
    int rofi_x = PADDING + group_count * (ICON_SIZE + PADDING);
    char *rofi_icon_path = get_rofi_icon_path();
    int launcher_hovered = (hovered_group_idx == -2);

    rounded_rect_path(bcr, rofi_x, PADDING, ICON_SIZE, ICON_SIZE, ICON_RADIUS);
    cairo_set_source_rgba(bcr, 1, 1, 1, launcher_hovered ? 0.16 : 0.09);
    cairo_fill(bcr);

    if (rofi_icon_path) {
        if (g_str_has_suffix(rofi_icon_path, ".svg")) {
            GError *error = NULL;
            RsvgHandle *handle = rsvg_handle_new_from_file(rofi_icon_path, &error);
            if (handle && !error) {
                double sz = ICON_SIZE - 10;
                cairo_surface_t *rec_surf = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, NULL);
                cairo_t *rec_cr = cairo_create(rec_surf);

                RsvgRectangle viewport = { .x = 0, .y = 0, .width = sz, .height = sz };
                rsvg_handle_render_document(handle, rec_cr, &viewport, NULL);
                cairo_destroy(rec_cr);

                cairo_save(bcr);
                cairo_set_source_rgba(bcr, 1.0, 1.0, 1.0, 0.95);
                cairo_mask_surface(bcr, rec_surf, rofi_x + 5, PADDING + 5);
                cairo_restore(bcr);

                cairo_surface_destroy(rec_surf);
                g_object_unref(handle);
            } else {
                if (error) g_error_free(error);
                draw_app_grid_icon(bcr, rofi_x, PADDING, ICON_SIZE, 1.0, 1.0, 1.0, 0.95);
            }
        } else {
            draw_icon(bcr, rofi_icon_path, rofi_x + 5, PADDING + 5, ICON_SIZE - 10, ICON_SIZE - 10);
        }
        free(rofi_icon_path);
    } else {
        draw_app_grid_icon(bcr, rofi_x, PADDING, ICON_SIZE, 1.0, 1.0, 1.0, 0.95);
    }

    for (int i = 0; i < group_count; i++) {
        if (groups[i].class_name) free(groups[i].class_name);
        if (groups[i].icon_path) free(groups[i].icon_path);
    }
    if (groups) free(groups);

    cairo_surface_flush(buffer);

    cairo_set_operator(dock_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(dock_cr, buffer, 0, 0);
    cairo_paint(dock_cr);
    cairo_surface_flush(dock_surface);

    cairo_destroy(bcr);
    cairo_surface_destroy(buffer);
}

void launch_rofi() {
    pid_t pid = fork();
    if (pid == 0) {
        execlp("rofi", "rofi", "-show", "drun", "-display-drun", "Apps", NULL);
        exit(0);
    }
    // if (pid == 0) {
    //     execl("/bin/bash",
    //           "bash",
    //           "/home/hammad/wm/apps-launcher/apps-launcher.sh",
    //           (char *)NULL);
    //
    //     perror("apps-launcher");
    //     _exit(127);
    // }
}

void draw_icon(cairo_t *cr, const char *icon_path, double x, double y, double width, double height) {
    if (!file_exists(icon_path)) return;

    if (g_str_has_suffix(icon_path, ".png") || g_str_has_suffix(icon_path, ".xpm")) {
        cairo_surface_t *image = cairo_image_surface_create_from_png(icon_path);
        if (cairo_surface_status(image) == CAIRO_STATUS_SUCCESS) {
            int img_w = cairo_image_surface_get_width(image);
            int img_h = cairo_image_surface_get_height(image);

            cairo_save(cr);
            cairo_translate(cr, x, y);
            cairo_scale(cr, width / img_w, height / img_h);
            cairo_set_source_surface(cr, image, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(image);
            return;
        }
        cairo_surface_destroy(image);
    } else {
        GError *error = NULL;
        RsvgHandle *handle = rsvg_handle_new_from_file(icon_path, &error);
        if (handle && !error) {
            RsvgRectangle viewport = { .x = x, .y = y, .width = width, .height = height };
            cairo_save(cr);
            rsvg_handle_render_document(handle, cr, &viewport, NULL);
            cairo_restore(cr);
            g_object_unref(handle);
            return;
        }
        if (error) {
            g_error_free(error);
        }
    }
}

void draw_grouped_apps(cairo_t *cr, int start_x, AppGroup *groups, int count) {
    int current_x = start_x;

    for (int i = 0; i < count; i++) {
        int icon_x = current_x;
        int icon_y = PADDING;

        if (i == bouncing_group_idx) {
            double offset = sin(bounce_progress * M_PI) * BOUNCE_HEIGHT;
            icon_y -= (int)offset;
        }

        int is_hovered = (hovered_group_idx == i);

        rounded_rect_path(cr, icon_x, icon_y, ICON_SIZE, ICON_SIZE, ICON_RADIUS);
        cairo_set_source_rgba(cr, 1, 1, 1, is_hovered ? 0.16 : 0.07);
        cairo_fill(cr);

        if (groups[i].icon_path && file_exists(groups[i].icon_path)) {
            draw_icon(cr, groups[i].icon_path, icon_x + 3, icon_y + 3, ICON_SIZE - 6, ICON_SIZE - 6);
        } else {
            rounded_rect_path(cr, icon_x + 4, icon_y + 4, ICON_SIZE - 8, ICON_SIZE - 8, ICON_RADIUS - 4);
            cairo_pattern_t *fallback = cairo_pattern_create_linear(icon_x, icon_y, icon_x, icon_y + ICON_SIZE);
            cairo_pattern_add_color_stop_rgb(fallback, 0.0, 0.35, 0.55, 0.85);
            cairo_pattern_add_color_stop_rgb(fallback, 1.0, 0.25, 0.4, 0.7);
            cairo_set_source(cr, fallback);
            cairo_fill(cr);
            cairo_pattern_destroy(fallback);

            if (groups[i].class_name) {
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, 15);
                cairo_move_to(cr, icon_x + ICON_SIZE / 2.0 - 5, icon_y + ICON_SIZE / 2.0 + 5);
                char letter[2] = {toupper(groups[i].class_name[0]), '\0'};
                cairo_show_text(cr, letter);
            }
        }

        double pill_w = 14, pill_h = 3.2;
        double pill_x = icon_x + (ICON_SIZE - pill_w) / 2.0;
        double pill_y = DOCK_HEIGHT - PADDING + 1;
        rounded_rect_path(cr, pill_x, pill_y, pill_w, pill_h, pill_h / 2.0);
        cairo_set_source_rgba(cr, 0.35, 0.75, 1.0, 0.95);
        cairo_fill(cr);

        if (groups[i].window_count > 1) {
            double badge_r = 8;
            double badge_cx = icon_x + ICON_SIZE - 3;
            double badge_cy = icon_y + 3;

            cairo_pattern_t *badge = cairo_pattern_create_linear(badge_cx - badge_r, badge_cy - badge_r,
                                                                 badge_cx + badge_r, badge_cy + badge_r);
            cairo_pattern_add_color_stop_rgb(badge, 0.0, 0.98, 0.45, 0.35);
            cairo_pattern_add_color_stop_rgb(badge, 1.0, 0.9, 0.2, 0.25);
            cairo_arc(cr, badge_cx, badge_cy, badge_r, 0, 2 * M_PI);
            cairo_set_source(cr, badge);
            cairo_fill(cr);
            cairo_pattern_destroy(badge);

            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 9.5);

            char count_str[8];
            snprintf(count_str, sizeof(count_str), "%d", groups[i].window_count);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, count_str, &ext);
            cairo_move_to(cr, badge_cx - ext.width / 2.0 - ext.x_bearing, badge_cy - ext.height / 2.0 - ext.y_bearing);
            cairo_show_text(cr, count_str);
        }

        current_x += ICON_SIZE + PADDING;
    }
}

int main() {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }
    g_display = display;

    build_desktop_cache();

    int screen = DefaultScreen(display);
    g_screen = screen;
    g_screen_width = DisplayWidth(display, screen);
    g_screen_height = DisplayHeight(display, screen);

    Window root = RootWindow(display, screen);

    XMatchVisualInfo(display, screen, 32, TrueColor, &g_vinfo);

    XSetWindowAttributes wattrs;
    wattrs.override_redirect = False;
    wattrs.background_pixel = 0;
    wattrs.border_pixel = 0;
    wattrs.colormap = XCreateColormap(display, root, g_vinfo.visual, AllocNone);

    int init_w = compute_dock_width(0);
    int init_x = (g_screen_width - init_w) / 2;
    int init_y = g_screen_height - DOCK_HEIGHT - BOTTOM_MARGIN;

    Window win = XCreateWindow(
        display, root,
        init_x, init_y, init_w, DOCK_HEIGHT, 0,
        g_vinfo.depth, InputOutput, g_vinfo.visual,
        CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap, &wattrs
    );
    dock_win = win;
    g_dock_x = init_x;
    g_dock_y = init_y;
    g_dock_w = init_w;

    Atom net_client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    XSelectInput(display, root, PropertyChangeMask);

    Atom wm_type = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_dock = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(display, win, wm_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_dock, 1);

    Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom state_atoms[2];
    state_atoms[0] = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
    state_atoms[1] = XInternAtom(display, "_NET_WM_STATE_STICKY", False);
    XChangeProperty(display, win, net_wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)state_atoms, 2);

    XSelectInput(display, win, ExposureMask | ButtonPressMask | KeyPressMask | PointerMotionMask | LeaveWindowMask);
    XMapWindow(display, win);

    redraw_dock();

    XEvent ev;
    while (1) {
        while (XPending(display)) {
            XNextEvent(display, &ev);

            if (ev.type == Expose) {
                redraw_dock();

            } else if (ev.type == PropertyNotify) {
                if (ev.xproperty.atom == net_client_list) {
                    XEvent peek;
                    while (XPending(display)) {
                        XPeekEvent(display, &peek);
                        if (peek.type == PropertyNotify && peek.xproperty.atom == net_client_list) {
                            XNextEvent(display, &peek);
                        } else {
                            break;
                        }
                    }
                    redraw_dock();
                }

            } else if (ev.type == MotionNotify) {
                int x = ev.xmotion.x;
                int icon_start = PADDING;
                int icon_width = ICON_SIZE + PADDING;

                int group_count = 0;
                AppGroup *groups = get_grouped_apps(display, &group_count);

                int apps_end_x = PADDING + group_count * (ICON_SIZE + PADDING);
                int sep_x = (group_count > 0) ? (apps_end_x - PADDING / 2) : PADDING;

                if (x >= icon_start && x < sep_x) {
                    int group_idx = (x - icon_start) / icon_width;

                    if (groups && group_idx < group_count) {
                        if (hovered_group_idx != group_idx) {
                            hovered_group_idx = group_idx;
                            trigger_icon_bounce(group_idx);

                            int icon_x_local = icon_start + (group_idx * icon_width);
                            show_popup(display, screen, g_dock_x + icon_x_local, g_dock_y, &groups[group_idx]);
                            redraw_dock();
                        }
                    } else if (hovered_group_idx != -1) {
                        hide_popup(display);
                        redraw_dock();
                    }
                } else if (hovered_group_idx != -1) {
                    hide_popup(display);
                    redraw_dock();
                }

                for (int i = 0; i < group_count; i++) {
                    if (groups[i].class_name) free(groups[i].class_name);
                    if (groups[i].icon_path) free(groups[i].icon_path);
                }
                if (groups) free(groups);

            } else if (ev.type == LeaveNotify && (ev.xcrossing.window == win || ev.xcrossing.window == popup_win)) {
                Window root_ret, child_ret;
                int root_x, root_y, win_x, win_y;
                unsigned int mask;
                XQueryPointer(display, root, &root_ret, &child_ret,
                              &root_x, &root_y, &win_x, &win_y, &mask);

                int over_dock = (root_x >= g_dock_x && root_x <= g_dock_x + g_dock_w &&
                root_y >= g_dock_y && root_y <= g_dock_y + DOCK_HEIGHT);

                if (!over_dock && !point_in_popup(root_x, root_y)) {
                    hide_popup(display);
                    redraw_dock();
                }

            } else if (ev.type == ButtonPress) {
                if (ev.xbutton.window == popup_win) {
                    int item_height = 30;
                    int clicked_idx = (ev.xbutton.y - 5) / item_height;

                    int group_count = 0;
                    AppGroup *groups = get_grouped_apps(display, &group_count);

                    if (groups && hovered_group_idx >= 0 && hovered_group_idx < group_count) {
                        if (clicked_idx >= 0 && clicked_idx < groups[hovered_group_idx].window_count) {
                            activate_window(display, groups[hovered_group_idx].windows[clicked_idx]);
                        }
                        for (int i = 0; i < group_count; i++) {
                            if (groups[i].class_name) free(groups[i].class_name);
                            if (groups[i].icon_path) free(groups[i].icon_path);
                        }
                        free(groups);
                    }
                    hide_popup(display);
                    redraw_dock();
                    continue;
                }

                int x = ev.xbutton.x;
                int y = ev.xbutton.y;

                int group_count = 0;
                AppGroup *groups = get_grouped_apps(display, &group_count);

                int apps_end_x = PADDING + group_count * (ICON_SIZE + PADDING);
                int sep_x = (group_count > 0) ? (apps_end_x - PADDING / 2) : PADDING;
                int rofi_x = sep_x + PADDING;

                // Right Side Rofi Click Check
                if (x >= rofi_x && x <= (rofi_x + ICON_SIZE) &&
                    y >= PADDING && y <= (PADDING + ICON_SIZE)) {
                    launch_rofi();
                for (int i = 0; i < group_count; i++) {
                    if (groups[i].class_name) free(groups[i].class_name);
                    if (groups[i].icon_path) free(groups[i].icon_path);
                }
                if (groups) free(groups);
                continue;
                    }

                    // Left Side App Icons Click Check
                    int icon_start = PADDING;
                    int icon_width = ICON_SIZE + PADDING;

                    if (x >= icon_start && x < sep_x) {
                        int group_index = (x - icon_start) / icon_width;

                        if (groups && group_index < group_count) {
                            AppGroup *g = &groups[group_index];

                            trigger_icon_bounce(group_index);
                            hide_popup(display);

                            if (g->window_count == 1) {
                                activate_window(display, g->windows[0]);
                            } else if (g->window_count > 1) {
                                current_window_cycle_index[group_index] =
                                (current_window_cycle_index[group_index] + 1) % g->window_count;
                                activate_window(display, g->windows[current_window_cycle_index[group_index]]);
                            }
                        }
                    }

                    for (int i = 0; i < group_count; i++) {
                        if (groups[i].class_name) free(groups[i].class_name);
                        if (groups[i].icon_path) free(groups[i].icon_path);
                    }
                    if (groups) free(groups);
                    redraw_dock();
            }
        }

        check_auto_hide(display, root, win);

        if (bouncing_group_idx != -1) {
            bounce_progress += 0.016 / BOUNCE_DURATION;
            if (bounce_progress >= 1.0) {
                bouncing_group_idx = -1;
                bounce_progress = 0.0;
            }
            redraw_dock();
        }

        usleep(16666);
    }

    if (desktop_cache) {
        g_hash_table_destroy(desktop_cache);
    }
    if (icon_resolve_cache) {
        g_hash_table_destroy(icon_resolve_cache);
    }

    if (dock_cr) cairo_destroy(dock_cr);
    if (dock_surface) cairo_surface_destroy(dock_surface);
    XCloseDisplay(display);
    return 0;
}
