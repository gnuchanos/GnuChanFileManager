/*
 * GnuChanFileManager — pure X11/Xft file manager.
 *
 * UI: Xlib + Xft (same stack as GnuChanWM, GnuChanDM, GnuChanDock).
 * File ops: GIO via gfm_file_ops.c (gio-2.0, no GTK dependency).
 * Volume monitor: GIO GVolumeMonitor.
 * Config: plain key=value file.
 * Clipboard: X11 selection protocol, x-special/gnome-copied-files +
 *            text/uri-list (compatible with Nemo, Thunar, etc.).
 *
 * License: GPL3
 */
#include "gfm_file_ops.h"

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <pwd.h>

/* ── dimensions ────────────────────────────────────────────────────── */
#define WIN_W        900
#define WIN_H        600
#define BAR_H         30   /* address + search bar height */
#define TOOL_H        28   /* action buttons bar height   */
#define STATUS_H      20   /* bottom status bar height    */
#define SIDE_W       190   /* sidebar width               */
#define ROW_H_DEFAULT 22   /* default file row height     */
#define FONT_PX_DEFAULT 13
#define FONT_PX_MIN   9
#define FONT_PX_MAX   28
#define SCROLL_W      12   /* scrollbar width             */
#define PAD            6
#define BTN_H         22
#define BTN_PAD        8

/* ── colours ────────────────────────────────────────────────────────── */
#define C_BG          "#1a0b2e"
#define C_PANEL       "#32143f"
#define C_EDGE        "#7b2cbf"
#define C_FIELD       "#241033"
#define C_FIELD_ON    "#3d1a55"
#define C_TEXT        "#e0c3fc"
#define C_MUTED       "#9d7bba"
#define C_ACCENT      "#c77dff"
#define C_SEL         "#5a2a8f"
#define C_BTN         "#32143f"
#define C_BTN_HOT     "#4a1f60"

/* ── limits ─────────────────────────────────────────────────────────── */
#define MAX_ENTRIES   4096
#define MAX_SIDE       256
#define MAX_HISTORY     64
#define MAX_BOOKMARKS   50
#define MAX_RECENT      10
#define MAX_COPY_Q     256
#define TEXT_SIZE      512
#define NAME_SIZE      256

/* ═══════════════════════════════════════════════════════════════════════
 * Data structures
 * ═════════════════════════════════════════════════════════════════════*/

typedef enum { FT_DIR, FT_REG, FT_LINK, FT_OTHER } FileType;

typedef enum { SORT_NAME, SORT_TYPE, SORT_SIZE, SORT_DATE } SortMode;

typedef struct {
    char name[NAME_SIZE];
    char size[32];        /* formatted */
    char type_str[16];    /* "Folder" / "File" / "Link" / "Other" */
    char content_type[96]; /* MIME/content type if known */
    FileType ftype;
    GFile *file;          /* owned reference */
    gint64   size_bytes;
    guint64  mtime;
    int selected;         /* multi-select flag */
    XImage *thumb;        /* scaled thumbnail for icon view (optional) */
    int thumb_w, thumb_h;
    int thumb_req_size;   /* size thumbnail was generated for */
    GdkPixbuf *thumb_pixbuf; /* decoded source for fast rescaling */
    int thumb_failed;     /* tried and no thumbnail available */
} Entry;

static int g_thumb_load_budget = 0;

typedef enum { ST_PLACES_GROUP, ST_HOME, ST_ROOT, ST_BM_GROUP, ST_RECENT_GROUP,
               ST_DEV_GROUP, ST_BM, ST_RECENT, ST_VOL } SideKind;

typedef struct {
    char  name[NAME_SIZE];
    SideKind kind;
    GFile    *file;    /* NULL for group headers and unmounted volumes */
    GVolume  *volume;  /* non-NULL for volumes                        */
    GMount   *mount;   /* non-NULL for mounts                         */
    int       indent;  /* 0 = root, 1 = child                         */
    int       expandable;
    int       expanded;
} SideEntry;

typedef struct {
    GFile *source;
    GFile *destination_directory;
    int    is_move;
} CopyJob;

typedef struct {
    Display  *dpy;
    int       scr;
    Window    win;
    Pixmap    backbuf;
    Atom      wm_delete;
    GC        gc;

    /* fonts / colours */
    XftFont  *font;
    XftFont  *font_bold;
    XftDraw  *draw;

    XftColor  bg, panel, edge, field, field_on, text, muted, accent, sel,
              btn, btn_hot;

    int win_w, win_h;    /* current window size */

    /* ── file listing ── */
    Entry     entries[MAX_ENTRIES];
    int       entry_count;
    int       list_scroll;   /* first visible row  */
    int       list_sel;      /* focused/cursor row (-1) */
    int       show_hidden;
    int       icon_mode;     /* 0=list 1=icon (icon not impl yet, placeholder) */

    /* zoom / layout */
    int       font_px;
    int       row_h;
    int       icon_zoom;  /* affects file-area icon/thumbnail size */

    /* sorting */
    SortMode  sort_mode;
    int       sort_desc;

    /* selection */
    int       sel_anchor;

    /* rubber-band selection */
    int       band_active;
    int       band_x0, band_y0;
    int       band_x1, band_y1;
    int       band_additive;
    int       band_press; /* potential start */

    /* realtime directory monitoring */
    GFileMonitor *dir_monitor;
    gulong    dir_monitor_id;
    guint     refresh_idle_id;

    char      search[TEXT_SIZE];
    int       search_len;
    int       search_focused;

    char      address[TEXT_SIZE];
    int       address_len;
    int       address_cursor; /* caret position */
    int       address_focused;

    /* current location */
    GFile    *location;

    /* history */
    GFile    *history[MAX_HISTORY];
    int       history_count;
    int       history_index;

    /* bookmarks / recent */
    GFile    *bookmarks[MAX_BOOKMARKS];
    int       bookmark_count;
    GFile    *recent[MAX_RECENT];
    int       recent_count;

    /* ── sidebar ── */
    SideEntry side[MAX_SIDE];
    int       side_count;
    int       side_scroll;
    int       side_sel;
    int       places_expanded;
    int       bm_expanded;
    int       recent_expanded;
    int       dev_expanded;

    /* ── volume monitor ── */
    GVolumeMonitor *volume_monitor;
    gulong    vol_add_id, vol_rem_id, mnt_add_id, mnt_rem_id;

    /* ── clipboard ── */
    Atom      atom_clipboard;
    Atom      atom_targets;
    Atom      atom_gnome_files;  /* x-special/gnome-copied-files */
    Atom      atom_uri_list;     /* text/uri-list                */
    Atom      atom_utf8;
    char     *clipboard_data;    /* owned; NULL when we don't own selection */
    int       clipboard_is_cut;

    /* ── XDND (external drag source) ── */
    Atom      xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_leave,
              xdnd_drop, xdnd_finished, xdnd_selection, xdnd_action_copy,
              xdnd_action_move, xdnd_type_list;
    Window    xdnd_target;
    int       xdnd_version;
    char     *xdnd_data; /* owned uri-list payload */

    /* ── copy queue ── */
    CopyJob   copy_queue[MAX_COPY_Q];
    int       copy_q_head, copy_q_tail, copy_q_len;
    pthread_mutex_t copy_mutex;
    pthread_cond_t  copy_cond;
    pthread_t copy_thread;
    int       copy_running;
    double    copy_fraction;
    char      copy_current[TEXT_SIZE];
    char      copy_error[TEXT_SIZE];
    int       shutting_down;

    /* ── config ── */
    char config_path[PATH_MAX];

    /* ── context menu ── */
    int       menu_open;
    int       menu_x, menu_y;
    int       menu_on_file;   /* 1=right-click on entry, 0=background */
    int       menu_sel;       /* -1=none, 0..N=hovered item */

    /* ── input dialog ── */
    int       dialog_open;
    char      dialog_title[TEXT_SIZE];
    char      dialog_prompt[TEXT_SIZE];
    char      dialog_value[TEXT_SIZE];
    int       dialog_len;
    void    (*dialog_ok)(void *app);  /* callback when user confirms */

    /* ── directory picker (for copy/move) ── */
    int       picker_open;
    int       picker_move;
    GFile    *picker_dir;
    char      picker_names[256][NAME_SIZE];
    GFile    *picker_files[256];
    int       picker_count;
    int       picker_sel;
    int       picker_scroll;

    /* ── properties dialog ── */
    int       props_open;
    char      props_path[TEXT_SIZE];
    char      props_type[32];
    char      props_size[64];
    char      props_mtime[64];
    guint32   props_mode;
    int       props_has_mode;
    GFile    *props_file;    /* NULL if not set */

    /* ── error dialog ── */
    int       err_open;
    char      err_text[TEXT_SIZE];
    char      err_detail[TEXT_SIZE];

    /* resize splitter */
    int       side_w;        /* current sidebar width (draggable) */
    int       split_drag;
    int       split_drag_x0;
    int       split_side_w0;

    /* drag and drop (internal + XDND source) */
    int       dnd_press;
    int       dnd_dragging;
    int       dnd_start_x;
    int       dnd_start_y;
    int       dnd_cur_x;
    int       dnd_cur_y;
    int       dnd_index;      /* pressed item index */
    int       dnd_target_side; /* sidebar drop target index, -1 none */
    int       dnd_target_item; /* main view folder drop target index, -1 none */
    int       dnd_external;    /* dragging to external XDND target */

    /* caret blink for text inputs */
    int       caret_on;
    long      caret_tick;

    /* XDND throttle */
    int       xdnd_last_x;
    int       xdnd_last_y;

    /* layout cache */
    int       layout_w;
    int       layout_h;
    int       layout_side_w;

    /* UI button icons */
    XImage   *ui_icons[16];
    int       ui_icon_w[16];
    int       ui_icon_h[16];

    /* cursor */
    Cursor    drag_cursor;

    /* external drag icon */
    Window    drag_icon;
    XftDraw  *drag_icon_draw;
    int       drag_icon_w, drag_icon_h;

    /* inline rename */
    int       rename_active;
    int       rename_index;
    char      rename_text[NAME_SIZE];
    int       rename_len;
    int       rename_cursor;

    /* status bar */
    char      free_space[128];

    int       running;
} App;

/* ════════════════════════════════════════════════════════════════════
 * Forward declarations
 * ══════════════════════════════════════════════════════════════════*/
static void draw_app(App *a);
static void refresh_listing(App *a);
static void refresh_sidebar(App *a);
static void go_to(App *a, GFile *file, int add_history);
static void open_entry(App *a, int index);
static void save_config(App *a);
static void show_error(App *a, const char *summary, const char *detail);
static int item_at(App *a, int x, int y);
static int item_rect(App *a, int i, int *rx, int *ry, int *rw, int *rh);
static void apply_band_selection(App *a);
static void show_drag_icon(App *a, int root_x, int root_y);
static void move_drag_icon(App *a, int root_x, int root_y);
static void hide_drag_icon(App *a);
static void start_dialog(App *a, const char *title, const char *prompt,
                         void (*ok_cb)(void *));
static void show_props(App *a, int index);
static int  selected_count(App *a);
static void clear_selection(App *a);
static void select_only(App *a, int index);
static void toggle_selection(App *a, int index);
static void select_all(App *a);
static void schedule_refresh(App *a);
static void update_dir_monitor(App *a);
static void free_entry_thumb(Entry *e);
static void clear_entry_thumbs(App *a);
static void ensure_backbuf(App *a);
static void recreate_backbuf(App *a);
static void open_picker(App *a, int is_move);
static void close_picker(App *a);
static void picker_refresh(App *a, GFile *dir);
static void xdnd_send(App *a, Window target, Atom msg, long d1, long d2, long d3, long d4);
static Window xdnd_find_target(App *a, int root_x, int root_y, int *ver_out);
static void build_xdnd_data(App *a);
static void xdnd_update_target(App *a, int root_x, int root_y);
static char *get_effective_home_path(void);
static GFile *get_home_file(void);
static gboolean is_under_home(GFile *file);

/* ════════════════════════════════════════════════════════════════════
 * Selection / zoom helpers
 * ══════════════════════════════════════════════════════════════════*/
static int selected_count(App *a) {
    int n = 0;
    for (int i = 0; i < a->entry_count; i++) if (a->entries[i].selected) n++;
    return n;
}

static void clear_selection(App *a) {
    for (int i = 0; i < a->entry_count; i++) a->entries[i].selected = 0;
}

static void select_only(App *a, int index) {
    clear_selection(a);
    a->list_sel = index;
    a->sel_anchor = index;
    if (index >= 0 && index < a->entry_count) a->entries[index].selected = 1;
}

static void toggle_selection(App *a, int index) {
    if (index < 0 || index >= a->entry_count) return;
    a->entries[index].selected = !a->entries[index].selected;
    a->list_sel = index;
    a->sel_anchor = index;
    if (selected_count(a) == 0) a->list_sel = -1;
}

static void select_range(App *a, int from, int to) {
    if (a->entry_count <= 0) return;
    if (from < 0) from = 0;
    if (to < 0) to = 0;
    if (from >= a->entry_count) from = a->entry_count - 1;
    if (to >= a->entry_count) to = a->entry_count - 1;
    int lo = from < to ? from : to;
    int hi = from < to ? to : from;
    clear_selection(a);
    for (int i = lo; i <= hi; i++) a->entries[i].selected = 1;
    a->list_sel = to;
}

static void select_all(App *a) {
    for (int i = 0; i < a->entry_count; i++) a->entries[i].selected = 1;
    if (a->entry_count > 0 && a->list_sel < 0) a->list_sel = 0;
}

static void load_fonts(App *a) {
    if (!a->dpy) return;
    if (a->draw) { XftDrawDestroy(a->draw); a->draw = NULL; }
    if (a->font_bold && a->font_bold != a->font)
        XftFontClose(a->dpy, a->font_bold);
    if (a->font) XftFontClose(a->dpy, a->font);
    a->font = NULL; a->font_bold = NULL;

    char fname[64];
    snprintf(fname, sizeof(fname), "monospace:pixelsize=%d", a->font_px);
    a->font = XftFontOpenName(a->dpy, a->scr, fname);
    if (!a->font) a->font = XftFontOpenName(a->dpy, a->scr, "fixed:size=12");
    snprintf(fname, sizeof(fname), "monospace:pixelsize=%d:bold", a->font_px);
    a->font_bold = XftFontOpenName(a->dpy, a->scr, fname);
    if (!a->font_bold) a->font_bold = a->font;

    a->row_h = a->font_px + 9;
    if (a->row_h < 16) a->row_h = 16;

    recreate_backbuf(a);
}

static void clear_entry_thumbs(App *a) {
    for (int i = 0; i < a->entry_count; i++) free_entry_thumb(&a->entries[i]);
}

static int icon_thumb_size(App *a) {
    int s = 96 + a->icon_zoom * 16;
    if (s < 48) s = 48;
    if (s > 256) s = 256;
    return s;
}

static void apply_zoom(App *a, int delta) {
    /* Zoom changes icon/thumbnail size in the file area (not the whole UI). */
    a->icon_zoom += delta;
    if (a->icon_zoom < -3) a->icon_zoom = -3;
    if (a->icon_zoom > 10) a->icon_zoom = 10;
    /* thumbnails regenerated lazily at new size to avoid lag */
    draw_app(a);
}

static int is_video_content_type(const char *ctype) {
    return ctype && g_str_has_prefix(ctype, "video/");
}

static int is_image_content_type(const char *ctype) {
    return ctype && g_str_has_prefix(ctype, "image/");
}

static gboolean content_type_is(const char *ctype, const char *want) {
    return ctype && g_strcmp0(ctype, want) == 0;
}

static gboolean has_suffix_ci(const char *name, const char *suf) {
    if (!name || !suf) return FALSE;
    size_t n = strlen(name), m = strlen(suf);
    if (m > n) return FALSE;
    return g_ascii_strcasecmp(name + n - m, suf) == 0;
}

static gboolean is_pdf_entry(const Entry *e) {
    return content_type_is(e->content_type, "application/pdf") || has_suffix_ci(e->name, ".pdf");
}

static gboolean is_svg_entry(const Entry *e) {
    return content_type_is(e->content_type, "image/svg+xml") ||
           content_type_is(e->content_type, "image/svg") ||
           has_suffix_ci(e->name, ".svg") || has_suffix_ci(e->name, ".svgz");
}

static gboolean is_office_entry(const Entry *e) {
    const char *ct = e->content_type;
    if (ct) {
        if (g_str_has_prefix(ct, "application/vnd.oasis.opendocument.")) return TRUE;
        if (g_str_has_prefix(ct, "application/vnd.openxmlformats-officedocument.")) return TRUE;
        if (g_str_has_prefix(ct, "application/vnd.ms-")) return TRUE;
        if (content_type_is(ct, "application/msword")) return TRUE;
        if (content_type_is(ct, "application/vnd.ms-excel")) return TRUE;
        if (content_type_is(ct, "application/vnd.ms-powerpoint")) return TRUE;
        if (content_type_is(ct, "application/rtf") || content_type_is(ct, "text/rtf")) return TRUE;
    }
    return has_suffix_ci(e->name, ".odt") || has_suffix_ci(e->name, ".ods") || has_suffix_ci(e->name, ".odp") ||
           has_suffix_ci(e->name, ".doc") || has_suffix_ci(e->name, ".docx") ||
           has_suffix_ci(e->name, ".xls") || has_suffix_ci(e->name, ".xlsx") ||
           has_suffix_ci(e->name, ".ppt") || has_suffix_ci(e->name, ".pptx") ||
           has_suffix_ci(e->name, ".rtf");
}

static gboolean ensure_xdg_thumb_dir(char *out, size_t outsz) {
    const char *cache = g_get_user_cache_dir();
    if (!cache) return FALSE;
    g_snprintf(out, outsz, "%s/thumbnails/normal", cache);
    return g_mkdir_with_parents(out, 0700) == 0;
}

static gboolean xdg_thumbnail_path_for_file(GFile *file, char *out, size_t outsz) {
    char dir[PATH_MAX];
    if (!ensure_xdg_thumb_dir(dir, sizeof(dir))) return FALSE;
    char *uri = g_file_get_uri(file);
    if (!uri) return FALSE;
    char *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    g_free(uri);
    if (!md5) return FALSE;
    g_snprintf(out, outsz, "%s/%s.png", dir, md5);
    g_free(md5);
    return TRUE;
}

static gboolean spawn_ok_sync(char **argv) {
    gint status = 1;
    GError *err = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, &err)) {
        if (err) g_error_free(err);
        return FALSE;
    }
    if (err) g_error_free(err);
    return status == 0;
}

static gboolean generate_video_thumbnail(const char *path, const char *thumb_path) {
    gchar *argv[] = {
        "ffmpeg", "-y", "-loglevel", "error",
        "-ss", "1", "-i", (gchar *)path,
        "-frames:v", "1",
        "-vf", "scale=256:256:force_original_aspect_ratio=decrease",
        (gchar *)thumb_path,
        NULL
    };
    if (!spawn_ok_sync(argv)) return FALSE;
    return g_file_test(thumb_path, G_FILE_TEST_EXISTS);
}

static gboolean generate_pdf_thumbnail(const char *path, const char *thumb_path) {
    char prefix[PATH_MAX];
    g_snprintf(prefix, sizeof(prefix), "%s.pdf", thumb_path);
    gchar *argv1[] = {
        "pdftoppm", "-f", "1", "-singlefile", "-png",
        "-scale-to", "256", (gchar *)path, prefix, NULL
    };
    char out1[PATH_MAX];
    g_snprintf(out1, sizeof(out1), "%s.png", prefix);
    if (spawn_ok_sync(argv1) && g_file_test(out1, G_FILE_TEST_EXISTS)) {
        if (rename(out1, thumb_path) == 0) return TRUE;
        unlink(out1);
    }
    /* fallback: mutool draw */
    gchar *argv2[] = {
        "mutool", "draw", "-o", (gchar *)thumb_path, "-w", "256", "-h", "256", "-F", "png", (gchar *)path, "1", NULL
    };
    if (spawn_ok_sync(argv2) && g_file_test(thumb_path, G_FILE_TEST_EXISTS)) return TRUE;
    return FALSE;
}

