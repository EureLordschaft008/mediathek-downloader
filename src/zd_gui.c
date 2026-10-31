/*
 * ZDF-Downloader – grafische Oberfläche (GTK 3)
 * =============================================
 * Aufbau und Optik wie beim AV1-Converter. Vier Seiten, oben umschaltbar:
 *   Suche          Treffer als Baum Serie -> Staffel -> Folge; jede Ebene hat ein Häkchen, und
 *                  jede Folge zeigt, ob sie schon geladen ist
 *   Downloads      Warteschlange mit Pause, Fortsetzen und Tempolimit
 *   Bibliothek     alles, was schon geladen wurde, nach Sendung sortiert
 *   Einstellungen  Speicherort, Qualität, AV1
 *
 * Suche läuft in einem eigenen Thread, das Ergebnis kommt über g_idle_add() in den
 * GTK-Hauptthread. Die Warteschlange arbeitet in der Engine (zd_core.c); ein Timer holt alle
 * 500 ms den Stand ab. Der gesamte Zustand der Oberfläche liegt in der Struktur "ui".
 *
 * Aufbau dieser Datei: kleine Helfer – Einstellungen – Trefferbaum – Spurenauswahl –
 * Seite Suche – Seite Downloads – Seite Bibliothek – Seite Einstellungen – Fenster, Menü,
 * Programmstart
 *
 * Aufruf:
 *     zdf-downloader
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "zd_core.h"

#define APP_ID "de.zdfdownloader.ZDFDownloader"
#define APP_NAME "ZDF-Downloader"
#define SETTINGS_GROUP "Einstellungen"
#define SETTINGS_FILE "einstellungen.ini"
#define RESULT_ROWS_BATCH 200   /* Zeilen der Trefferliste auf einmal (weitere per Knopf) */
#define TICK_MS 500

static const char *const KIND_TEXT[3] = {"Alles", "Filme", "Serien"};
static const char *const FILTER_TEXT[3] = {"Alle anzeigen", "Nur fehlende", "Nur geladene"};
static const char *const QUALITY_TEXT[3] = {"Hoch – 1080p (ca. 4,5 GB je 90 min)",
                                            "Mittel (ca. 2,3 GB je 90 min)",
                                            "Niedrig (ca. 0,5 GB je 90 min)"};
static const char *const ENCODER_TEXT[2] = {"GPU – schnell (VAAPI)", "CPU – langsam (SVT-AV1)"};

enum { FILTER_ALL, FILTER_MISSING, FILTER_DONE };
typedef enum { ROW_GROUP, ROW_SEASON, ROW_ITEM } RowKind;

/* ein Treffer samt Stand */
typedef struct {
    ZdTitle *title;
    char *path;                 /* vorhandene Datei */
    ZdStatus status;
    bool selected;
} Item;

typedef struct {
    int number;                 /* -1 = ohne Staffelangabe */
    GPtrArray *items;           /* Item* */
    bool expanded;
} Season;

/* eine Serie, Sendung oder ein einzelner Film */
typedef struct {
    char *name;
    bool is_film;
    GPtrArray *seasons;         /* Season* */
    bool expanded;
} Group;

/* angelegte Zeile der Trefferliste */
typedef struct {
    RowKind kind;
    gpointer data;              /* Group*, Season* oder Item* */
    GtkWidget *check;
} RowRef;

/* eine Suche im Hintergrund */
typedef struct {
    gint cancel;
    ZdQuery query;
    char *text, *dir;
    GPtrArray *items;           /* Item* */
    char *error;
    bool truncated;
} SearchJob;

/* Zeile der Warteschlange */
typedef struct {
    guint id;
    GtkWidget *row, *title, *subtitle, *bar, *pause, *retry, *remove, *open;
    char *target;
    bool seen;
} JobRow;

typedef struct {
    char *dir;
    int quality, encoder, gpu_quality, cpu_rf;
    int parallel, conversions, min_minutes;
    double limit_mb;
    bool limit_on, convert, keep_original, show_signed;
    int width, height;
    bool maximized;
} Config;

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *header, *stack, *infobar, *info_label;
    Config cfg;
    int layout_width;
    GtkWidget *page_columns[6];
    int page_column_count;
    bool quitting, gpu_ok;
    guint inhibit_cookie;

    /* Suche */
    GtkWidget *search_entry, *kind_combo, *filter_combo, *search_button, *search_spinner;
    GtkWidget *search_note, *results_list, *selection_label, *target_label, *download_button;
    GtkWidget *select_missing_button;
    SearchJob *search_job;
    GPtrArray *groups;          /* Group* */
    GPtrArray *refs;            /* RowRef* der angelegten Zeilen */
    int rows_limit;
    bool updating_checks, has_searched;

    /* Downloads */
    GtkWidget *downloads_page, *hero, *hero_sub, *jobs_list, *jobs_empty;
    GtkWidget *limit_switch, *limit_spin;
    GHashTable *job_rows;       /* id -> JobRow* */
    char *fingerprint;          /* erkennt Änderungen, die den Stand der Titel berühren */

    /* Bibliothek */
    GtkWidget *library_hero, *library_sub, *library_list, *library_search;
    GHashTable *library_open;   /* aufgeklappte Sendungen */

    /* Einstellungen */
    GtkWidget *dir_sub, *encoder_row, *encoder_sub, *gpu_row, *cpu_row, *keep_row;
} UiState;

static UiState ui;

static void rebuild_results(void);
static void rebuild_library(void);
static void update_selection(void);
static void start_search(void);

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
static void add_css_class(GtkWidget *w, const char *cls)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* Label mit linksbündigem Text und optionaler CSS-Klasse */
static GtkWidget *new_label(const char *text, const char *cls)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (cls) add_css_class(widget, cls);
    return widget;
}

/* umbrechender Text, der die Inhaltsspalte nicht verbreitert */
static GtkWidget *set_wrapping(GtkWidget *widget, int max_chars)
{
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), max_chars);
    return widget;
}

/* Text eines Labels setzen (ungültiges UTF-8 wird ersetzt) */
static void set_label_text(GtkWidget *widget, const char *text)
{
    char *valid = g_utf8_make_valid(text ? text : "", -1);
    gtk_label_set_text(GTK_LABEL(widget), valid);
    g_free(valid);
}

/* Pfad im Standard-Programm öffnen (Ordner im Dateimanager, Video im Player) */
static void open_in_default_app(const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (uri && gtk_show_uri_on_window(GTK_WINDOW(ui.win), uri, GDK_CURRENT_TIME, &err)) {
        g_free(uri);
        return;
    }
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                          "Konnte nicht geöffnet werden");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
                                             err ? err->message : path);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_clear_error(&err);
    g_free(uri);
}

/* Ordner einer Datei öffnen; fehlt der Ordner, den nächsten vorhandenen darüber */
static void open_folder_of(const char *path)
{
    char *dir = g_path_get_dirname(path);
    while (!g_file_test(dir, G_FILE_TEST_IS_DIR) && strlen(dir) > 1) {
        char *parent = g_path_get_dirname(dir);
        g_free(dir);
        dir = parent;
    }
    open_in_default_app(dir);
    g_free(dir);
}

/* Trennlinie zwischen den Zeilen einer Liste */
static void list_separator_header(GtkListBoxRow *this_row, GtkListBoxRow *before, gpointer data)
{
    (void)data;
    if (before && !gtk_list_box_row_get_header(this_row))
        gtk_list_box_row_set_header(this_row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

/* umrandete Liste mit Überschrift (Einstellungsgruppe) */
static GtkWidget *add_group(GtkWidget *box, const char *title)
{
    if (title) gtk_box_pack_start(GTK_BOX(box), new_label(title, "zd-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "zd-group");
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), list_separator_header, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), list);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return list;
}

/* Zeile: [prefix]  Titel / Untertitel  [suffix] */
static GtkWidget *add_row(GtkWidget *list, const char *title, const char *subtitle,
                          GtkWidget *prefix, GtkWidget *suffix, GtkWidget **sub_out)
{
    GtkWidget *r = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 10);
    gtk_widget_set_margin_bottom(h, 10);
    gtk_widget_set_margin_start(h, 14);
    gtk_widget_set_margin_end(h, 12);
    if (prefix) {
        gtk_widget_set_valign(prefix, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(h), prefix, FALSE, FALSE, 0);
    }
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *t = new_label(title, "zd-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *s = NULL;
    if (subtitle) {
        s = new_label(subtitle, "zd-sub");
        add_css_class(s, "dim-label");
        gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(s), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_width_chars(GTK_LABEL(s), 20);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 60);
        gtk_box_pack_start(GTK_BOX(v), s, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (suffix) {
        gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(h), suffix, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(r), h);
    gtk_container_add(GTK_CONTAINER(list), r);
    if (sub_out) *sub_out = s;
    return r;
}

/* Inhaltsspalte; ihre Ränder folgen der Fensterbreite */
static GtkWidget *new_column(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    if (ui.page_column_count < (int)G_N_ELEMENTS(ui.page_columns))
        ui.page_columns[ui.page_column_count++] = box;
    return box;
}

/* Seitenrahmen: Spalte in einem Scrollbereich */
static GtkWidget *new_page_column(GtkWidget **scroller)
{
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *box = new_column();
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    *scroller = scrolled;
    return box;
}

/* runder Hauptknopf */
static GtkWidget *new_pill_button(const char *text, const char *cls)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    add_css_class(b, "zd-pill");
    if (cls) add_css_class(b, cls);
    return b;
}

/* flacher Knopf mit Symbol */
static GtkWidget *new_icon_button(const char *icon, const char *tooltip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tooltip);
    return b;
}

/* Zahlenfeld mit Bereich */
static GtkWidget *new_spin(double lo, double hi, double step, double value)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    return s;
}

/* Auswahlliste aus festen Texten */
static GtkWidget *new_combo(const char *const *texts, int count, int active)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < count; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), texts[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    return combo;
}

/* kleine farbige Marke; cls = "ok", "warn" oder NULL */
static GtkWidget *new_badge(const char *text, const char *cls)
{
    GtkWidget *badge = gtk_label_new(text);
    add_css_class(badge, "zd-badge");
    if (cls) add_css_class(badge, cls);
    return badge;
}

/* Ja/Nein-Rückfrage (modal) -> true bei "yes_label" */
static bool ask_confirmation(const char *title, const char *text, const char *no_label,
                             const char *yes_label, bool destructive)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    if (text) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_add_button(GTK_DIALOG(d), no_label, GTK_RESPONSE_CANCEL);
    GtkWidget *yes = gtk_dialog_add_button(GTK_DIALOG(d), yes_label, GTK_RESPONSE_YES);
    add_css_class(yes, destructive ? "destructive-action" : "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    bool r = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(d);
    return r;
}

