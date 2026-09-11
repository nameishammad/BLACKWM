#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAIN_WIDTH 360
#define MAIN_HEIGHT 400

typedef struct {
  int x, y, w, h;
  char label[32];
  char status[32];
  int active;
  int has_arrow;
} Tile;

Tile tiles[4] = {{15, 15, 160, 65, "Wi-Fi", "Checking...", 0, 1},
                 {185, 15, 160, 65, "Bluetooth", "Checking...", 0, 1},
                 {15, 90, 160, 65, "Do Not Disturb", "Off", 0, 0},
                 {185, 90, 160, 65, "Airplane Mode", "Off", 0, 0}};

int volume_val = 50;
int mic_val = 50;
int is_dragging_vol = 0;
int is_dragging_mic = 0;
int hovered_element = -1;

unsigned long get_color(Display *d, const char *hex) {
  XColor col;
  Colormap cmap = DefaultColormap(d, DefaultScreen(d));
  XParseColor(d, cmap, hex, &col);
  XAllocColor(d, cmap, &col);
  return col.pixel;
}

void draw_rounded_rectangle(Display *d, Drawable w, GC gc, int x, int y,
                            int width, int height, int r, int fill) {
  int screen = DefaultScreen(d);

  cairo_surface_t *surface = cairo_xlib_surface_create(
      d, w, DefaultVisual(d, screen), MAIN_WIDTH, MAIN_HEIGHT);

  cairo_t *cr = cairo_create(surface);

  double radius = r;

  if (radius > height / 2.0)
    radius = height / 2.0;

  if (radius > width / 2.0)
    radius = width / 2.0;

  cairo_new_path(cr);

  cairo_move_to(cr, x + radius, y);
  cairo_line_to(cr, x + width - radius, y);

  cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI / 2.0, 0);

  cairo_line_to(cr, x + width, y + height - radius);

  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI / 2.0);

  cairo_line_to(cr, x + radius, y + height);

  cairo_arc(cr, x + radius, y + height - radius, radius, M_PI / 2.0, M_PI);

  cairo_line_to(cr, x, y + radius);

  cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3.0 * M_PI / 2.0);

  cairo_close_path(cr);

  if (fill) {
    XGCValues values;
    XGetGCValues(d, gc, GCForeground, &values);

    XColor xc;
    xc.pixel = values.foreground;

    Colormap cmap = DefaultColormap(d, screen);
    XQueryColor(d, cmap, &xc);

    cairo_set_source_rgb(cr, xc.red / 65535.0, xc.green / 65535.0,
                         xc.blue / 65535.0);

    cairo_fill(cr);
  }

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

void draw_text(Display *d, XftDraw *xft_draw, XftFont *font,
               const char *hex_color, int x, int y, const char *text) {
  XftColor xft_col;
  Visual *visual = DefaultVisual(d, DefaultScreen(d));
  Colormap cmap = DefaultColormap(d, DefaultScreen(d));
  XftColorAllocName(d, visual, cmap, hex_color, &xft_col);
  XftDrawStringUtf8(xft_draw, &xft_col, font, x, y, (const FcChar8 *)text,
                    strlen(text));
  XftColorFree(d, visual, cmap, &xft_col);
}

// ================= SYSTEM HARDWARE LOGIC =================

void get_wifi_name(char *out_name, int max_len, int *is_active) {
  FILE *fp_radio = popen("nmcli radio wifi 2>/dev/null", "r");
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

  FILE *fp = popen("nmcli -t -f GENERAL.CONNECTION dev show 2>/dev/null | head "
                   "-n 1 | cut -d':' -f2",
                   "r");
  if (!fp) {
    fp = popen("nmcli -t -f active,ssid dev wifi 2>/dev/null | grep -i '^yes:' "
               "| cut -d':' -f2",
               "r");
  }

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
    system("nmcli radio wifi off 2>/dev/null");
  } else {
    system("nmcli radio wifi on 2>/dev/null");
  }
}

