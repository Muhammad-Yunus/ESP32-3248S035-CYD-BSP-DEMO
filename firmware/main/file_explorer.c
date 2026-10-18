/*
 * microSD file explorer for the ESP32-3248S035.
 *
 * Lists the SD card contents, lets the user walk into directories, and opens
 * text files (.txt / .json / .cfg) in a scrollable viewer. Any other
 * file type raises an "unavailable to view" popup.
 */
#include "file_explorer.h"
#include "mp3_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "explorer";

#define SD_MOUNT        "/sdcard"
#define MAX_ENTRIES     128
#define PATH_MAX_LEN    384
#define VIEW_MAX_BYTES  8192

static char s_cwd[PATH_MAX_LEN] = SD_MOUNT;
static lv_obj_t *s_path_label;
static lv_obj_t *s_list;
static lv_obj_t *s_viewer;
static lv_obj_t *s_msgbox;

static char *s_names[MAX_ENTRIES];
static bool  s_isdir[MAX_ENTRIES];
static int   s_count;

/* ------------------------------------------------------------------ */
/* Popups                                                              */
/* ------------------------------------------------------------------ */

static void style_dialog(lv_obj_t *box)
{
    lv_obj_set_style_bg_color(box, lv_color_hex(0x111822), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2A3644), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
}

static void show_message(const char *title, const char *text)
{
    if (s_msgbox) lv_msgbox_close(s_msgbox);
    s_msgbox = lv_msgbox_create(NULL);
    style_dialog(s_msgbox);
    lv_msgbox_add_title(s_msgbox, title);
    lv_msgbox_add_text(s_msgbox, text);
    lv_msgbox_add_close_button(s_msgbox);
    lv_obj_center(s_msgbox);
}

/* ------------------------------------------------------------------ */
/* File viewer                                                         */
/* ------------------------------------------------------------------ */

static void viewer_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_viewer) {
        lv_obj_delete(s_viewer);
        s_viewer = NULL;
    }
}

/* Keep only printable ASCII + tab/newline so the default font can render.
 * Embedded NULs and control bytes become '.'. Returns the resulting length. */
static size_t sanitize(char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\t') continue;
        if (c == '\r') { s[i] = '\n'; continue; }
        if (c < 0x20 || c > 0x7E) s[i] = '.';
    }
    s[n] = '\0';
    return n;
}

static void open_viewer(const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        show_message("Open failed", "Could not read this file.");
        return;
    }

    char *buf = malloc(VIEW_MAX_BYTES + 32);
    if (!buf) {
        fclose(f);
        show_message("Out of memory", "Not enough RAM to open the file.");
        return;
    }

    size_t n = fread(buf, 1, VIEW_MAX_BYTES, f);
    bool truncated = (n == VIEW_MAX_BYTES) && (fgetc(f) != EOF);
    fclose(f);
    size_t len = sanitize(buf, n);

    /* A file is "blank" if it holds no visible characters at all. */
    bool blank = true;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n') {
            blank = false;
            break;
        }
    }
    ESP_LOGI(TAG, "viewing %s (%u bytes, %u shown%s)", name, (unsigned)n,
             (unsigned)len, blank ? ", blank" : "");

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x05080C), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 8, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ov, 6, 0);

    lv_obj_t *head = lv_obj_create(ov);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, 32);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);

    lv_obj_t *title = lv_label_create(head);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_color(title, lv_color_hex(0x39D2C0), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 350);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);

    lv_obj_t *close = lv_button_create(head);
    lv_obj_set_size(close, 84, 28);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0xF04444), 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, viewer_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, "Close");
    lv_obj_center(close_lbl);

    lv_obj_t *body = lv_obj_create(ov);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x1F2A37), 0);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_radius(body, 10, 0);
    lv_obj_set_style_pad_all(body, 8, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);

    lv_obj_t *text = lv_label_create(body);
    if (blank) {
        lv_obj_set_style_flex_main_place(body, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_set_style_flex_cross_place(body, LV_FLEX_ALIGN_CENTER, 0);
        lv_label_set_text(text, "empty");
        lv_obj_set_style_text_color(text, lv_color_hex(0x6E7B8B), 0);
    } else {
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(text, LV_PCT(100));
        lv_obj_set_style_text_color(text, lv_color_hex(0xC9D4E0), 0);

        if (truncated) {
            size_t cap = n + 32;
            char *big = malloc(cap);
            if (big) {
                snprintf(big, cap, "%s\n\n-- file truncated --", buf);
                lv_label_set_text(text, big);
                free(big);
            } else {
                lv_label_set_text(text, buf);
            }
        } else {
            lv_label_set_text(text, buf);
        }
    }

    free(buf);
    s_viewer = ov;
}