/* Meldungsleiste oben im Fenster (NULL schließt sie) */
static void show_info_bar(GtkMessageType type, const char *text)
{
    if (!text) {
        gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), FALSE);
        return;
    }
    gtk_info_bar_set_message_type(GTK_INFO_BAR(ui.infobar), type);
    set_label_text(ui.info_label, text);
    gtk_widget_show(ui.infobar);
    gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), TRUE);
}

/* Datei- und Ordnerpfad, der am Knopf hängt, öffnen */
static void on_open_file_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    open_in_default_app(g_object_get_data(G_OBJECT(b), "path"));
}

static void on_open_folder_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    open_folder_of(g_object_get_data(G_OBJECT(b), "path"));
}

/* Knopf, der mit dem Pfad path arbeitet */
static GtkWidget *new_path_button(const char *icon, const char *tooltip, const char *path,
                                  GCallback callback)
{
    GtkWidget *b = new_icon_button(icon, tooltip);
    g_object_set_data_full(G_OBJECT(b), "path", g_strdup(path), g_free);
    g_signal_connect(b, "clicked", callback, NULL);
    return b;
}

/* Datum für die Anzeige */
static char *format_date(gint64 unix_time)
{
    GDateTime *when = g_date_time_new_from_unix_local(unix_time);
    char *text = g_date_time_format(when, "%d.%m.%Y");
    g_date_time_unref(when);
    return text;
}

/* Mehrzahl: "1 Folge", "2 Folgen" */
static const char *plural(int count, const char *one, const char *many)
{
    return count == 1 ? one : many;
}

static const char CSS[] =
    ".zd-heading { font-weight: bold; margin-top: 18px; margin-bottom: 4px; }\n"
    "frame.zd-group > border { border-radius: 10px; background-color: @theme_base_color; }\n"
    "frame.zd-group list { background-color: transparent; }\n"
    "frame.zd-group list > row:first-child { border-top-left-radius: 10px;"
    " border-top-right-radius: 10px; }\n"
    "frame.zd-group list > row:last-child { border-bottom-left-radius: 10px;"
    " border-bottom-right-radius: 10px; }\n"
    ".zd-sub { font-size: 90%; }\n"
    ".zd-hero { font-size: 170%; font-weight: 800; }\n"
    ".zd-lead { font-size: 105%; }\n"
    ".zd-pill { border-radius: 999px; padding: 8px 30px; font-weight: bold; }\n"
    ".zd-badge { border-radius: 999px; padding: 1px 9px; font-size: 85%; font-weight: bold;"
    " color: @theme_selected_bg_color;"
    " background-color: alpha(@theme_selected_bg_color, 0.16); }\n"
    ".zd-badge.ok { color: @success_color; background-color: alpha(@success_color, 0.16); }\n"
    ".zd-badge.warn { color: @warning_color; background-color: alpha(@warning_color, 0.16); }\n"
    ".zd-badge.err { color: @error_color; background-color: alpha(@error_color, 0.16); }\n"
    ".zd-footer { border-top: 1px solid @borders; padding-top: 10px; padding-bottom: 12px; }\n";

/* ---------------------------------------------------------------------------
 * Einstellungen
 * ------------------------------------------------------------------------- */
static char *settings_path(void)
{
    return g_build_filename(zd_app_dir(), SETTINGS_FILE, NULL);
}

/* Ganzzahl aus der Einstellungsdatei, begrenzt auf lo..hi */
static int settings_int(GKeyFile *file, const char *key, int fallback, int lo, int hi)
{
    GError *err = NULL;
    int value = g_key_file_get_integer(file, SETTINGS_GROUP, key, &err);
    if (err) value = fallback;
    g_clear_error(&err);
    return CLAMP(value, lo, hi);
}

static bool settings_bool(GKeyFile *file, const char *key, bool fallback)
{
    GError *err = NULL;
    bool value = g_key_file_get_boolean(file, SETTINGS_GROUP, key, &err);
    if (err) value = fallback;
    g_clear_error(&err);
    return value;
}

/* Einstellungen laden; fehlende Werte bekommen die Standardwerte */
static void settings_load(void)
{
    GKeyFile *file = g_key_file_new();
    char *path = settings_path();
    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    Config *c = &ui.cfg;
    c->dir = g_key_file_get_string(file, SETTINGS_GROUP, "Speicherort", NULL);
    if (!c->dir || !*c->dir) {
        const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        g_free(c->dir);
        c->dir = g_build_filename(videos ? videos : g_get_home_dir(), "ZDF Mediathek", NULL);
    }
    c->quality = settings_int(file, "Qualitaet", ZD_QUALITY_HIGH, 0, 2);
    c->encoder = settings_int(file, "Encoder", ZD_ENCODER_GPU, 0, 1);
    c->gpu_quality = settings_int(file, "GpuQualitaet", ZD_DEFAULT_GPU_QUALITY, 1, 255);
    c->cpu_rf = settings_int(file, "CpuRf", ZD_DEFAULT_CPU_RF, 1, 63);
    c->parallel = settings_int(file, "Downloads", 2, 1, ZD_MAX_DOWNLOADS);
    c->conversions = settings_int(file, "Umwandlungen", 1, 1, ZD_MAX_CONVERSIONS);
    c->min_minutes = settings_int(file, "Mindestlaenge", 10, 0, 300);
    c->limit_mb = CLAMP(settings_int(file, "LimitMBs", 10, 1, 1000), 1, 1000);
    c->limit_on = settings_bool(file, "LimitAktiv", false);
    c->convert = settings_bool(file, "AV1", false);
    c->keep_original = settings_bool(file, "OriginalBehalten", false);
    c->show_signed = settings_bool(file, "Gebaerdensprache", false);
    c->width = settings_int(file, "Breite", 980, 0, 10000);
    c->height = settings_int(file, "Hoehe", 800, 0, 10000);
    c->maximized = settings_bool(file, "Maximiert", false);
    g_key_file_free(file);
}

/* Einstellungen speichern (nach jeder Änderung) */
static void settings_save(void)
{
    const Config *c = &ui.cfg;
    GKeyFile *file = g_key_file_new();
    g_key_file_set_string(file, SETTINGS_GROUP, "Speicherort", c->dir);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Qualitaet", c->quality);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Encoder", c->encoder);
    g_key_file_set_integer(file, SETTINGS_GROUP, "GpuQualitaet", c->gpu_quality);
    g_key_file_set_integer(file, SETTINGS_GROUP, "CpuRf", c->cpu_rf);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Downloads", c->parallel);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Umwandlungen", c->conversions);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Mindestlaenge", c->min_minutes);
    g_key_file_set_integer(file, SETTINGS_GROUP, "LimitMBs", (int)c->limit_mb);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "LimitAktiv", c->limit_on);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "AV1", c->convert);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "OriginalBehalten", c->keep_original);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "Gebaerdensprache", c->show_signed);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Breite", c->width);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Hoehe", c->height);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "Maximiert", c->maximized);
    char *path = settings_path();
    if (!g_key_file_save_to_file(file, path, NULL)) zd_log("Einstellungen nicht gespeichert");
    g_free(path);
    g_key_file_free(file);
}

/* Tempolimit und Parallelität an die Engine geben */
static void apply_queue_settings(void)
{
    zd_set_rate_limit(ui.cfg.limit_on ? (gint64)(ui.cfg.limit_mb * 1e6) : 0);
    zd_set_parallel(ui.cfg.parallel, ui.cfg.conversions);
}

/* ---------------------------------------------------------------------------
 * Trefferbaum
 * ------------------------------------------------------------------------- */
static void item_free(gpointer data)
{
    Item *item = data;
    zd_title_free(item->title);
    g_free(item->path);
    g_free(item);
}

static void season_free(gpointer data)
{
    Season *season = data;
    g_ptr_array_free(season->items, TRUE);
    g_free(season);
}

static void group_free(gpointer data)
{
    Group *group = data;
    g_free(group->name);
    g_ptr_array_free(group->seasons, TRUE);
    g_free(group);
}

/* lässt sich dieser Treffer auswählen? */
static bool item_open(const Item *item)
{
    return item->status != ZD_STATUS_DONE && item->status != ZD_STATUS_QUEUED;
}

/* passt der Treffer zum Filter "Anzeigen"? */
static bool item_visible(const Item *item)
{
    int filter = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.filter_combo));
    if (filter == FILTER_MISSING) return item_open(item);
    if (filter == FILTER_DONE) return item->status == ZD_STATUS_DONE;
    return true;
}

/* Zähler einer Staffel oder Serie */
typedef struct {
    int total, visible, open, done, queued, selected;
} Counts;

static void counts_add_season(Counts *c, const Season *season)
{
    for (guint i = 0; i < season->items->len; i++) {
        const Item *item = season->items->pdata[i];
        c->total++;
        c->visible += item_visible(item);
        c->open += item_open(item);
        c->done += item->status == ZD_STATUS_DONE;
        c->queued += item->status == ZD_STATUS_QUEUED;
        c->selected += item->selected;
    }
}

static void counts_add_group(Counts *c, const Group *group)
{
    for (guint i = 0; i < group->seasons->len; i++) counts_add_season(c, group->seasons->pdata[i]);
}

/* Häkchen aller offenen Treffer einer Staffel setzen */
static void season_select(Season *season, bool selected)
{
    for (guint i = 0; i < season->items->len; i++) {
        Item *item = season->items->pdata[i];
        if (item_open(item) && item_visible(item)) item->selected = selected;
    }
}

static void group_select(Group *group, bool selected)
{
    for (guint i = 0; i < group->seasons->len; i++)
        season_select(group->seasons->pdata[i], selected);
}

/* Reihenfolge innerhalb einer Staffel */
static int compare_items(gconstpointer a, gconstpointer b)
{
    const ZdTitle *x = (*(Item *const *)a)->title, *y = (*(Item *const *)b)->title;
    if (x->episode != y->episode) return x->episode - y->episode;
    return x->timestamp < y->timestamp ? 1 : x->timestamp > y->timestamp ? -1 : 0;
}

/* Staffeln aufsteigend, Sendungen ohne Staffel zum Schluss */
static int compare_seasons(gconstpointer a, gconstpointer b)
{
    int x = (*(Season *const *)a)->number, y = (*(Season *const *)b)->number;
    if (x < 0 || y < 0) return y - x;
    return x - y;
}

static int compare_groups(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate((*(Group *const *)a)->name, (*(Group *const *)b)->name);
}

