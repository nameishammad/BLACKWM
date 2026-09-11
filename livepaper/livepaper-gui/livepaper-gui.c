#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CONFIG_DIR "/.config/livepaper"
#define CONFIG_FILE "/.config/livepaper/config.txt"

static GtkWidget *window;
static GtkWidget *flowbox;
static GtkWidget *mode_combo;
static char selected_video[1024] = "";
static char current_folder[1024] = "";

char *get_config_path() {
  const char *home = getenv("HOME");
  static char path[1024];
  snprintf(path, sizeof(path), "%s%s", home, CONFIG_FILE);
  return path;
}

void ensure_config_dir() {
  const char *home = getenv("HOME");
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s%s", home, CONFIG_DIR);
  mkdir(dir, 0755);
}

void save_config(const char *video_path, const char *mode,
                 const char *folder_path) {
  ensure_config_dir();
  FILE *f = fopen(get_config_path(), "w");
  if (f) {
    fprintf(f, "%s\n%s\n%s\n", video_path, mode, folder_path);
    fclose(f);
  }
}

int read_config(char *video_path, size_t v_len, char *mode, size_t m_len,
                char *folder_path, size_t f_len) {
  FILE *f = fopen(get_config_path(), "r");
  if (!f)
    return 0;

  if (fgets(video_path, v_len, f)) {
    video_path[strcspn(video_path, "\r\n")] = 0;
    if (fgets(mode, m_len, f)) {
      mode[strcspn(mode, "\r\n")] = 0;
      if (fgets(folder_path, f_len, f)) {
        folder_path[strcspn(folder_path, "\r\n")] = 0;
      } else {
        folder_path[0] = '\0';
      }
      fclose(f);
      return 1;
    }
  }
  fclose(f);
  return 0;
}

static const char *get_selected_mode() {
  int active_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(mode_combo));
  if (active_mode == 1)
    return "--fit";
  if (active_mode == 2)
    return "--scale";
  return "--fill";
}

void apply_wallpaper(const char *video_path, const char *mode) {
  if (!video_path || strlen(video_path) == 0)
    return;

  char command[2048];
  snprintf(command, sizeof(command), "livepaper %s \"%s\" >/dev/null 2>&1 &",
           mode, video_path);

  system(command);

  // Preserve folder path if current_folder is empty during --restore
  char dummy_v[1024], dummy_m[16], saved_f[1024] = "";
  if (strlen(current_folder) == 0) {
    read_config(dummy_v, sizeof(dummy_v), dummy_m, sizeof(dummy_m), saved_f, sizeof(saved_f));
  } else {
    strncpy(saved_f, current_folder, sizeof(saved_f) - 1);
  }

  save_config(video_path, mode, saved_f);
}

int is_video_file(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename)
    return 0;

  if (g_ascii_strcasecmp(dot, ".mp4") == 0 ||
      g_ascii_strcasecmp(dot, ".mkv") == 0 ||
      g_ascii_strcasecmp(dot, ".avi") == 0 ||
      g_ascii_strcasecmp(dot, ".webm") == 0) {
    return 1;
  }
  return 0;
}

void get_thumb_path(const char *video_path, char *out_thumb_path, size_t size) {
  guint hash = g_str_hash(video_path);
  snprintf(out_thumb_path, size, "/tmp/livepaper_thumb_%u.png", hash);
}

GtkWidget *create_tile(const char *video_path) {
  char thumb_path[1024];
  get_thumb_path(video_path, thumb_path, sizeof(thumb_path));

  if (access(thumb_path, F_OK) != 0) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpegthumbnailer -i \"%s\" -o \"%s\" -s 180 >/dev/null 2>&1",
             video_path, thumb_path);
    system(cmd);
  }

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  GtkWidget *image;

  if (access(thumb_path, F_OK) == 0) {
    image = gtk_image_new_from_file(thumb_path);
  } else {
    image = gtk_image_new_from_icon_name("video-x-generic", GTK_ICON_SIZE_DIALOG);
  }

  char *basename = g_path_get_basename(video_path);
  GtkWidget *label = gtk_label_new(basename);
  g_free(basename);

  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 15);

  gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  g_object_set_data_full(G_OBJECT(box), "video_path", g_strdup(video_path), g_free);

  return box;
}

void load_folder(const char *folder_path) {
  if (!folder_path || strlen(folder_path) == 0)
    return;

  strncpy(current_folder, folder_path, sizeof(current_folder) - 1);
  current_folder[sizeof(current_folder) - 1] = '\0';

  GList *children = gtk_container_get_children(GTK_CONTAINER(flowbox));
  for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  }
  g_list_free(children);

  GDir *dir = g_dir_open(folder_path, 0, NULL);
  if (!dir)
    return;

  const char *filename;
  while ((filename = g_dir_read_name(dir)) != NULL) {
    if (is_video_file(filename)) {
      char full_path[2048];
      snprintf(full_path, sizeof(full_path), "%s/%s", folder_path, filename);
      GtkWidget *tile = create_tile(full_path);
      gtk_flow_box_insert(GTK_FLOW_BOX(flowbox), tile, -1);
    }
  }
  g_dir_close(dir);
  gtk_widget_show_all(flowbox);
}