static gboolean generate_office_thumbnail(const char *path, const char *thumb_path) {
    char tmpdir[] = "/tmp/opencode/gfmofficeXXXXXX";
    if (!g_mkdtemp(tmpdir)) return FALSE;
    gchar *argv[] = {
        "soffice", "--headless", "--nologo", "--nolockcheck", "--nodefault", "--norestore",
        "--convert-to", "png", "--outdir", tmpdir, (gchar *)path, NULL
    };
    gboolean ok = spawn_ok_sync(argv);
    char *base = g_path_get_basename(path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char outpng[PATH_MAX];
    g_snprintf(outpng, sizeof(outpng), "%s/%s.png", tmpdir, base);
    g_free(base);
    if (ok && g_file_test(outpng, G_FILE_TEST_EXISTS)) {
        GError *err = NULL;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(outpng, 256, 256, TRUE, &err);
        if (!pb) { g_clear_error(&err); pb = gdk_pixbuf_new_from_file(outpng, &err); }
        if (pb) {
            gboolean saved = gdk_pixbuf_save(pb, thumb_path, "png", &err, NULL);
            g_object_unref(pb);
            if (!saved) { if (err) g_error_free(err); ok = FALSE; }
            else ok = TRUE;
        } else {
            if (err) g_error_free(err);
            ok = FALSE;
        }
    } else {
        ok = FALSE;
    }
    /* cleanup temp dir contents */
    GDir *d = g_dir_open(tmpdir, 0, NULL);
    if (d) {
        const char *fn;
        while ((fn = g_dir_read_name(d)) != NULL) {
            char fp[PATH_MAX];
            g_snprintf(fp, sizeof(fp), "%s/%s", tmpdir, fn);
            unlink(fp);
        }
        g_dir_close(d);
    }
    rmdir(tmpdir);
    return ok && g_file_test(thumb_path, G_FILE_TEST_EXISTS);
}

static void ensure_entry_thumb(App *a, Entry *e) {
    int size = icon_thumb_size(a);
    if (e->ftype == FT_DIR) return;
    if (e->thumb && e->thumb_req_size == size) return;
    if (e->thumb_failed && e->thumb_req_size == size) return;
    if (g_thumb_load_budget <= 0) return;
    g_thumb_load_budget--;

    /* decode source pixbuf once (expensive), then rescale cheaply for zoom */
    if (!e->thumb_pixbuf && !e->thumb_failed) {
        if (!e->file) { e->thumb_failed = 1; e->thumb_req_size = size; return; }
        char *path = g_file_get_path(e->file);
        if (!path) { e->thumb_failed = 1; e->thumb_req_size = size; return; }
        GError *err = NULL;
        int maxs = 256;
        GdkPixbuf *pb = NULL;
        gboolean is_video = is_video_content_type(e->content_type);
        gboolean is_pdf = is_pdf_entry(e);
        gboolean is_office = is_office_entry(e);
        gboolean is_svg = is_svg_entry(e);

        char thumb_path[PATH_MAX];
        gboolean have_thumb_path = xdg_thumbnail_path_for_file(e->file, thumb_path, sizeof(thumb_path));

        if (is_video || is_pdf || is_office) {
            if (have_thumb_path) {
                if (!g_file_test(thumb_path, G_FILE_TEST_EXISTS)) {
                    if (is_video) generate_video_thumbnail(path, thumb_path);
                    else if (is_pdf) generate_pdf_thumbnail(path, thumb_path);
                    else if (is_office) generate_office_thumbnail(path, thumb_path);
                }
                if (g_file_test(thumb_path, G_FILE_TEST_EXISTS)) {
                    pb = gdk_pixbuf_new_from_file_at_scale(thumb_path, maxs, maxs, TRUE, &err);
                    if (!pb) {
                        g_clear_error(&err);
                        GdkPixbuf *orig = gdk_pixbuf_new_from_file(thumb_path, &err);
                        if (orig) {
                            int ow = gdk_pixbuf_get_width(orig), oh = gdk_pixbuf_get_height(orig);
                            if (ow > 0 && oh > 0) {
                                double scale = (double)maxs / (double)(ow > oh ? ow : oh);
                                int nw = (int)(ow * scale); if (nw < 1) nw = 1;
                                int nh = (int)(oh * scale); if (nh < 1) nh = 1;
                                pb = gdk_pixbuf_scale_simple(orig, nw, nh, GDK_INTERP_BILINEAR);
                            }
                            g_object_unref(orig);
                        }
                    }
                }
            }
            /* Avoid slow PIL fallback for generated-preview types */
        } else {
            /* images (JPEG/PNG/BMP/WebP/SVG via gdk-pixbuf loaders, etc.) */
            pb = gdk_pixbuf_new_from_file_at_scale(path, maxs, maxs, TRUE, &err);
            if (!pb) {
                g_clear_error(&err);
                GdkPixbuf *orig = gdk_pixbuf_new_from_file(path, &err);
                if (orig) {
                    int ow = gdk_pixbuf_get_width(orig), oh = gdk_pixbuf_get_height(orig);
                    if (ow > 0 && oh > 0) {
                        double scale = (double)maxs / (double)(ow > oh ? ow : oh);
                        int nw = (int)(ow * scale); if (nw < 1) nw = 1;
                        int nh = (int)(oh * scale); if (nh < 1) nh = 1;
                        pb = gdk_pixbuf_scale_simple(orig, nw, nh, GDK_INTERP_BILINEAR);
                    }
                    g_object_unref(orig);
                }
            }
            /* SVG: try rsvg-convert if gdk-pixbuf loader unavailable */
            if (!pb && is_svg) {
                g_clear_error(&err);
                if (have_thumb_path) {
                    if (!g_file_test(thumb_path, G_FILE_TEST_EXISTS)) {
                        char wh[16]; snprintf(wh, sizeof(wh), "%d", maxs);
                        gchar *argv[] = {
                            "rsvg-convert", "-w", wh, "-h", wh, "-a",
                            "-o", thumb_path, (gchar *)path, NULL
                        };
                        spawn_ok_sync(argv);
                    }
                    if (g_file_test(thumb_path, G_FILE_TEST_EXISTS)) {
                        pb = gdk_pixbuf_new_from_file_at_scale(thumb_path, maxs, maxs, TRUE, &err);
                    }
                }
            }
            /* generic slow fallback only for likely images */
            if (!pb && (is_image_content_type(e->content_type) || is_svg)) {
                g_clear_error(&err);
                char tmp[] = "/tmp/opencode/gfmthumbXXXXXX.png";
                int fd = mkstemps(tmp, 4);
                if (fd >= 0) {
                    close(fd);
                    gchar *cmd = g_strdup_printf(
                        "python3 -c \"from PIL import Image; Image.open(r'''%s''').convert('RGBA').save(r'''%s''')\" 2>/dev/null",
                        path, tmp);
                    int rc = system(cmd);
                    g_free(cmd);
                    if (rc == 0) {
                        pb = gdk_pixbuf_new_from_file_at_scale(tmp, maxs, maxs, TRUE, &err);
                        if (!pb) {
                            g_clear_error(&err);
                            GdkPixbuf *orig = gdk_pixbuf_new_from_file(tmp, &err);
                            if (orig) {
                                int ow = gdk_pixbuf_get_width(orig), oh = gdk_pixbuf_get_height(orig);
                                if (ow > 0 && oh > 0) {
                                    double scale = (double)maxs / (double)(ow > oh ? ow : oh);
                                    int nw = (int)(ow * scale); if (nw < 1) nw = 1;
                                    int nh = (int)(oh * scale); if (nh < 1) nh = 1;
                                    pb = gdk_pixbuf_scale_simple(orig, nw, nh, GDK_INTERP_BILINEAR);
                                }
                                g_object_unref(orig);
                            }
                        }
                    }
                    unlink(tmp);
                }
            }
        }

        g_free(path);
        if (!pb) { if (err) g_error_free(err); e->thumb_failed = 1; e->thumb_req_size = size; return; }
        e->thumb_pixbuf = pb;
    }

    if (!e->thumb_pixbuf) { e->thumb_failed = 1; e->thumb_req_size = size; return; }

    if (e->thumb) { XDestroyImage(e->thumb); e->thumb = NULL; e->thumb_w = e->thumb_h = 0; }
    e->thumb_req_size = size;

    GdkPixbuf *src = e->thumb_pixbuf;
    int ow = gdk_pixbuf_get_width(src), oh = gdk_pixbuf_get_height(src);
    double scale = (double)size / (double)(ow > oh ? ow : oh);
    int w = (int)(ow * scale); if (w < 1) w = 1;
    int h = (int)(oh * scale); if (h < 1) h = 1;
    GdkPixbuf *pb = gdk_pixbuf_scale_simple(src, w, h, GDK_INTERP_BILINEAR);
    if (!pb) { e->thumb_failed = 1; return; }

    int stride = gdk_pixbuf_get_rowstride(pb);
    int nchan = gdk_pixbuf_get_n_channels(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);

    Visual *vis = DefaultVisual(a->dpy, a->scr);
    int depth = DefaultDepth(a->dpy, a->scr);
    if (depth < 24) depth = 24;
    int bpp = 32;
    int bytes_per_line = w * 4;
    char *data = calloc(1, (size_t)bytes_per_line * (size_t)h);
    if (!data) { g_object_unref(pb); e->thumb_failed = 1; return; }

    unsigned long rm = vis->red_mask, gm = vis->green_mask, bm = vis->blue_mask;
    int rs=0, gs=0, bs=0;
    if (rm) while (((rm >> rs) & 1UL) == 0UL) rs++;
    if (gm) while (((gm >> gs) & 1UL) == 0UL) gs++;
    if (bm) while (((bm >> bs) & 1UL) == 0UL) bs++;

    for (int yy = 0; yy < h; yy++) {
        guchar *row = px + yy * stride;
        for (int xx = 0; xx < w; xx++) {
            guchar r = row[xx * nchan + 0];
            guchar g = row[xx * nchan + 1];
            guchar b = row[xx * nchan + 2];
            unsigned long pixel = ((unsigned long)r << rs) |
                                  ((unsigned long)g << gs) |
                                  ((unsigned long)b << bs);
            memcpy(data + (yy * bytes_per_line + xx * 4), &pixel, 4);
        }
    }

    XImage *img = XCreateImage(a->dpy, vis, (unsigned)depth, ZPixmap, 0, data, w, h, bpp, bytes_per_line);
    g_object_unref(pb);
    if (!img) { free(data); e->thumb_failed = 1; return; }

    e->thumb = img;
    e->thumb_w = w;
    e->thumb_h = h;
}

/* ════════════════════════════════════════════════════════════════════
 * Colour helpers
 * ══════════════════════════════════════════════════════════════════*/
static void alloc_color(App *a, const char *name, XftColor *out) {
    XftColorAllocName(a->dpy, DefaultVisual(a->dpy, a->scr),
                      DefaultColormap(a->dpy, a->scr), name, out);
}

/* ════════════════════════════════════════════════════════════════════
 * Text helpers
 * ══════════════════════════════════════════════════════════════════*/
static int text_w(App *a, XftFont *f, const char *s) {
    XGlyphInfo ext;
    XftTextExtentsUtf8(a->dpy, f, (const FcChar8 *)s, (int)strlen(s), &ext);
    return ext.xOff;
}

static void draw_text(App *a, XftFont *f, XftColor *c,
                      int x, int baseline, const char *s) {
    if (!s || !s[0]) return;
    int len = (int)strlen(s);
    XftDrawStringUtf8(a->draw, c, f, x, baseline,
                      (const FcChar8 *)s, len);
}

/* clip text to fit `max_w` pixels, appending "…" if needed */
static void draw_text_clip(App *a, XftFont *f, XftColor *c,
                           int x, int baseline, int max_w, const char *s) {
    if (!s || !s[0] || max_w <= 0) return;
    if (text_w(a, f, s) <= max_w) {
        draw_text(a, f, c, x, baseline, s);
        return;
    }
    char buf[TEXT_SIZE + 4];
    snprintf(buf, sizeof(buf), "%s", s);
    int len = (int)strlen(buf);
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xc0) == 0x80) len--;
        buf[len] = '\0';
        strncat(buf, "…", sizeof(buf) - strlen(buf) - 1);
        if (text_w(a, f, buf) <= max_w) { draw_text(a, f, c, x, baseline, buf); return; }
        buf[len] = '\0';
    }
}

static Drawable draw_target(App *a) {
    return a->backbuf ? a->backbuf : a->win;
}

static void fill_rect(App *a, XftColor *c, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    XSetForeground(a->dpy, a->gc, c->pixel);
    XFillRectangle(a->dpy, draw_target(a), a->gc, x, y, (unsigned)w, (unsigned)h);
}

static void draw_rect(App *a, XftColor *c, int x, int y, int w, int h) {
    if (w <= 1 || h <= 1) return;
    XSetForeground(a->dpy, a->gc, c->pixel);
    XDrawRectangle(a->dpy, draw_target(a), a->gc, x, y, (unsigned)(w-1), (unsigned)(h-1));
}

static void draw_line(App *a, XftColor *c, int x1, int y1, int x2, int y2) {
    XSetForeground(a->dpy, a->gc, c->pixel);
    XDrawLine(a->dpy, draw_target(a), a->gc, x1, y1, x2, y2);
}

/* ════════════════════════════════════════════════════════════════════
 * Layout helpers
 * ══════════════════════════════════════════════════════════════════*/
static int bar_y(void)   { return 0; }
static int tool_y(void)  { return BAR_H; }
static int list_y(void)  { return BAR_H + TOOL_H; }

static int list_h(App *a) {
    int h = a->win_h - list_y() - STATUS_H;
    return h > 0 ? h : 1;
}

static int list_x(App *a) { return a->side_w + 2; }
static int list_w(App *a) {
    int w = a->win_w - a->side_w - 2 - SCROLL_W;
    return w > 0 ? w : 1;
}

/* ════════════════════════════════════════════════════════════════════
 * Button drawing
 * ══════════════════════════════════════════════════════════════════*/
#define UI_ICON_BACK       0
#define UI_ICON_FORWARD    1
#define UI_ICON_UP         2
#define UI_ICON_GO         3
#define UI_ICON_HIDDEN     4
#define UI_ICON_NEW_FOLDER 5
#define UI_ICON_NEW_FILE   6
#define UI_ICON_BM_ADD     7
#define UI_ICON_BM_REMOVE  8
#define UI_ICON_VIEW_LIST  9
#define UI_ICON_VIEW_ICONS 10
#define UI_ICON_VIEW_LARGE 11
#define UI_ICON_COUNT      12

typedef struct { int x, y, w, h; const char *label; int id; int icon_id; } Btn;

static void draw_btn(App *a, Btn *b, int hot) {
    fill_rect(a, hot ? &a->btn_hot : &a->btn, b->x, b->y, b->w, b->h);
    draw_rect(a, &a->edge, b->x, b->y, b->w, b->h);
    if (b->icon_id >= 0 && b->icon_id < UI_ICON_COUNT && a->ui_icons[b->icon_id]) {
        int iw = a->ui_icon_w[b->icon_id];
        int ih = a->ui_icon_h[b->icon_id];
        int ix = b->x + (b->w - iw) / 2;
        int iy = b->y + (b->h - ih) / 2;
        XPutImage(a->dpy, draw_target(a), a->gc, a->ui_icons[b->icon_id],
                  0, 0, ix, iy, (unsigned)iw, (unsigned)ih);
    } else if (b->label && b->label[0]) {
        int bw = text_w(a, a->font, b->label);
        int tx = b->x + (b->w - bw) / 2;
        int ty = b->y + (b->h + a->font->ascent - a->font->descent) / 2;
        draw_text_clip(a, a->font, &a->text, tx, ty, b->w - 4, b->label);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * File size formatter
 * ══════════════════════════════════════════════════════════════════*/
static void fmt_size(guint64 bytes, char *out, size_t out_size) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, out_size, "%.1f GiB",
                 (double)bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ULL * 1024ULL)
        snprintf(out, out_size, "%.1f MiB",
                 (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        snprintf(out, out_size, "%.1f KiB", (double)bytes / 1024.0);
    else
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
}

static void fmt_time(guint64 epoch, char *out, size_t out_size) {
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, out_size, "%Y-%m-%d %H:%M", &tm);
}

/* ════════════════════════════════════════════════════════════════════
 * Config I/O
 * ══════════════════════════════════════════════════════════════════*/
static void load_config(App *a) {
    FILE *f = fopen(a->config_path, "r");
    if (!f) return;
    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        /* trim key */
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        /* trim val */
        while (*val == ' ' || *val == '\t') val++;
        char *ve = val + strlen(val) - 1;
        while (ve >= val && (*ve == '\n' || *ve == '\r' ||
                              *ve == ' ' || *ve == '\t')) *ve-- = '\0';

        if (strcmp(key, "show_hidden") == 0)
            a->show_hidden = strcmp(val, "true") == 0 || strcmp(val, "1") == 0;
        else if (strcmp(key, "start_location") == 0) {
            /* parse into address bar; go_to called after init */
            snprintf(a->address, sizeof(a->address), "%s", val);
            a->address_len = (int)strlen(a->address);
        } else if (strcmp(key, "bookmark") == 0 &&
                   a->bookmark_count < MAX_BOOKMARKS) {
            GFile *gf = g_file_new_for_commandline_arg(val);
            if (gf && g_file_query_exists(gf, NULL))
                a->bookmarks[a->bookmark_count++] = gf;
            else if (gf) g_object_unref(gf);
        } else if (strcmp(key, "recent") == 0 &&
                   a->recent_count < MAX_RECENT) {
            GFile *gf = g_file_new_for_commandline_arg(val);
            if (gf && g_file_query_exists(gf, NULL) && is_under_home(gf))
                a->recent[a->recent_count++] = gf;
            else if (gf) g_object_unref(gf);
        }
    }
    fclose(f);
}

static void save_config(App *a) {
    /* ensure directory */
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", a->config_path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; g_mkdir_with_parents(dir, 0700); }

    FILE *f = fopen(a->config_path, "w");
    if (!f) return;
    fprintf(f, "show_hidden=%s\n", a->show_hidden ? "true" : "false");
    if (a->location) {
        char *p = g_file_get_path(a->location);
        if (!p) p = g_file_get_uri(a->location);
        fprintf(f, "start_location=%s\n", p);
        g_free(p);
    } else {
        fprintf(f, "start_location=~\n");
    }
    for (int i = 0; i < a->bookmark_count; i++) {
        char *p = g_file_get_path(a->bookmarks[i]);
        if (!p) p = g_file_get_uri(a->bookmarks[i]);
        fprintf(f, "bookmark=%s\n", p);
        g_free(p);
    }
    for (int i = 0; i < a->recent_count; i++) {
        char *p = g_file_get_path(a->recent[i]);
        if (!p) p = g_file_get_uri(a->recent[i]);
        fprintf(f, "recent=%s\n", p);
        g_free(p);
    }
    fclose(f);
}


/* ════════════════════════════════════════════════════════════════════
 * Home / recent helpers
 * ══════════════════════════════════════════════════════════════════*/
static char *get_effective_home_path(void) {
    /* prefer passwd entry for real user, then env, then fallbacks */
    const char *pw_dir = g_get_home_dir();
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && g_file_test(pw->pw_dir, G_FILE_TEST_IS_DIR))
        return g_strdup(pw->pw_dir);
    if (pw_dir && g_file_test(pw_dir, G_FILE_TEST_IS_DIR))
        return g_strdup(pw_dir);
    const char *envh = getenv("HOME");
    if (envh && g_file_test(envh, G_FILE_TEST_IS_DIR))
        return g_strdup(envh);
    const char *user = g_get_user_name();
    if (user && *user) {
        char *p = g_build_filename("/home", user, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_DIR)) return p;
        g_free(p);
    }
    /* avoid returning "/" as "home" unless nothing else exists */
    if (envh && *envh) return g_strdup(envh);
    if (pw_dir && *pw_dir) return g_strdup(pw_dir);
    return g_strdup("/");
}

static GFile *get_home_file(void) {
    char *p = get_effective_home_path();
    GFile *f = g_file_new_for_path(p);
    g_free(p);
    return f;
}

static gboolean is_under_home(GFile *file) {
    if (!file) return FALSE;
    char *homep = get_effective_home_path();
    GFile *home = g_file_new_for_path(homep);
    gboolean ok = g_file_equal(file, home) || g_file_has_prefix(file, home);
    g_object_unref(home);
    g_free(homep);
    return ok;
}

/* ════════════════════════════════════════════════════════════════════
 * Address bar
 * ══════════════════════════════════════════════════════════════════*/
static void address_set(App *a, GFile *file) {
    char *p = g_file_get_path(file);
    if (!p) p = g_file_get_uri(file);
    snprintf(a->address, sizeof(a->address), "%s", p);
    a->address_len = (int)strlen(a->address);
    a->address_cursor = a->address_len;
    g_free(p);
}

/* ════════════════════════════════════════════════════════════════════
 * Recent locations
 * ══════════════════════════════════════════════════════════════════*/
static void remember_location(App *a, GFile *file) {
    if (!is_under_home(file)) return;
    for (int i = 0; i < a->recent_count; i++) {
        if (g_file_equal(a->recent[i], file)) {
            g_object_unref(a->recent[i]);
            memmove(&a->recent[i], &a->recent[i+1],
                    (a->recent_count - i - 1) * sizeof(GFile*));
            a->recent_count--;
            break;
        }
    }
    if (a->recent_count < MAX_RECENT) {
        memmove(&a->recent[1], &a->recent[0],
                a->recent_count * sizeof(GFile*));
        a->recent[0] = g_object_ref(file);
        a->recent_count++;
    } else {
        g_object_unref(a->recent[a->recent_count - 1]);
        memmove(&a->recent[1], &a->recent[0],
                (a->recent_count - 1) * sizeof(GFile*));
        a->recent[0] = g_object_ref(file);
    }
    save_config(a);
    refresh_sidebar(a);
}

/* ════════════════════════════════════════════════════════════════════
 * File listing
 * ══════════════════════════════════════════════════════════════════*/
static SortMode g_sort_mode = SORT_NAME;
static int g_sort_desc = 0;

static const char *file_ext_of(const char *name) {
    if (!name) return "";
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return "";
    return dot + 1;
}

static int entry_cmp(const void *a_, const void *b_) {
    const Entry *a = a_, *b = b_;
    if (a->ftype == FT_DIR && b->ftype != FT_DIR) return -1;
    if (a->ftype != FT_DIR && b->ftype == FT_DIR) return  1;

    int r = 0;
    switch (g_sort_mode) {
    case SORT_TYPE: {
        const char *ea = file_ext_of(a->name);
        const char *eb = file_ext_of(b->name);
        r = strcasecmp(ea, eb);
        if (r == 0) r = strcasecmp(a->type_str, b->type_str);
        if (r == 0) r = strcasecmp(a->name, b->name);
        break;
    }
    case SORT_SIZE:
        if (a->size_bytes < b->size_bytes) r = -1;
        else if (a->size_bytes > b->size_bytes) r = 1;
        else r = strcasecmp(a->name, b->name);
        break;
    case SORT_DATE:
        if (a->mtime < b->mtime) r = -1;
        else if (a->mtime > b->mtime) r = 1;
        else r = strcasecmp(a->name, b->name);
        break;
    case SORT_NAME:
    default:
        r = strcasecmp(a->name, b->name);
        break;
    }
    return g_sort_desc ? -r : r;
}

static void free_entry_thumb(Entry *e) {
    if (e->thumb) {
        /* XDestroyImage frees the data buffer we provided */
        XDestroyImage(e->thumb);
        e->thumb = NULL;
    }
    if (e->thumb_pixbuf) {
        g_object_unref(e->thumb_pixbuf);
        e->thumb_pixbuf = NULL;
    }
    e->thumb_w = e->thumb_h = 0;
    e->thumb_req_size = 0;
    e->thumb_failed = 0;
}