/* Staffel einer Gruppe holen oder anlegen */
static Season *group_season(Group *group, int number)
{
    for (guint i = 0; i < group->seasons->len; i++) {
        Season *season = group->seasons->pdata[i];
        if (season->number == number) return season;
    }
    Season *season = g_new0(Season, 1);
    season->number = number;
    season->items = g_ptr_array_new_with_free_func(item_free);
    g_ptr_array_add(group->seasons, season);
    return season;
}

/* Trefferliste zum Baum ordnen; übernimmt die Items */
static void build_groups(GPtrArray *items)
{
    if (ui.groups) g_ptr_array_free(ui.groups, TRUE);
    ui.groups = g_ptr_array_new_with_free_func(group_free);
    GHashTable *by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < items->len; i++) {
        Item *item = items->pdata[i];
        const ZdTitle *t = item->title;
        const char *name = t->is_film ? t->name : t->topic;
        char *key = g_strconcat(t->is_film ? "F:" : "S:", name, NULL);
        Group *group = g_hash_table_lookup(by_name, key);
        if (!group) {
            group = g_new0(Group, 1);
            group->name = g_strdup(name);
            group->is_film = t->is_film;
            group->seasons = g_ptr_array_new_with_free_func(season_free);
            g_ptr_array_add(ui.groups, group);
            g_hash_table_insert(by_name, g_strdup(key), group);
        }
        g_free(key);
        g_ptr_array_add(group_season(group, t->season)->items, item);
    }
    g_hash_table_destroy(by_name);
    g_ptr_array_sort(ui.groups, compare_groups);
    for (guint i = 0; i < ui.groups->len; i++) {
        Group *group = ui.groups->pdata[i];
        g_ptr_array_sort(group->seasons, compare_seasons);
        for (guint n = 0; n < group->seasons->len; n++) {
            Season *season = group->seasons->pdata[n];
            g_ptr_array_sort(season->items, compare_items);
            season->expanded = group->seasons->len == 1;
        }
        group->expanded = ui.groups->len == 1;
    }
    ui.rows_limit = RESULT_ROWS_BATCH;
}

/* Marke zum Stand eines Treffers */
static GtkWidget *status_badge(ZdStatus status)
{
    switch (status) {
    case ZD_STATUS_DONE: return new_badge("Geladen", "ok");
    case ZD_STATUS_QUEUED: return new_badge("In Warteschlange", NULL);
    case ZD_STATUS_PARTIAL: return new_badge("Angefangen", "warn");
    case ZD_STATUS_GONE: return new_badge("Datei fehlt", "err");
    default: return NULL;
    }
}

/* Marke zum Stand einer Staffel oder Serie */
static GtkWidget *counts_badge(const Counts *c)
{
    if (c->done == c->total) return new_badge("Alles geladen", "ok");
    if (c->done + c->queued == c->total) return new_badge("In Warteschlange", NULL);
    if (!c->done) return NULL;
    char *text = g_strdup_printf("%d von %d geladen", c->done, c->total);
    GtkWidget *badge = new_badge(text, "warn");
    g_free(text);
    return badge;
}

static void ref_free(gpointer data)
{
    g_free(data);
}

/* Häkchen je nach Stand der Treffer setzen */
static void update_checks(void)
{
    ui.updating_checks = true;
    for (guint i = 0; ui.refs && i < ui.refs->len; i++) {
        const RowRef *ref = ui.refs->pdata[i];
        Counts c = {0};
        if (ref->kind == ROW_ITEM) {
            const Item *item = ref->data;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ref->check), item->selected);
            continue;
        }
        if (ref->kind == ROW_GROUP) counts_add_group(&c, ref->data);
        else counts_add_season(&c, ref->data);
        bool all = c.open > 0 && c.selected == c.open;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ref->check), all);
        gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(ref->check), c.selected && !all);
        gtk_widget_set_sensitive(ref->check, c.open > 0);
    }
    ui.updating_checks = false;
    update_selection();
}

/* Häkchen einer Zeile angeklickt. Gruppen wählen nur aus, was noch fehlt. */
static void row_toggled(const RowRef *ref)
{
    if (ref->kind == ROW_ITEM) {
        Item *item = ref->data;
        item->selected = !item->selected;
    } else {
        Counts c = {0};
        if (ref->kind == ROW_GROUP) counts_add_group(&c, ref->data);
        else counts_add_season(&c, ref->data);
        bool select = c.selected < c.open;
        if (ref->kind == ROW_GROUP) group_select(ref->data, select);
        else season_select(ref->data, select);
    }
    update_checks();
}

static void on_check_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b;
    if (!ui.updating_checks) row_toggled(d);
}

/* Klick auf die Zeile: Serie/Staffel auf- und zuklappen, Folge an- oder abwählen */
static void on_result_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d)
{
    (void)list;
    (void)d;
    const RowRef *ref = g_object_get_data(G_OBJECT(row), "ref");
    if (!ref) {                                     /* "Weitere anzeigen" */
        ui.rows_limit += RESULT_ROWS_BATCH;
        rebuild_results();
    } else if (ref->kind == ROW_GROUP) {
        Group *group = ref->data;
        group->expanded = !group->expanded;
        rebuild_results();
    } else if (ref->kind == ROW_SEASON) {
        Season *season = ref->data;
        season->expanded = !season->expanded;
        rebuild_results();
    } else if (item_open(ref->data)) {
        row_toggled(ref);
    }
}

/* Vorderteil einer Zeile: Pfeil (nur Serie/Staffel) und Häkchen */
static GtkWidget *row_prefix(RowRef *ref, bool expandable, bool expanded)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (expandable)
        gtk_box_pack_start(GTK_BOX(box),
                           gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic"
                                                                 : "pan-end-symbolic",
                                                        GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    ref->check = gtk_check_button_new();
    gtk_widget_set_valign(ref->check, GTK_ALIGN_CENTER);
    g_signal_connect(ref->check, "toggled", G_CALLBACK(on_check_toggled), ref);
    gtk_box_pack_start(GTK_BOX(box), ref->check, FALSE, FALSE, 0);
    return box;
}

/* Zeile der Trefferliste anlegen; depth rückt sie ein */
static void add_result_row(RowRef *ref, const char *title, const char *subtitle, GtkWidget *prefix,
                           GtkWidget *suffix, int depth, bool activatable)
{
    GtkWidget *row = add_row(ui.results_list, title, subtitle, prefix, suffix, NULL);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), activatable);
    gtk_widget_set_margin_start(gtk_bin_get_child(GTK_BIN(row)), 14 + depth * 26);
    g_object_set_data(G_OBJECT(row), "ref", ref);
    g_ptr_array_add(ui.refs, ref);
}

/* Beschreibung einer Folge oder eines Films */
static char *item_subtitle(const ZdTitle *t)
{
    char *date = format_date(t->timestamp);
    char *size = zd_format_size((double)zd_title_estimate(t, ui.cfg.quality));
    GString *s = g_string_new(NULL);
    if (t->season >= 0) g_string_append_printf(s, "S%02dE%02d · ", t->season, t->episode);
    g_string_append_printf(s, "%d min · %s · ca. %s", t->duration / 60, date, size);
    if (t->audio->len > 1 || t->subtitles->len) {
        g_string_append(s, " · ");
        for (guint i = 0; i < t->audio->len; i++)
            g_string_append_printf(s, "%s%s", i ? ", " : "",
                                   ((ZdTrack *)t->audio->pdata[i])->label);
        if (t->subtitles->len) g_string_append(s, " · Untertitel");
    }
    g_free(date);
    g_free(size);
    return g_string_free(s, FALSE);
}

/* Zeile eines Treffers */
static void add_item_row(Item *item, int depth)
{
    RowRef *ref = g_new0(RowRef, 1);
    ref->kind = ROW_ITEM;
    ref->data = item;
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *badge = status_badge(item->status);
    if (badge) {
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
    }
    if (item->path) {
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("media-playback-start-symbolic", "Abspielen", item->path,
                                           G_CALLBACK(on_open_file_clicked)), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("folder-open-symbolic", "Ordner öffnen", item->path,
                                           G_CALLBACK(on_open_folder_clicked)), FALSE, FALSE, 0);
    }
    char *subtitle = item_subtitle(item->title);
    GtkWidget *prefix = row_prefix(ref, false, false);
    gtk_widget_set_sensitive(ref->check, item_open(item));
    add_result_row(ref, item->title->name, subtitle, prefix, suffix, depth, item_open(item));
    if (*item->title->description) {
        GtkWidget *row = gtk_widget_get_parent(gtk_widget_get_parent(prefix));
        gtk_widget_set_tooltip_text(row, item->title->description);
    }
    g_free(subtitle);
}

/* Überschriftszeile einer Staffel */
static void add_season_row(Season *season)
{
    Counts c = {0};
    counts_add_season(&c, season);
    RowRef *ref = g_new0(RowRef, 1);
    ref->kind = ROW_SEASON;
    ref->data = season;
    char *title = season->number >= 0 ? g_strdup_printf("Staffel %d", season->number)
                                      : g_strdup("Einzelne Sendungen");
    char *subtitle = g_strdup_printf("%d %s", c.total, plural(c.total, "Folge", "Folgen"));
    add_result_row(ref, title, subtitle, row_prefix(ref, true, season->expanded), counts_badge(&c),
                   1, true);
    g_free(title);
    g_free(subtitle);
}

/* Überschriftszeile einer Serie */
static void add_group_row(Group *group)
{
    Counts c = {0};
    counts_add_group(&c, group);
    RowRef *ref = g_new0(RowRef, 1);
    ref->kind = ROW_GROUP;
    ref->data = group;
    int seasons = 0, seconds = 0;
    for (guint i = 0; i < group->seasons->len; i++) {
        Season *season = group->seasons->pdata[i];
        seasons += season->number >= 0;
        for (guint n = 0; n < season->items->len; n++)
            seconds += ((Item *)season->items->pdata[n])->title->duration;
    }
    char *time_text = zd_format_time(seconds);
    char *subtitle;
    if (seasons)
        subtitle = g_strdup_printf("%d %s · %d %s · %s", c.total,
                                   plural(c.total, "Folge", "Folgen"), seasons,
                                   plural(seasons, "Staffel", "Staffeln"), time_text);
    else
        subtitle = g_strdup_printf("%d %s · %s", c.total,
                                   plural(c.total, "Sendung", "Sendungen"), time_text);
    add_result_row(ref, group->name, subtitle, row_prefix(ref, true, group->expanded),
                   counts_badge(&c), 0, true);
    g_free(time_text);
    g_free(subtitle);
}