static void on_select_folder_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  (void)user_data;

  GtkFileChooserNative *native = gtk_file_chooser_native_new(
      "Select Wallpaper Folder", GTK_WINDOW(window),
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");

  if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(native)) == GTK_RESPONSE_ACCEPT) {
    char *folder_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(native));
    if (folder_path) {
      load_folder(folder_path);
      save_config(selected_video, get_selected_mode(), folder_path);
      g_free(folder_path);
    }
  }
  g_object_unref(native);
}

static void on_child_selected(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data) {
  (void)box;
  (void)user_data;
  if (!child)
    return;

  GtkWidget *custom_box = gtk_bin_get_child(GTK_BIN(child));
  const char *path = g_object_get_data(G_OBJECT(custom_box), "video_path");
  if (path) {
    strncpy(selected_video, path, sizeof(selected_video) - 1);
    selected_video[sizeof(selected_video) - 1] = '\0';
  }
}

static void on_child_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data) {
  (void)box;
  (void)user_data;
  if (!child)
    return;

  GtkWidget *custom_box = gtk_bin_get_child(GTK_BIN(child));
  const char *path = g_object_get_data(G_OBJECT(custom_box), "video_path");
  if (path) {
    strncpy(selected_video, path, sizeof(selected_video) - 1);
    selected_video[sizeof(selected_video) - 1] = '\0';
    const char *mode = get_selected_mode();
    apply_wallpaper(selected_video, mode);
  }
}

static void on_apply_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  (void)user_data;
  if (strlen(selected_video) == 0)
    return;

  const char *mode = get_selected_mode();
  apply_wallpaper(selected_video, mode);
}

static void on_restore_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  (void)user_data;

  char video_path[1024];
  char mode[16];
  char folder_path[1024];

  if (read_config(video_path, sizeof(video_path), mode, sizeof(mode),
                  folder_path, sizeof(folder_path))) {
    if (strlen(folder_path) > 0)
      load_folder(folder_path);

    if (strlen(video_path) > 0) {
      apply_wallpaper(video_path, mode);
      strncpy(selected_video, video_path, sizeof(selected_video) - 1);
      selected_video[sizeof(selected_video) - 1] = '\0';
    }

    if (strcmp(mode, "--fit") == 0) {
      gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), 1);
    } else if (strcmp(mode, "--scale") == 0) {
      gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), 2);
    } else {
      gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), 0);
    }
  }
}

int main(int argc, char *argv[]) {
  gtk_init(&argc, &argv);

  if (argc > 1 && strcmp(argv[1], "--restore") == 0) {
    char video_path[1024];
    char mode[16];
    char folder_path[1024];

    if (read_config(video_path, sizeof(video_path), mode, sizeof(mode),
                    folder_path, sizeof(folder_path))) {
      if (strlen(folder_path) > 0) {
        strncpy(current_folder, folder_path, sizeof(current_folder) - 1);
      }
      if (strlen(video_path) > 0) {
        apply_wallpaper(video_path, mode);
      }
    }
    return 0;
  }

  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "Waypaper C-GTK");
  gtk_window_set_default_size(GTK_WINDOW(window), 650, 500);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *btn_folder = gtk_button_new_with_label("Select Folder");
  g_signal_connect(btn_folder, "clicked", G_CALLBACK(on_select_folder_clicked), NULL);
  gtk_box_pack_start(GTK_BOX(top_bar), btn_folder, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), top_bar, FALSE, FALSE, 0);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

  flowbox = gtk_flow_box_new();
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flowbox), 5);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flowbox), GTK_SELECTION_SINGLE);
  g_signal_connect(flowbox, "child-activated", G_CALLBACK(on_child_activated), NULL);
  g_signal_connect(flowbox, "child-selected", G_CALLBACK(on_child_selected), NULL);
  gtk_container_add(GTK_CONTAINER(scrolled), flowbox);

  GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  mode_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "Fill");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "Fit");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode_combo), "Scale");
  gtk_combo_box_set_active(GTK_COMBO_BOX(mode_combo), 0);

  GtkWidget *btn_apply = gtk_button_new_with_label("Set Wallpaper");
  GtkWidget *btn_restore = gtk_button_new_with_label("Restore Last");
  g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_apply_clicked), NULL);
  g_signal_connect(btn_restore, "clicked", G_CALLBACK(on_restore_clicked), NULL);

  gtk_box_pack_start(GTK_BOX(bottom_bar), mode_combo, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_bar), btn_apply, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bottom_bar), btn_restore, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), bottom_bar, FALSE, FALSE, 0);

  char v_path[1024];
  char m_val[16];
  char f_path[1024];

  if (read_config(v_path, sizeof(v_path), m_val, sizeof(m_val), f_path, sizeof(f_path))) {
    if (strlen(f_path) > 0)
      load_folder(f_path);
    if (strlen(v_path) > 0)
      strncpy(selected_video, v_path, sizeof(selected_video) - 1);
  }

  gtk_widget_show_all(window);
  gtk_main();

  return 0;
}