static void refresh_listing(App *a) {
    /* preserve thumbnails/selection by name where possible */
    Entry old[MAX_ENTRIES];
    int old_count = a->entry_count;
    if (old_count > MAX_ENTRIES) old_count = MAX_ENTRIES;
    memcpy(old, a->entries, (size_t)old_count * sizeof(Entry));

    a->entry_count = 0;
    a->list_sel = -1;
    a->list_scroll = 0;
    a->sel_anchor = -1;

    if (!a->location) {
        for (int i = 0; i < old_count; i++) {
            free_entry_thumb(&old[i]);
            if (old[i].file) g_object_unref(old[i].file);
        }
        return;
    }

    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children(
        a->location,
        G_FILE_ATTRIBUTE_STANDARD_NAME ","
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_STANDARD_SIZE ","
        G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
        G_FILE_ATTRIBUTE_TIME_MODIFIED,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &err);
    if (!en) {
        for (int i = 0; i < old_count; i++) {
            free_entry_thumb(&old[i]);
            if (old[i].file) g_object_unref(old[i].file);
        }
        show_error(a, "Cannot open directory",
                   err ? err->message : "Unknown error");
        if (err) g_error_free(err);
        return;
    }

    char search_lower[TEXT_SIZE] = {0};
    for (int i = 0; a->search[i] && i < TEXT_SIZE - 1; i++)
        search_lower[i] = (char)tolower((unsigned char)a->search[i]);

    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        const char *name = g_file_info_get_name(info);
        gboolean hidden = g_file_info_get_is_hidden(info) ||
                          (name && name[0] == '.');
        if (hidden && !a->show_hidden) { g_object_unref(info); continue; }

        if (search_lower[0]) {
            char nl[NAME_SIZE];
            int j; for (j = 0; name[j] && j < NAME_SIZE - 1; j++)
                nl[j] = (char)tolower((unsigned char)name[j]);
            nl[j] = '\0';
            if (!strstr(nl, search_lower)) { g_object_unref(info); continue; }
        }

        if (a->entry_count >= MAX_ENTRIES) { g_object_unref(info); continue; }

        Entry *e = &a->entries[a->entry_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", name);

        GFileType ft = g_file_info_get_file_type(info);
        switch (ft) {
            case G_FILE_TYPE_DIRECTORY:    e->ftype = FT_DIR;   snprintf(e->type_str, sizeof(e->type_str), "Folder"); break;
            case G_FILE_TYPE_SYMBOLIC_LINK:e->ftype = FT_LINK;  snprintf(e->type_str, sizeof(e->type_str), "Link");   break;
            case G_FILE_TYPE_REGULAR:      e->ftype = FT_REG;   snprintf(e->type_str, sizeof(e->type_str), "File");   break;
            default:                       e->ftype = FT_OTHER; snprintf(e->type_str, sizeof(e->type_str), "Other");  break;
        }

        const char *ctype = g_file_info_get_content_type(info);
        snprintf(e->content_type, sizeof(e->content_type), "%s", ctype ? ctype : "");

        e->size_bytes = (gint64)g_file_info_get_size(info);
        if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
            e->mtime = (guint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        } else {
            e->mtime = 0;
        }
        if (ft == G_FILE_TYPE_DIRECTORY) {
            snprintf(e->size, sizeof(e->size), "—");
        } else {
            fmt_size((guint64)e->size_bytes, e->size, sizeof(e->size));
        }

        e->file = g_file_get_child(a->location, name);
        e->selected = 0;
        e->thumb = NULL;
        e->thumb_w = e->thumb_h = 0;
        e->thumb_req_size = 0;
        e->thumb_pixbuf = NULL;
        e->thumb_failed = 0;

        /* reuse thumb cache from old entry with same name and type */
        for (int oi = 0; oi < old_count; oi++) {
            if (strcmp(old[oi].name, e->name) == 0 && old[oi].ftype == e->ftype) {
                e->thumb = old[oi].thumb; old[oi].thumb = NULL;
                e->thumb_w = old[oi].thumb_w;
                e->thumb_h = old[oi].thumb_h;
                e->thumb_req_size = old[oi].thumb_req_size;
                e->thumb_pixbuf = old[oi].thumb_pixbuf; old[oi].thumb_pixbuf = NULL;
                e->thumb_failed = old[oi].thumb_failed;
                e->selected = old[oi].selected;
                break;
            }
        }
        g_object_unref(info);
    }
    g_object_unref(en);

    /* free remaining old entries */
    for (int i = 0; i < old_count; i++) {
        free_entry_thumb(&old[i]);
        if (old[i].file) g_object_unref(old[i].file);
    }

    g_sort_mode = a->sort_mode;
    g_sort_desc = a->sort_desc;
    qsort(a->entries, (size_t)a->entry_count, sizeof(Entry), entry_cmp);
    draw_app(a);
}

/* ════════════════════════════════════════════════════════════════════
 * Sidebar
 * ══════════════════════════════════════════════════════════════════*/
static void side_add(App *a, const char *name, SideKind kind,
                     GFile *file, GVolume *vol, GMount *mnt, int indent,
                     int expandable, int expanded) {
    if (a->side_count >= MAX_SIDE) return;
    SideEntry *s = &a->side[a->side_count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->kind   = kind;
    s->file   = file   ? g_object_ref(file)   : NULL;
    s->volume = vol    ? g_object_ref(vol)    : NULL;
    s->mount  = mnt    ? g_object_ref(mnt)    : NULL;
    s->indent = indent;
    s->expandable = expandable;
    s->expanded = expanded;
}

static void side_free_all(App *a) {
    for (int i = 0; i < a->side_count; i++) {
        if (a->side[i].file)   { g_object_unref(a->side[i].file);   a->side[i].file   = NULL; }
        if (a->side[i].volume) { g_object_unref(a->side[i].volume); a->side[i].volume = NULL; }
        if (a->side[i].mount)  { g_object_unref(a->side[i].mount);  a->side[i].mount  = NULL; }
    }
    a->side_count = 0;
}

static void refresh_sidebar(App *a) {
    side_free_all(a);

    side_add(a, "Places", ST_PLACES_GROUP, NULL, NULL, NULL, 0, 1, a->places_expanded);
    if (a->places_expanded) {
        GFile *home = get_home_file();
        GFile *root = g_file_new_for_path("/");
        side_add(a, "Home",        ST_HOME, home, NULL, NULL, 1, 0, 0);
        side_add(a, "File System", ST_ROOT, root, NULL, NULL, 1, 0, 0);
        g_object_unref(home);
        g_object_unref(root);
    }

    side_add(a, "Bookmarks", ST_BM_GROUP, NULL, NULL, NULL, 0, 1, a->bm_expanded);
    if (a->bm_expanded) {
        for (int i = 0; i < a->bookmark_count; i++) {
            char *bn = g_file_get_basename(a->bookmarks[i]);
            if (!bn || !bn[0]) { g_free(bn); bn = gfm_get_path_or_uri(a->bookmarks[i]); }
            side_add(a, bn, ST_BM, a->bookmarks[i], NULL, NULL, 1, 0, 0);
            g_free(bn);
        }
    }

    side_add(a, "Recent", ST_RECENT_GROUP, NULL, NULL, NULL, 0, 1, a->recent_expanded);
    if (a->recent_expanded) {
        for (int i = 0; i < a->recent_count; i++) {
            if (!is_under_home(a->recent[i])) continue;
            char *bn = g_file_get_basename(a->recent[i]);
            if (!bn || !bn[0]) { g_free(bn); bn = gfm_get_path_or_uri(a->recent[i]); }
            side_add(a, bn, ST_RECENT, a->recent[i], NULL, NULL, 1, 0, 0);
            g_free(bn);
        }
    }

    side_add(a, "Devices", ST_DEV_GROUP, NULL, NULL, NULL, 0, 1, a->dev_expanded);
    if (a->dev_expanded && a->volume_monitor) {
        GList *vols = g_volume_monitor_get_volumes(a->volume_monitor);
        for (GList *item = vols; item; item = item->next) {
            GVolume *vol = item->data;
            GMount  *mnt = g_volume_get_mount(vol);
            if (mnt || g_volume_can_mount(vol)) {
                char *vname = g_volume_get_name(vol);
                GFile *mroot = mnt ? g_mount_get_root(mnt) : NULL;
                side_add(a, vname ? vname : "Volume", ST_VOL,
                         mroot, vol, mnt, 1, 0, 0);
                g_free(vname);
                if (mroot) g_object_unref(mroot);
            }
            if (mnt) g_object_unref(mnt);
        }
        g_list_free_full(vols, g_object_unref);

        GList *mnts = g_volume_monitor_get_mounts(a->volume_monitor);
        for (GList *item = mnts; item; item = item->next) {
            GMount  *mnt = item->data;
            GVolume *vol = g_mount_get_volume(mnt);
            if (!vol && (g_mount_can_unmount(mnt) || g_mount_can_eject(mnt))) {
                char *mname = g_mount_get_name(mnt);
                char label[NAME_SIZE];
                snprintf(label, sizeof(label), "Mounted: %s", mname ? mname : "?");
                GFile *mroot = g_mount_get_root(mnt);
                side_add(a, label, ST_VOL, mroot, NULL, mnt, 1, 0, 0);
                g_free(mname);
                if (mroot) g_object_unref(mroot);
            }
            if (vol) g_object_unref(vol);
        }
        g_list_free_full(mnts, g_object_unref);
    }
}

static void update_free_space(App *a) {
    if (!a->location) { snprintf(a->free_space, sizeof(a->free_space), "Bos alan: -"); return; }
    GError *err = NULL;
    GFileInfo *info = g_file_query_filesystem_info(a->location,
        G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
        NULL, &err);
    if (!info) {
        if (err) g_error_free(err);
        snprintf(a->free_space, sizeof(a->free_space), "Bos alan: -");
        return;
    }
    guint64 freeb = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    char fbuf[64];
    fmt_size(freeb, fbuf, sizeof(fbuf));
    snprintf(a->free_space, sizeof(a->free_space), "Bos alan: %.100s", fbuf);
    g_object_unref(info);
}


/* ════════════════════════════════════════════════════════════════════
 * Navigation
 * ══════════════════════════════════════════════════════════════════*/
static void go_to(App *a, GFile *file, int add_history) {
    GError *err = NULL;
    GFileInfo *info = g_file_query_info(file,
        G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, NULL, &err);
    if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
        if (err) g_error_free(err);
        if (info) g_object_unref(info);

        /* fallback to effective home if requested location is unavailable */
        GFile *home = get_home_file();
        if (file && !g_file_equal(file, home)) {
            go_to(a, home, add_history);
            g_object_unref(home);
            return;
        }
        g_object_unref(home);
        show_error(a, "Cannot open location", "Start location is not available");
        return;
    }
    g_object_unref(info);

    if (a->location) g_object_unref(a->location);
    a->location = g_object_ref(file);

    if (add_history) {
        /* truncate forward */
        for (int i = a->history_index + 1; i < a->history_count; i++)
            g_object_unref(a->history[i]);
        a->history_count = a->history_index + 1;
        if (a->history_count < MAX_HISTORY) {
            a->history[a->history_count++] = g_object_ref(file);
            a->history_index = a->history_count - 1;
        }
    }

    address_set(a, file);
    update_free_space(a);
    update_dir_monitor(a);
    refresh_listing(a);
    remember_location(a, file);
}

static void nav_back(App *a) {
    if (a->history_index > 0) {
        a->history_index--;
        go_to(a, a->history[a->history_index], 0);
    }
}

static void nav_forward(App *a) {
    if (a->history_index + 1 < a->history_count) {
        a->history_index++;
        go_to(a, a->history[a->history_index], 0);
    }
}

static void nav_up(App *a) {
    if (!a->location) return;
    GFile *parent = g_file_get_parent(a->location);
    if (parent) { go_to(a, parent, 1); g_object_unref(parent); }
}

/* ════════════════════════════════════════════════════════════════════
 * Realtime directory monitoring
 * ══════════════════════════════════════════════════════════════════*/
static gboolean refresh_idle_cb(gpointer ud) {
    App *a = ud;
    a->refresh_idle_id = 0;
    refresh_listing(a);
    return G_SOURCE_REMOVE;
}

static void schedule_refresh(App *a) {
    if (a->refresh_idle_id) return;
    a->refresh_idle_id = g_timeout_add(200, refresh_idle_cb, a);
}

static void on_dir_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                           GFileMonitorEvent event_type, gpointer ud) {
    (void)monitor; (void)file; (void)other; (void)event_type;
    schedule_refresh((App *)ud);
}

static void update_dir_monitor(App *a) {
    if (a->refresh_idle_id) {
        g_source_remove(a->refresh_idle_id);
        a->refresh_idle_id = 0;
    }
    if (a->dir_monitor) {
        if (a->dir_monitor_id)
            g_signal_handler_disconnect(a->dir_monitor, a->dir_monitor_id);
        g_file_monitor_cancel(a->dir_monitor);
        g_object_unref(a->dir_monitor);
        a->dir_monitor = NULL;
        a->dir_monitor_id = 0;
    }
    if (!a->location) return;
    GError *err = NULL;
    a->dir_monitor = g_file_monitor_directory(a->location,
                                              G_FILE_MONITOR_WATCH_MOVES,
                                              NULL, &err);
    if (!a->dir_monitor) {
        if (err) g_error_free(err);
        return;
    }
    a->dir_monitor_id = g_signal_connect(a->dir_monitor, "changed",
                                         G_CALLBACK(on_dir_changed), a);
}

/* ════════════════════════════════════════════════════════════════════
 * Open entry
 * ══════════════════════════════════════════════════════════════════*/
static void open_entry(App *a, int index) {
    if (index < 0 || index >= a->entry_count) return;
    Entry *e = &a->entries[index];
    if (e->ftype == FT_DIR) {
        go_to(a, e->file, 1);
    } else {
        char *uri = g_file_get_uri(e->file);
        GError *err = NULL;
        g_app_info_launch_default_for_uri(uri, NULL, &err);
        if (err) { show_error(a, "Cannot open file", err->message); g_error_free(err); }
        g_free(uri);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Volume mount callback
 * ══════════════════════════════════════════════════════════════════*/
static void on_volume_mounted(GObject *src, GAsyncResult *res, gpointer ud) {
    App *a = ud;
    GError *err = NULL;
    if (!g_volume_mount_finish(G_VOLUME(src), res, &err)) {
        show_error(a, "Could not mount device", err ? err->message : "");
        if (err) g_error_free(err);
        return;
    }
    GMount *mnt = g_volume_get_mount(G_VOLUME(src));
    if (mnt) {
        GFile *root = g_mount_get_root(mnt);
        go_to(a, root, 1);
        g_object_unref(root);
        g_object_unref(mnt);
    }
    refresh_sidebar(a);
    draw_app(a);
}

static void on_mount_unmounted(GObject *src, GAsyncResult *res, gpointer ud) {
    App *a = ud;
    GError *err = NULL;
    if (!g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err)) {
        show_error(a, "Could not unmount", err ? err->message : "");
        if (err) g_error_free(err);
    }
    refresh_sidebar(a);
    draw_app(a);
}

static void on_volume_changed(GVolumeMonitor *m, GObject *o, gpointer ud) {
    (void)m; (void)o;
    App *a = ud;
    refresh_sidebar(a);
    draw_app(a);
}

/* ════════════════════════════════════════════════════════════════════
 * Copy queue (background thread)
 * ══════════════════════════════════════════════════════════════════*/
static void copy_progress_cb(goffset cur, goffset total, gpointer ud) {
    App *a = ud;
    pthread_mutex_lock(&a->copy_mutex);
    a->copy_fraction = total > 0 ? (double)cur / (double)total : 0.0;
    pthread_mutex_unlock(&a->copy_mutex);
}

static void *copy_worker(void *ud) {
    App *a = ud;
    for (;;) {
        pthread_mutex_lock(&a->copy_mutex);
        while (a->copy_q_len == 0 && !a->shutting_down)
            pthread_cond_wait(&a->copy_cond, &a->copy_mutex);
        if (a->shutting_down) { pthread_mutex_unlock(&a->copy_mutex); break; }

        CopyJob job = a->copy_queue[a->copy_q_head];
        a->copy_q_head = (a->copy_q_head + 1) % MAX_COPY_Q;
        a->copy_q_len--;
        char *p = g_file_get_path(job.source);
        if (!p) p = g_file_get_uri(job.source);
        snprintf(a->copy_current, sizeof(a->copy_current), "%s", p);
        g_free(p);
        a->copy_fraction = 0.0;
        pthread_mutex_unlock(&a->copy_mutex);

        GError *err = NULL;
        if (job.is_move)
            gfm_move_to_directory(job.source, job.destination_directory, &err);
        else
            gfm_copy_to_directory_full(job.source, job.destination_directory,
                                       NULL, copy_progress_cb, a, &err);

        pthread_mutex_lock(&a->copy_mutex);
        if (err) {
            snprintf(a->copy_error, sizeof(a->copy_error), "%s", err->message);
            g_error_free(err);
        }
        if (a->copy_q_len == 0) {
            a->copy_running = 0;
            a->copy_fraction = 0.0;
            a->copy_current[0] = '\0';
        }
        pthread_mutex_unlock(&a->copy_mutex);

        g_object_unref(job.source);
        g_object_unref(job.destination_directory);
    }
    return NULL;
}

static void queue_copy(App *a, GFile *src, GFile *dst, int is_move) {
    pthread_mutex_lock(&a->copy_mutex);
    if (a->copy_q_len < MAX_COPY_Q) {
        CopyJob *j = &a->copy_queue[a->copy_q_tail];
        j->source = g_object_ref(src);
        j->destination_directory = g_object_ref(dst);
        j->is_move = is_move;
        a->copy_q_tail = (a->copy_q_tail + 1) % MAX_COPY_Q;
        a->copy_q_len++;
        a->copy_running = 1;
        pthread_cond_signal(&a->copy_cond);
    }
    pthread_mutex_unlock(&a->copy_mutex);
}

/* ════════════════════════════════════════════════════════════════════
 * Clipboard (X11 selection protocol)
 * ══════════════════════════════════════════════════════════════════*/
static void clipboard_set_selected(App *a, int is_cut) {
    free(a->clipboard_data);
    a->clipboard_data = NULL;
    a->clipboard_is_cut = is_cut;

    /* collect selected or focused */
    int count = selected_count(a);
    GString *gs = g_string_new(is_cut ? "cut\n" : "copy\n");
    if (count > 0) {
        for (int i = 0; i < a->entry_count; i++) {
            if (!a->entries[i].selected) continue;
            char *uri = g_file_get_uri(a->entries[i].file);
            g_string_append(gs, uri);
            g_string_append_c(gs, '\n');
            g_free(uri);
        }
    } else if (a->list_sel >= 0 && a->list_sel < a->entry_count) {
        char *uri = g_file_get_uri(a->entries[a->list_sel].file);
        g_string_append(gs, uri);
        g_string_append_c(gs, '\n');
        g_free(uri);
    } else {
        g_string_free(gs, TRUE);
        return;
    }

    a->clipboard_data = g_string_free(gs, FALSE);
    XSetSelectionOwner(a->dpy, a->atom_clipboard, a->win, CurrentTime);
    XFlush(a->dpy);
}

static void handle_selection_request(App *a, XSelectionRequestEvent *req) {
    XSelectionEvent ev = {0};
    ev.type      = SelectionNotify;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target    = req->target;
    ev.time      = req->time;
    ev.property  = None;

    if (!a->clipboard_data) {
        XSendEvent(a->dpy, req->requestor, False, 0, (XEvent *)&ev);
        return;
    }

    if (req->target == a->atom_targets) {
        Atom targets[4] = { a->atom_gnome_files, a->atom_uri_list,
                             a->atom_utf8, XA_STRING };
        XChangeProperty(a->dpy, req->requestor, req->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)targets, 4);
        ev.property = req->property;
    } else if (req->target == a->atom_gnome_files ||
               req->target == a->atom_uri_list    ||
               req->target == a->atom_utf8 ||
               req->target == XA_STRING) {
        XChangeProperty(a->dpy, req->requestor, req->property,
                        req->target == XA_STRING ? XA_STRING : req->target,
                        8, PropModeReplace,
                        (unsigned char *)a->clipboard_data,
                        (int)strlen(a->clipboard_data));
        ev.property = req->property;
    }
    XSendEvent(a->dpy, req->requestor, False, 0, (XEvent *)&ev);
}

static void clipboard_paste(App *a) {
    if (XGetSelectionOwner(a->dpy, a->atom_clipboard) == None) return;

    /* Request GNOME format first */
    XConvertSelection(a->dpy, a->atom_clipboard, a->atom_gnome_files,
                      a->atom_gnome_files, a->win, CurrentTime);
    XFlush(a->dpy);
    /* Result handled in SelectionNotify event */
}

static void handle_selection_notify(App *a, XSelectionEvent *ev) {
    if (ev->property == None) return;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    XGetWindowProperty(a->dpy, a->win, ev->property, 0, 65536, True,
                       AnyPropertyType, &actual_type, &actual_format,
                       &nitems, &bytes_after, &data);
    if (!data) return;

    char *text = (char *)data;
    gboolean is_move = FALSE;
    GPtrArray *files = gfm_parse_uri_list(text, &is_move);
    XFree(data);

    if (!files) return;
    for (guint i = 0; i < files->len; i++) {
        GFile *src = g_ptr_array_index(files, i);
        queue_copy(a, src, a->location, (int)is_move);
    }
    g_ptr_array_unref(files);
    refresh_listing(a);
}

/* ════════════════════════════════════════════════════════════════════
 * Dialogs
 * ══════════════════════════════════════════════════════════════════*/
static void show_error(App *a, const char *summary, const char *detail) {
    snprintf(a->err_text,   sizeof(a->err_text),   "%s", summary ? summary : "Error");
    snprintf(a->err_detail, sizeof(a->err_detail), "%s", detail  ? detail  : "");
    a->err_open    = 1;
    a->dialog_open = 0;
    a->props_open  = 0;
    draw_app(a);
}

static void start_dialog(App *a, const char *title, const char *prompt,
                         void (*ok_cb)(void *)) {
    snprintf(a->dialog_title,  sizeof(a->dialog_title),  "%s", title);
    snprintf(a->dialog_prompt, sizeof(a->dialog_prompt), "%s", prompt);
    a->dialog_value[0] = '\0';
    a->dialog_len = 0;
    a->dialog_ok  = ok_cb;
    a->dialog_open = 1;
    a->err_open    = 0;
    a->props_open  = 0;
    draw_app(a);
}

static void close_picker(App *a);

static void picker_clear(App *a) {
    for (int i = 0; i < a->picker_count; i++) {
        if (a->picker_files[i]) { g_object_unref(a->picker_files[i]); a->picker_files[i] = NULL; }
    }
    a->picker_count = 0;
    a->picker_sel = -1;
    a->picker_scroll = 0;
}

static void close_picker(App *a) {
    picker_clear(a);
    if (a->picker_dir) { g_object_unref(a->picker_dir); a->picker_dir = NULL; }
    a->picker_open = 0;
    a->picker_move = 0;
}

static void picker_refresh(App *a, GFile *dir) {
    picker_clear(a);
    if (a->picker_dir) g_object_unref(a->picker_dir);
    a->picker_dir = g_object_ref(dir);

    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children(
        dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &err);
    if (!en) { if (err) g_error_free(err); return; }

    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, NULL, NULL)) != NULL) {
        const char *name = g_file_info_get_name(info);
        gboolean hidden = g_file_info_get_is_hidden(info) || (name && name[0] == '.');
        GFileType ft = g_file_info_get_file_type(info);
        if (hidden || ft != G_FILE_TYPE_DIRECTORY) { g_object_unref(info); continue; }
        if (a->picker_count >= 256) { g_object_unref(info); continue; }
        snprintf(a->picker_names[a->picker_count], NAME_SIZE, "%s", name);
        a->picker_files[a->picker_count] = g_file_get_child(dir, name);
        a->picker_count++;
        g_object_unref(info);
    }
    g_object_unref(en);

    /* sort by name */
    for (int i = 0; i < a->picker_count; i++) {
        for (int j = i + 1; j < a->picker_count; j++) {
            if (strcasecmp(a->picker_names[i], a->picker_names[j]) > 0) {
                char tmpn[NAME_SIZE]; snprintf(tmpn, sizeof(tmpn), "%s", a->picker_names[i]);
                snprintf(a->picker_names[i], NAME_SIZE, "%s", a->picker_names[j]);
                snprintf(a->picker_names[j], NAME_SIZE, "%s", tmpn);
                GFile *tmpf = a->picker_files[i]; a->picker_files[i] = a->picker_files[j]; a->picker_files[j] = tmpf;
            }
        }
    }
}