/* Zeile ohne Häkchen: Hinweis oder "Weitere anzeigen" */
static void add_note_row(const char *text, bool activatable)
{
    GtkWidget *label = new_label(text, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_widget_set_margin_top(label, 16);
    gtk_widget_set_margin_bottom(label, 16);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), activatable);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_container_add(GTK_CONTAINER(ui.results_list), row);
}

/* Liste neu aufbauen (aufgeklappte Ebenen, Filter, Stand) */
static void rebuild_results(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(ui.results_list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);
    if (ui.refs) g_ptr_array_set_size(ui.refs, 0);
    else ui.refs = g_ptr_array_new_with_free_func(ref_free);

    int rows = 0;
    bool more = false;
    for (guint g = 0; ui.groups && g < ui.groups->len && !more; g++) {
        Group *group = ui.groups->pdata[g];
        Counts c = {0};
        counts_add_group(&c, group);
        if (!c.visible) continue;
        if (rows >= ui.rows_limit) {
            more = true;
            break;
        }
        rows++;
        if (group->is_film && c.total == 1) {
            add_item_row(((Season *)group->seasons->pdata[0])->items->pdata[0], 0);
            continue;
        }
        add_group_row(group);
        if (!group->expanded) continue;
        bool flat = group->seasons->len == 1 && ((Season *)group->seasons->pdata[0])->number < 0;
        for (guint s = 0; s < group->seasons->len && !more; s++) {
            Season *season = group->seasons->pdata[s];
            Counts sc = {0};
            counts_add_season(&sc, season);
            if (!sc.visible) continue;
            if (!flat) {
                add_season_row(season);
                rows++;
            }
            if (!flat && !season->expanded) continue;
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                if (!item_visible(item)) continue;
                if (rows >= ui.rows_limit) {
                    more = true;
                    break;
                }
                add_item_row(item, flat ? 1 : 2);
                rows++;
            }
        }
    }
    if (more) add_note_row("Weitere anzeigen …", true);
    else if (!rows) add_note_row(ui.has_searched ? "Keine passenden Einträge."
                                 : "Suchbegriff eingeben oder gleich auf „Suchen“ klicken.", false);
    gtk_widget_show_all(ui.results_list);
    update_checks();
}

/* Stand aller Treffer neu bestimmen, z. B. nach einem fertigen Download */
static void refresh_statuses(void)
{
    bool changed = false;
    for (guint g = 0; ui.groups && g < ui.groups->len; g++) {
        Group *group = ui.groups->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                char *path = NULL;
                ZdStatus status = zd_title_status(item->title, ui.cfg.dir, &path);
                changed |= status != item->status || g_strcmp0(path, item->path);
                item->status = status;
                g_free(item->path);
                item->path = path;
                if (!item_open(item)) item->selected = false;
            }
        }
    }
    if (changed) rebuild_results();
}

/* alle gewählten Treffer in Baumreihenfolge */
static GPtrArray *collect_selected(void)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint g = 0; ui.groups && g < ui.groups->len; g++) {
        Group *group = ui.groups->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                if (item->selected) g_ptr_array_add(out, item);
            }
        }
    }
    return out;
}

/* Fußzeile: Anzahl, geschätzte Größe, Ziel */
static void update_selection(void)
{
    GPtrArray *selected = collect_selected();
    gint64 bytes = 0;
    for (guint i = 0; i < selected->len; i++)
        bytes += zd_title_estimate(((Item *)selected->pdata[i])->title, ui.cfg.quality);
    if (selected->len) {
        char *size = zd_format_size((double)bytes);
        char *text = g_strdup_printf("%u ausgewählt · ca. %s", selected->len, size);
        set_label_text(ui.selection_label, text);
        g_free(text);
        g_free(size);
    } else {
        set_label_text(ui.selection_label, "Nichts ausgewählt");
    }
    gtk_widget_set_sensitive(ui.download_button, selected->len > 0);
    char *display = g_filename_display_name(ui.cfg.dir);
    char *target = g_strdup_printf("Ziel: %s%s", display,
                                   ui.cfg.convert ? " · danach AV1" : "");
    set_label_text(ui.target_label, target);
    g_free(display);
    g_free(target);
    g_ptr_array_free(selected, TRUE);
}

/* ---------------------------------------------------------------------------
 * Spurenauswahl
 * ------------------------------------------------------------------------- */
/* Bezeichnungen aller Spuren der Auswahl, ohne Doppelte */
static void add_labels(GPtrArray *labels, GPtrArray *tracks)
{
    for (guint i = 0; i < tracks->len; i++) {
        const char *label = ((ZdTrack *)tracks->pdata[i])->label;
        bool known = false;
        for (guint n = 0; n < labels->len; n++) known |= !strcmp(labels->pdata[n], label);
        if (!known) g_ptr_array_add(labels, (char *)label);
    }
}

/* Häkchen je Bezeichnung in die Liste; alles ist vorgewählt */
static GPtrArray *add_track_checks(GtkWidget *box, const char *heading, GPtrArray *labels)
{
    GPtrArray *checks = g_ptr_array_new();
    GtkWidget *list = add_group(box, heading);
    for (guint i = 0; i < labels->len; i++) {
        GtkWidget *check = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
        add_row(list, labels->pdata[i], NULL, check, NULL, NULL);
        g_ptr_array_add(checks, check);
    }
    return checks;
}

/* mindestens eine Tonspur muss gewählt bleiben */
static void on_track_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b;
    GPtrArray *audio = d;
    bool any = false;
    for (guint i = 0; i < audio->len; i++)
        any |= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(audio->pdata[i]));
    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(audio->pdata[0]), "ok"), any);
}

/* abgewählte Bezeichnungen als NULL-terminierte Liste */
static char **unchecked_labels(GPtrArray *labels, GPtrArray *checks)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < labels->len; i++)
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks->pdata[i])))
            g_ptr_array_add(out, g_strdup(labels->pdata[i]));
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

/* Dialog "Sprachen und Untertitel". Standard: alles laden. -> false = abgebrochen */
static bool ask_tracks(GPtrArray *selected, char ***skip_audio, char ***skip_subtitles)
{
    GPtrArray *audio = g_ptr_array_new(), *subtitles = g_ptr_array_new();
    for (guint i = 0; i < selected->len; i++) {
        const ZdTitle *t = ((Item *)selected->pdata[i])->title;
        add_labels(audio, t->audio);
        add_labels(subtitles, t->subtitles);
    }
    bool ok = true;
    if (audio->len > 1 || subtitles->len) {
        GtkWidget *d = gtk_dialog_new_with_buttons("Sprachen und Untertitel", GTK_WINDOW(ui.win),
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                   "Abbrechen", GTK_RESPONSE_CANCEL, NULL);
        GtkWidget *go = gtk_dialog_add_button(GTK_DIALOG(d), "Herunterladen", GTK_RESPONSE_OK);
        add_css_class(go, "suggested-action");
        gtk_window_set_default_size(GTK_WINDOW(d), 440, -1);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(box, 18);
        gtk_widget_set_margin_end(box, 18);
        gtk_widget_set_margin_bottom(box, 18);
        char *hint = g_strdup_printf("Gilt für %u %s. Eine Folge, die eine gewählte Spur nicht "
                                     "hat, wird ohne sie geladen.", selected->len,
                                     plural((int)selected->len, "Titel", "Titel"));
        gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label(hint, "zd-sub"), 50), FALSE, FALSE,
                           6);
        g_free(hint);
        GPtrArray *audio_checks = add_track_checks(box, "Sprachausgaben", audio);
        GPtrArray *subtitle_checks = subtitles->len
                                     ? add_track_checks(box, "Untertitel", subtitles)
                                     : g_ptr_array_new();
        for (guint i = 0; i < audio_checks->len; i++)
            g_signal_connect(audio_checks->pdata[i], "toggled", G_CALLBACK(on_track_toggled),
                             audio_checks);
        g_object_set_data(G_OBJECT(audio_checks->pdata[0]), "ok", go);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
        gtk_widget_show_all(d);
        ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
        if (ok) {
            *skip_audio = unchecked_labels(audio, audio_checks);
            *skip_subtitles = unchecked_labels(subtitles, subtitle_checks);
        }
        gtk_widget_destroy(d);
        g_ptr_array_free(audio_checks, TRUE);
        g_ptr_array_free(subtitle_checks, TRUE);
    }
    g_ptr_array_free(audio, TRUE);
    g_ptr_array_free(subtitles, TRUE);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Seite Suche
 * ------------------------------------------------------------------------- */
static void search_job_free(SearchJob *job)
{
    g_free(job->text);
    g_free(job->dir);
    g_free(job->error);
    if (job->items) g_ptr_array_free(job->items, TRUE);
    g_free(job);
}

/* Ergebnis im Hauptthread übernehmen */
static gboolean on_search_done(gpointer data)
{
    SearchJob *job = data;
    if (job != ui.search_job) {                     /* von einer neueren Suche überholt */
        search_job_free(job);
        return G_SOURCE_REMOVE;
    }
    ui.search_job = NULL;
    gtk_spinner_stop(GTK_SPINNER(ui.search_spinner));
    gtk_widget_hide(ui.search_spinner);
    gtk_button_set_label(GTK_BUTTON(ui.search_button), "Suchen");
    if (job->error) {
        show_info_bar(GTK_MESSAGE_ERROR, job->error);
        set_label_text(ui.search_note, "");
    } else if (job->items) {
        ui.has_searched = true;
        guint count = job->items->len;
        GPtrArray *items = job->items;
        job->items = NULL;
        build_groups(items);
        g_ptr_array_set_free_func(items, NULL);     /* gehören jetzt den Staffeln */
        g_ptr_array_free(items, TRUE);
        char *note = g_strdup_printf("%u %s%s", count, plural((int)count, "Treffer", "Treffer"),
                                     job->truncated ? " – Liste gekürzt, bitte genauer suchen"
                                                    : "");
        set_label_text(ui.search_note, note);
        g_free(note);
        rebuild_results();
    }
    search_job_free(job);
    return G_SOURCE_REMOVE;
}

/* Suche im Hintergrund; der Stand je Treffer braucht Plattenzugriffe und läuft hier mit */
static gpointer search_thread(gpointer data)
{
    SearchJob *job = data;
    GPtrArray *titles = zd_search(&job->query, &job->cancel, &job->truncated, &job->error);
    if (titles) {
        job->items = g_ptr_array_new_with_free_func(item_free);
        for (guint i = 0; i < titles->len; i++) {
            Item *item = g_new0(Item, 1);
            item->title = titles->pdata[i];
            item->status = zd_title_status(item->title, job->dir, &item->path);
            g_ptr_array_add(job->items, item);
        }
        g_ptr_array_set_free_func(titles, NULL);    /* gehören jetzt den Items */
        g_ptr_array_free(titles, TRUE);
    }
    g_idle_add(on_search_done, job);
    return NULL;
}