void get_bluetooth_name(char *out_name, int max_len, int *is_active) {
  FILE *fp = popen("timeout 0.2s bluetoothctl info 2>/dev/null | grep 'Name:' "
                   "| cut -d' ' -f2-",
                   "r");
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

void get_dnd_status(char *out_name, int max_len, int *is_active) {
  FILE *fp = popen("dunstctl is-paused 2>/dev/null", "r");
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

// FIX: Airplane mode tabhi ON mana jayega jab koi bhi unblocked radio nahi
// milega
void get_airplane_status(char *out_name, int max_len, int *is_active) {
  FILE *fp = popen("rfkill list 2>/dev/null | grep -i 'Soft blocked: no'", "r");
  if (fp) {
    char buf[64] = {0};
    if (fgets(buf, sizeof(buf), fp) == NULL) {
      // Koi bhi unblocked device nahi mila, matlab Airplane Mode ON hai
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
    system("rfkill unblock all 2>/dev/null");
    system("nmcli radio all on 2>/dev/null");
  } else {
    system("rfkill block all 2>/dev/null");
  }
}

int get_system_volume() {
  int vol = 50;
  FILE *fp = popen(
      "timeout 0.2s pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      char *pct = strchr(buf, '%');
      if (pct) {
        char *start = pct;
        while (start > buf && *(start - 1) >= '0' && *(start - 1) <= '9')
          start--;
        sscanf(start, "%d", &vol);
      }
    }
    pclose(fp);
  }
  return (vol < 0) ? 0 : (vol > 100) ? 100 : vol;
}

int get_system_mic() {
  int mic = 50;
  FILE *fp = popen(
      "timeout 0.2s pactl get-source-volume @DEFAULT_SOURCE@ 2>/dev/null", "r");
  if (fp) {
    char buf[256];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      char *pct = strchr(buf, '%');
      if (pct) {
        char *start = pct;
        while (start > buf && *(start - 1) >= '0' && *(start - 1) <= '9')
          start--;
        sscanf(start, "%d", &mic);
      }
    }
    pclose(fp);
  }
  return (mic < 0) ? 0 : (mic > 100) ? 100 : mic;
}

void set_system_volume(int val) {
  char cmd[128];
  snprintf(cmd, sizeof(cmd),
           "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null", val);

  char bg_cmd[160];
  snprintf(bg_cmd, sizeof(bg_cmd), "%s >/dev/null 2>&1 &", cmd);
  system(bg_cmd);
}

void set_system_mic(int val) {
  char cmd[128];
  snprintf(cmd, sizeof(cmd),
           "pactl set-source-volume @DEFAULT_SOURCE@ %d%% 2>/dev/null", val);

  char bg_cmd[160];
  snprintf(bg_cmd, sizeof(bg_cmd), "%s >/dev/null 2>&1 &", cmd);
  system(bg_cmd);
}

void update_device_statuses() {
  get_wifi_name(tiles[0].status, sizeof(tiles[0].status), &tiles[0].active);
  get_bluetooth_name(tiles[1].status, sizeof(tiles[1].status),
                     &tiles[1].active);
  get_dnd_status(tiles[2].status, sizeof(tiles[2].status), &tiles[2].active);
  get_airplane_status(tiles[3].status, sizeof(tiles[3].status),
                      &tiles[3].active);
}

// ================= SUB MENU WINDOW =================

void fetch_system_data(const char *cmd, char lines[15][128], int *line_count) {
  FILE *fp = popen(cmd, "r");
  *line_count = 0;
  if (!fp)
    return;

  char buffer[128];
  while (fgets(buffer, sizeof(buffer), fp) != NULL && *line_count < 15) {
    buffer[strcspn(buffer, "\r\n")] = 0;
    if (strlen(buffer) > 0) {
      strncpy(lines[*line_count], buffer, 127);
      (*line_count)++;
    }
  }
  pclose(fp);
}

void show_sub_menu(Display *d, int screen, const char *title, const char *cmd) {
  char raw_items[15][128];
  char formatted_items[15][128];
  int item_count = 0;

  fetch_system_data(cmd, raw_items, &item_count);

  for (int i = 0; i < item_count; i++) {
    char *colon = strrchr(raw_items[i], ':');
    if (colon != NULL && colon != raw_items[i]) {
      *colon = '\0';
      char *name = raw_items[i];
      char *val = colon + 1;

      if (val[0] >= '0' && val[0] <= '9') {
        snprintf(formatted_items[i], sizeof(formatted_items[i]), "%s %s%%",
                 name, val);
      } else {
        snprintf(formatted_items[i], sizeof(formatted_items[i]), "%s %s", name,
                 val);
      }
    } else {
      snprintf(formatted_items[i], sizeof(formatted_items[i]), "%s",
               raw_items[i]);
    }
  }

  int sub_w = 360, sub_h = 400;
  int pos_x = DisplayWidth(d, screen) - sub_w - 10;
  int pos_y = 40;

  XSetWindowAttributes attr;
  attr.override_redirect = True;
  attr.background_pixel = get_color(d, "#18181a");

  Window sub_win = XCreateWindow(
      d, RootWindow(d, screen), pos_x, pos_y, sub_w, sub_h, 0, CopyFromParent,
      InputOutput, CopyFromParent, CWOverrideRedirect | CWBackPixel, &attr);

  XSelectInput(d, sub_win, ExposureMask | ButtonPressMask | KeyPressMask);
  XMapWindow(d, sub_win);

  GC gc = XCreateGC(d, sub_win, 0, NULL);
  XftDraw *xft_draw = XftDrawCreate(d, sub_win, DefaultVisual(d, screen),
                                    DefaultColormap(d, screen));
  XftFont *font_bold =
      XftFontOpenName(d, screen, "Sans:pixelsize=13:weight=bold");
  XftFont *font_reg = XftFontOpenName(d, screen, "Sans:pixelsize=11");

  XEvent ev;
  int running = 1;

  while (running) {
    XNextEvent(d, &ev);

    if (ev.type == Expose) {
      XSetForeground(d, gc, get_color(d, "#11141E"));
      XFillRectangle(d, sub_win, gc, 0, 0, sub_w, sub_h);

      XSetForeground(d, gc, get_color(d, "#181D2A"));
      draw_rounded_rectangle(d, sub_win, gc, 15, 12, 70, 30, 8, 1);
      draw_text(d, xft_draw, font_bold, "#5687EF", 25, 31, "< Back");

      draw_text(d, xft_draw, font_bold, "#ffffff", 100, 31, title);

      XSetForeground(d, gc, get_color(d, "#11141E"));
      XDrawLine(d, sub_win, gc, 15, 52, sub_w - 15, 52);

      if (item_count == 0) {
        draw_text(d, xft_draw, font_reg, "#a1a1aa", 20, 80,
                  "Scanning/No devices found...");
      } else {
        for (int i = 0; i < item_count; i++) {
          XSetForeground(d, gc, get_color(d, "#27272a"));
          draw_rounded_rectangle(d, sub_win, gc, 15, 65 + (i * 35), sub_w - 30,
                                 28, 6, 1);
          draw_text(d, xft_draw, font_reg, "#ffffff", 25, 84 + (i * 35),
                    formatted_items[i]);
        }
      }
    }

    if (ev.type == KeyPress)
      running = 0;
    if (ev.type == ButtonPress) {
      if (ev.xbutton.x >= 15 && ev.xbutton.x <= 85 && ev.xbutton.y >= 12 &&
          ev.xbutton.y <= 42) {
        running = 0;
      }
    }
  }

  XftDrawDestroy(xft_draw);
  XftFontClose(d, font_bold);
  XftFontClose(d, font_reg);
  XDestroyWindow(d, sub_win);
}

// ================= MAIN RENDER ENGINE =================

void draw_ui_to_pixmap(Display *d, Drawable pix, GC gc, XftDraw *xft_draw,
                       XftFont *font_bold, XftFont *font_regular) {
  unsigned long bg_color = get_color(d, "#11141E");
  unsigned long tile_off = get_color(d, "#181D2A");
  unsigned long tile_off_hov = get_color(d, "#181D2A");
  unsigned long tile_on = get_color(d, "#2563eb");
  unsigned long tile_on_hov = get_color(d, "#1d4ed8");
  unsigned long slider_bg = get_color(d, "#181D2A");
  unsigned long slider_fill = get_color(d, "#3b82f6");
  unsigned long btn_bg = get_color(d, "#181D2A");
  unsigned long btn_hover = get_color(d, "#252B3A");

  XSetForeground(d, gc, bg_color);
  XFillRectangle(d, pix, gc, 0, 0, MAIN_WIDTH, MAIN_HEIGHT);

  for (int i = 0; i < 4; i++) {
    unsigned long cur_color;
    if (tiles[i].active) {
      cur_color = (hovered_element == i) ? tile_on_hov : tile_on;
    } else {
      cur_color = (hovered_element == i) ? tile_off_hov : tile_off;
    }

    XSetForeground(d, gc, cur_color);
    draw_rounded_rectangle(d, pix, gc, tiles[i].x, tiles[i].y, tiles[i].w,
                           tiles[i].h, 12, 1);

    const char *title_col = tiles[i].active ? "#ffffff" : "#f4f4f5";
    const char *sub_col = tiles[i].active ? "#e0e7ff" : "#a1a1aa";

    draw_text(d, xft_draw, font_bold, title_col, tiles[i].x + 15,
              tiles[i].y + 28, tiles[i].label);
    draw_text(d, xft_draw, font_regular, sub_col, tiles[i].x + 15,
              tiles[i].y + 48, tiles[i].status);

    if (tiles[i].has_arrow) {
      draw_text(d, xft_draw, font_bold, sub_col, tiles[i].x + tiles[i].w - 20,
                tiles[i].y + 38, ">");
    }
  }

  char vol_lbl[32];
  snprintf(vol_lbl, sizeof(vol_lbl), "Volume (%d%%)", volume_val);
  draw_text(d, xft_draw, font_bold, "#f4f4f5", 15, 182, vol_lbl);

  int v_x = 15, v_y = 195, v_w = 330, v_h = 10;
  XSetForeground(d, gc,
                 (hovered_element == 4) ? get_color(d, "#52525b") : slider_bg);
  draw_rounded_rectangle(d, pix, gc, v_x, v_y, v_w, v_h, 5, 1);

  int v_fill_w = (v_w * volume_val) / 100;
  if (v_fill_w > 0) {
    XSetForeground(d, gc, slider_fill);
    draw_rounded_rectangle(d, pix, gc, v_x, v_y, v_fill_w, v_h, 5, 1);
  }

  int v_thumb_x = v_x + v_fill_w;
  {
    cairo_surface_t *surface = cairo_xlib_surface_create(
        d, pix, DefaultVisual(d, DefaultScreen(d)), MAIN_WIDTH, MAIN_HEIGHT);
    cairo_t *cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_arc(cr, v_thumb_x, v_y + v_h / 2.0, 7.0, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
  }

  char mic_lbl[32];
  snprintf(mic_lbl, sizeof(mic_lbl), "Microphone (%d%%)", mic_val);
  draw_text(d, xft_draw, font_bold, "#f4f4f5", 15, 235, mic_lbl);

  int m_x = 15, m_y = 248, m_w = 330, m_h = 10;
  XSetForeground(d, gc,
                 (hovered_element == 5) ? get_color(d, "#52525b") : slider_bg);
  draw_rounded_rectangle(d, pix, gc, m_x, m_y, m_w, m_h, 5, 1);

  int m_fill_w = (m_w * mic_val) / 100;
  if (m_fill_w > 0) {
    XSetForeground(d, gc, slider_fill);
    draw_rounded_rectangle(d, pix, gc, m_x, m_y, m_fill_w, m_h, 5, 1);
  }

  int m_thumb_x = m_x + m_fill_w;
  {
    cairo_surface_t *surface = cairo_xlib_surface_create(
        d, pix, DefaultVisual(d, DefaultScreen(d)), MAIN_WIDTH, MAIN_HEIGHT);
    cairo_t *cr = cairo_create(surface);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_arc(cr, m_thumb_x, m_y + m_h / 2.0, 7.0, 0, 2.0 * M_PI);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
  }

  XSetForeground(d, gc, get_color(d, "#27272a"));
  XDrawLine(d, pix, gc, 15, 285, MAIN_WIDTH - 15, 285);

  const char *btn_labels[3] = {"Logout", "Restart", "Poweroff"};
  for (int b = 0; b < 3; b++) {
    int bx = 15 + (b * 115);
    int by = 305, bw = 100, bh = 40;

    XSetForeground(d, gc, (hovered_element == 6 + b) ? btn_hover : btn_bg);
    draw_rounded_rectangle(d, pix, gc, bx, by, bw, bh, 8, 1);

    draw_text(d, xft_draw, font_bold, "#f4f4f5", bx + 22, by + 25,
              btn_labels[b]);
  }
}

void render_screen(Display *d, Window w, GC gc, Pixmap pix, XftDraw *xft_draw,
                   XftFont *font_bold, XftFont *font_regular) {
  draw_ui_to_pixmap(d, pix, gc, xft_draw, font_bold, font_regular);
  XCopyArea(d, pix, w, gc, 0, 0, MAIN_WIDTH, MAIN_HEIGHT, 0, 0);
  XFlush(d);
}

int is_on_interactive_element(int mx, int my) {
  for (int i = 0; i < 4; i++) {
    if (mx >= tiles[i].x && mx <= tiles[i].x + tiles[i].w && my >= tiles[i].y &&
        my <= tiles[i].y + tiles[i].h)
      return 1;
  }
  if (mx >= 15 && mx <= 345 && my >= 180 && my <= 215)
    return 1;
  if (mx >= 15 && mx <= 345 && my >= 230 && my <= 265)
    return 1;
  if (mx >= 15 && mx <= 115 && my >= 305 && my <= 345)
    return 1;
  if (mx >= 130 && mx <= 230 && my >= 305 && my <= 345)
    return 1;
  if (mx >= 245 && mx <= 345 && my >= 305 && my <= 345)
    return 1;

  return 0;
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

  XftFont *font_bold = XftFontOpenName(d, s, "Sans:pixelsize=13:weight=bold");
  XftFont *font_regular = XftFontOpenName(d, s, "Sans:pixelsize=11");

  volume_val = get_system_volume();
  mic_val = get_system_mic();
  update_device_statuses();

  int pos_x = DisplayWidth(d, s) - MAIN_WIDTH - 10;
  int pos_y = 40;

  XSetWindowAttributes attr;
  attr.override_redirect = True;
  attr.background_pixel = get_color(d, "#18181a");

  Window w = XCreateWindow(d, root, pos_x, pos_y, MAIN_WIDTH, MAIN_HEIGHT, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel, &attr);

  XSelectInput(d, w,
               ExposureMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | KeyPressMask);
  XMapWindow(d, w);

  XGrabKeyboard(d, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  XGrabPointer(d, w, True,
               ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
               GrabModeAsync, GrabModeAsync, None, None, CurrentTime);

  GC gc = XCreateGC(d, w, 0, NULL);
  Pixmap pix = XCreatePixmap(d, w, MAIN_WIDTH, MAIN_HEIGHT, DefaultDepth(d, s));
  XftDraw *xft_draw =
      XftDrawCreate(d, pix, DefaultVisual(d, s), DefaultColormap(d, s));

  XEvent e;
  while (1) {
    XNextEvent(d, &e);

    if (e.type == Expose) {
      render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
    }

    if (e.type == KeyPress) {
      break;
    }

    if (e.type == ButtonPress) {
      int mx = e.xbutton.x, my = e.xbutton.y;

      if (mx < 0 || mx > MAIN_WIDTH || my < 0 || my > MAIN_HEIGHT ||
          !is_on_interactive_element(mx, my)) {
        break;
      }

      for (int i = 0; i < 4; i++) {
        if (mx >= tiles[i].x && mx <= tiles[i].x + tiles[i].w &&
            my >= tiles[i].y && my <= tiles[i].y + tiles[i].h) {

          if (tiles[i].has_arrow && mx >= (tiles[i].x + tiles[i].w - 30)) {
            if (i == 0) {
              show_sub_menu(
                  d, s, "Wi-Fi Networks",
                  "timeout 1s nmcli -t -f SSID,SIGNAL dev wifi 2>/dev/null");
            } else if (i == 1) {
              show_sub_menu(d, s, "Bluetooth Devices",
                            "timeout 1s bluetoothctl devices 2>/dev/null");
            }
          } else {
            if (i == 0)
              toggle_wifi(tiles[0].active);
            else if (i == 2)
              toggle_dnd();
            else if (i == 3)
              toggle_airplane(tiles[3].active);
          }
          update_device_statuses();
          render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
        }
      }

      if (mx >= 15 && mx <= 345 && my >= 180 && my <= 215) {
        is_dragging_vol = 1;
        volume_val = ((mx - 15) * 100) / 330;
        if (volume_val < 0)
          volume_val = 0;
        if (volume_val > 100)
          volume_val = 100;
        set_system_volume(volume_val);
        render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
      }

      if (mx >= 15 && mx <= 345 && my >= 230 && my <= 265) {
        is_dragging_mic = 1;
        mic_val = ((mx - 15) * 100) / 330;
        if (mic_val < 0)
          mic_val = 0;
        if (mic_val > 100)
          mic_val = 100;
        set_system_mic(mic_val);
        render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
      }

      if (mx >= 15 && mx <= 115 && my >= 305 && my <= 345) {
        system("pkill blackwm && pkill statusbar");
        break;
      }
      if (mx >= 130 && mx <= 230 && my >= 305 && my <= 345) {
        system("systemctl reboot || reboot");
        break;
      }
      if (mx >= 245 && mx <= 345 && my >= 305 && my <= 345) {
        system("systemctl poweroff || poweroff");
        break;
      }
    }

    if (e.type == MotionNotify) {
      int mx = e.xmotion.x, my = e.xmotion.y;
      int prev_hover = hovered_element;

      if (is_dragging_vol) {
        if (mx < 15 || mx > 345 || my < 180 || my > 215) {
          is_dragging_vol = 0;
          hovered_element = -1;
          render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
        } else {
          int new_val = ((mx - 15) * 100) / 330;

          if (new_val < 0)
            new_val = 0;
          if (new_val > 100)
            new_val = 100;

          if (new_val != volume_val) {
            volume_val = new_val;
            set_system_volume(volume_val);
            render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
          }
        }
      } else if (is_dragging_mic) {
        if (mx < 15 || mx > 345 || my < 230 || my > 265) {
          is_dragging_mic = 0;
          hovered_element = -1;
          render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
        } else {
          int new_val = ((mx - 15) * 100) / 330;

          if (new_val < 0)
            new_val = 0;
          if (new_val > 100)
            new_val = 100;

          if (new_val != mic_val) {
            mic_val = new_val;
            set_system_mic(mic_val);
            render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
          }
        }
      } else {
        if (mx >= 15 && mx <= 175 && my >= 15 && my <= 80)
          hovered_element = 0;
        else if (mx >= 185 && mx <= 345 && my >= 15 && my <= 80)
          hovered_element = 1;
        else if (mx >= 15 && mx <= 175 && my >= 90 && my <= 155)
          hovered_element = 2;
        else if (mx >= 185 && mx <= 345 && my >= 90 && my <= 155)
          hovered_element = 3;
        else if (mx >= 15 && mx <= 345 && my >= 180 && my <= 215)
          hovered_element = 4;
        else if (mx >= 15 && mx <= 345 && my >= 230 && my <= 265)
          hovered_element = 5;
        else if (mx >= 15 && mx <= 115 && my >= 305 && my <= 345)
          hovered_element = 6;
        else if (mx >= 130 && mx <= 230 && my >= 305 && my <= 345)
          hovered_element = 7;
        else if (mx >= 245 && mx <= 345 && my >= 305 && my <= 345)
          hovered_element = 8;
        else
          hovered_element = -1;
      }

      if (prev_hover != hovered_element || is_dragging_vol || is_dragging_mic) {
        render_screen(d, w, gc, pix, xft_draw, font_bold, font_regular);
      }
    }

    if (e.type == ButtonRelease) {
      is_dragging_vol = 0;
      is_dragging_mic = 0;
    }
  }

  XUngrabPointer(d, CurrentTime);
  XUngrabKeyboard(d, CurrentTime);

  XftDrawDestroy(xft_draw);
  XftFontClose(d, font_bold);
  XftFontClose(d, font_regular);
  XFreePixmap(d, pix);
  XCloseDisplay(d);
  return 0;
}