static void picker_select_current(App *a) {
    if (!a->picker_dir) return;
    GFile *dst = a->picker_dir;
    int count = selected_count(a);
    if (count > 0) {
        for (int i = 0; i < a->entry_count; i++)
            if (a->entries[i].selected)
                queue_copy(a, a->entries[i].file, dst, a->picker_move);
    } else if (a->list_sel >= 0 && a->list_sel < a->entry_count) {
        queue_copy(a, a->entries[a->list_sel].file, dst, a->picker_move);
    }
    close_picker(a);
}

static void open_picker(App *a, int is_move) {
    close_picker(a);
    a->picker_open = 1;
    a->picker_move = is_move;
    a->dialog_open = 0;
    a->menu_open = 0;
    GFile *start = a->location ? g_object_ref(a->location) : get_home_file();
    picker_refresh(a, start);
    g_object_unref(start);
    draw_app(a);
}

static void show_props(App *a, int index) {
    if (index < 0 || index >= a->entry_count) return;
    Entry *e = &a->entries[index];
    snprintf(a->props_path, sizeof(a->props_path), "%s", e->name);
    snprintf(a->props_type, sizeof(a->props_type), "%s", e->type_str);
    snprintf(a->props_size, sizeof(a->props_size), "%s", e->size);

    GError *err = NULL;
    GFileInfo *info = g_file_query_info(e->file,
        G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_UNIX_MODE,
        G_FILE_QUERY_INFO_NONE, NULL, &err);
    if (info) {
        guint64 mt = g_file_info_get_attribute_uint64(
            info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        fmt_time(mt, a->props_mtime, sizeof(a->props_mtime));
        a->props_has_mode = g_file_info_has_attribute(
            info, G_FILE_ATTRIBUTE_UNIX_MODE);
        if (a->props_has_mode)
            a->props_mode = g_file_info_get_attribute_uint32(
                info, G_FILE_ATTRIBUTE_UNIX_MODE);
        g_object_unref(info);
    } else {
        snprintf(a->props_mtime, sizeof(a->props_mtime), "—");
        a->props_has_mode = 0;
        if (err) g_error_free(err);
    }
    if (a->props_file) g_object_unref(a->props_file);
    a->props_file = g_object_ref(e->file);
    a->props_open  = 1;
    a->dialog_open = 0;
    a->err_open    = 0;
    draw_app(a);
}

/* ════════════════════════════════════════════════════════════════════
 * Context menu items
 * ══════════════════════════════════════════════════════════════════*/
typedef struct { const char *icon; const char *label; int id; } MenuItem;

#define MENU_NONE         0
#define MENU_OPEN         1
#define MENU_OPEN_WINE    2
#define MENU_EXTRACT_HERE 3
#define MENU_COPY_PATH    4
#define MENU_COPY         5
#define MENU_CUT          6
#define MENU_PASTE        7
#define MENU_COPYTO       8
#define MENU_MOVETO       9
#define MENU_RENAME      10
#define MENU_TRASH       11
#define MENU_DELETE      12
#define MENU_PROPS       13
#define MENU_NEWFOLDER   14
#define MENU_NEWFILE     15
#define MENU_REFRESH     16
#define MENU_VIEW_LIST   17
#define MENU_VIEW_ICONS  18
#define MENU_SORT_NAME   19
#define MENU_SORT_TYPE   20
#define MENU_SORT_SIZE   21
#define MENU_SORT_DATE   22
#define MENU_SORT_ASC    23
#define MENU_SORT_DESC   24

static MenuItem g_menu_items[40];
static int g_menu_count = 0;

static int file_is_archive(const Entry *e) {
    if (!e) return 0;
    const char *n = e->name;
    size_t l = strlen(n);
    const char *exts[] = {".zip",".tar",".tar.gz",".tgz",".tar.bz2",".tbz2",".tar.xz",".txz",".tar.zst",".7z",".rar",".gz",".bz2",".xz",NULL};
    for (int i = 0; exts[i]; i++) {
        size_t el = strlen(exts[i]);
        if (l >= el && strcasecmp(n + l - el, exts[i]) == 0) return 1;
    }
    if (strcasestr(e->content_type, "zip") || strcasestr(e->content_type, "tar") ||
        strcasestr(e->content_type, "rar") || strcasestr(e->content_type, "7z") ||
        strcasestr(e->content_type, "compressed")) return 1;
    return 0;
}

static int file_is_windows_exec(const Entry *e) {
    if (!e) return 0;
    const char *n = e->name; size_t l = strlen(n);
    return (l>=4 && strcasecmp(n+l-4,".exe")==0) ||
           (l>=4 && strcasecmp(n+l-4,".msi")==0) ||
           (l>=4 && strcasecmp(n+l-4,".bat")==0);
}

static int has_cmd(const char *cmd) {
    gchar *p = g_find_program_in_path(cmd);
    if (p) { g_free(p); return 1; }
    return 0;
}

static void menu_push(const char *icon, const char *label, int id) {
    if (g_menu_count >= (int)(sizeof(g_menu_items)/sizeof(g_menu_items[0]))) return;
    g_menu_items[g_menu_count].icon = icon;
    g_menu_items[g_menu_count].label = label;
    g_menu_items[g_menu_count].id = id;
    g_menu_count++;
}

static void build_context_menu(App *a) {
    g_menu_count = 0;
    Entry *sel = (a->list_sel >= 0 && a->list_sel < a->entry_count) ? &a->entries[a->list_sel] : NULL;
    if (a->menu_on_file && sel) {
        menu_push("↗", "Open", MENU_OPEN);
        if (file_is_windows_exec(sel) && has_cmd("wine"))
            menu_push("🍷", "Open with Wine", MENU_OPEN_WINE);
        if (file_is_archive(sel))
            menu_push("📦", "Extract Here", MENU_EXTRACT_HERE);
        menu_push("", "", MENU_NONE);
        menu_push("⧉", "Copy", MENU_COPY);
        menu_push("✂", "Cut", MENU_CUT);
        menu_push("📋", "Paste", MENU_PASTE);
        menu_push("⌘", "Copy Path", MENU_COPY_PATH);
        menu_push("⎘", "Copy to...", MENU_COPYTO);
        menu_push("➜", "Move to...", MENU_MOVETO);
        menu_push("", "", MENU_NONE);
        menu_push("✎", "Rename", MENU_RENAME);
        menu_push("🗑", "Move to Trash", MENU_TRASH);
        menu_push("✖", "Delete Permanently", MENU_DELETE);
        menu_push("", "", MENU_NONE);
        menu_push("ℹ", "Properties", MENU_PROPS);
    } else {
        menu_push("+", "New Folder", MENU_NEWFOLDER);
        menu_push("+", "New File", MENU_NEWFILE);
        menu_push("📋", "Paste", MENU_PASTE);
        menu_push("↻", "Refresh", MENU_REFRESH);
        menu_push("", "", MENU_NONE);
        menu_push("▦", a->icon_mode ? "List View" : "Icon View", a->icon_mode ? MENU_VIEW_LIST : MENU_VIEW_ICONS);
        menu_push("", "", MENU_NONE);
        menu_push(a->sort_mode == SORT_NAME ? "●" : "○", "Sort by Name", MENU_SORT_NAME);
        menu_push(a->sort_mode == SORT_DATE ? "●" : "○", "Sort by Date", MENU_SORT_DATE);
        menu_push(a->sort_mode == SORT_TYPE ? "●" : "○", "Sort by Type", MENU_SORT_TYPE);
        menu_push(a->sort_mode == SORT_SIZE ? "●" : "○", "Sort by Size", MENU_SORT_SIZE);
        menu_push("", "", MENU_NONE);
        menu_push(!a->sort_desc ? "●" : "○", "Ascending", MENU_SORT_ASC);
        menu_push(a->sort_desc ? "●" : "○", "Descending", MENU_SORT_DESC);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Context callbacks
 * ══════════════════════════════════════════════════════════════════*/
static void rename_cancel(App *a) {
    a->rename_active = 0;
    a->rename_index = -1;
    a->rename_text[0] = '\0';
    a->rename_len = 0;
    a->rename_cursor = 0;
}

static void rename_commit(App *a) {
    if (!a->rename_active || a->rename_index < 0 || a->rename_index >= a->entry_count) {
        rename_cancel(a);
        return;
    }
    Entry *e = &a->entries[a->rename_index];
    if (a->rename_len <= 0) { rename_cancel(a); draw_app(a); return; }
    if (strcmp(e->name, a->rename_text) == 0) { rename_cancel(a); draw_app(a); return; }

    char newname[NAME_SIZE];
    snprintf(newname, sizeof(newname), "%.255s", a->rename_text);

    XImage *thumb = e->thumb;
    GdkPixbuf *pb = e->thumb_pixbuf;
    int tw = e->thumb_w, th = e->thumb_h, trs = e->thumb_req_size, tf = e->thumb_failed;
    e->thumb = NULL;
    e->thumb_pixbuf = NULL;

    GError *err = NULL;
    if (!gfm_rename(e->file, a->rename_text, &err)) {
        e->thumb = thumb; e->thumb_pixbuf = pb;
        e->thumb_w = tw; e->thumb_h = th; e->thumb_req_size = trs; e->thumb_failed = tf;
        rename_cancel(a);
        show_error(a, "Rename failed", err ? err->message : "Unknown error");
        if (err) g_error_free(err);
        return;
    }

    rename_cancel(a);
    refresh_listing(a);
    for (int i = 0; i < a->entry_count; i++) {
        if (strcmp(a->entries[i].name, newname) == 0) {
            if (a->entries[i].thumb) XDestroyImage(a->entries[i].thumb);
            if (a->entries[i].thumb_pixbuf) g_object_unref(a->entries[i].thumb_pixbuf);
            a->entries[i].thumb = thumb;
            a->entries[i].thumb_pixbuf = pb;
            a->entries[i].thumb_w = tw; a->entries[i].thumb_h = th;
            a->entries[i].thumb_req_size = trs; a->entries[i].thumb_failed = tf;
            a->list_sel = i; a->entries[i].selected = 1; a->sel_anchor = i;
            draw_app(a);
            return;
        }
    }
    if (thumb) XDestroyImage(thumb);
    if (pb) g_object_unref(pb);
    draw_app(a);
}

static void rename_start(App *a, int index) {
    if (index < 0 || index >= a->entry_count) return;
    a->menu_open = 0;
    a->dialog_open = 0;
    a->err_open = 0;
    a->props_open = 0;
    select_only(a, index);
    a->rename_active = 1;
    a->rename_index = index;
    snprintf(a->rename_text, sizeof(a->rename_text), "%s", a->entries[index].name);
    a->rename_len = (int)strlen(a->rename_text);
    a->rename_cursor = a->rename_len;
    a->caret_on = 1; a->caret_tick = 0;
    draw_app(a);
}

static void cb_newfile_ok(void *ud) {
    App *a = ud;
    GError *err = NULL;
    gfm_create_file(a->location, a->dialog_value, &err);
    if (err) { show_error(a, "Cannot create file", err->message); g_error_free(err); }
    else refresh_listing(a);
}

static void cb_newdir_ok(void *ud) {
    App *a = ud;
    GError *err = NULL;
    gfm_create_directory(a->location, a->dialog_value, &err);
    if (err) { show_error(a, "Cannot create folder", err->message); g_error_free(err); }
    else refresh_listing(a);
}

static gboolean run_cmd_in_dir(GFile *dir, char **argv, GError **error) {
    char *cwd = dir ? g_file_get_path(dir) : NULL;
    gboolean ok = g_spawn_sync(cwd, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, error);
    g_free(cwd);
    return ok;
}

static void extract_archive(App *a, Entry *e) {
    if (!e || !e->file || !a->location) return;
    char *path = g_file_get_path(e->file);
    if (!path) { show_error(a, "Extract failed", "File has no local path"); return; }
    const char *name = e->name;
    size_t l = strlen(name);
    GError *err = NULL;
    gboolean ok = FALSE;
    if (l>=4 && strcasecmp(name+l-4, ".zip")==0) {
        char *argv[] = {"unzip", "-o", path, NULL};
        ok = run_cmd_in_dir(a->location, argv, &err);
    } else if (l>=4 && strcasecmp(name+l-4, ".rar")==0) {
        if (has_cmd("unrar")) { char *argv[] = {"unrar", "x", "-o+", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err); }
        else { char *argv[] = {"7z", "x", "-y", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err); }
    } else if (l>=3 && strcasecmp(name+l-3, ".7z")==0) {
        char *argv[] = {"7z", "x", "-y", path, NULL};
        ok = run_cmd_in_dir(a->location, argv, &err);
    } else if ((l>=7 && strcasecmp(name+l-7, ".tar.gz")==0) || (l>=4 && strcasecmp(name+l-4, ".tgz")==0)) {
        char *argv[] = {"tar", "-xzf", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if ((l>=8 && strcasecmp(name+l-8, ".tar.bz2")==0) || (l>=5 && strcasecmp(name+l-5, ".tbz2")==0)) {
        char *argv[] = {"tar", "-xjf", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if ((l>=7 && strcasecmp(name+l-7, ".tar.xz")==0) || (l>=4 && strcasecmp(name+l-4, ".txz")==0)) {
        char *argv[] = {"tar", "-xJf", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if (l>=8 && strcasecmp(name+l-8, ".tar.zst")==0) {
        char *argv[] = {"tar", "--zstd", "-xf", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if (l>=4 && strcasecmp(name+l-4, ".tar")==0) {
        char *argv[] = {"tar", "-xf", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if (l>=3 && strcasecmp(name+l-3, ".gz")==0) {
        char *argv[] = {"gunzip", "-k", "-f", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if (l>=4 && strcasecmp(name+l-4, ".bz2")==0) {
        char *argv[] = {"bunzip2", "-k", "-f", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else if (l>=3 && strcasecmp(name+l-3, ".xz")==0) {
        char *argv[] = {"unxz", "-k", "-f", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err);
    } else {
        /* try 7z as generic fallback */
        if (has_cmd("7z")) { char *argv[] = {"7z", "x", "-y", path, NULL}; ok = run_cmd_in_dir(a->location, argv, &err); }
        else { g_set_error_literal(&err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "No suitable extractor found"); }
    }
    g_free(path);
    if (!ok) { show_error(a, "Extract failed", err ? err->message : "Unknown error"); if (err) g_error_free(err); }
    else refresh_listing(a);
}

static void open_with_wine(App *a, Entry *e) {
    if (!e || !e->file) return;
    char *path = g_file_get_path(e->file);
    if (!path) { show_error(a, "Wine", "File has no local path"); return; }
    GError *err = NULL;
    char *argv[] = {"wine", path, NULL};
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
        show_error(a, "Open with Wine failed", err ? err->message : "Unknown error");
        if (err) g_error_free(err);
    }
    g_free(path);
}

static void perform_menu(App *a, int id) {
    Entry *sel = (a->list_sel >= 0 && a->list_sel < a->entry_count)
                 ? &a->entries[a->list_sel] : NULL;
    switch (id) {
    case MENU_OPEN:
        if (sel) open_entry(a, a->list_sel);
        break;
    case MENU_COPY_PATH:
        if (sel || selected_count(a) > 0) {
            /* copy filesystem path(s) as plain text */
            free(a->clipboard_data);
            a->clipboard_data = NULL;
            GString *gs = g_string_new(NULL);
            int count = selected_count(a);
            if (count > 0) {
                for (int i = 0; i < a->entry_count; i++) {
                    if (!a->entries[i].selected) continue;
                    char *p = g_file_get_path(a->entries[i].file);
                    if (!p) p = g_file_get_uri(a->entries[i].file);
                    if (gs->len > 0) g_string_append_c(gs, '\n');
                    g_string_append(gs, p);
                    g_free(p);
                }
            } else {
                char *p = g_file_get_path(a->entries[a->list_sel].file);
                if (!p) p = g_file_get_uri(a->entries[a->list_sel].file);
                g_string_append(gs, p);
                g_free(p);
            }
            a->clipboard_data = g_string_free(gs, FALSE);
            a->clipboard_is_cut = 0;
            XSetSelectionOwner(a->dpy, a->atom_clipboard, a->win, CurrentTime);
            XFlush(a->dpy);
        }
        break;
    case MENU_COPY:
        if (sel || selected_count(a) > 0) clipboard_set_selected(a, 0);
        break;
    case MENU_CUT:
        if (sel || selected_count(a) > 0) clipboard_set_selected(a, 1);
        break;
    case MENU_PASTE:
        clipboard_paste(a);
        break;
    case MENU_COPYTO:
        if (sel || selected_count(a) > 0) open_picker(a, 0);
        break;
    case MENU_MOVETO:
        if (sel || selected_count(a) > 0) open_picker(a, 1);
        break;
    case MENU_RENAME:
        if (a->list_sel >= 0) rename_start(a, a->list_sel);
        break;
    case MENU_TRASH: {
        int count = selected_count(a);
        if (count == 0 && !sel) break;
        GError *err = NULL;
        gboolean ok = TRUE;
        if (count > 0) {
            for (int i = 0; i < a->entry_count; i++) {
                if (!a->entries[i].selected) continue;
                g_clear_error(&err);
                if (!gfm_trash(a->entries[i].file, &err)) ok = FALSE;
            }
        } else {
            if (!gfm_trash(sel->file, &err)) ok = FALSE;
        }
        if (!ok && err) { show_error(a, "Move to Trash failed", err->message); g_error_free(err); }
        else if (err) g_error_free(err);
        refresh_listing(a);
        break;
    }
    case MENU_DELETE: {
        int count = selected_count(a);
        if (count == 0 && !sel) break;
        GError *err = NULL;
        gboolean ok = TRUE;
        if (count > 0) {
            for (int i = 0; i < a->entry_count; i++) {
                if (!a->entries[i].selected) continue;
                g_clear_error(&err);
                if (!gfm_delete(a->entries[i].file, &err)) ok = FALSE;
            }
        } else {
            if (!gfm_delete(sel->file, &err)) ok = FALSE;
        }
        if (!ok && err) { show_error(a, "Delete failed", err->message); g_error_free(err); }
        else if (err) g_error_free(err);
        refresh_listing(a);
        break;
    }
    case MENU_PROPS:
        if (sel) show_props(a, a->list_sel);
        break;
    case MENU_NEWFOLDER:
        start_dialog(a, "New Folder", "Folder name:", cb_newdir_ok);
        break;
    case MENU_NEWFILE:
        start_dialog(a, "New File", "File name:", cb_newfile_ok);
        break;
    case MENU_OPEN_WINE:
        if (sel) open_with_wine(a, sel);
        break;
    case MENU_EXTRACT_HERE:
        if (sel) extract_archive(a, sel);
        break;
    case MENU_REFRESH:
        refresh_listing(a);
        break;
    case MENU_VIEW_LIST:
        a->icon_mode = 0; a->list_scroll = 0; draw_app(a);
        break;
    case MENU_VIEW_ICONS:
        a->icon_mode = 1; a->icon_zoom = 0; a->list_scroll = 0; clear_entry_thumbs(a); draw_app(a);
        break;
    case MENU_SORT_NAME: a->sort_mode = SORT_NAME; refresh_listing(a); break;
    case MENU_SORT_TYPE: a->sort_mode = SORT_TYPE; refresh_listing(a); break;
    case MENU_SORT_SIZE: a->sort_mode = SORT_SIZE; refresh_listing(a); break;
    case MENU_SORT_DATE: a->sort_mode = SORT_DATE; refresh_listing(a); break;
    case MENU_SORT_ASC:  a->sort_desc = 0; refresh_listing(a); break;
    case MENU_SORT_DESC: a->sort_desc = 1; refresh_listing(a); break;
    default:
        break;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Drawing
 * ══════════════════════════════════════════════════════════════════*/

/* ── buttons in toolbar ── */
#define MAX_BTNS 20
static Btn g_btns[MAX_BTNS];
static int g_btn_count = 0;
#define BTN_BACK     0
#define BTN_FORWARD  1
#define BTN_UP       2
#define BTN_GO       3
#define BTN_HIDDEN   4
#define BTN_NEWFOLDER 5
#define BTN_NEWFILE  6
#define BTN_BM_ADD   7
#define BTN_BM_REM   8
#define BTN_VIEW_LIST 9
#define BTN_VIEW_ICONS 10
#define BTN_VIEW_LARGE 11

static void layout_buttons(App *a) {
    g_btn_count = 0;
    int y0 = bar_y() + (BAR_H - BTN_H) / 2;
    int x  = PAD;

    /* nav icon buttons */
    int sw = 28;
    g_btns[g_btn_count++] = (Btn){x, y0, sw, BTN_H, "", BTN_BACK, UI_ICON_BACK}; x += sw;
    g_btns[g_btn_count++] = (Btn){x, y0, sw, BTN_H, "", BTN_FORWARD, UI_ICON_FORWARD}; x += sw;
    g_btns[g_btn_count++] = (Btn){x, y0, sw, BTN_H, "", BTN_UP, UI_ICON_UP}; x += sw + PAD;

    /* address bar — stretchy */
    int go_w = 28;
    int addr_w = a->win_w - x - go_w - PAD * 2;
    if (addr_w < 60) addr_w = 60;
    g_btns[g_btn_count++] = (Btn){x, y0, addr_w, BTN_H, "", -1, -1};
    x += addr_w + PAD;
    g_btns[g_btn_count++] = (Btn){x, y0, go_w, BTN_H, "", BTN_GO, UI_ICON_GO};

    /* toolbar icon buttons */
    int ty = tool_y() + (TOOL_H - BTN_H) / 2;
    x = PAD;
    int ib = 28;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_NEWFOLDER, UI_ICON_NEW_FOLDER}; x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_NEWFILE,   UI_ICON_NEW_FILE};   x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_HIDDEN,    UI_ICON_HIDDEN};     x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_BM_ADD,    UI_ICON_BM_ADD};     x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_BM_REM,    UI_ICON_BM_REMOVE};  x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_VIEW_LIST, UI_ICON_VIEW_LIST};  x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_VIEW_ICONS,UI_ICON_VIEW_ICONS}; x += ib + PAD;
    g_btns[g_btn_count++] = (Btn){x, ty, ib, BTN_H, "", BTN_VIEW_LARGE,UI_ICON_VIEW_LARGE};
}

static void draw_address_bar(App *a) {
    /* background strip */
    fill_rect(a, &a->panel, 0, bar_y(), a->win_w, BAR_H);
    draw_line(a, &a->edge, 0, BAR_H - 1, a->win_w, BAR_H - 1);

    for (int i = 0; i < g_btn_count; i++) {
        Btn *b = &g_btns[i];
        if (b->y < tool_y()) {
            if (b->id == -1) {
                /* address field */
                fill_rect(a, a->address_focused ? &a->field_on : &a->field,
                          b->x, b->y, b->w, b->h);
                draw_rect(a, a->address_focused ? &a->accent : &a->edge,
                          b->x, b->y, b->w, b->h);
                int ty2 = b->y + (b->h + a->font->ascent - a->font->descent) / 2;
                draw_text_clip(a, a->font, &a->text,
                               b->x + 4, ty2, b->w - 8, a->address);
                if (a->address_focused && a->caret_on) {
                    int cw = text_w(a, a->font, a->address);
                    int maxw = b->w - 10;
                    if (cw > maxw) cw = maxw;
                    int cx = b->x + 4 + cw;
                    draw_line(a, &a->text, cx, b->y + 3, cx, b->y + b->h - 3);
                }
            } else {
                draw_btn(a, b, 0);
            }
        }
    }
}

static void draw_toolbar(App *a) {
    fill_rect(a, &a->panel, 0, tool_y(), a->win_w, TOOL_H);
    draw_line(a, &a->edge, 0, tool_y() + TOOL_H - 1,
              a->win_w, tool_y() + TOOL_H - 1);

    /* search field — right side of toolbar */
    int sw_x = a->win_w - 180 - PAD;
    int sw_y = tool_y() + (TOOL_H - BTN_H) / 2;
    fill_rect(a, a->search_focused ? &a->field_on : &a->field,
              sw_x, sw_y, 180, BTN_H);
    draw_rect(a, a->search_focused ? &a->accent : &a->edge,
              sw_x, sw_y, 180, BTN_H);
    if (!a->search[0]) {
        int ty2 = sw_y + (BTN_H + a->font->ascent - a->font->descent) / 2;
        draw_text(a, a->font, &a->muted, sw_x + 4, ty2, "Search…");
    } else {
        int ty2 = sw_y + (BTN_H + a->font->ascent - a->font->descent) / 2;
        draw_text_clip(a, a->font, &a->text, sw_x + 4, ty2, 172, a->search);
        if (a->search_focused && a->caret_on) {
            int cw = text_w(a, a->font, a->search);
            if (cw > 168) cw = 168;
            int cx = sw_x + 4 + cw;
            draw_line(a, &a->text, cx, sw_y + 3, cx, sw_y + BTN_H - 3);
        }
    }

    for (int i = 0; i < g_btn_count; i++) {
        Btn *b = &g_btns[i];
        if (b->y >= tool_y() && b->id != -1)
            draw_btn(a, b, 0);
    }
}

static void draw_sidebar(App *a) {
    fill_rect(a, &a->panel, 0, list_y(), a->side_w, list_h(a));
    draw_line(a, &a->edge, a->side_w, list_y(),
              a->side_w, list_y() + list_h(a));

    int y    = list_y();
    int rows = list_h(a) / a->row_h;

    for (int i = a->side_scroll;
         i < a->side_count && i < a->side_scroll + rows; i++) {
        SideEntry *s = &a->side[i];
        int row_y = y + (i - a->side_scroll) * a->row_h;
        int indent = s->indent * 14 + PAD;

        int is_group = (s->kind == ST_PLACES_GROUP || s->kind == ST_BM_GROUP ||
                        s->kind == ST_RECENT_GROUP || s->kind == ST_DEV_GROUP);
        int is_sel   = (i == a->side_sel && !is_group);
        int is_drop  = (a->dnd_dragging && i == a->dnd_target_side);

        if (is_drop)
            fill_rect(a, &a->accent, 0, row_y, a->side_w, a->row_h);
        else if (is_sel)
            fill_rect(a, &a->sel, 0, row_y, a->side_w, a->row_h);
        else if (is_group)
            fill_rect(a, &a->field, 0, row_y, a->side_w, a->row_h);

        int ty2 = row_y + (a->row_h + a->font->ascent - a->font->descent) / 2;
        char label[NAME_SIZE + 4];
        if (s->expandable) {
            snprintf(label, sizeof(label), "%s %s", s->expanded ? "v" : ">", s->name);
        } else {
            snprintf(label, sizeof(label), "%s", s->name);
        }
        draw_text_clip(a, is_group ? a->font_bold : a->font,
                       is_group ? &a->accent : (is_sel ? &a->text : &a->muted),
                       indent, ty2, a->side_w - indent - PAD, label);
    }
}

/* column widths for list view */
static void col_widths(App *a, int *cname, int *ctype, int *csize) {
    int w = list_w(a);
    *ctype = 70;
    *csize = 80;
    *cname = w - *ctype - *csize;
    if (*cname < 60) *cname = 60;
}

static void draw_list_header(App *a) {
    int x  = list_x(a);
    int y  = list_y();
    int cname, ctype, csize;
    col_widths(a, &cname, &ctype, &csize);

    fill_rect(a, &a->field, x, y, list_w(a), a->row_h);
    draw_line(a, &a->edge, x, y + a->row_h, x + list_w(a), y + a->row_h);

    int ty2 = y + (a->row_h + a->font_bold->ascent - a->font_bold->descent) / 2;
    draw_text(a, a->font_bold, &a->accent, x + PAD, ty2, "Name");
    draw_text(a, a->font_bold, &a->accent, x + cname + PAD, ty2, "Type");
    draw_text(a, a->font_bold, &a->accent, x + cname + ctype + PAD, ty2, "Size");

    draw_line(a, &a->edge, x + cname, y, x + cname, y + a->row_h);
    draw_line(a, &a->edge, x + cname + ctype, y, x + cname + ctype, y + a->row_h);
}

static void icon_grid(App *a, int *cols, int *cell_w, int *cell_h) {
    int thumb = icon_thumb_size(a);
    *cell_w = thumb + PAD * 4;
    *cell_h = thumb + a->row_h + PAD * 2;
    if (*cell_w < 60) *cell_w = 60;
    if (*cell_h < 60) *cell_h = 60;
    *cols = list_w(a) / *cell_w;
    if (*cols < 1) *cols = 1;
}

static void draw_copy_status(App *a, int x, int w) {
    pthread_mutex_lock(&a->copy_mutex);
    int running = a->copy_running;
    double frac = a->copy_fraction;
    char cur[TEXT_SIZE]; snprintf(cur, sizeof(cur), "%s", a->copy_current);
    char cerr[TEXT_SIZE]; snprintf(cerr, sizeof(cerr), "%s", a->copy_error);
    pthread_mutex_unlock(&a->copy_mutex);

    if (!(running || cerr[0])) return;
    int bary = a->win_h - STATUS_H - 18;
    if (bary < list_y()) bary = list_y();
    fill_rect(a, &a->panel, x, bary, w, 18);
    if (running) {
        int pw = (int)((w - 2) * frac);
        fill_rect(a, &a->sel, x + 1, bary + 2, pw, 14);
        char msg[TEXT_SIZE];
        snprintf(msg, sizeof(msg), "Copying: %.480s", cur);
        draw_text_clip(a, a->font, &a->text, x + 4, bary + 14, w - 8, msg);
    } else if (cerr[0]) {
        draw_text_clip(a, a->font, &a->accent, x + 4, bary + 14, w - 8, cerr);
    }
}

static void draw_list_view(App *a) {
    int cname, ctype, csize;
    col_widths(a, &cname, &ctype, &csize);

    int x    = list_x(a);
    int y0   = list_y() + a->row_h; /* below header */
    int rows = (list_h(a) - a->row_h) / a->row_h;
    if (rows < 1) rows = 1;
    int end  = a->list_scroll + rows;
    if (end > a->entry_count) end = a->entry_count;

    fill_rect(a, &a->bg, x, y0, list_w(a), list_h(a) - a->row_h);

    for (int i = a->list_scroll; i < end; i++) {
        Entry *e = &a->entries[i];
        int row_y = y0 + (i - a->list_scroll) * a->row_h;

        int is_drop_target = (a->dnd_dragging && i == a->dnd_target_item);
        if (is_drop_target)
            fill_rect(a, &a->accent, x, row_y, list_w(a), a->row_h);
        else if (e->selected || i == a->list_sel)
            fill_rect(a, &a->sel, x, row_y, list_w(a), a->row_h);
        else if ((i - a->list_scroll) % 2 == 1)
            fill_rect(a, &a->field, x, row_y, list_w(a), a->row_h);

        int ty2 = row_y + (a->row_h + a->font->ascent - a->font->descent) / 2;
        XftColor *tc = e->ftype == FT_DIR ? &a->accent : &a->text;

        if (a->rename_active && a->rename_index == i) {
            int rx = x + PAD - 2;
            int ry = row_y + 2;
            int rw = cname - 2 * PAD + 4;
            int rh = a->row_h - 4;
            fill_rect(a, &a->field_on, rx, ry, rw, rh);
            draw_rect(a, &a->accent, rx, ry, rw, rh);
            draw_text_clip(a, a->font, &a->text, rx + 2, ty2, rw - 4, a->rename_text);
            if (a->caret_on) {
                int cw = text_w(a, a->font, a->rename_text);
                if (cw > rw - 6) cw = rw - 6;
                int cx = rx + 2 + cw;
                draw_line(a, &a->text, cx, ry + 2, cx, ry + rh - 2);
            }
        } else {
            draw_text_clip(a, a->font, tc, x + PAD, ty2, cname - 2 * PAD, e->name);
        }
        draw_text_clip(a, a->font, &a->muted, x + cname + PAD, ty2, ctype - PAD, e->type_str);
        draw_text_clip(a, a->font, &a->muted, x + cname + ctype + PAD, ty2, csize - PAD, e->size);

        draw_line(a, &a->edge, x + cname, row_y, x + cname, row_y + a->row_h);
        draw_line(a, &a->edge, x + cname + ctype, row_y, x + cname + ctype, row_y + a->row_h);
    }

    if (a->entry_count > rows) {
        int sx = list_x(a) + list_w(a);
        int sh = list_h(a) - a->row_h;
        fill_rect(a, &a->field, sx, y0, SCROLL_W, sh);
        int thumb_h = sh * rows / a->entry_count;
        if (thumb_h < 16) thumb_h = 16;
        int max_scroll = a->entry_count - rows;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = y0 + (sh - thumb_h) * a->list_scroll / max_scroll;
        fill_rect(a, &a->edge, sx + 2, thumb_y, SCROLL_W - 4, thumb_h);
    }

    draw_copy_status(a, x, list_w(a));
}

static void draw_icon_view(App *a) {
    g_thumb_load_budget = 4; /* limit decodes per frame to keep UI responsive */
    int cols, cell_w, cell_h;
    icon_grid(a, &cols, &cell_w, &cell_h);
    int x0 = list_x(a);
    int y0 = list_y();
    int w = list_w(a);
    int h = list_h(a);
    fill_rect(a, &a->bg, x0, y0, w, h);

    int grid_w = cols * cell_w;
    int xoff = (w - grid_w) / 2;
    if (xoff < 0) xoff = 0;

    int vis_rows = h / cell_h;
    if (vis_rows < 1) vis_rows = 1;
    int start = a->list_scroll * cols;
    int end = start + vis_rows * cols;
    if (end > a->entry_count) end = a->entry_count;

    for (int i = start; i < end; i++) {
        int idx = i - start;
        int col = idx % cols;
        int row = idx / cols;
        int cx = x0 + xoff + col * cell_w;
        int cy = y0 + row * cell_h;
        Entry *e = &a->entries[i];

        int is_drop_target = (a->dnd_dragging && i == a->dnd_target_item);
        if (is_drop_target) {
            fill_rect(a, &a->accent, cx + 4, cy + 4, cell_w - 8, cell_h - 8);
            draw_rect(a, &a->edge, cx + 4, cy + 4, cell_w - 8, cell_h - 8);
        } else if (e->selected || i == a->list_sel) {
            fill_rect(a, &a->sel, cx + 4, cy + 4, cell_w - 8, cell_h - 8);
            draw_rect(a, &a->edge, cx + 4, cy + 4, cell_w - 8, cell_h - 8);
        }

        int thumb = icon_thumb_size(a);
        int ty = cy + PAD;

        if (e->ftype == FT_DIR) {
            int fw = thumb * 3 / 4, fh = thumb / 2;
            int fx = cx + (cell_w - fw) / 2;
            int fy = ty + (thumb - fh) / 2;
            fill_rect(a, &a->field_on, fx, fy, fw, fh);
            draw_rect(a, &a->accent, fx, fy, fw, fh);
            draw_rect(a, &a->accent, fx, fy - fh / 5, fw / 2, fh / 5 + 1);
        } else {
            ensure_entry_thumb(a, e);
            if (e->thumb) {
                int ix = cx + (cell_w - e->thumb_w) / 2;
                int iy = ty + (thumb - e->thumb_h) / 2;
                XPutImage(a->dpy, draw_target(a), a->gc, e->thumb, 0, 0, ix, iy,
                          (unsigned)e->thumb_w, (unsigned)e->thumb_h);
            } else {
                int fw = thumb / 2, fh = thumb * 2 / 3;
                int fx = cx + (cell_w - fw) / 2;
                int fy = ty + (thumb - fh) / 2;
                fill_rect(a, &a->field, fx, fy, fw, fh);
                draw_rect(a, &a->muted, fx, fy, fw, fh);
            }
        }

        int ly = cy + PAD + thumb + (a->row_h + a->font->ascent - a->font->descent) / 2;
        XftColor *tc = e->ftype == FT_DIR ? &a->accent : &a->text;
        int max_tw = cell_w - 2 * PAD;
        if (a->rename_active && a->rename_index == i) {
            int rw = max_tw;
            int rx = cx + (cell_w - rw) / 2;
            int ry = cy + PAD + thumb + 1;
            int rh = a->row_h;
            fill_rect(a, &a->field_on, rx, ry, rw, rh);
            draw_rect(a, &a->accent, rx, ry, rw, rh);
            draw_text_clip(a, a->font, &a->text, rx + 2, ly, rw - 4, a->rename_text);
            if (a->caret_on) {
                int cw = text_w(a, a->font, a->rename_text);
                if (cw > rw - 6) cw = rw - 6;
                int cxr = rx + 2 + cw;
                draw_line(a, &a->text, cxr, ry + 2, cxr, ry + rh - 2);
            }
        } else {
            int tw = text_w(a, a->font, e->name);
            if (tw > max_tw) tw = max_tw;
            int tx = cx + (cell_w - tw) / 2;
            draw_text_clip(a, a->font, tc, tx, ly, max_tw, e->name);
        }
    }

    int total_rows = (a->entry_count + cols - 1) / cols;
    if (total_rows > vis_rows) {
        int sx = list_x(a) + list_w(a);
        fill_rect(a, &a->field, sx, y0, SCROLL_W, h);
        int thumb_h = h * vis_rows / total_rows;
        if (thumb_h < 16) thumb_h = 16;
        int max_scroll = total_rows - vis_rows;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_y = y0 + (h - thumb_h) * a->list_scroll / max_scroll;
        fill_rect(a, &a->edge, sx + 2, thumb_y, SCROLL_W - 4, thumb_h);
    }

    draw_copy_status(a, x0, w);
}

static void draw_listing(App *a) {
    if (a->icon_mode) draw_icon_view(a);
    else draw_list_view(a);
}

static void draw_menu(App *a) {
    if (!a->menu_open) return;
    int count = g_menu_count;
    int mw = 240, mh = count * a->row_h + 4;
    int mx = a->menu_x, my = a->menu_y;
    if (mx + mw > a->win_w) mx = a->win_w - mw;
    if (my + mh > a->win_h) my = a->win_h - mh;

    fill_rect(a, &a->panel, mx, my, mw, mh);
    draw_rect(a, &a->edge, mx, my, mw, mh);

    for (int i = 0; i < count; i++) {
        int iy = my + 2 + i * a->row_h;
        if (g_menu_items[i].id == MENU_NONE) {
            int ly = iy + a->row_h / 2;
            draw_line(a, &a->edge, mx + 6, ly, mx + mw - 6, ly);
            continue;
        }
        if (i == a->menu_sel)
            fill_rect(a, &a->sel, mx + 1, iy, mw - 2, a->row_h);
        int ty2 = iy + (a->row_h + a->font->ascent - a->font->descent) / 2;
        int tx = mx + PAD;
        if (g_menu_items[i].icon && g_menu_items[i].icon[0]) {
            draw_text(a, a->font, &a->accent, tx, ty2, g_menu_items[i].icon);
            tx += 22;
        }
        draw_text(a, a->font, &a->text, tx, ty2, g_menu_items[i].label);
    }
}

static void draw_dialog(App *a) {
    if (!a->dialog_open) return;
    int dw = 480, dh = 160;
    int dx = (a->win_w - dw) / 2, dy = (a->win_h - dh) / 2;
    fill_rect(a, &a->panel, dx, dy, dw, dh);
    draw_rect(a, &a->edge, dx, dy, dw, dh);

    draw_text(a, a->font_bold, &a->accent, dx + PAD, dy + 20, a->dialog_title);
    draw_text(a, a->font, &a->text, dx + PAD, dy + 48, a->dialog_prompt);

    /* input field */
    fill_rect(a, &a->field_on, dx + PAD, dy + 58, dw - 2*PAD, BTN_H + 4);
    draw_rect(a, &a->accent, dx + PAD, dy + 58, dw - 2*PAD, BTN_H + 4);
    int ty2 = dy + 58 + (BTN_H + 4 + a->font->ascent - a->font->descent) / 2;
    draw_text_clip(a, a->font, &a->text, dx + PAD + 4, ty2,
                   dw - 2*PAD - 8, a->dialog_value);
    if (a->caret_on) {
        int cw = text_w(a, a->font, a->dialog_value);
        int maxw = dw - 2*PAD - 10;
        if (cw > maxw) cw = maxw;
        int cx = dx + PAD + 4 + cw;
        draw_line(a, &a->text, cx, dy + 58 + 3, cx, dy + 58 + BTN_H + 1);
    }

    /* buttons */
    Btn ok_btn  = {dx + dw - 70 - PAD, dy + dh - BTN_H - PAD,
                   64, BTN_H, "OK", 0, -1};
    Btn can_btn = {dx + dw - 70 - PAD - 70, dy + dh - BTN_H - PAD,
                   64, BTN_H, "Cancel", 1, -1};
    draw_btn(a, &ok_btn, 0);
    draw_btn(a, &can_btn, 0);

    draw_text(a, a->font, &a->muted, dx + PAD,
              dy + dh - 6, "Enter = confirm, Escape = cancel");
}

static void draw_error(App *a) {
    if (!a->err_open) return;
    int dw = 480, dh = 140;
    int dx = (a->win_w - dw) / 2, dy = (a->win_h - dh) / 2;
    fill_rect(a, &a->panel, dx, dy, dw, dh);
    draw_rect(a, &a->edge, dx, dy, dw, dh);

    draw_text(a, a->font_bold, &a->accent, dx + PAD, dy + 22, a->err_text);
    if (a->err_detail[0])
        draw_text_clip(a, a->font, &a->muted, dx + PAD, dy + 46,
                       dw - 2*PAD, a->err_detail);

    draw_text(a, a->font, &a->muted, dx + PAD,
              dy + dh - 8, "Press Enter or click anywhere to close");
}

static void draw_props(App *a) {
    if (!a->props_open) return;
    int dw = 420, dh = 240;
    int dx = (a->win_w - dw) / 2, dy = (a->win_h - dh) / 2;
    fill_rect(a, &a->panel, dx, dy, dw, dh);
    draw_rect(a, &a->edge, dx, dy, dw, dh);

    draw_text(a, a->font_bold, &a->accent, dx + PAD, dy + 22, "Properties");

    int y = dy + 44;
    int lx = dx + PAD, vx = dx + 100;
#define PROP_ROW(label, val) \
    draw_text(a, a->font, &a->muted, lx, y, label); \
    draw_text_clip(a, a->font, &a->text, vx, y, dw - 100 - PAD, val); \
    y += 20;
    PROP_ROW("Name:",     a->props_path)
    PROP_ROW("Type:",     a->props_type)
    PROP_ROW("Size:",     a->props_size)
    PROP_ROW("Modified:", a->props_mtime)
#undef PROP_ROW

    if (a->props_has_mode) {
        char perm[12] = "---------";
        for (int i = 0; i < 9; i++) {
            int bit = (a->props_mode >> (8 - i)) & 1;
            if (bit) perm[i] = "rwxrwxrwx"[i];
        }
        draw_text(a, a->font, &a->muted, lx, y, "Mode:");
        draw_text(a, a->font, &a->text, vx, y, perm);
        y += 20;
    }

    Btn close_btn = {dx + dw - 74, dy + dh - BTN_H - PAD,
                     68, BTN_H, "Close", 0, -1};
    draw_btn(a, &close_btn, 0);
}

static void draw_picker(App *a) {
    if (!a->picker_open) return;
    int dw = a->win_w * 2 / 3; if (dw < 420) dw = 420; if (dw > a->win_w - 40) dw = a->win_w - 40;
    int dh = a->win_h * 2 / 3; if (dh < 300) dh = 300; if (dh > a->win_h - 40) dh = a->win_h - 40;
    int dx = (a->win_w - dw) / 2, dy = (a->win_h - dh) / 2;
    fill_rect(a, &a->panel, dx, dy, dw, dh);
    draw_rect(a, &a->edge, dx, dy, dw, dh);

    const char *title = a->picker_move ? "➜ Move to" : "⎘ Copy to";
    draw_text(a, a->font_bold, &a->accent, dx + PAD, dy + 20, title);

    char *pstr = a->picker_dir ? g_file_get_path(a->picker_dir) : NULL;
    if (!pstr && a->picker_dir) pstr = g_file_get_uri(a->picker_dir);
    char loc[TEXT_SIZE]; snprintf(loc, sizeof(loc), "%s", pstr ? pstr : "");
    g_free(pstr);
    draw_text_clip(a, a->font, &a->muted, dx + PAD, dy + 40, dw - 2*PAD, loc);

    /* buttons */
    int by = dy + dh - BTN_H - PAD;
    Btn sel_btn = {dx + dw - PAD - 90, by, 90, BTN_H, "✓ Select", 100, -1};
    Btn up_btn  = {dx + dw - PAD - 90 - 58, by, 52, BTN_H, "↑ Up", 101, -1};
    Btn home_btn= {dx + dw - PAD - 90 - 58 - 64, by, 58, BTN_H, "⌂ Home", 102, -1};
    Btn can_btn = {dx + PAD, by, 64, BTN_H, "✖", 103, -1};
    draw_btn(a, &sel_btn, 0);
    draw_btn(a, &up_btn, 0);
    draw_btn(a, &home_btn, 0);
    draw_btn(a, &can_btn, 0);

    /* list area */
    int lx = dx + PAD, ly = dy + 52;
    int lw = dw - 2*PAD, lh = by - ly - PAD;
    fill_rect(a, &a->bg, lx, ly, lw, lh);
    draw_rect(a, &a->edge, lx, ly, lw, lh);
    int rows = lh / a->row_h; if (rows < 1) rows = 1;
    int end = a->picker_scroll + rows;
    if (end > a->picker_count) end = a->picker_count;
    for (int i = a->picker_scroll; i < end; i++) {
        int ry = ly + (i - a->picker_scroll) * a->row_h;
        if (i == a->picker_sel) fill_rect(a, &a->sel, lx+1, ry+1, lw-2, a->row_h-1);
        int ty = ry + (a->row_h + a->font->ascent - a->font->descent) / 2;
        draw_text_clip(a, a->font, &a->text, lx + PAD, ty, lw - 2*PAD, a->picker_names[i]);
    }
}

static void ensure_backbuf(App *a) {
    if (!a->dpy || !a->win) return;
    if (a->win_w < 1) a->win_w = 1;
    if (a->win_h < 1) a->win_h = 1;
    if (a->backbuf) return;
    a->backbuf = XCreatePixmap(a->dpy, a->win, (unsigned)a->win_w, (unsigned)a->win_h,
                               (unsigned)DefaultDepth(a->dpy, a->scr));
    if (a->draw) XftDrawDestroy(a->draw);
    a->draw = XftDrawCreate(a->dpy, a->backbuf,
                            DefaultVisual(a->dpy, a->scr),
                            DefaultColormap(a->dpy, a->scr));
}

static void recreate_backbuf(App *a) {
    if (a->draw) { XftDrawDestroy(a->draw); a->draw = NULL; }
    if (a->backbuf) { XFreePixmap(a->dpy, a->backbuf); a->backbuf = 0; }
    ensure_backbuf(a);
}


static void draw_status_bar(App *a) {
    int y = a->win_h - STATUS_H;
    if (y < 0) y = 0;
    fill_rect(a, &a->panel, 0, y, a->win_w, STATUS_H);
    draw_line(a, &a->edge, 0, y, a->win_w, y);
    int ty = y + (STATUS_H + a->font->ascent - a->font->descent) / 2;
    draw_text_clip(a, a->font, &a->muted, PAD, ty, a->win_w - 2*PAD, a->free_space);
}

static void draw_app(App *a) {
    ensure_backbuf(a);
    fill_rect(a, &a->bg, 0, 0, a->win_w, a->win_h);
    if (a->layout_w != a->win_w || a->layout_h != a->win_h || a->layout_side_w != a->side_w) {
        layout_buttons(a);
        a->layout_w = a->win_w;
        a->layout_h = a->win_h;
        a->layout_side_w = a->side_w;
    }
    draw_address_bar(a);
    draw_toolbar(a);
    draw_sidebar(a);
    if (!a->icon_mode) draw_list_header(a);
    draw_listing(a);

    /* split handle */
    draw_line(a, &a->edge, a->side_w + 1, list_y(),
              a->side_w + 1, list_y() + list_h(a));
    draw_status_bar(a);

    if (a->menu_open)   draw_menu(a);
    if (a->dialog_open) draw_dialog(a);
    if (a->picker_open) draw_picker(a);
    if (a->props_open)  draw_props(a);
    if (a->err_open)    draw_error(a);

    /* rubber-band selection */
    if (a->band_active) {
        int x1 = a->band_x0 < a->band_x1 ? a->band_x0 : a->band_x1;
        int y1 = a->band_y0 < a->band_y1 ? a->band_y0 : a->band_y1;
        int x2 = a->band_x0 > a->band_x1 ? a->band_x0 : a->band_x1;
        int y2 = a->band_y0 > a->band_y1 ? a->band_y0 : a->band_y1;
        int bw = x2 - x1, bh = y2 - y1;
        if (bw > 0 && bh > 0) {
            draw_rect(a, &a->accent, x1, y1, bw, bh);
            if (bw > 2 && bh > 2) draw_rect(a, &a->edge, x1 + 1, y1 + 1, bw - 2, bh - 2);
        }
    }

    /* drag ghost is shown via external drag icon window */

    XCopyArea(a->dpy, a->backbuf, a->win, a->gc, 0, 0,
              (unsigned)a->win_w, (unsigned)a->win_h, 0, 0);
    XFlush(a->dpy);
}

/* ════════════════════════════════════════════════════════════════════
 * Hit-testing helpers
 * ══════════════════════════════════════════════════════════════════*/
static int hit_btn(int x, int y, int *btn_id) {
    for (int i = 0; i < g_btn_count; i++) {
        Btn *b = &g_btns[i];
        if (x >= b->x && x < b->x + b->w &&
            y >= b->y && y < b->y + b->h) {
            *btn_id = b->id;
            return 1;
        }
    }
    return 0;
}

static int item_at(App *a, int x, int y) {
    if (x < list_x(a) || x > list_x(a) + list_w(a)) return -1;
    if (a->icon_mode) {
        int cols, cell_w, cell_h;
        icon_grid(a, &cols, &cell_w, &cell_h);
        int grid_w = cols * cell_w;
        int xoff = (list_w(a) - grid_w) / 2;
        if (xoff < 0) xoff = 0;
        if (y < list_y() || y > list_y() + list_h(a)) return -1;
        int col = (x - list_x(a) - xoff) / cell_w;
        int row = (y - list_y()) / cell_h + a->list_scroll;
        if (col < 0 || col >= cols) return -1;
        int idx = row * cols + col;
        if (idx >= 0 && idx < a->entry_count) return idx;
        return -1;
    } else {
        if (y < list_y() + a->row_h) return -1;    /* header */
        int row = (y - (list_y() + a->row_h)) / a->row_h + a->list_scroll;
        if (row >= 0 && row < a->entry_count) return row;
        return -1;
    }
}

static int side_row_at(App *a, int x, int y) {
    if (x < 0 || x > a->side_w) return -1;
    if (y < list_y()) return -1;
    int row = (y - list_y()) / a->row_h + a->side_scroll;
    if (row >= 0 && row < a->side_count) return row;
    return -1;
}

/* ════════════════════════════════════════════════════════════════════
 * Address bar commit
 * ══════════════════════════════════════════════════════════════════*/
static void address_commit(App *a) {
    if (!a->address[0]) return;
    char expanded[PATH_MAX];
    if (strcmp(a->address, "~") == 0 ||
        strncmp(a->address, "~/", 2) == 0) {
        const char *home = g_get_home_dir();
        if (strcmp(a->address, "~") == 0)
            snprintf(expanded, sizeof(expanded), "%s", home);
        else
            snprintf(expanded, sizeof(expanded), "%s%s", home, a->address + 1);
    } else {
        snprintf(expanded, sizeof(expanded), "%s", a->address);
    }
    GFile *f = g_file_new_for_commandline_arg(expanded);
    go_to(a, f, 1);
    g_object_unref(f);
}

/* ════════════════════════════════════════════════════════════════════
 * Keyboard input into text fields
 * ══════════════════════════════════════════════════════════════════*/
static void field_key(char *buf, int *len, int max_len,
                      KeySym sym, const char *input, int input_len) {
    if (sym == XK_BackSpace) {
        if (*len > 0) {
            (*len)--;
            while (*len > 0 && ((unsigned char)buf[*len] & 0xc0) == 0x80)
                (*len)--;
            buf[*len] = '\0';
        }
        return;
    }
    for (int i = 0; i < input_len && *len < max_len - 1; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 32 || c >= 0x80) buf[(*len)++] = (char)c;
    }
    buf[*len] = '\0';
}

/* ════════════════════════════════════════════════════════════════════
 * Event handling
 * ══════════════════════════════════════════════════════════════════*/
static void handle_key(App *a, XKeyEvent *ev) {
    char input[32]; KeySym sym;
    int len = XLookupString(ev, input, sizeof(input)-1, &sym, NULL);
    input[len] = '\0';

    int ctrl = !!(ev->state & ControlMask);

    /* error dialog */
    if (a->err_open) {
        if (sym == XK_Return || sym == XK_Escape) a->err_open = 0;
        draw_app(a); return;
    }
    /* inline rename */
    if (a->rename_active) {
        if (sym == XK_Return) {
            rename_commit(a);
        } else if (sym == XK_Escape) {
            rename_cancel(a);
            draw_app(a);
        } else if (sym == XK_Left || sym == XK_Right || sym == XK_Home || sym == XK_End) {
            /* keep cursor at end for now */
            a->rename_cursor = a->rename_len;
            draw_app(a);
        } else {
            field_key(a->rename_text, &a->rename_len,
                      (int)sizeof(a->rename_text), sym, input, len);
            a->rename_cursor = a->rename_len;
            a->caret_on = 1; a->caret_tick = 0;
            draw_app(a);
        }
        return;
    }
    /* input dialog */
    if (a->dialog_open) {
        if (sym == XK_Return) {
            if (a->dialog_len > 0 && a->dialog_ok) a->dialog_ok(a);
            a->dialog_open = 0;
        } else if (sym == XK_Escape) {
            a->dialog_open = 0;
        } else {
            field_key(a->dialog_value, &a->dialog_len,
                      (int)sizeof(a->dialog_value), sym, input, len);
            a->caret_on = 1; a->caret_tick = 0;
        }
        draw_app(a); return;
    }
    /* directory picker */
    if (a->picker_open) {
        if (sym == XK_Escape) {
            close_picker(a);
        } else if (sym == XK_Return) {
            if (a->picker_sel >= 0 && a->picker_sel < a->picker_count)
                picker_refresh(a, a->picker_files[a->picker_sel]);
            else
                picker_select_current(a);
        } else if (sym == XK_BackSpace) {
            if (a->picker_dir) {
                GFile *parent = g_file_get_parent(a->picker_dir);
                if (parent) { picker_refresh(a, parent); g_object_unref(parent); }
            }
        } else if (sym == XK_Up) {
            if (a->picker_sel > 0) { a->picker_sel--; if (a->picker_sel < a->picker_scroll) a->picker_scroll = a->picker_sel; }
        } else if (sym == XK_Down) {
            if (a->picker_sel < a->picker_count - 1) {
                a->picker_sel++;
                int dw = a->win_w * 2 / 3; if (dw < 420) dw = 420; if (dw > a->win_w - 40) dw = a->win_w - 40;
                int dh = a->win_h * 2 / 3; if (dh < 300) dh = 300; if (dh > a->win_h - 40) dh = a->win_h - 40;
                int dy = (a->win_h - dh) / 2, by = dy + dh - BTN_H - PAD, ly = dy + 52;
                int rows = (by - ly - PAD) / a->row_h; if (rows < 1) rows = 1;
                if (a->picker_sel >= a->picker_scroll + rows) a->picker_scroll = a->picker_sel - rows + 1;
            }
        }
        draw_app(a); return;
    }
    /* props dialog */
    if (a->props_open) {
        if (sym == XK_Return || sym == XK_Escape) {
            a->props_open = 0;
            if (a->props_file) { g_object_unref(a->props_file); a->props_file = NULL; }
        }
        draw_app(a); return;
    }
    /* menu */
    if (a->menu_open) {
        a->menu_open = 0; draw_app(a); return;
    }

    /* address bar */
    if (a->address_focused) {
        if (sym == XK_Return) {
            address_commit(a);
            a->address_focused = 0;
        } else if (sym == XK_Escape) {
            a->address_focused = 0;
            if (a->location) address_set(a, a->location);
        } else {
            field_key(a->address, &a->address_len,
                      (int)sizeof(a->address), sym, input, len);
            a->caret_on = 1; a->caret_tick = 0;
        }
        draw_app(a); return;
    }
    /* search */
    if (a->search_focused) {
        if (sym == XK_Return || sym == XK_Escape) {
            a->search_focused = 0;
        } else {
            field_key(a->search, &a->search_len,
                      (int)sizeof(a->search), sym, input, len);
            a->caret_on = 1; a->caret_tick = 0;
            refresh_listing(a);
        }
        draw_app(a); return;
    }

    /* global shortcuts */
    if (ctrl) {
        switch (sym) {
        case XK_l: a->address_focused = 1; a->search_focused = 0; a->caret_on = 1; a->caret_tick = 0; break;
        case XK_h: a->show_hidden = !a->show_hidden; refresh_listing(a); break;
        case XK_a: select_all(a); break;
        case XK_c: if (a->list_sel >= 0 || selected_count(a) > 0) clipboard_set_selected(a, 0); break;
        case XK_x: if (a->list_sel >= 0 || selected_count(a) > 0) clipboard_set_selected(a, 1); break;
        case XK_v: clipboard_paste(a); break;
        case XK_n: start_dialog(a, "New File", "File name:", cb_newfile_ok); break;
        case XK_N: start_dialog(a, "New Folder", "Folder name:", cb_newdir_ok); break;
        case XK_1: a->icon_mode = 0; a->list_scroll = 0; break;
        case XK_2: a->icon_mode = 1; a->list_scroll = 0; clear_entry_thumbs(a); break;
        }
        draw_app(a); return;
    }

    switch (sym) {
    case XK_Up:
    case XK_Down:
    case XK_Left:
    case XK_Right:
    case XK_Home:
    case XK_End:
    case XK_Page_Up:
    case XK_Page_Down: {
        if (a->entry_count <= 0) break;
        int idx = a->list_sel < 0 ? 0 : a->list_sel;
        if (a->icon_mode) {
            int cols, cw, ch;
            icon_grid(a, &cols, &cw, &ch);
            int vis_rows = list_h(a) / ch; if (vis_rows < 1) vis_rows = 1;
            int total_rows = (a->entry_count + cols - 1) / cols;
            if (sym == XK_Up) idx -= cols;
            else if (sym == XK_Down) idx += cols;
            else if (sym == XK_Left) idx -= 1;
            else if (sym == XK_Right) idx += 1;
            else if (sym == XK_Home) idx = 0;
            else if (sym == XK_End) idx = a->entry_count - 1;
            else if (sym == XK_Page_Up) idx -= cols * vis_rows;
            else if (sym == XK_Page_Down) idx += cols * vis_rows;
            if (idx < 0) idx = 0;
            if (idx >= a->entry_count) idx = a->entry_count - 1;
            select_only(a, idx);
            int sel_row = idx / cols;
            if (sel_row < a->list_scroll) a->list_scroll = sel_row;
            if (sel_row >= a->list_scroll + vis_rows) a->list_scroll = sel_row - vis_rows + 1;
            if (a->list_scroll < 0) a->list_scroll = 0;
            if (total_rows > vis_rows && a->list_scroll > total_rows - vis_rows) a->list_scroll = total_rows - vis_rows;
        } else {
            int rows = (list_h(a) - a->row_h) / a->row_h; if (rows < 1) rows = 1;
            if (sym == XK_Up) idx -= 1;
            else if (sym == XK_Down) idx += 1;
            else if (sym == XK_Left || sym == XK_Right) { /* no-op in list */ }
            else if (sym == XK_Home) idx = 0;
            else if (sym == XK_End) idx = a->entry_count - 1;
            else if (sym == XK_Page_Up) idx -= rows;
            else if (sym == XK_Page_Down) idx += rows;
            if (idx < 0) idx = 0;
            if (idx >= a->entry_count) idx = a->entry_count - 1;
            select_only(a, idx);
            if (a->list_sel < a->list_scroll) a->list_scroll = a->list_sel;
            if (a->list_sel >= a->list_scroll + rows) a->list_scroll = a->list_sel - rows + 1;
            int max_scroll = a->entry_count > rows ? a->entry_count - rows : 0;
            if (a->list_scroll > max_scroll) a->list_scroll = max_scroll;
            if (a->list_scroll < 0) a->list_scroll = 0;
        }
        break;
    }
    case XK_Return:
        if (a->list_sel >= 0) open_entry(a, a->list_sel);
        break;
    case XK_Delete:
        if (a->list_sel >= 0 || selected_count(a) > 0) {
            if (ev->state & ShiftMask) perform_menu(a, MENU_DELETE);
            else perform_menu(a, MENU_TRASH);
        }
        break;
    case XK_F2:
        if (a->list_sel >= 0)
            rename_start(a, a->list_sel);
        break;
    case XK_F5:
        refresh_sidebar(a);
        refresh_listing(a);
        break;
    case XK_Escape:
        clear_selection(a);
        a->list_sel = -1;
        break;
    case XK_BackSpace:
        nav_up(a); break;
    case XK_Alt_L: case XK_Alt_R:
        break;
    }

    /* Alt+Left / Right / Up */
    if (ev->state & Mod1Mask) {
        if (sym == XK_Left)  nav_back(a);
        if (sym == XK_Right) nav_forward(a);
        if (sym == XK_Up)    nav_up(a);
    }

    draw_app(a);
}

static void handle_button_press(App *a, XButtonEvent *ev) {
    int x = ev->x, y = ev->y;

    if (a->rename_active && ev->button == Button1) {
        /* clicking commits rename; if click is on another item, selection handled after */
        rename_commit(a);
    }

    /* close overlays on click outside */
    if (a->err_open) {
        a->err_open = 0; draw_app(a); return;
    }
    if (a->props_open) {
        a->props_open = 0;
        if (a->props_file) { g_object_unref(a->props_file); a->props_file = NULL; }
        draw_app(a); return;
    }
    if (a->dialog_open) {
        /* check OK/Cancel buttons */
        int dw = 480, dh = 160;
        int dx = (a->win_w - dw) / 2, dy = (a->win_h - dh) / 2;
        int ok_x = dx + dw - 70 - PAD, ok_y = dy + dh - BTN_H - PAD;
        int can_x = ok_x - 70, can_y = ok_y;
        if (x >= ok_x && x < ok_x+64 && y >= ok_y && y < ok_y+BTN_H) {
            if (a->dialog_len > 0 && a->dialog_ok) a->dialog_ok(a);
            a->dialog_open = 0;
        } else if (x >= can_x && x < can_x+64 && y >= can_y && y < can_y+BTN_H) {
            a->dialog_open = 0;
        }
        draw_app(a); return;
    }
    if (a->picker_open) {
        int dw = a->win_w * 2 / 3; if (dw < 420) dw = 420; if (dw > a->win_w - 40) dw = a->win_w - 40;
        int dh = a->win_h * 2 / 3; if (dh < 300) dh = 300; if (dh > a->win_h - 40) dh = a->win_h - 40;
        int dx = (a->win_w - dw) / 2, dy = (a->win_h - dh) / 2;
        int by = dy + dh - BTN_H - PAD;
        int lx = dx + PAD, ly = dy + 52;
        int lw = dw - 2*PAD, lh = by - ly - PAD;
        int rows = lh / a->row_h; if (rows < 1) rows = 1;

        /* mouse wheel scroll for picker list */
        if (ev->button == Button4) {
            if (a->picker_scroll > 0) a->picker_scroll--;
            draw_app(a); return;
        }
        if (ev->button == Button5) {
            int max_scroll = a->picker_count > rows ? a->picker_count - rows : 0;
            if (a->picker_scroll < max_scroll) a->picker_scroll++;
            draw_app(a); return;
        }

        /* buttons */
        if (ev->button == Button1 && y >= by && y < by + BTN_H) {
            if (x >= dx + dw - PAD - 90 && x < dx + dw - PAD) { picker_select_current(a); draw_app(a); return; }
            if (x >= dx + dw - PAD - 90 - 58 && x < dx + dw - PAD - 90) {
                if (a->picker_dir) { GFile *p = g_file_get_parent(a->picker_dir); if (p) { picker_refresh(a, p); g_object_unref(p); } }
                draw_app(a); return;
            }
            if (x >= dx + dw - PAD - 90 - 58 - 64 && x < dx + dw - PAD - 90 - 58) {
                GFile *h = get_home_file(); picker_refresh(a, h); g_object_unref(h);
                draw_app(a); return;
            }
            if (x >= dx + PAD && x < dx + PAD + 64) { close_picker(a); draw_app(a); return; }
        }
        /* list click/double-click */
        if (ev->button == Button1 && x >= lx && x < lx + lw && y >= ly && y < ly + lh) {
            int row = (y - ly) / a->row_h + a->picker_scroll;
            if (row >= 0 && row < a->picker_count) {
                a->picker_sel = row;
                /* double click to enter */
                static Time last_t = 0; static int last_r = -1;
                int dbl = (row == last_r && ev->time - last_t < 400);
                last_t = ev->time; last_r = row;
                if (dbl) picker_refresh(a, a->picker_files[row]);
            }
        }
        draw_app(a); return;
    }

    /* context menu click */
    if (a->menu_open) {
        int count = g_menu_count;
        int mw = 240, mh = count * a->row_h + 4;
        int mx = a->menu_x, my = a->menu_y;
        if (mx + mw > a->win_w) mx = a->win_w - mw;
        if (my + mh > a->win_h) my = a->win_h - mh;

        a->menu_open = 0;
        if (x >= mx && x < mx + mw && y >= my && y < my + mh) {
            int row = (y - my - 2) / a->row_h;
            if (row >= 0 && row < count && g_menu_items[row].id != MENU_NONE)
                perform_menu(a, g_menu_items[row].id);
        }
        draw_app(a); return;
    }

    /* scroll wheel */
    if (ev->button == Button4) {
        if (ev->state & ControlMask) {
            apply_zoom(a, +1);
        } else if (x < a->side_w) {
            if (a->side_scroll > 0) a->side_scroll--;
        } else if (a->list_scroll > 0) {
            a->list_scroll--;
        }
        draw_app(a); return;
    }
    if (ev->button == Button5) {
        if (ev->state & ControlMask) {
            apply_zoom(a, -1);
        } else if (x < a->side_w) {
            int rows = list_h(a) / a->row_h;
            if (a->side_scroll + rows < a->side_count) a->side_scroll++;
        } else {
            if (a->icon_mode) {
                int cols, cw, ch;
                icon_grid(a, &cols, &cw, &ch);
                int vis_rows = list_h(a) / ch; if (vis_rows < 1) vis_rows = 1;
                int total_rows = (a->entry_count + cols - 1) / cols;
                if (a->list_scroll + vis_rows < total_rows) a->list_scroll++;
            } else {
                int rows = (list_h(a) - a->row_h) / a->row_h;
                if (a->list_scroll + rows < a->entry_count) a->list_scroll++;
            }
        }
        draw_app(a); return;
    }

    /* splitter drag start */
    if (abs(x - a->side_w - 1) <= 3 && y >= list_y()) {
        a->split_drag = 1;
        a->split_drag_x0 = x;
        a->split_side_w0 = a->side_w;
        return;
    }

    /* address bar focus (top path input) */
    {
        int y0 = bar_y() + (BAR_H - BTN_H) / 2;
        int ax = PAD + 28*3 + PAD; /* after back/forward/up icon buttons */
        int go_w = 28;
        int addr_w = a->win_w - ax - go_w - PAD*2;
        if (addr_w < 60) addr_w = 60;
        if (y >= y0 && y < y0 + BTN_H && x >= ax && x < ax + addr_w) {
            a->address_focused = 1;
            a->search_focused  = 0;
            a->caret_on = 1; a->caret_tick = 0;
            a->dnd_press = 0; a->dnd_dragging = 0;
            draw_app(a); return;
        }
    }

    /* search field focus */
    {
        int sw_x = a->win_w - 180 - PAD;
        int sw_y = tool_y() + (TOOL_H - BTN_H) / 2;
        if (x >= sw_x && x < sw_x + 180 && y >= sw_y && y < sw_y + BTN_H) {
            a->search_focused  = 1;
            a->address_focused = 0;
            a->caret_on = 1; a->caret_tick = 0;
            a->dnd_press = 0; a->dnd_dragging = 0;
            draw_app(a); return;
        }
    }

    /* toolbar / nav buttons */
    if (ev->button == Button1) {
        int btn_id = -99;
        if (hit_btn(x, y, &btn_id)) {
            if (btn_id != -1) { a->address_focused = 0; a->search_focused = 0; }
            switch (btn_id) {
            case BTN_BACK:     nav_back(a);    break;
            case BTN_FORWARD:  nav_forward(a); break;
            case BTN_UP:       nav_up(a);      break;
            case BTN_GO:       address_commit(a); break;
            case BTN_HIDDEN:
                a->show_hidden = !a->show_hidden;
                refresh_listing(a); break;
            case BTN_NEWFOLDER:
                start_dialog(a, "New Folder", "Folder name:", cb_newdir_ok); break;
            case BTN_NEWFILE:
                start_dialog(a, "New File", "File name:", cb_newfile_ok); break;
            case BTN_BM_ADD:
                if (a->location && a->bookmark_count < MAX_BOOKMARKS) {
                    a->bookmarks[a->bookmark_count++] = g_object_ref(a->location);
                    refresh_sidebar(a); save_config(a);
                }
                break;
            case BTN_BM_REM:
                if (a->location) {
                    for (int i = 0; i < a->bookmark_count; i++) {
                        if (g_file_equal(a->bookmarks[i], a->location)) {
                            g_object_unref(a->bookmarks[i]);
                            memmove(&a->bookmarks[i], &a->bookmarks[i+1],
                                    (a->bookmark_count - i - 1) * sizeof(GFile*));
                            a->bookmark_count--;
                            refresh_sidebar(a); save_config(a);
                            break;
                        }
                    }
                }
                break;
            case BTN_VIEW_LIST:
                a->icon_mode = 0; a->list_scroll = 0; break;
            case BTN_VIEW_ICONS:
                a->icon_mode = 1; a->icon_zoom = 0; a->list_scroll = 0; clear_entry_thumbs(a); break;
            case BTN_VIEW_LARGE:
                a->icon_mode = 1; a->icon_zoom = 4; a->list_scroll = 0; clear_entry_thumbs(a); break;
            }
            draw_app(a); return;
        }
    }

    /* sidebar click */
    int sr = side_row_at(a, x, y);
    if (sr >= 0) {
        SideEntry *s = &a->side[sr];
        a->side_sel = sr;
        if (s->expandable) {
            if (s->kind == ST_PLACES_GROUP) a->places_expanded = !a->places_expanded;
            else if (s->kind == ST_BM_GROUP) a->bm_expanded = !a->bm_expanded;
            else if (s->kind == ST_RECENT_GROUP) a->recent_expanded = !a->recent_expanded;
            else if (s->kind == ST_DEV_GROUP) a->dev_expanded = !a->dev_expanded;
            refresh_sidebar(a);
        } else if (s->kind != ST_BM_GROUP && s->kind != ST_RECENT_GROUP &&
                   s->kind != ST_PLACES_GROUP && s->kind != ST_DEV_GROUP) {
            if (s->file) {
                go_to(a, s->file, 1);
            } else if (s->volume && g_volume_can_mount(s->volume)) {
                g_volume_mount(s->volume, G_MOUNT_MOUNT_NONE, NULL, NULL,
                               on_volume_mounted, a);
            }
        }
        /* right-click on volume: unmount */
        if (ev->button == Button3 && s->mount) {
            g_mount_unmount_with_operation(s->mount, G_MOUNT_UNMOUNT_NONE,
                                           NULL, NULL, on_mount_unmounted, a);
        }
        draw_app(a); return;
    }

    /* list area click */
    if (x > a->side_w && y > list_y()) {
        int row = item_at(a, x, y);
        if (ev->button == Button1) {
            a->band_active = 0;
            a->band_press = 0;
            if (row >= 0) {
                if (ev->state & ShiftMask) {
                    int anchor = a->sel_anchor >= 0 ? a->sel_anchor : (a->list_sel >= 0 ? a->list_sel : row);
                    select_range(a, anchor, row);
                    a->sel_anchor = anchor;
                    a->dnd_press = 0;
                    a->dnd_dragging = 0;
                    a->dnd_index = row;
                } else if (ev->state & ControlMask) {
                    /* Ctrl+click toggles selection; still allow drag if item ends up selected */
                    if (!a->entries[row].selected) {
                        toggle_selection(a, row);
                    } else {
                        a->list_sel = row;
                    }
                    a->sel_anchor = row;
                    a->dnd_press = 1;
                    a->dnd_dragging = 0;
                    a->dnd_start_x = x;
                    a->dnd_start_y = y;
                    a->dnd_cur_x = x;
                    a->dnd_cur_y = y;
                    a->dnd_index = row;
                    a->dnd_target_side = -1;
                    a->dnd_target_item = -1;
                } else {
                    if (!a->entries[row].selected) select_only(a, row);
                    else { a->list_sel = row; a->sel_anchor = row; }
                    /* potential drag start */
                    a->dnd_press = 1;
                    a->dnd_dragging = 0;
                    a->dnd_start_x = x;
                    a->dnd_start_y = y;
                    a->dnd_cur_x = x;
                    a->dnd_cur_y = y;
                    a->dnd_index = row;
                    a->dnd_target_side = -1;
                    a->dnd_target_item = -1;
                }
                a->address_focused = 0;
                a->search_focused  = 0;
            } else {
                if (!(ev->state & ControlMask)) {
                    clear_selection(a);
                    a->list_sel = -1;
                    a->sel_anchor = -1;
                }
                /* start rubber-band selection on empty area */
                a->dnd_press = 0;
                a->dnd_dragging = 0;
                a->band_press = 1;
                a->band_active = 0;
                a->band_x0 = a->band_x1 = x;
                a->band_y0 = a->band_y1 = y;
                a->band_additive = (ev->state & ControlMask) ? 1 : 0;
                a->address_focused = 0;
                a->search_focused  = 0;
            }
        } else if (ev->button == Button3) {
            a->dnd_press = 0; a->dnd_dragging = 0;
            a->band_press = 0; a->band_active = 0;
            if (row >= 0) {
                if (!(ev->state & ControlMask) && !a->entries[row].selected)
                    select_only(a, row);
                else
                    a->list_sel = row;
            }
            a->menu_open = 1;
            a->menu_x = x;
            a->menu_y = y;
            a->menu_on_file = (row >= 0);
            a->menu_sel = -1;
            build_context_menu(a);
        }
        draw_app(a); return;
    }
    draw_app(a);
}

static void handle_button_release(App *a, XButtonEvent *ev) {
    if (ev->button != Button1) {
        a->split_drag = 0;
        return;
    }

    if (a->band_active) {
        a->band_x1 = ev->x;
        a->band_y1 = ev->y;
        apply_band_selection(a);
    }

    if (a->dnd_dragging) {
        GFile *dst = NULL;
        if (a->dnd_target_side >= 0 && a->dnd_target_side < a->side_count) {
            SideEntry *s = &a->side[a->dnd_target_side];
            if (s->file) dst = s->file;
        } else if (a->dnd_target_item >= 0 && a->dnd_target_item < a->entry_count) {
            if (a->entries[a->dnd_target_item].ftype == FT_DIR)
                dst = a->entries[a->dnd_target_item].file;
        }
        if (dst) {
            int is_move = !(ev->state & ControlMask);
            int count = selected_count(a);
            if (count > 0) {
                for (int i = 0; i < a->entry_count; i++)
                    if (a->entries[i].selected)
                        queue_copy(a, a->entries[i].file, dst, is_move);
            } else if (a->dnd_index >= 0 && a->dnd_index < a->entry_count) {
                queue_copy(a, a->entries[a->dnd_index].file, dst, is_move);
            }
            if (a->xdnd_target != None) {
                xdnd_send(a, a->xdnd_target, a->xdnd_leave, 0,0,0,0);
                a->xdnd_target = None;
            }
        } else if (a->xdnd_target != None) {
            /* external drop */
            xdnd_send(a, a->xdnd_target, a->xdnd_drop, 0, ev->time, 0, 0);
            XFlush(a->dpy);
            a->xdnd_target = None;
        }
    }

    hide_drag_icon(a);
    if (a->band_active || a->band_press || a->dnd_dragging || a->dnd_press) {
        XUngrabPointer(a->dpy, CurrentTime);
    }
    XUndefineCursor(a->dpy, a->win);

    a->band_active = 0;
    a->band_press = 0;
    a->dnd_dragging = 0;
    a->dnd_press = 0;
    a->dnd_target_side = -1;
    a->dnd_target_item = -1;
    a->split_drag = 0;
    draw_app(a);
}

static void xdnd_send(App *a, Window target, Atom msg, long d1, long d2, long d3, long d4) {
    XClientMessageEvent cm = {0};
    cm.type = ClientMessage;
    cm.window = target;
    cm.message_type = msg;
    cm.format = 32;
    cm.data.l[0] = (long)a->win;
    cm.data.l[1] = d1; cm.data.l[2] = d2; cm.data.l[3] = d3; cm.data.l[4] = d4;
    XSendEvent(a->dpy, target, False, NoEventMask, (XEvent *)&cm);
}

static Window xdnd_find_target(App *a, int root_x, int root_y, int *ver_out) {
    Window root_ret, child_ret, target = None;
    int rx, ry, wx, wy;
    unsigned int mask;
    Window root = DefaultRootWindow(a->dpy);
    Window w = root;
    int x = root_x, y = root_y;
    /* descend to leaf window under pointer */
    while (XQueryPointer(a->dpy, w, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask) && child_ret != None) {
        if (child_ret == a->win) { *ver_out = 0; return None; }
        w = child_ret; x = wx; y = wy;
    }
    if (w == root || w == a->win) { *ver_out = 0; return None; }
    /* walk up to find XdndAware window */
    target = w;
    while (target != None && target != root) {
        Atom actual_type; int actual_format; unsigned long nitems, bytes_after; unsigned char *data = NULL;
        if (XGetWindowProperty(a->dpy, target, a->xdnd_aware, 0, 1, False, AnyPropertyType,
                               &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success && data) {
            int ver = *(long *)data; /* XdndAware is ATOM/32 often read as long */
            XFree(data);
            if (ver >= 3) { *ver_out = ver; return target; }
        }
        if (data) XFree(data);
        Window parent, *children = NULL; unsigned int nchild = 0;
        if (!XQueryTree(a->dpy, target, &root_ret, &parent, &children, &nchild)) break;
        if (children) XFree(children);
        target = parent;
    }
    *ver_out = 0; (void)x; (void)y;
    return None;
}

static void build_xdnd_data(App *a) {
    free(a->xdnd_data);
    GString *gs = g_string_new(NULL);
    int count = selected_count(a);
    if (count > 0) {
        for (int i = 0; i < a->entry_count; i++) {
            if (!a->entries[i].selected) continue;
            char *uri = g_file_get_uri(a->entries[i].file);
            g_string_append(gs, uri); g_string_append(gs, "\r\n"); g_free(uri);
        }
    } else if (a->dnd_index >= 0 && a->dnd_index < a->entry_count) {
        char *uri = g_file_get_uri(a->entries[a->dnd_index].file);
        g_string_append(gs, uri); g_string_append(gs, "\r\n"); g_free(uri);
    }
    a->xdnd_data = g_string_free(gs, FALSE);
}

static void drag_icon_label(App *a, char *out, size_t outsz) {
    int count = selected_count(a);
    if (count > 1) snprintf(out, outsz, "%d items", count);
    else if (a->dnd_index >= 0 && a->dnd_index < a->entry_count)
        snprintf(out, outsz, "%s", a->entries[a->dnd_index].name);
    else snprintf(out, outsz, "Dragging");
}

static void show_drag_icon(App *a, int root_x, int root_y) {
    char label[256];
    drag_icon_label(a, label, sizeof(label));
    int pad = 8;
    int tw = text_w(a, a->font, label);
    int w = tw + pad * 2;
    int h = a->row_h + 4;
    if (w < 40) w = 40;
    if (h < 20) h = 20;

    if (!a->drag_icon) {
        XSetWindowAttributes wa;
        memset(&wa, 0, sizeof(wa));
        wa.override_redirect = True;
        wa.background_pixel = a->panel.pixel;
        wa.border_pixel = a->accent.pixel;
        wa.event_mask = ExposureMask;
        a->drag_icon = XCreateWindow(a->dpy, RootWindow(a->dpy, a->scr),
                                     root_x + 16, root_y + 16,
                                     (unsigned)w, (unsigned)h, 1,
                                     CopyFromParent, InputOutput, CopyFromParent,
                                     CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask,
                                     &wa);
        a->drag_icon_draw = XftDrawCreate(a->dpy, a->drag_icon,
                                          DefaultVisual(a->dpy, a->scr),
                                          DefaultColormap(a->dpy, a->scr));
    } else {
        XResizeWindow(a->dpy, a->drag_icon, (unsigned)w, (unsigned)h);
    }
    a->drag_icon_w = w;
    a->drag_icon_h = h;

    XMoveWindow(a->dpy, a->drag_icon, root_x + 16, root_y + 16);
    XMapRaised(a->dpy, a->drag_icon);

    XSetForeground(a->dpy, a->gc, a->panel.pixel);
    XFillRectangle(a->dpy, a->drag_icon, a->gc, 0, 0, (unsigned)w, (unsigned)h);
    XSetForeground(a->dpy, a->gc, a->accent.pixel);
    XDrawRectangle(a->dpy, a->drag_icon, a->gc, 0, 0, (unsigned)(w - 1), (unsigned)(h - 1));
    if (a->drag_icon_draw) {
        int baseline = (h + a->font->ascent - a->font->descent) / 2;
        XftDrawStringUtf8(a->drag_icon_draw, &a->text, a->font, pad, baseline,
                          (const FcChar8 *)label, (int)strlen(label));
    }
    XFlush(a->dpy);
}

static void move_drag_icon(App *a, int root_x, int root_y) {
    if (!a->drag_icon) return;
    XMoveWindow(a->dpy, a->drag_icon, root_x + 16, root_y + 16);
    XFlush(a->dpy);
}

static void hide_drag_icon(App *a) {
    if (a->drag_icon) {
        XUnmapWindow(a->dpy, a->drag_icon);
        XFlush(a->dpy);
    }
}

static void xdnd_update_target(App *a, int root_x, int root_y) {
    int ver = 0;
    Window target = xdnd_find_target(a, root_x, root_y, &ver);
    if (target != a->xdnd_target) {
        if (a->xdnd_target != None) {
            xdnd_send(a, a->xdnd_target, a->xdnd_leave, 0, 0, 0, 0);
        }
        a->xdnd_target = target;
        a->xdnd_version = ver;
        if (target != None) {
            Atom tlist[3] = { a->atom_uri_list, a->atom_utf8, a->atom_gnome_files };
            XChangeProperty(a->dpy, a->win, a->xdnd_type_list, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)tlist, 3);
            XSetSelectionOwner(a->dpy, a->xdnd_selection, a->win, CurrentTime);
            long d1 = ((long)(ver > 5 ? 5 : ver) << 24) | 1L; /* >3 types, see XdndTypeList */
            xdnd_send(a, target, a->xdnd_enter, d1, (long)a->atom_uri_list, (long)a->atom_utf8, (long)a->atom_gnome_files);
        }
    }
    if (a->xdnd_target != None) {
        long pos = ((long)root_x << 16) | ((long)root_y & 0xffff);
        xdnd_send(a, a->xdnd_target, a->xdnd_position, 0, pos, CurrentTime, (long)a->xdnd_action_copy);
    }
}

static int rects_intersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) return 0;
    return !(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay);
}

static int item_rect(App *a, int i, int *rx, int *ry, int *rw, int *rh) {
    if (i < 0 || i >= a->entry_count) return 0;
    if (a->icon_mode) {
        int cols, cell_w, cell_h;
        icon_grid(a, &cols, &cell_w, &cell_h);
        int grid_w = cols * cell_w;
        int xoff = (list_w(a) - grid_w) / 2;
        if (xoff < 0) xoff = 0;
        int vis_start = a->list_scroll * cols;
        if (i < vis_start) return 0;
        int idx = i - vis_start;
        int col = idx % cols;
        int row = idx / cols;
        int vis_rows = list_h(a) / cell_h;
        if (row >= vis_rows + 1) return 0;
        *rx = list_x(a) + xoff + col * cell_w + 4;
        *ry = list_y() + row * cell_h + 4;
        *rw = cell_w - 8;
        *rh = cell_h - 8;
        return 1;
    } else {
        int start = a->list_scroll;
        int rows = (list_h(a) - a->row_h) / a->row_h;
        if (i < start || i >= start + rows) return 0;
        int y0 = list_y() + a->row_h + (i - start) * a->row_h;
        *rx = list_x(a);
        *ry = y0;
        *rw = list_w(a);
        *rh = a->row_h;
        return 1;
    }
}

static void apply_band_selection(App *a) {
    int x1 = a->band_x0 < a->band_x1 ? a->band_x0 : a->band_x1;
    int y1 = a->band_y0 < a->band_y1 ? a->band_y0 : a->band_y1;
    int x2 = a->band_x0 > a->band_x1 ? a->band_x0 : a->band_x1;
    int y2 = a->band_y0 > a->band_y1 ? a->band_y0 : a->band_y1;
    int bw = x2 - x1, bh = y2 - y1;
    if (!a->band_additive) clear_selection(a);
    for (int i = 0; i < a->entry_count; i++) {
        int rx, ry, rw, rh;
        if (!item_rect(a, i, &rx, &ry, &rw, &rh)) continue;
        if (rects_intersect(x1, y1, bw, bh, rx, ry, rw, rh))
            a->entries[i].selected = 1;
    }
    if (selected_count(a) == 0) a->list_sel = -1;
}

static void handle_motion(App *a, XMotionEvent *ev) {
    if (a->split_drag) {
        int new_w = a->split_side_w0 + (ev->x - a->split_drag_x0);
        if (new_w < 80)  new_w = 80;
        if (new_w > a->win_w / 2) new_w = a->win_w / 2;
        a->side_w = new_w;
        draw_app(a); return;
    }

    /* rubber-band selection */
    if (a->band_press && (ev->state & Button1Mask)) {
        int dx = ev->x - a->band_x0;
        int dy = ev->y - a->band_y0;
        if (!a->band_active && (dx*dx + dy*dy) > 16) {
            a->band_active = 1;
            XGrabPointer(a->dpy, a->win, False,
                         PointerMotionMask | ButtonReleaseMask | ButtonPressMask,
                         GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        }
        if (a->band_active) {
            a->band_x1 = ev->x;
            a->band_y1 = ev->y;
            apply_band_selection(a);
            draw_app(a);
            return;
        }
    }

    /* internal drag and drop */
    if (a->dnd_press && (ev->state & Button1Mask)) {
        a->dnd_cur_x = ev->x;
        a->dnd_cur_y = ev->y;
        int dx = ev->x - a->dnd_start_x;
        int dy = ev->y - a->dnd_start_y;
        if (!a->dnd_dragging && (dx*dx + dy*dy) > 25) {
            a->dnd_dragging = 1;
            a->xdnd_target = None;
            a->xdnd_version = 0;
            a->xdnd_last_x = -9999;
            a->xdnd_last_y = -9999;
            build_xdnd_data(a);
            XSetSelectionOwner(a->dpy, a->xdnd_selection, a->win, CurrentTime);
            if (a->drag_cursor) XDefineCursor(a->dpy, a->win, a->drag_cursor);
            XGrabPointer(a->dpy, a->win, False,
                         PointerMotionMask | ButtonReleaseMask | ButtonPressMask,
                         GrabModeAsync, GrabModeAsync, None, a->drag_cursor, CurrentTime);
            show_drag_icon(a, ev->x_root, ev->y_root);
        }
        if (a->dnd_dragging) {
            int sr = side_row_at(a, ev->x, ev->y);
            a->dnd_target_side = -1;
            a->dnd_target_item = -1;
            if (sr >= 0) {
                SideEntry *s = &a->side[sr];
                if (s->file || s->volume) a->dnd_target_side = sr;
                if (a->xdnd_target != None) { xdnd_send(a, a->xdnd_target, a->xdnd_leave, 0,0,0,0); a->xdnd_target = None; }
            } else {
                int ir = item_at(a, ev->x, ev->y);
                if (ir >= 0 && ir < a->entry_count && a->entries[ir].ftype == FT_DIR) {
                    if (!(a->entries[ir].selected)) a->dnd_target_item = ir;
                }
                if (a->dnd_target_item < 0) {
                    /* check for external XDND target (throttled) */
                    int dxr = ev->x_root - a->xdnd_last_x;
                    int dyr = ev->y_root - a->xdnd_last_y;
                    if (dxr*dxr + dyr*dyr > 16 || a->xdnd_target == None) {
                        a->xdnd_last_x = ev->x_root;
                        a->xdnd_last_y = ev->y_root;
                        xdnd_update_target(a, ev->x_root, ev->y_root);
                    }
                } else if (a->xdnd_target != None) {
                    xdnd_send(a, a->xdnd_target, a->xdnd_leave, 0,0,0,0); a->xdnd_target = None;
                    a->xdnd_last_x = -9999; a->xdnd_last_y = -9999;
                }
            }
            move_drag_icon(a, ev->x_root, ev->y_root);
            draw_app(a);
            return;
        }
    }
    /* update menu hover */
    if (a->menu_open) {
        int count = g_menu_count;
        int mw = 240, mh = count * a->row_h + 4;
        int mx = a->menu_x, my = a->menu_y;
        if (mx + mw > a->win_w) mx = a->win_w - mw;
        if (my + mh > a->win_h) my = a->win_h - mh;
        int old_sel = a->menu_sel;
        a->menu_sel = -1;
        if (ev->x >= mx && ev->x < mx + mw &&
            ev->y >= my && ev->y < my + mh) {
            int row = (ev->y - my - 2) / a->row_h;
            if (row >= 0 && row < count && g_menu_items[row].id != MENU_NONE) a->menu_sel = row;
        }
        if (a->menu_sel != old_sel) draw_app(a);
    }
}

/* double-click detection */
static Time g_last_click_time = 0;
static int  g_last_click_row  = -1;

static void handle_click_or_dblclick(App *a, XButtonEvent *ev) {
    if (ev->button != Button1) return;
    if (a->menu_open || a->dialog_open || a->err_open || a->props_open) return;
    if (a->dnd_dragging) return;
    int row = item_at(a, ev->x, ev->y);
    if (row < 0) return;

    int is_dbl = (row == g_last_click_row &&
                  ev->time - g_last_click_time < 400);
    g_last_click_time = ev->time;
    g_last_click_row  = row;

    if (is_dbl) {
        g_last_click_row = -1;
        open_entry(a, row);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * GLib main-loop integration via timer fd
 * ══════════════════════════════════════════════════════════════════*/
static void pump_glib(void) {
    GMainContext *ctx = g_main_context_default();
    while (g_main_context_iteration(ctx, FALSE))
        ;
}


/* ════════════════════════════════════════════════════════════════════
 * UI icon loading (toolbar buttons)
 * ══════════════════════════════════════════════════════════════════*/
static XImage *load_ui_icon(App *a, const char *path) {
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
    if (!pb) { if (err) g_error_free(err); return NULL; }
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    int nchan = gdk_pixbuf_get_n_channels(pb);
    int has_alpha = gdk_pixbuf_get_has_alpha(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);

    Visual *vis = DefaultVisual(a->dpy, a->scr);
    int depth = DefaultDepth(a->dpy, a->scr);
    if (depth < 24) depth = 24;
    int bpp = 32;
    int bytes_per_line = w * 4;
    char *data = calloc(1, (size_t)bytes_per_line * (size_t)h);
    if (!data) { g_object_unref(pb); return NULL; }

    unsigned long rm = vis->red_mask, gm = vis->green_mask, bm = vis->blue_mask;
    int rs=0, gs=0, bs=0;
    if (rm) while (((rm >> rs) & 1UL) == 0UL) rs++;
    if (gm) while (((gm >> gs) & 1UL) == 0UL) gs++;
    if (bm) while (((bm >> bs) & 1UL) == 0UL) bs++;

    for (int yy = 0; yy < h; yy++) {
        guchar *row = px + yy * stride;
        for (int xx = 0; xx < w; xx++) {
            guchar r = row[xx * nchan + 0];
            guchar g = row[xx * nchan + 1];
            guchar b = row[xx * nchan + 2];
            guchar alpha = (has_alpha && nchan >= 4) ? row[xx * nchan + 3] : 255;
            if (alpha < 255) {
                r = (guchar)((r * alpha + 42 * (255 - alpha)) / 255);
                g = (guchar)((g * alpha + 34 * (255 - alpha)) / 255);
                b = (guchar)((b * alpha + 48 * (255 - alpha)) / 255);
            }
            unsigned long pixel = ((unsigned long)r << rs) |
                                  ((unsigned long)g << gs) |
                                  ((unsigned long)b << bs);
            memcpy(data + (yy * bytes_per_line + xx * 4), &pixel, 4);
        }
    }

    XImage *img = XCreateImage(a->dpy, vis, (unsigned)depth, ZPixmap, 0, data, w, h, bpp, bytes_per_line);
    g_object_unref(pb);
    if (!img) { free(data); return NULL; }
    return img;
}

static void load_ui_icons(App *a) {
    for (int i = 0; i < UI_ICON_COUNT; i++) { a->ui_icons[i] = NULL; a->ui_icon_w[i] = 0; a->ui_icon_h[i] = 0; }
    static const char *names[UI_ICON_COUNT] = {
        "back","forward","up","go","hidden","new_folder","new_file",
        "bm_add","bm_remove","view_list","view_icons","view_large"
    };
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n > 0) exe[n] = '\0';
    char exedir[PATH_MAX] = {0};
    if (exe[0]) { snprintf(exedir, sizeof(exedir), "%s", exe); char *sl = strrchr(exedir,'/'); if (sl) *sl = '\0'; }
    const char *home = g_get_home_dir();
    for (int i = 0; i < UI_ICON_COUNT; i++) {
        char candidates[10][PATH_MAX];
        int c = 0;
        if (exedir[0]) snprintf(candidates[c++], PATH_MAX, "%s/src/icons/ui/%s.png", exedir, names[i]);
        if (exedir[0]) snprintf(candidates[c++], PATH_MAX, "%s/icons/ui/%s.png", exedir, names[i]);
        snprintf(candidates[c++], PATH_MAX, "src/icons/ui/%s.png", names[i]);
        snprintf(candidates[c++], PATH_MAX, "/usr/local/share/gnuchanfilemanager/icons/ui/%s.png", names[i]);
        snprintf(candidates[c++], PATH_MAX, "/usr/share/gnuchanfilemanager/icons/ui/%s.png", names[i]);
        if (home) snprintf(candidates[c++], PATH_MAX, "%s/.local/share/gnuchanfilemanager/icons/ui/%s.png", home, names[i]);
        for (int k = 0; k < c; k++) {
            if (access(candidates[k], R_OK) != 0) continue;
            XImage *img = load_ui_icon(a, candidates[k]);
            if (img) {
                a->ui_icons[i] = img;
                a->ui_icon_w[i] = img->width;
                a->ui_icon_h[i] = img->height;
                break;
            }
        }
    }
}

static void free_ui_icons(App *a) {
    for (int i = 0; i < UI_ICON_COUNT; i++) {
        if (a->ui_icons[i]) { XDestroyImage(a->ui_icons[i]); a->ui_icons[i] = NULL; }
        a->ui_icon_w[i] = 0; a->ui_icon_h[i] = 0;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Initialisation
 * ══════════════════════════════════════════════════════════════════*/
static int init_app(App *a, int argc, char **argv) {
    memset(a, 0, sizeof(*a));

    /* GLib/GIO need type system initialized — g_type_init is a no-op
       in GLib >= 2.36 but calling it explicitly avoids the deprecation. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    g_type_init();
#pragma GCC diagnostic pop

    a->side_w     = SIDE_W;
    a->list_sel   = -1;
    a->side_sel   = -1;
    a->menu_sel   = -1;
    a->font_px    = FONT_PX_DEFAULT;
    a->row_h      = ROW_H_DEFAULT;
    a->dir_monitor = NULL;
    a->dir_monitor_id = 0;
    a->refresh_idle_id = 0;
    a->places_expanded = 1;
    a->bm_expanded = 1;
    a->recent_expanded = 1;
    a->dev_expanded = 1;
    a->picker_open = 0;
    a->picker_move = 0;
    a->picker_dir = NULL;
    a->picker_count = 0;
    a->picker_sel = -1;
    a->picker_scroll = 0;
    a->dnd_press = 0;
    a->dnd_dragging = 0;
    a->dnd_cur_x = 0;
    a->dnd_cur_y = 0;
    a->dnd_index = -1;
    a->dnd_target_side = -1;
    a->dnd_target_item = -1;
    a->dnd_external = 0;
    a->xdnd_target = None;
    a->xdnd_version = 0;
    a->xdnd_data = NULL;
    a->caret_on = 1;
    a->caret_tick = 0;
    a->xdnd_last_x = -9999;
    a->xdnd_last_y = -9999;
    a->layout_w = 0;
    a->layout_h = 0;
    a->layout_side_w = -1;
    a->icon_mode = 1; /* default to icon view so thumbnails/icons are visible */
    a->icon_zoom = 0;
    a->sort_mode = SORT_NAME;
    a->sort_desc = 0;
    a->sel_anchor = -1;
    a->band_active = 0;
    a->band_press = 0;
    a->band_x0 = a->band_y0 = a->band_x1 = a->band_y1 = 0;
    a->band_additive = 0;
    a->rename_active = 0;
    a->rename_index = -1;
    a->rename_text[0] = '\0';
    a->rename_len = 0;
    a->rename_cursor = 0;
    snprintf(a->free_space, sizeof(a->free_space), "Bos alan: -");

    /* config path */
    const char *cfg_home = getenv("XDG_CONFIG_HOME");
    if (cfg_home && cfg_home[0] == '/')
        snprintf(a->config_path, sizeof(a->config_path),
                 "%s/GnuChanFileManager/config.conf", cfg_home);
    else {
        char *hp = get_effective_home_path();
        snprintf(a->config_path, sizeof(a->config_path),
                 "%s/.config/GnuChanFileManager/config.conf", hp);
        g_free(hp);
    }

    /* default address */
    {
        char *hp = get_effective_home_path();
        snprintf(a->address, sizeof(a->address), "%s", hp);
        a->address_len = (int)strlen(a->address);
        g_free(hp);
    }

    load_config(a);

    /* always start in current user's home unless an explicit path was provided */
    if (argc < 2) {
        char *hp = get_effective_home_path();
        snprintf(a->address, sizeof(a->address), "%s", hp);
        a->address_len = (int)strlen(a->address);
        a->address_cursor = a->address_len;
        g_free(hp);
    }

    /* copy queue */
    pthread_mutex_init(&a->copy_mutex, NULL);
    pthread_cond_init(&a->copy_cond, NULL);
    pthread_create(&a->copy_thread, NULL, copy_worker, a);

    /* X11 */
    a->dpy = XOpenDisplay(NULL);
    if (!a->dpy) { fprintf(stderr, "GnuChanFileManager: cannot open display\n"); return 0; }
    a->scr = DefaultScreen(a->dpy);
    a->drag_cursor = XCreateFontCursor(a->dpy, XC_fleur);
    a->drag_icon = None;
    a->drag_icon_draw = NULL;
    a->drag_icon_w = a->drag_icon_h = 0;

    a->win_w = WIN_W;
    a->win_h = WIN_H;

    XSetWindowAttributes wa;
    memset(&wa, 0, sizeof(wa));
    wa.background_pixel  = BlackPixel(a->dpy, a->scr);
    wa.bit_gravity       = NorthWestGravity;
    wa.event_mask        = ExposureMask | KeyPressMask | ButtonPressMask |
                           ButtonReleaseMask | PointerMotionMask |
                           StructureNotifyMask | PropertyChangeMask;
    a->win = XCreateWindow(a->dpy, RootWindow(a->dpy, a->scr),
                           60, 60, (unsigned)a->win_w, (unsigned)a->win_h,
                           0, CopyFromParent, InputOutput, CopyFromParent,
                           CWBackPixel | CWBitGravity | CWEventMask, &wa);
    XSetWindowBackgroundPixmap(a->dpy, a->win, None);

    /* WM hints */
    XStoreName(a->dpy, a->win, "GnuChan File Manager");
    XClassHint ch = {"GnuChanFileManager", "GnuChanFileManager"};
    XSetClassHint(a->dpy, a->win, &ch);
    a->wm_delete = XInternAtom(a->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(a->dpy, a->win, &a->wm_delete, 1);
    XSizeHints sh; memset(&sh, 0, sizeof(sh));
    sh.flags = PMinSize; sh.min_width = 540; sh.min_height = 320;
    XSetWMNormalHints(a->dpy, a->win, &sh);

    /* GC */
    a->gc = XCreateGC(a->dpy, a->win, 0, NULL);

    /* Colours */
    alloc_color(a, C_BG,       &a->bg);
    alloc_color(a, C_PANEL,    &a->panel);
    alloc_color(a, C_EDGE,     &a->edge);
    alloc_color(a, C_FIELD,    &a->field);
    alloc_color(a, C_FIELD_ON, &a->field_on);
    alloc_color(a, C_TEXT,     &a->text);
    alloc_color(a, C_MUTED,    &a->muted);
    alloc_color(a, C_ACCENT,   &a->accent);
    alloc_color(a, C_SEL,      &a->sel);
    alloc_color(a, C_BTN,      &a->btn);
    alloc_color(a, C_BTN_HOT,  &a->btn_hot);

    /* Fonts / draw */
    load_fonts(a);
    if (!a->font) { fprintf(stderr, "GnuChanFileManager: no font\n"); return 0; }

    /* Clipboard / XDND atoms */
    a->atom_clipboard   = XInternAtom(a->dpy, "CLIPBOARD",                       False);
    a->atom_targets     = XInternAtom(a->dpy, "TARGETS",                          False);
    a->atom_gnome_files = XInternAtom(a->dpy, "x-special/gnome-copied-files",    False);
    a->atom_uri_list    = XInternAtom(a->dpy, "text/uri-list",                    False);
    a->atom_utf8        = XInternAtom(a->dpy, "UTF8_STRING",                      False);
    a->xdnd_aware       = XInternAtom(a->dpy, "XdndAware", False);
    a->xdnd_enter       = XInternAtom(a->dpy, "XdndEnter", False);
    a->xdnd_position    = XInternAtom(a->dpy, "XdndPosition", False);
    a->xdnd_status      = XInternAtom(a->dpy, "XdndStatus", False);
    a->xdnd_leave       = XInternAtom(a->dpy, "XdndLeave", False);
    a->xdnd_drop        = XInternAtom(a->dpy, "XdndDrop", False);
    a->xdnd_finished    = XInternAtom(a->dpy, "XdndFinished", False);
    a->xdnd_selection   = XInternAtom(a->dpy, "XdndSelection", False);
    a->xdnd_action_copy = XInternAtom(a->dpy, "XdndActionCopy", False);
    a->xdnd_action_move = XInternAtom(a->dpy, "XdndActionMove", False);
    a->xdnd_type_list   = XInternAtom(a->dpy, "XdndTypeList", False);
    {
        long xdnd_ver = 5;
        XChangeProperty(a->dpy, a->win, a->xdnd_aware, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&xdnd_ver, 1);
    }

    load_ui_icons(a);

    /* Volume monitor */
    a->volume_monitor = g_volume_monitor_get();
    a->vol_add_id = g_signal_connect(a->volume_monitor, "volume-added",
                                      G_CALLBACK(on_volume_changed), a);
    a->vol_rem_id = g_signal_connect(a->volume_monitor, "volume-removed",
                                      G_CALLBACK(on_volume_changed), a);
    a->mnt_add_id = g_signal_connect(a->volume_monitor, "mount-added",
                                      G_CALLBACK(on_volume_changed), a);
    a->mnt_rem_id = g_signal_connect(a->volume_monitor, "mount-removed",
                                      G_CALLBACK(on_volume_changed), a);

    refresh_sidebar(a);

    /* navigate to initial location */
    GFile *start = NULL;
    if (argc >= 2) {
        start = g_file_new_for_commandline_arg(argv[1]);
    } else {
        start = g_file_new_for_commandline_arg(a->address);
    }
    if (start) {
        /* prime history without adding a duplicate */
        if (a->history_count == 0) {
            a->history[a->history_count++] = g_object_ref(start);
            a->history_index = 0;
        }
        go_to(a, start, 0);
        g_object_unref(start);
    }

    XMapWindow(a->dpy, a->win);
    a->running = 1;
    return 1;
}

static void close_app(App *a) {
    /* stop copy thread */
    pthread_mutex_lock(&a->copy_mutex);
    a->shutting_down = 1;
    pthread_cond_broadcast(&a->copy_cond);
    pthread_mutex_unlock(&a->copy_mutex);
    pthread_join(a->copy_thread, NULL);
    pthread_mutex_destroy(&a->copy_mutex);
    pthread_cond_destroy(&a->copy_cond);

    free(a->clipboard_data);
    free(a->xdnd_data);
    free_ui_icons(a);

    if (a->refresh_idle_id) {
        g_source_remove(a->refresh_idle_id);
        a->refresh_idle_id = 0;
    }
    if (a->dir_monitor) {
        if (a->dir_monitor_id)
            g_signal_handler_disconnect(a->dir_monitor, a->dir_monitor_id);
        g_file_monitor_cancel(a->dir_monitor);
        g_object_unref(a->dir_monitor);
        a->dir_monitor = NULL;
    }

    /* free entries */
    for (int i = 0; i < a->entry_count; i++)
        if (a->entries[i].file) g_object_unref(a->entries[i].file);

    side_free_all(a);

    for (int i = 0; i < a->bookmark_count; i++) g_object_unref(a->bookmarks[i]);
    for (int i = 0; i < a->recent_count;   i++) g_object_unref(a->recent[i]);
    for (int i = 0; i < a->history_count;  i++) g_object_unref(a->history[i]);

    if (a->location)   g_object_unref(a->location);
    if (a->props_file) g_object_unref(a->props_file);

    /* volume monitor */
    if (a->volume_monitor) {
        g_signal_handler_disconnect(a->volume_monitor, a->vol_add_id);
        g_signal_handler_disconnect(a->volume_monitor, a->vol_rem_id);
        g_signal_handler_disconnect(a->volume_monitor, a->mnt_add_id);
        g_signal_handler_disconnect(a->volume_monitor, a->mnt_rem_id);
        g_object_unref(a->volume_monitor);
    }

    /* X11 */
    if (a->drag_icon_draw) XftDrawDestroy(a->drag_icon_draw);
    if (a->drag_icon)      XDestroyWindow(a->dpy, a->drag_icon);
    if (a->drag_cursor)    XFreeCursor(a->dpy, a->drag_cursor);
    if (a->draw)           XftDrawDestroy(a->draw);
    if (a->backbuf)        XFreePixmap(a->dpy, a->backbuf);
    if (a->font_bold && a->font_bold != a->font)
        XftFontClose(a->dpy, a->font_bold);
    if (a->font)           XftFontClose(a->dpy, a->font);
    if (a->gc)             XFreeGC(a->dpy, a->gc);
    if (a->win)            XDestroyWindow(a->dpy, a->win);
    if (a->dpy)            XCloseDisplay(a->dpy);
}

/* ════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════*/
int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("GnuChan File Manager %s\n", APP_VERSION);
        return 0;
    }

    static App app;
    if (!init_app(&app, argc, argv)) return 1;

    /* event loop */
    int xfd = ConnectionNumber(app.dpy);
    while (app.running) {
        /* pump GLib (volumes, async copies) */
        pump_glib();

        /* wait for X or GLib events */
        if (!XPending(app.dpy)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(xfd, &fds);
            struct timeval tv = {0, 50000}; /* 50 ms tick */
            select(xfd + 1, &fds, NULL, NULL, &tv);
        }

        while (XPending(app.dpy)) {
            XEvent ev;
            XNextEvent(app.dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) draw_app(&app);
                break;
            case ConfigureNotify:
                if (ev.xconfigure.width  != app.win_w ||
                    ev.xconfigure.height != app.win_h) {
                    app.win_w = ev.xconfigure.width;
                    app.win_h = ev.xconfigure.height;
                    recreate_backbuf(&app);
                    draw_app(&app);
                }
                break;
            case KeyPress:
                handle_key(&app, &ev.xkey);
                break;
            case ButtonPress:
                handle_button_press(&app, &ev.xbutton);
                handle_click_or_dblclick(&app, &ev.xbutton);
                break;
            case ButtonRelease:
                handle_button_release(&app, &ev.xbutton);
                break;
            case MotionNotify:
                /* coalesce motion events to reduce redraws */
                while (XPending(app.dpy)) {
                    XEvent next;
                    XPeekEvent(app.dpy, &next);
                    if (next.type != MotionNotify) break;
                    XNextEvent(app.dpy, &next);
                    ev = next;
                }
                handle_motion(&app, &ev.xmotion);
                break;
            case SelectionRequest:
                /* handle XDND selection requests */
                if (ev.xselectionrequest.selection == app.xdnd_selection) {
                    XSelectionEvent sev = {0};
                    sev.type = SelectionNotify; sev.requestor = ev.xselectionrequest.requestor;
                    sev.selection = ev.xselectionrequest.selection; sev.target = ev.xselectionrequest.target;
                    sev.time = ev.xselectionrequest.time; sev.property = None;
                    if (app.xdnd_data && (ev.xselectionrequest.target == app.atom_uri_list ||
                                          ev.xselectionrequest.target == app.atom_utf8 ||
                                          ev.xselectionrequest.target == app.atom_gnome_files)) {
                        Atom prop = ev.xselectionrequest.property ? ev.xselectionrequest.property : ev.xselectionrequest.target;
                        XChangeProperty(app.dpy, ev.xselectionrequest.requestor, prop,
                                        ev.xselectionrequest.target, 8, PropModeReplace,
                                        (unsigned char*)app.xdnd_data, (int)strlen(app.xdnd_data));
                        sev.property = prop;
                    } else if (ev.xselectionrequest.target == app.atom_targets) {
                        Atom prop = ev.xselectionrequest.property ? ev.xselectionrequest.property : app.atom_targets;
                        Atom t[3] = { app.atom_uri_list, app.atom_utf8, app.atom_gnome_files };
                        XChangeProperty(app.dpy, ev.xselectionrequest.requestor, prop,
                                        XA_ATOM, 32, PropModeReplace, (unsigned char*)t, 3);
                        sev.property = prop;
                    }
                    XSendEvent(app.dpy, ev.xselectionrequest.requestor, False, 0, (XEvent*)&sev);
                } else {
                    handle_selection_request(&app, &ev.xselectionrequest);
                }
                break;
            case SelectionNotify:
                handle_selection_notify(&app, &ev.xselection);
                break;
            case SelectionClear:
                free(app.clipboard_data);
                app.clipboard_data = NULL;
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == app.wm_delete)
                    app.running = 0;
                else if (ev.xclient.message_type == app.xdnd_status ||
                         ev.xclient.message_type == app.xdnd_finished) {
                    /* acknowledge; no-op for basic source */
                }
                break;
            }
        }

        /* caret blink + copy progress redraw (throttled) */
        {
            int need_draw = 0;
            static int copy_tick = 0;
            if (app.address_focused || app.search_focused || app.dialog_open) {
                app.caret_tick++;
                if (app.caret_tick >= 10) { /* ~500ms at 50ms tick */
                    app.caret_tick = 0;
                    app.caret_on = !app.caret_on;
                    need_draw = 1;
                }
            } else {
                if (!app.caret_on) app.caret_on = 1;
                app.caret_tick = 0;
            }
            pthread_mutex_lock(&app.copy_mutex);
            int cr = app.copy_running;
            pthread_mutex_unlock(&app.copy_mutex);
            if (cr) {
                if (++copy_tick >= 4) { copy_tick = 0; need_draw = 1; } /* ~200ms */
            } else {
                copy_tick = 0;
            }
            if (need_draw) draw_app(&app);
        }
    }

    save_config(&app);
    close_app(&app);
    return 0;
}