static void start_search(void)
{
    if (ui.search_job) {                            /* zweiter Klick: abbrechen */
        g_atomic_int_set(&ui.search_job->cancel, 1);
        ui.search_job = NULL;
        gtk_spinner_stop(GTK_SPINNER(ui.search_spinner));
        gtk_widget_hide(ui.search_spinner);
        gtk_button_set_label(GTK_BUTTON(ui.search_button), "Suchen");
        return;
    }
    SearchJob *job = g_new0(SearchJob, 1);
    job->text = g_strdup(gtk_entry_get_text(GTK_ENTRY(ui.search_entry)));
    g_strstrip(job->text);
    job->dir = g_strdup(ui.cfg.dir);
    job->query.text = job->text;
    job->query.kind = (ZdKind)gtk_combo_box_get_active(GTK_COMBO_BOX(ui.kind_combo));
    job->query.min_minutes = ui.cfg.min_minutes;
    job->query.hide_variants = !ui.cfg.show_signed;
    ui.search_job = job;
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    gtk_widget_show(ui.search_spinner);
    gtk_spinner_start(GTK_SPINNER(ui.search_spinner));
    gtk_button_set_label(GTK_BUTTON(ui.search_button), "Abbrechen");
    set_label_text(ui.search_note, "");
    g_thread_unref(g_thread_new("suche", search_thread, job));
}

static void on_search_clicked(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    start_search();
}

static void on_filter_changed(GtkComboBox *c, gpointer d)
{
    (void)c;
    (void)d;
    /* Unsichtbares darf nicht heimlich mitgeladen werden */
    GPtrArray *selected = collect_selected();
    for (guint i = 0; i < selected->len; i++) {
        Item *item = selected->pdata[i];
        if (!item_visible(item)) item->selected = false;
    }
    g_ptr_array_free(selected, TRUE);
    ui.rows_limit = RESULT_ROWS_BATCH;
    rebuild_results();
}

/* "Alle fehlenden auswählen": was noch nicht da ist, in einem Schritt */
static void on_select_missing_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GPtrArray *selected = collect_selected();
    bool any = selected->len > 0;
    g_ptr_array_free(selected, TRUE);
    for (guint g = 0; ui.groups && g < ui.groups->len; g++) group_select(ui.groups->pdata[g], !any);
    update_checks();
}

/* Auswahl in die Warteschlange stellen */
static void on_download_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GPtrArray *selected = collect_selected();
    char **skip_audio = NULL, **skip_subtitles = NULL;
    if (!selected->len || !ask_tracks(selected, &skip_audio, &skip_subtitles)) {
        g_ptr_array_free(selected, TRUE);
        return;
    }
    ZdJobOptions options = {.dir = ui.cfg.dir, .quality = (ZdQuality)ui.cfg.quality,
                            .convert = ui.cfg.convert, .encoder = (ZdEncoder)ui.cfg.encoder,
                            .gpu_quality = ui.cfg.gpu_quality, .cpu_rf = ui.cfg.cpu_rf,
                            .keep_original = ui.cfg.keep_original,
                            .skip_audio = (const char *const *)skip_audio,
                            .skip_subtitles = (const char *const *)skip_subtitles};
    for (guint i = 0; i < selected->len; i++) {
        Item *item = selected->pdata[i];
        zd_queue_add(item->title, &options);
        item->selected = false;
    }
    g_strfreev(skip_audio);
    g_strfreev(skip_subtitles);
    g_ptr_array_free(selected, TRUE);
    refresh_statuses();
    update_checks();
    gtk_stack_set_visible_child(GTK_STACK(ui.stack), ui.downloads_page);
}

static GtkWidget *build_search_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *top = new_column();
    gtk_widget_set_margin_top(top, 14);
    gtk_widget_set_margin_bottom(top, 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.search_entry), "Film oder Serie suchen …");
    gtk_widget_set_hexpand(ui.search_entry, TRUE);
    g_signal_connect(ui.search_entry, "activate", G_CALLBACK(on_search_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), ui.search_entry, TRUE, TRUE, 0);
    ui.kind_combo = new_combo(KIND_TEXT, 3, ZD_KIND_ALL);
    gtk_box_pack_start(GTK_BOX(bar), ui.kind_combo, FALSE, FALSE, 0);
    ui.search_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(bar), ui.search_spinner, FALSE, FALSE, 0);
    ui.search_button = gtk_button_new_with_label("Suchen");
    add_css_class(ui.search_button, "suggested-action");
    g_signal_connect(ui.search_button, "clicked", G_CALLBACK(on_search_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), ui.search_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), bar, FALSE, FALSE, 0);

    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.search_note = new_label("", "zd-sub");
    add_css_class(ui.search_note, "dim-label");
    gtk_box_pack_start(GTK_BOX(line), ui.search_note, TRUE, TRUE, 0);
    ui.filter_combo = new_combo(FILTER_TEXT, 3, FILTER_ALL);
    gtk_widget_set_tooltip_text(ui.filter_combo, "Zeigt, was schon geladen ist und was noch fehlt");
    g_signal_connect(ui.filter_combo, "changed", G_CALLBACK(on_filter_changed), NULL);
    gtk_box_pack_end(GTK_BOX(line), ui.filter_combo, FALSE, FALSE, 0);
    ui.select_missing_button = gtk_button_new_with_label("Alle fehlenden an/aus");
    g_signal_connect(ui.select_missing_button, "clicked", G_CALLBACK(on_select_missing_clicked),
                     NULL);
    gtk_box_pack_end(GTK_BOX(line), ui.select_missing_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), line, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *scrolled, *column = new_page_column(&scrolled);
    gtk_widget_set_margin_top(column, 4);
    ui.results_list = add_group(column, NULL);
    g_signal_connect(ui.results_list, "row-activated", G_CALLBACK(on_result_row_activated), NULL);
    gtk_box_pack_start(GTK_BOX(page), scrolled, TRUE, TRUE, 0);

    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    add_css_class(footer, "zd-footer");
    GtkWidget *text = new_column();
    gtk_widget_set_margin_end(text, 0);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    ui.selection_label = new_label("", "zd-title");
    ui.target_label = new_label("", "zd-sub");
    add_css_class(ui.target_label, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(ui.target_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(text), ui.selection_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), ui.target_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), text, TRUE, TRUE, 0);
    ui.download_button = new_pill_button("Herunterladen", "suggested-action");
    gtk_widget_set_margin_end(ui.download_button, 24);
    gtk_widget_set_valign(ui.download_button, GTK_ALIGN_CENTER);
    g_signal_connect(ui.download_button, "clicked", G_CALLBACK(on_download_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(footer), ui.download_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), footer, FALSE, FALSE, 0);
    return page;
}

/* ---------------------------------------------------------------------------
 * Seite Downloads
 * ------------------------------------------------------------------------- */
static void job_row_free(gpointer data)
{
    JobRow *row = data;
    g_free(row->target);
    g_free(row);
}

static void on_job_pause_clicked(GtkButton *b, gpointer d)
{
    JobRow *row = d;
    bool paused = g_object_get_data(G_OBJECT(b), "paused") != NULL;
    zd_job_set_paused(row->id, !paused);
}

static void on_job_retry_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    zd_job_retry(((JobRow *)d)->id);
}

static void on_job_remove_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    JobRow *row = d;
    if (g_object_get_data(G_OBJECT(row->row), "open") &&
        !ask_confirmation("Download abbrechen?", "Der bisher geladene Teil wird gelöscht.",
                          "Behalten", "Abbrechen und löschen", true))
        return;
    zd_job_remove(row->id);
}

static void on_job_open_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    open_folder_of(((JobRow *)d)->target);
}

/* Zeile der Warteschlange anlegen */
static JobRow *new_job_row(const ZdJobView *j)
{
    JobRow *row = g_new0(JobRow, 1);
    row->id = j->id;
    row->target = g_strdup(j->target);
    row->row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row->row), FALSE);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(v, 10);
    gtk_widget_set_margin_bottom(v, 10);
    gtk_widget_set_margin_start(v, 14);
    gtk_widget_set_margin_end(v, 12);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    row->title = new_label(j->title, "zd-title");
    gtk_label_set_ellipsize(GTK_LABEL(row->title), PANGO_ELLIPSIZE_END);
    row->subtitle = new_label("", "zd-sub");
    add_css_class(row->subtitle, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(row->subtitle), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(text), row->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), row->subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), text, TRUE, TRUE, 0);
    row->pause = new_icon_button("media-playback-pause-symbolic", "Pausieren");
    row->retry = new_icon_button("view-refresh-symbolic", "Erneut versuchen");
    row->open = new_icon_button("folder-open-symbolic", "Ordner öffnen");
    row->remove = new_icon_button("window-close-symbolic", "Aus der Liste entfernen");
    g_signal_connect(row->pause, "clicked", G_CALLBACK(on_job_pause_clicked), row);
    g_signal_connect(row->retry, "clicked", G_CALLBACK(on_job_retry_clicked), row);
    g_signal_connect(row->open, "clicked", G_CALLBACK(on_job_open_clicked), row);
    g_signal_connect(row->remove, "clicked", G_CALLBACK(on_job_remove_clicked), row);
    GtkWidget *buttons[] = {row->pause, row->retry, row->open, row->remove};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
        gtk_widget_set_valign(buttons[i], GTK_ALIGN_CENTER);
        gtk_widget_set_no_show_all(buttons[i], TRUE);
        gtk_box_pack_start(GTK_BOX(h), buttons[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    row->bar = gtk_progress_bar_new();
    gtk_widget_set_no_show_all(row->bar, TRUE);
    gtk_box_pack_start(GTK_BOX(v), row->bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row->row), v);
    gtk_container_add(GTK_CONTAINER(ui.jobs_list), row->row);
    gtk_widget_show_all(row->row);
    return row;
}