/* ------------------------------------------------------------------ */
/* Listing                                                             */
/* ------------------------------------------------------------------ */

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + (n - e), ext) == 0;
}

/* File types the built-in text viewer can render. */
static bool is_viewable(const char *name)
{
    static const char *exts[] = { ".txt", ".json", ".cfg" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (has_ext(name, exts[i])) return true;
    }
    return false;
}

static void refresh_list(void);

void file_explorer_set_path(const char *path)
{
    strncpy(s_cwd, path, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    refresh_list();
}

static int cmp_name(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    if (s_isdir[ia] != s_isdir[ib]) return s_isdir[ia] ? -1 : 1;
    return strcasecmp(s_names[ia], s_names[ib]);
}

static void free_entries(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_names[i]);
        s_names[i] = NULL;
    }
    s_count = 0;
}

static void entry_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_count) return;

    char name[260];
    strncpy(name, s_names[idx], sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", s_cwd, name);

    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        ESP_LOGI(TAG, "enter dir %s", full);
        file_explorer_set_path(full);
        return;
    }

    if (is_viewable(name)) {
        ESP_LOGI(TAG, "open %s", full);
        open_viewer(full, name);
    } else if (has_ext(name, ".mp3")) {
        ESP_LOGI(TAG, "play %s", full);
        mp3_player_open(full, name);
    } else {
        ESP_LOGI(TAG, "no viewer for %s", name);
        show_message("Unavailable to view", "No viewer for this file type.");
    }
}

static void refresh_list(void)
{
    lv_obj_clean(s_list);
    free_entries();

    lv_label_set_text(s_path_label, s_cwd);

    DIR *d = opendir(s_cwd);
    if (!d) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "SD card not mounted / empty.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x6E7B8B), 0);
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", s_cwd, de->d_name);
        struct stat st;
        s_isdir[s_count] = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
        s_names[s_count] = strdup(de->d_name);
        if (!s_names[s_count]) break;
        s_count++;
    }
    closedir(d);

    int order[MAX_ENTRIES];
    for (int i = 0; i < s_count; i++) order[i] = i;
    qsort(order, s_count, sizeof(int), cmp_name);

    if (s_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_list);
        lv_label_set_text(lbl, "(empty folder)");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x6E7B8B), 0);
        return;
    }

    for (int i = 0; i < s_count; i++) {
        int idx = order[i];
        lv_obj_t *btn = lv_button_create(s_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 34);
        lv_obj_set_style_bg_color(btn, s_isdir[idx] ? lv_color_hex(0x1B2A3A)
                                                    : lv_color_hex(0x151E28), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%s %s", s_isdir[idx] ? LV_SYMBOL_DIRECTORY
                                                         : LV_SYMBOL_FILE,
                              s_names[idx]);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, LV_PCT(94));
        lv_obj_set_style_text_color(lbl, lv_color_hex(s_isdir[idx] ? 0x8AB4F8 : 0xC9D4E0), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_add_event_cb(btn, entry_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)idx);
    }
}

static void up_click_cb(lv_event_t *e)
{
    (void)e;
    size_t len = strlen(s_cwd);
    size_t root = strlen(SD_MOUNT);
    if (len <= root) return;

    char *slash = strrchr(s_cwd, '/');
    if (slash && (size_t)(slash - s_cwd) >= root) {
        *slash = '\0';
    } else {
        strncpy(s_cwd, SD_MOUNT, sizeof(s_cwd) - 1);
    }
    refresh_list();
}

/* ------------------------------------------------------------------ */
/* Tab page                                                            */
/* ------------------------------------------------------------------ */

void file_explorer_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 6, 0);

    lv_obj_t *header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 36);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111822), 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x1F2A37), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 10, 0);
    lv_obj_set_style_pad_all(header, 4, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_path_label = lv_label_create(header);
    lv_label_set_text(s_path_label, SD_MOUNT);
    lv_label_set_long_mode(s_path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_path_label, 320);
    lv_obj_set_style_text_color(s_path_label, lv_color_hex(0x39D2C0), 0);
    lv_obj_align(s_path_label, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *up = lv_button_create(header);
    lv_obj_set_size(up, 110, 28);
    lv_obj_align(up, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(up, lv_color_hex(0x1F2A37), 0);
    lv_obj_set_style_radius(up, 8, 0);
    lv_obj_add_event_cb(up, up_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *up_lbl = lv_label_create(up);
    lv_label_set_text(up_lbl, "back");
    lv_obj_center(up_lbl);

    s_list = lv_obj_create(parent);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x0E141C), 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x1F2A37), 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 12, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    refresh_list();
}