/* Statuszeile eines Auftrags */
static char *job_status_text(const ZdJobView *j)
{
    char *have = zd_format_size((double)j->have), *total = zd_format_size((double)j->total);
    char *speed = zd_format_size(j->speed);
    char *rest = j->remaining >= 0 ? zd_format_time(j->remaining) : g_strdup("?");
    char *text = NULL;
    switch (j->state) {
    case ZD_JOB_WAITING:
        if (j->total > 0 && j->paused)
            text = g_strdup_printf("Pausiert · %s von %s", have, total);
        else text = g_strdup(j->paused ? "Pausiert" : "Wartet");
        break;
    case ZD_JOB_DOWNLOADING:
        if (j->paused) text = g_strdup("Wird pausiert …");
        else if (j->total > 0)
            text = g_strdup_printf("%s von %s · %s/s · noch %s", have, total, speed, rest);
        else text = g_strdup("Verbindet …");
        break;
    case ZD_JOB_CONVERT_WAITING:
        text = g_strdup(j->paused ? "Download fertig · Weiterverarbeitung pausiert"
                                  : "Download fertig · wartet auf die Weiterverarbeitung");
        break;
    case ZD_JOB_CONVERTING:
        text = g_strdup_printf("%s · %.0f %% · noch %s%s",
                               j->merging ? "Spuren zusammenführen" : "Umwandlung nach AV1",
                               j->progress * 100, rest, j->paused ? " · pausiert" : "");
        break;
    case ZD_JOB_DONE:
        text = g_strdup(j->message ? j->message : "Fertig");
        break;
    case ZD_JOB_FAILED:
        text = g_strdup(j->message ? j->message : "Fehlgeschlagen");
        break;
    }
    g_free(have);
    g_free(total);
    g_free(speed);
    g_free(rest);
    return text;
}

/* Zeile auf den aktuellen Stand bringen */
static void job_row_update(JobRow *row, const ZdJobView *j)
{
    bool done = j->state == ZD_JOB_DONE, failed = j->state == ZD_JOB_FAILED;
    bool open = !done && !failed;
    char *status = job_status_text(j);
    set_label_text(row->subtitle, status);
    g_free(status);
    g_object_set_data(G_OBJECT(row->row), "open", GINT_TO_POINTER(open));

    gtk_widget_set_visible(row->bar, open);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(row->bar), CLAMP(j->progress, 0, 1));
    gtk_widget_set_visible(row->pause, open);
    gtk_widget_set_visible(row->retry, failed);
    gtk_widget_set_visible(row->open, done);
    gtk_widget_set_visible(row->remove, true);
    gtk_widget_set_tooltip_text(row->remove, open ? "Abbrechen" : "Aus der Liste entfernen");
    gtk_widget_set_tooltip_text(row->pause, j->paused ? "Fortsetzen" : "Pausieren");
    gtk_button_set_image(GTK_BUTTON(row->pause),
                         gtk_image_new_from_icon_name(j->paused ? "media-playback-start-symbolic"
                                                                : "media-playback-pause-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    g_object_set_data(G_OBJECT(row->pause), "paused", j->paused ? row : NULL);
}

/* Kopfzeile: was läuft gerade? */
static void update_downloads_hero(const ZdJobView *jobs, int count)
{
    int active = 0, open = 0, paused = 0, failed = 0, done = 0;
    double speed = 0, remaining = 0;
    for (int i = 0; i < count; i++) {
        const ZdJobView *j = &jobs[i];
        done += j->state == ZD_JOB_DONE;
        failed += j->state == ZD_JOB_FAILED;
        if (j->state == ZD_JOB_DONE || j->state == ZD_JOB_FAILED) continue;
        open++;
        if (j->paused) paused++;
        else if (j->state == ZD_JOB_DOWNLOADING || j->state == ZD_JOB_CONVERTING) active++;
        if (j->state == ZD_JOB_DOWNLOADING) {
            speed += j->speed;
            remaining = MAX(remaining, j->remaining);
        }
    }
    char *title;
    if (active)
        title = g_strdup_printf("%d %s aktiv", active, plural(active, "Auftrag", "Aufträge"));
    else if (paused) title = g_strdup_printf("%d pausiert", paused);
    else if (open) title = g_strdup_printf("%d %s", open, plural(open, "wartet", "warten"));
    else title = g_strdup(count ? "Alles erledigt" : "Keine Downloads");
    set_label_text(ui.hero, title);
    g_free(title);

    GString *sub = g_string_new(NULL);
    if (speed > 0) {
        char *text = zd_format_size(speed), *rest = zd_format_time(remaining);
        g_string_append_printf(sub, "%s/s · noch %s · ", text, rest);
        g_free(text);
        g_free(rest);
    }
    g_string_append_printf(sub, "%d offen · %d fertig", open, done);
    if (failed) g_string_append_printf(sub, " · %d fehlgeschlagen", failed);
    set_label_text(ui.hero_sub, sub->str);
    g_string_free(sub, TRUE);

    char *page = open ? g_strdup_printf("Downloads (%d)", open) : g_strdup("Downloads");
    gtk_container_child_set(GTK_CONTAINER(ui.stack), ui.downloads_page, "title", page, NULL);
    g_free(page);

    /* Standby sperren, solange gearbeitet wird */
    if (active && !ui.inhibit_cookie)
        ui.inhibit_cookie = gtk_application_inhibit(ui.app, GTK_WINDOW(ui.win),
                                                    GTK_APPLICATION_INHIBIT_SUSPEND,
                                                    "ZDF-Downloader arbeitet");
    if (!active && ui.inhibit_cookie) {
        gtk_application_uninhibit(ui.app, ui.inhibit_cookie);
        ui.inhibit_cookie = 0;
    }
}

/* Entfernte Aufträge aus der Liste nehmen */
static gboolean job_row_stale(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    JobRow *row = value;
    if (row->seen) {
        row->seen = false;
        return FALSE;
    }
    gtk_widget_destroy(row->row);
    return TRUE;
}

/* Timer: Warteschlange anzeigen; bei fertigen oder neuen Aufträgen auch den Stand der Titel */
static gboolean on_tick(gpointer d)
{
    (void)d;
    int count = 0;
    ZdJobView *jobs = zd_queue_snapshot(&count);
    GString *mark = g_string_new(NULL);
    for (int i = 0; i < count; i++) {
        const ZdJobView *j = &jobs[i];
        JobRow *row = g_hash_table_lookup(ui.job_rows, GUINT_TO_POINTER(j->id));
        if (!row) {
            row = new_job_row(j);
            g_hash_table_insert(ui.job_rows, GUINT_TO_POINTER(j->id), row);
        }
        row->seen = true;
        job_row_update(row, j);
        g_string_append_printf(mark, "%u:%d,", j->id, j->state >= ZD_JOB_DONE ? j->state : 0);
    }
    g_hash_table_foreach_remove(ui.job_rows, job_row_stale, NULL);
    gtk_widget_set_visible(ui.jobs_empty, count == 0);
    update_downloads_hero(jobs, count);
    zd_queue_snapshot_free(jobs, count);
    if (g_strcmp0(mark->str, ui.fingerprint)) {
        g_free(ui.fingerprint);
        ui.fingerprint = g_strdup(mark->str);
        refresh_statuses();
        rebuild_library();
    }
    g_string_free(mark, TRUE);
    return G_SOURCE_CONTINUE;
}

static void on_pause_all_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    zd_queue_set_paused(true);
}

static void on_resume_all_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    zd_queue_set_paused(false);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
}

static void on_clear_done_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    zd_queue_remove_finished();
}

static void on_limit_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    ui.cfg.limit_on = gtk_switch_get_active(GTK_SWITCH(ui.limit_switch));
    ui.cfg.limit_mb = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.limit_spin));
    gtk_widget_set_sensitive(ui.limit_spin, ui.cfg.limit_on);
    apply_queue_settings();
    settings_save();
}

static GtkWidget *build_downloads_page(void)
{
    GtkWidget *scrolled, *page = new_page_column(&scrolled);
    ui.hero = new_label("", "zd-hero");
    ui.hero_sub = new_label("", "zd-lead");
    add_css_class(ui.hero_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(page), ui.hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui.hero_sub, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(buttons, 10);
    GtkWidget *pause = gtk_button_new_with_label("Alle pausieren");
    GtkWidget *resume = gtk_button_new_with_label("Alle fortsetzen");
    GtkWidget *clear = gtk_button_new_with_label("Fertige entfernen");
    g_signal_connect(pause, "clicked", G_CALLBACK(on_pause_all_clicked), NULL);
    g_signal_connect(resume, "clicked", G_CALLBACK(on_resume_all_clicked), NULL);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_done_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), pause, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), resume, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);

    GtkWidget *limit = add_group(page, "Tempo");
    ui.limit_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(ui.limit_switch), ui.cfg.limit_on);
    ui.limit_spin = new_spin(1, 1000, 1, ui.cfg.limit_mb);
    gtk_widget_set_sensitive(ui.limit_spin, ui.cfg.limit_on);
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(suffix), ui.limit_spin, FALSE, FALSE, 0);
    GtkWidget *unit = new_label("MB/s", NULL);
    gtk_box_pack_start(GTK_BOX(suffix), unit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(suffix), ui.limit_switch, FALSE, FALSE, 8);
    gtk_widget_set_valign(ui.limit_switch, GTK_ALIGN_CENTER);
    add_row(limit, "Geschwindigkeit begrenzen", "Gilt für alle Downloads zusammen und sofort.",
            NULL, suffix, NULL);
    g_signal_connect(ui.limit_switch, "notify::active", G_CALLBACK(on_limit_changed), NULL);
    g_signal_connect(ui.limit_spin, "value-changed", G_CALLBACK(on_limit_changed), NULL);

    ui.jobs_list = add_group(page, "Aufträge");
    ui.jobs_empty = new_label("Noch nichts in der Warteschlange. Auf der Seite Suche auswählen "
                              "und „Herunterladen“ klicken.", "dim-label");
    gtk_widget_set_no_show_all(ui.jobs_empty, TRUE);
    set_wrapping(ui.jobs_empty, 70);
    gtk_widget_set_margin_top(ui.jobs_empty, 12);
    gtk_box_pack_start(GTK_BOX(page), ui.jobs_empty, FALSE, FALSE, 0);
    ui.job_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, job_row_free);
    return scrolled;
}

/* ---------------------------------------------------------------------------
 * Seite Bibliothek
 * ------------------------------------------------------------------------- */
/* Verlaufseintrag entfernen, wenn die Datei weg ist */
static void on_forget_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    zd_history_forget(g_object_get_data(G_OBJECT(b), "path"));
    refresh_statuses();
    rebuild_library();
}

/* "Fehlende Folgen": Serie auf der Seite Suche öffnen, gefiltert auf das, was fehlt */
static void on_find_missing_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    gtk_entry_set_text(GTK_ENTRY(ui.search_entry), g_object_get_data(G_OBJECT(b), "path"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.kind_combo), ZD_KIND_ALL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.filter_combo), FILTER_MISSING);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "suche");
    if (ui.search_job) start_search();              /* laufende Suche abbrechen */
    start_search();
}

static void on_library_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d)
{
    (void)list;
    (void)d;
    const char *topic = g_object_get_data(G_OBJECT(row), "topic");
    if (!topic) return;
    if (!g_hash_table_remove(ui.library_open, topic))
        g_hash_table_add(ui.library_open, g_strdup(topic));
    rebuild_library();
}

/* Überschrift einer Sendung */
static void add_library_topic(const char *topic, int count, int missing, gint64 bytes)
{
    bool expanded = g_hash_table_contains(ui.library_open, topic);
    GtkWidget *arrow = gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic"
                                                             : "pan-end-symbolic",
                                                    GTK_ICON_SIZE_MENU);
    char *size = zd_format_size((double)bytes);
    char *subtitle = g_strdup_printf("%d %s · %s", count, plural(count, "Titel", "Titel"), size);
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (missing) {
        char *text = g_strdup_printf("%d %s", missing,
                                     plural(missing, "Datei fehlt", "Dateien fehlen"));
        GtkWidget *badge = new_badge(text, "err");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
        g_free(text);
    }
    if (strcmp(topic, "Filme"))
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("edit-find-symbolic", "Fehlende Folgen suchen", topic,
                                           G_CALLBACK(on_find_missing_clicked)), FALSE, FALSE, 0);
    GtkWidget *row = add_row(ui.library_list, topic, subtitle, arrow, suffix, NULL);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "topic", g_strdup(topic), g_free);
    g_free(size);
    g_free(subtitle);
}

/* Zeile eines geladenen Titels */
static void add_library_entry(const ZdHistoryEntry *e)
{
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (e->exists) {
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("media-playback-start-symbolic", "Abspielen", e->path,
                                           G_CALLBACK(on_open_file_clicked)), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("folder-open-symbolic", "Ordner öffnen", e->path,
                                           G_CALLBACK(on_open_folder_clicked)), FALSE, FALSE, 0);
    } else {
        GtkWidget *badge = new_badge("Datei fehlt", "err");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("edit-clear-symbolic", "Aus dem Verlauf entfernen",
                                           e->path, G_CALLBACK(on_forget_clicked)),
                           FALSE, FALSE, 0);
    }
    char *date = format_date(e->finished), *size = zd_format_size((double)e->size);
    char *subtitle = g_strdup_printf("%s · geladen am %s", size, date);
    GtkWidget *row = add_row(ui.library_list, e->title, subtitle, NULL, suffix, NULL);
    gtk_widget_set_margin_start(gtk_bin_get_child(GTK_BIN(row)), 40);
    gtk_widget_set_tooltip_text(row, e->path);
    g_free(date);
    g_free(size);
    g_free(subtitle);
}

/* Trifft der Filter der Bibliothek auf diesen Eintrag zu? */
static bool library_matches(const ZdHistoryEntry *e, const char *filter)
{
    if (!*filter) return true;
    char *text = g_strconcat(e->topic, " ", e->title, NULL), *lower = g_utf8_strdown(text, -1);
    bool match = strstr(lower, filter) != NULL;
    g_free(text);
    g_free(lower);
    return match;
}

/* Bibliothek aus dem Verlauf neu aufbauen */
static void rebuild_library(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(ui.library_list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);

    int count = 0, shown = 0, gone = 0;
    gint64 total = 0;
    ZdHistoryEntry *entries = zd_history_snapshot(&count);
    char *filter = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(ui.library_search)), -1);
    for (int i = 0; i < count; ) {
        int end = i, matches = 0, missing = 0;
        gint64 bytes = 0;
        while (end < count && !strcmp(entries[end].topic, entries[i].topic)) {
            bool match = library_matches(&entries[end], filter);
            matches += match;
            missing += match && !entries[end].exists;
            bytes += match && entries[end].exists ? entries[end].size : 0;
            end++;
        }
        if (matches) add_library_topic(entries[i].topic, matches, missing, bytes);
        bool expanded = *filter || g_hash_table_contains(ui.library_open, entries[i].topic);
        for (int n = i; n < end && expanded; n++)
            if (library_matches(&entries[n], filter)) add_library_entry(&entries[n]);
        shown += matches;
        gone += missing;
        total += bytes;
        i = end;
    }
    if (!shown) {
        GtkWidget *label = new_label(count ? "Nichts gefunden." : "Noch nichts geladen.",
                                     "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.5);
        gtk_widget_set_margin_top(label, 16);
        gtk_widget_set_margin_bottom(label, 16);
        gtk_container_add(GTK_CONTAINER(ui.library_list), label);
    }
    char *size = zd_format_size((double)total);
    char *hero = g_strdup_printf("%d %s geladen", shown, plural(shown, "Titel", "Titel"));
    char *sub = gone ? g_strdup_printf("%s · bei %d %s fehlt die Datei", size, gone,
                                       plural(gone, "Titel", "Titeln"))
                     : g_strdup_printf("%s auf dem Laufwerk", size);
    set_label_text(ui.library_hero, hero);
    set_label_text(ui.library_sub, sub);
    g_free(size);
    g_free(hero);
    g_free(sub);
    g_free(filter);
    zd_history_snapshot_free(entries, count);
    gtk_widget_show_all(ui.library_list);
}

static void on_library_search_changed(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    (void)d;
    rebuild_library();
}

static GtkWidget *build_library_page(void)
{
    GtkWidget *scrolled, *page = new_page_column(&scrolled);
    ui.library_hero = new_label("", "zd-hero");
    ui.library_sub = new_label("", "zd-lead");
    add_css_class(ui.library_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(page), ui.library_hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui.library_sub, FALSE, FALSE, 0);
    ui.library_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.library_search), "In der Bibliothek suchen …");
    gtk_widget_set_margin_top(ui.library_search, 10);
    g_signal_connect(ui.library_search, "search-changed", G_CALLBACK(on_library_search_changed),
                     NULL);
    gtk_box_pack_start(GTK_BOX(page), ui.library_search, FALSE, FALSE, 0);
    ui.library_list = add_group(page, "Nach Sendung");
    g_signal_connect(ui.library_list, "row-activated", G_CALLBACK(on_library_row_activated), NULL);
    ui.library_open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return scrolled;
}

/* ---------------------------------------------------------------------------
 * Seite Einstellungen
 * ------------------------------------------------------------------------- */
/* Zeilen je nach AV1-Schalter und Encoder ein- oder ausgrauen */
static void update_settings_rows(void)
{
    gtk_widget_set_sensitive(ui.encoder_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.keep_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.gpu_row, ui.cfg.convert && ui.cfg.encoder == ZD_ENCODER_GPU);
    gtk_widget_set_sensitive(ui.cpu_row, ui.cfg.convert && ui.cfg.encoder == ZD_ENCODER_CPU);
}

static void update_dir_label(void)
{
    char *display = g_filename_display_name(ui.cfg.dir);
    set_label_text(ui.dir_sub, display);
    g_free(display);
}

static void on_choose_dir_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Speicherort wählen", GTK_WINDOW(ui.win),
                                                     GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                     "Abbrechen", GTK_RESPONSE_CANCEL,
                                                     "Auswählen", GTK_RESPONSE_ACCEPT, NULL);
    g_mkdir_with_parents(ui.cfg.dir, 0755);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), ui.cfg.dir);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        g_free(ui.cfg.dir);
        ui.cfg.dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        update_dir_label();
        settings_save();
        refresh_statuses();
        update_selection();
    }
    gtk_widget_destroy(chooser);
}

/* gemeinsamer Handler aller übrigen Einstellungen: alles einlesen und speichern */
static GtkWidget *quality_combo, *encoder_combo, *gpu_spin, *cpu_spin, *parallel_spin;
static GtkWidget *convert_spin, *av1_switch, *keep_switch, *signed_switch, *min_spin;

static void on_setting_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    Config *c = &ui.cfg;
    int old_quality = c->quality;
    c->quality = gtk_combo_box_get_active(GTK_COMBO_BOX(quality_combo));
    c->encoder = gtk_combo_box_get_active(GTK_COMBO_BOX(encoder_combo));
    c->gpu_quality = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(gpu_spin));
    c->cpu_rf = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(cpu_spin));
    c->parallel = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(parallel_spin));
    c->conversions = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(convert_spin));
    c->min_minutes = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(min_spin));
    c->convert = gtk_switch_get_active(GTK_SWITCH(av1_switch));
    c->keep_original = gtk_switch_get_active(GTK_SWITCH(keep_switch));
    c->show_signed = gtk_switch_get_active(GTK_SWITCH(signed_switch));
    apply_queue_settings();
    settings_save();
    update_settings_rows();
    if (c->quality != old_quality) rebuild_results();   /* Größen der Treffer */
    update_selection();
}

/* Schalter anlegen und verdrahten */
static GtkWidget *new_setting_switch(bool active)
{
    GtkWidget *s = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(s), active);
    g_signal_connect(s, "notify::active", G_CALLBACK(on_setting_changed), NULL);
    return s;
}

static GtkWidget *new_setting_spin(double lo, double hi, double value)
{
    GtkWidget *s = new_spin(lo, hi, 1, value);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    return s;
}

static GtkWidget *new_setting_combo(const char *const *texts, int count, int active)
{
    GtkWidget *c = new_combo(texts, count, active);
    g_signal_connect(c, "changed", G_CALLBACK(on_setting_changed), NULL);
    return c;
}

static GtkWidget *build_settings_page(void)
{
    const Config *c = &ui.cfg;
    GtkWidget *scrolled, *page = new_page_column(&scrolled);

    GtkWidget *where = add_group(page, "Speichern");
    GtkWidget *choose = gtk_button_new_with_label("Ändern …");
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_dir_clicked), NULL);
    add_row(where, "Speicherort", "", NULL, choose, &ui.dir_sub);
    quality_combo = new_setting_combo(QUALITY_TEXT, 3, c->quality);
    add_row(where, "Qualität", NULL, NULL, quality_combo, NULL);

    GtkWidget *av1 = add_group(page, "AV1");
    av1_switch = new_setting_switch(c->convert);
    add_row(av1, "Nach dem Download nach AV1 umwandeln",
            "Spart meist die Hälfte des Platzes. Das Ergebnis wird geprüft; ist es nicht kleiner, "
            "bleibt die MP4-Datei.", NULL, av1_switch, NULL);
    encoder_combo = new_setting_combo(ENCODER_TEXT, 2, c->encoder);
    ui.encoder_row = add_row(av1, "Encoder", "GPU wird geprüft …", NULL, encoder_combo,
                             &ui.encoder_sub);
    gpu_spin = new_setting_spin(1, 255, c->gpu_quality);
    ui.gpu_row = add_row(av1, "GPU-Qualität", "Kleiner = besser und größer. Standard 64.", NULL,
                         gpu_spin, NULL);
    cpu_spin = new_setting_spin(1, 63, c->cpu_rf);
    ui.cpu_row = add_row(av1, "CPU-Qualität (RF)", "Kleiner = besser und größer. Standard 35.",
                         NULL, cpu_spin, NULL);
    keep_switch = new_setting_switch(c->keep_original);
    ui.keep_row = add_row(av1, "Original behalten",
                          "Sonst wird die MP4-Datei nach geprüfter Umwandlung gelöscht.", NULL,
                          keep_switch, NULL);

    GtkWidget *queue = add_group(page, "Warteschlange");
    parallel_spin = new_setting_spin(1, ZD_MAX_DOWNLOADS, c->parallel);
    add_row(queue, "Gleichzeitige Downloads", NULL, NULL, parallel_spin, NULL);
    convert_spin = new_setting_spin(1, ZD_MAX_CONVERSIONS, c->conversions);
    add_row(queue, "Gleichzeitige Umwandlungen", NULL, NULL, convert_spin, NULL);

    GtkWidget *search = add_group(page, "Suche");
    min_spin = new_setting_spin(0, 300, c->min_minutes);
    add_row(search, "Mindestlänge in Minuten", "Blendet Trailer und kurze Clips aus.", NULL,
            min_spin, NULL);
    signed_switch = new_setting_switch(c->show_signed);
    add_row(search, "Fassungen mit Gebärdensprache anzeigen",
            "Die Einblendung ist fest im Bild, darum sind das eigene Einträge.", NULL,
            signed_switch, NULL);
    return scrolled;
}

/* GPU-Prüfung fertig (Hauptthread) */
static gboolean on_gpu_checked(gpointer d)
{
    ui.gpu_ok = d != NULL;
    set_label_text(ui.encoder_sub, ui.gpu_ok ? "GPU-Encoder gefunden. Empfohlen."
                   : "Kein GPU-Encoder gefunden – die CPU übernimmt, das dauert sehr viel länger.");
    return G_SOURCE_REMOVE;
}

static gpointer gpu_check_thread(gpointer d)
{
    (void)d;
    g_idle_add(on_gpu_checked, zd_gpu_available() ? GINT_TO_POINTER(1) : NULL);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Fenster, Menü, Programmstart
 * ------------------------------------------------------------------------- */
static int page_margin(int window_w)
{
    return CLAMP(window_w * 4 / 100, 16, 120);
}

static gboolean on_idle_layout(gpointer d)
{
    (void)d;
    int w = ui.win ? gtk_widget_get_allocated_width(ui.win) : 0;
    if (w > 1 && w != ui.layout_width) {
        ui.layout_width = w;
        int margin = page_margin(w);
        for (int i = 0; i < ui.page_column_count; i++) {
            gtk_widget_set_margin_start(ui.page_columns[i], margin);
            gtk_widget_set_margin_end(ui.page_columns[i], margin);
        }
        gtk_widget_set_margin_end(ui.download_button, margin);
    }
    return G_SOURCE_REMOVE;
}

static void on_window_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w;
    (void)d;
    if (a->width != ui.layout_width) g_idle_add(on_idle_layout, NULL);
}

/* Fenstergröße merken */
static gboolean on_window_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d)
{
    (void)e;
    (void)d;
    if (!ui.cfg.maximized) gtk_window_get_size(GTK_WINDOW(w), &ui.cfg.width, &ui.cfg.height);
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer d)
{
    (void)w;
    (void)d;
    ui.cfg.maximized = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
}

/* Läuft oder wartet noch etwas? */
static int open_jobs(void)
{
    int count = 0, open = 0;
    ZdJobView *jobs = zd_queue_snapshot(&count);
    for (int i = 0; i < count; i++)
        open += jobs[i].state != ZD_JOB_DONE && jobs[i].state != ZD_JOB_FAILED && !jobs[i].paused;
    zd_queue_snapshot_free(jobs, count);
    return open;
}

/* Fenster schließen: bei laufenden Downloads erst nachfragen */
static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;
    (void)e;
    (void)d;
    if (open_jobs() && !ui.quitting &&
        !ask_confirmation("Downloads laufen noch",
                          "Sie werden pausiert und lassen sich beim nächsten Start an derselben "
                          "Stelle fortsetzen.", "Weiterlaufen lassen", "Pausieren und beenden",
                          false))
        return TRUE;
    settings_save();
    return FALSE;
}

static gboolean on_close_after_signal(gpointer d)
{
    (void)d;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
    return G_SOURCE_REMOVE;
}

/* SIGINT/SIGTERM/SIGHUP (z. B. beim Abmelden): geordnet beenden. Ein offener Dialog hält das
 * Hauptfenster gesperrt und wird darum zuerst geschlossen. */
static gboolean on_unix_signal(gpointer d)
{
    (void)d;
    ui.quitting = true;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next)
        if (GTK_IS_DIALOG(l->data)) gtk_dialog_response(l->data, GTK_RESPONSE_CANCEL);
    g_list_free(windows);
    g_timeout_add(100, on_close_after_signal, NULL);
    return G_SOURCE_CONTINUE;
}

static void on_info_bar_response(GtkInfoBar *info_bar, gint response, gpointer d)
{
    (void)response;
    (void)d;
    gtk_info_bar_set_revealed(info_bar, FALSE);
}

static void on_action_open_log(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (g_file_test(zd_log_path(), G_FILE_TEST_IS_REGULAR)) open_in_default_app(zd_log_path());
    else show_info_bar(GTK_MESSAGE_INFO, "Es gibt noch kein Protokoll.");
}

static void on_action_open_data(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    open_in_default_app(zd_app_dir());
}

static void on_action_about(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    gtk_show_about_dialog(GTK_WINDOW(ui.win), "program-name", APP_NAME, "version", ZD_VERSION,
                          "logo-icon-name", "zdf-downloader", "comments",
                          "Lädt Filme und Serien aus der ZDF-Mediathek für den privaten Gebrauch "
                          "– mit Pause, Tempolimit, allen Sprachen und Untertiteln und auf Wunsch "
                          "als AV1. Suche über MediathekViewWeb.\n"
                          "Einstellungen, Verlauf und Protokoll: ~/.zdf-downloader",
                          NULL);
}

static void on_action_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
}

static void on_startup(GApplication *app, gpointer d)
{
    (void)d;
    /* GTK stellt das Zahlenformat auf die Landessprache um – die Engine braucht Punkte */
    setlocale(LC_NUMERIC, "C");
    static const GActionEntry actions[] = {
        {.name = "open-log", .activate = on_action_open_log},
        {.name = "open-data", .activate = on_action_open_data},
        {.name = "about", .activate = on_action_about},
        {.name = "quit", .activate = on_action_quit},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
    const char *quit_accels[] = {"<Control>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
}

static GtkWidget *build_menu_button(void)
{
    GMenu *menu = g_menu_new(), *about = g_menu_new();
    g_menu_append(menu, "Protokoll öffnen", "app.open-log");
    g_menu_append(menu, "Datenordner öffnen", "app.open-data");
    g_menu_append(about, "Über ZDF-Downloader", "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(about));
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_button_set_image(GTK_BUTTON(menu_button),
                         gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(menu_button, "Menü");
    g_object_unref(menu);
    g_object_unref(about);
    return menu_button;
}

/* Beim Start: Hinweis auf fehlende Programme und liegengebliebene Downloads */
static void show_start_notes(void)
{
    char *missing = NULL;
    if (!zd_have_tools(&missing)) {
        char *text = g_strdup_printf("%s ist nicht installiert – bitte über die "
                                     "Anwendungsverwaltung nachinstallieren.", missing);
        show_info_bar(GTK_MESSAGE_ERROR, text);
        g_free(text);
        g_free(missing);
        return;
    }
    int count = 0, paused = 0;
    ZdJobView *jobs = zd_queue_snapshot(&count);
    for (int i = 0; i < count; i++) paused += jobs[i].paused;
    zd_queue_snapshot_free(jobs, count);
    if (paused) {
        char *text = g_strdup_printf("%d %s vom letzten Mal – auf der Seite Downloads fortsetzen.",
                                     paused, plural(paused, "Download wartet", "Downloads warten"));
        show_info_bar(GTK_MESSAGE_INFO, text);
        g_free(text);
    }
}

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (ui.win) {                                   /* zweiter Start: vorhandenes Fenster zeigen */
        gtk_window_present(GTK_WINDOW(ui.win));
        return;
    }
    settings_load();
    apply_queue_settings();
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    ui.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.win), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(ui.win), ui.cfg.width, ui.cfg.height);
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "zdf-downloader");
    if (ui.cfg.maximized) gtk_window_maximize(GTK_WINDOW(ui.win));
    ui.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui.header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(ui.win), ui.header);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(ui.header), build_menu_button());

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.infobar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(ui.infobar), TRUE);
    g_signal_connect(ui.infobar, "response", G_CALLBACK(on_info_bar_response), NULL);
    ui.info_label = new_label("", NULL);
    set_wrapping(ui.info_label, 90);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ui.infobar))),
                      ui.info_label);
    gtk_box_pack_start(GTK_BOX(vbox), ui.infobar, FALSE, FALSE, 0);
    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(ui.stack), 180);
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_search_page(), "suche", "Suche");
    ui.downloads_page = build_downloads_page();
    gtk_stack_add_titled(GTK_STACK(ui.stack), ui.downloads_page, "downloads", "Downloads");
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_library_page(), "bibliothek", "Bibliothek");
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_settings_page(), "einstellungen",
                         "Einstellungen");
    gtk_box_pack_start(GTK_BOX(vbox), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(ui.stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(ui.header), switcher);

    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(ui.win, "configure-event", G_CALLBACK(on_window_configure), NULL);
    g_signal_connect(ui.win, "window-state-event", G_CALLBACK(on_window_state), NULL);
    g_signal_connect(ui.win, "size-allocate", G_CALLBACK(on_window_size_allocate), NULL);
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.infobar);
    gtk_widget_hide(ui.search_spinner);
    gtk_widget_set_no_show_all(ui.search_spinner, TRUE);
    update_dir_label();
    update_settings_rows();
    rebuild_results();
    rebuild_library();
    show_start_notes();
    on_tick(NULL);
    g_timeout_add(TICK_MS, on_tick, NULL);
    g_thread_unref(g_thread_new("gpu", gpu_check_thread, NULL));
    gtk_widget_grab_focus(ui.search_entry);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    zd_init(true);
    g_set_prgname("zdf-downloader");
    g_set_application_name(APP_NAME);
    ui.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(ui.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(ui.app, "activate", G_CALLBACK(on_activate), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGHUP, on_unix_signal, NULL);
    int rc = g_application_run(G_APPLICATION(ui.app), argc, argv);
    zd_shutdown();
    g_object_unref(ui.app);
    return rc;
}
