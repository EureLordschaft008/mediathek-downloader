/*
 * ZDF-Downloader – Engine
 * =======================
 * Sucht Filme und Serien der ZDF-Mediathek, lädt sie herunter und wandelt sie auf
 * Wunsch nach AV1 um. Bedient wird die Engine über zd_core.h – von der
 * Terminal-Version (zd_cli.c) und der grafischen Oberfläche (zd_gui.c).
 *
 * Quellen und Werkzeuge:
 *   * Suche: API von MediathekViewWeb (Metadaten und direkte MP4-Adressen).
 *   * Download: das Programm curl. "-C -" setzt .part-Dateien fort, "--limit-rate"
 *     begrenzt das Tempo. Pausieren beendet curl – die .part-Datei bleibt, deshalb
 *     überlebt eine Pause auch das Beenden des Programms und einen Neustart des PCs.
 *     Ändert sich das Tempolimit, wird curl mit dem neuen Wert neu gestartet.
 *   * AV1: ffmpeg mit denselben Werten wie der AV1-Converter – GPU per VAAPI
 *     (Qualität 64, 10 Bit), CPU per SVT-AV1 (RF 35). Der Ton wird unverändert
 *     übernommen. Eine pausierte Umwandlung wird per SIGSTOP angehalten.
 *   * Sprachen: Das ZDF liefert jede Sprache als eigene MP4-Datei (…a1a2… Deutsch,
 *     …a3a4… Englisch, …a5a6… Audiodeskription). Die Suche fasst diese Fassungen zu
 *     einem Eintrag zusammen. Das Bild kommt aus der Hauptdatei, jede weitere Sprache
 *     aus der kleinsten Datei – der Ton ist in allen Qualitäten gleich (AAC 128 kbit/s).
 *     ffmpeg legt alles ohne Neukodierung in eine MKV-Datei; Untertitel kommen als
 *     WebVTT und landen als SRT-Spur darin.
 *
 * Sicherheit:
 *   * Fertige Downloads heißen erst nach der Größenprüfung <Name>.mp4.
 *   * AV1-Ergebnisse entstehen in einer versteckten Temp-Datei und werden geprüft
 *     (AV1-Spur, Dauer). Ist das Ergebnis nicht kleiner, bleibt die MP4-Datei.
 *   * Die Warteschlange steht in ~/.zdf-downloader/warteschlange.tsv. Nach dem
 *     Start sind offene Aufträge pausiert und warten auf "Fortsetzen".
 *
 * Aufbau dieser Datei: Konfiguration – kleine Helfer – Protokoll – Prozesse –
 * JSON-Leser – Suche – Pfade – Warteschlange (Speichern, Laden) – Download –
 * Umwandlung – Verteiler – öffentliche Schnittstelle
 *
 * Namenskonventionen: zd_* = öffentliche Schnittstelle (zd_core.h), q.* = globaler
 * Zustand der Warteschlange (geschützt durch q.lock), GROSSBUCHSTABEN = Konstanten.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "zd_core.h"

/* ---------------------------------------------------------------------------
 * Konfiguration
 * ------------------------------------------------------------------------- */
#define API_URL "https://mediathekviewweb.de/api/query"
#define USER_AGENT "zdf-downloader/" ZD_VERSION
#define COVER_PAGE_PREFIX "https://www.zdf.de/"
#define COVER_URL_PREFIX "https://www.zdf.de/assets/"
#define COVER_SIZE "384x216"
#define IMDB_API "https://v3.sg.media-imdb.com/suggestion/x/"
#define PAGE_SIZE 500               /* Einträge je Anfrage */
#define SEARCH_MAX 3000             /* mehr Treffer lädt die Suche nicht */
#define BROWSE_MIN_SECONDS 1200     /* Stöbern ohne Suchbegriff: Mindestlänge für Serien */
#define POLL_MS 200                 /* so oft prüfen die Worker ihren Kindprozess */
#define RATE_TOLERANCE 0.05         /* kleinere Änderungen starten curl nicht neu */
#define MAX_FAILURES 5              /* Verbindungsfehler in Folge ohne Fortschritt */
#define RETRY_WAIT_MS 5000
#define MIN_FREE_SPACE (512 * 1024 * 1024LL)    /* Reserve auf dem Ziellaufwerk */
#define NAME_MAX_BYTES 180
#define ENCODER_NICE 5
#define SVT_PRESET "6"
#define GOP_FRAMES "500"            /* Keyframe alle 10 s bei 50 Bildern/s */
#define QUEUE_HEADER "#zdf-downloader-warteschlange 1"
#define HISTORY_FILE "verlauf.tsv"
#define AUDIO_BYTES_PER_SECOND 16000    /* Zusatzspur: AAC 128 kbit/s */
#define VARIANT_MAX_DIFFERENCE 30       /* Sekunden: so nah muss die Länge der Fassungen sein */

/* Sprachfassungen, die das ZDF am Titelende in Klammern nennt */
static const struct {
    const char *tag, *label, *lang;
} VARIANTS[] = {
    {"(Audiodeskription)", "Audiodeskription", "deu"}, {"(Hörfassung)", "Audiodeskription", "deu"},
    {"(Englisch)", "Englisch", "eng"}, {"(Französisch)", "Französisch", "fra"},
    {"(Spanisch)", "Spanisch", "spa"}, {"(Italienisch)", "Italienisch", "ita"},
    {"(Dänisch)", "Dänisch", "dan"}, {"(Schwedisch)", "Schwedisch", "swe"},
    {"(Norwegisch)", "Norwegisch", "nor"}, {"(Originalfassung)", "Originalfassung", "und"},
    {"(Originalversion)", "Originalfassung", "und"},
};

/* Rubriken, deren Einträge Filme sind (Stöbern ohne Suchbegriff) */
static const char *const FILM_TOPICS[] = {"Filme", "Spielfilme", "Spielfilm-Highlights",
                                          "Der Fernsehfilm der Woche",
                                          "Das kleine Fernsehspiel"};

/* zusätzliche Datei eines Auftrags: weitere Tonspur oder Untertitel */
typedef struct {
    bool subtitle;
    char *label, *lang, *url;
} Extra;

typedef struct {
    guint id;
    char *title, *url, *target;     /* target = <Name>.mp4 */
    char *main_label, *main_lang;   /* Tonspur der Hauptdatei */
    bool skip_main_audio;           /* Ton der Hauptdatei abgewählt */
    bool merging;                   /* Worker führt nur Spuren zusammen, kein AV1 */
    char *key, *topic;              /* für den Verlauf */
    int season, episode;
    GPtrArray *extras;              /* Extra* */
    bool convert, keep_original;
    ZdEncoder encoder;
    int gpu_quality, cpu_rf;

    ZdJobState state;
    bool paused, remove_requested;
    bool worker;                    /* ein Thread arbeitet an diesem Auftrag */
    char *message;
    gint64 have, total;
    double speed, progress, remaining;
} Job;

static struct {
    GMutex lock;
    GCond wake;                     /* Verteiler aufwecken */
    GCond worker_done;
    GPtrArray *jobs;
    guint next_id;
    gint64 rate_limit;              /* Bytes/s für alle Downloads zusammen, 0 = unbegrenzt */
    int max_downloads, max_conversions;
    int workers;
    bool persist, quitting;
    GThread *dispatcher;
    GHashTable *history;            /* Schlüssel -> History* */
} q;

static char *g_app_dir, *g_log_path;
static GMutex g_log_lock;

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
const char *zd_app_dir(void)
{
    return g_app_dir;
}

const char *zd_log_path(void)
{
    return g_log_path;
}

/* Dezimalpunkt durch Komma ersetzen (Anzeige auf Deutsch) */
static char *with_decimal_comma(char *text)
{
    for (char *p = text; *p; p++)
        if (*p == '.') *p = ',';
    return text;
}

char *zd_format_size(double bytes)
{
    if (bytes >= 1e9) return with_decimal_comma(g_strdup_printf("%.1f GB", bytes / 1e9));
    if (bytes >= 1e6) return g_strdup_printf("%.0f MB", bytes / 1e6);
    return g_strdup_printf("%.0f kB", bytes / 1e3);
}

char *zd_format_time(double seconds)
{
    int s = (int)(seconds + 0.5);
    if (s >= 3600) return g_strdup_printf("%d h %02d min", s / 3600, s / 60 % 60);
    if (s >= 60) return g_strdup_printf("%d min", s / 60);
    return g_strdup_printf("%d s", s);
}

/* Größe einer Datei, -1 wenn es sie nicht gibt */
static gint64 file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (gint64)st.st_size : -1;
}

/* Sekunden seit einem festen Zeitpunkt */
static double now_seconds(void)
{
    return g_get_monotonic_time() / 1e6;
}

/* versteckte Datei neben dem Ziel: .<Name><suffix> */
static char *hidden_path(const char *target, const char *suffix)
{
    char *dir = g_path_get_dirname(target), *base = g_path_get_basename(target);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    char *name = g_strdup_printf(".%s%s", base, suffix);
    char *path = g_build_filename(dir, name, NULL);
    g_free(dir);
    g_free(base);
    g_free(name);
    return path;
}

/* Temp-Datei der Umwandlung */
static char *temp_path(const char *target)
{
    return hidden_path(target, ".av1-tmp.mkv");
}

/* Datei der Zusatzspur Nummer index */
static char *extra_path(const char *target, guint index, bool subtitle)
{
    char *suffix = g_strdup_printf(".spur%u.%s", index, subtitle ? "vtt" : "mp4");
    char *path = hidden_path(target, suffix);
    g_free(suffix);
    return path;
}

/* <Name>.mp4 -> <Name>.mkv */
static char *result_path(const char *target)
{
    char *path = g_strdup(target);
    char *dot = strrchr(path, '.');
    if (dot) *dot = 0;
    char *result = g_strconcat(path, ".mkv", NULL);
    g_free(path);
    return result;
}

/* ---------------------------------------------------------------------------
 * Protokoll
 * ------------------------------------------------------------------------- */
void zd_log(const char *format, ...)
{
    if (!g_log_path) return;
    va_list args;
    va_start(args, format);
    char *text = g_strdup_vprintf(format, args);
    va_end(args);
    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    g_mutex_lock(&g_log_lock);
    FILE *f = fopen(g_log_path, "a");
    if (f) {
        fprintf(f, "%s  %s\n", stamp, text);
        fclose(f);
    }
    g_mutex_unlock(&g_log_lock);
    g_free(text);
}

/* ---------------------------------------------------------------------------
 * Prozesse
 * Kindprozesse enden mit dem Thread, der sie gestartet hat (PR_SET_PDEATHSIG),
 * damit nach einem Absturz kein curl oder ffmpeg weiterläuft.
 * ------------------------------------------------------------------------- */
/* im Kindprozess vor exec: data != NULL = Encoder, Priorität senken */
static void child_setup(gpointer data)
{
    signal(SIGPIPE, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (data) {
        setpriority(PRIO_PROCESS, 0, ENCODER_NICE);
        syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, (2 << 13) | 7);
    }
}

/* Kurzes Werkzeug starten und die Ausgabe einsammeln -> Exit-Code, -1 = nicht startbar */
static int run_capture(char **argv, char **out, char **err)
{
    int status = 0;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, out, err, &status, NULL))
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Prozess beenden (auch wenn er angehalten ist) und auf ihn warten */
static void stop_child(pid_t pid)
{
    kill(pid, SIGCONT);
    kill(pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        g_usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ---------------------------------------------------------------------------
 * JSON-Leser
 * Die Antworten der API sind einfach aufgebaut; gelesen wird direkt beim Durchlaufen.
 * Fehlerhafte Eingaben setzen ok = false, die Tiefe ist begrenzt.
 * ------------------------------------------------------------------------- */
#define JSON_MAX_DEPTH 64

typedef struct {
    const char *p;
    bool ok;
} Json;

static void json_spaces(Json *j)
{
    while (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t') j->p++;
}

static bool json_accept(Json *j, char c)
{
    json_spaces(j);
    if (*j->p != c) return false;
    j->p++;
    return true;
}

/* vier Hex-Ziffern von \uXXXX -> Wert, -1 bei Fehler */
static int json_hex4(const char *p)
{
    int value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = g_ascii_xdigit_value(p[i]);
        if (digit < 0) return -1;
        value = value * 16 + digit;
    }
    return value;
}

/* Zeichenkette lesen -> neu angelegter Text in gültigem UTF-8 */
static char *json_string(Json *j)
{
    if (!json_accept(j, '"')) {
        j->ok = false;
        return g_strdup("");
    }
    GString *s = g_string_new(NULL);
    while (*j->p && *j->p != '"') {
        if (*j->p != '\\') {
            g_string_append_c(s, *j->p++);
            continue;
        }
        j->p++;
        char c = *j->p;
        if (!c) break;
        j->p++;
        switch (c) {
        case 'n': g_string_append_c(s, '\n'); break;
        case 't': g_string_append_c(s, '\t'); break;
        case 'r': g_string_append_c(s, '\r'); break;
        case 'b': g_string_append_c(s, '\b'); break;
        case 'f': g_string_append_c(s, '\f'); break;
        case 'u': {
            int code = json_hex4(j->p);
            if (code < 0) {
                j->ok = false;
                break;
            }
            j->p += 4;
            if (code >= 0xD800 && code < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u') {
                int low = json_hex4(j->p + 2);
                if (low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    j->p += 6;
                }
            }
            if (code >= 0xD800 && code < 0xE000) code = 0xFFFD;
            g_string_append_unichar(s, code);
            break;
        }
        default: g_string_append_c(s, c); break;
        }
    }
    if (!json_accept(j, '"')) j->ok = false;
    char *valid = g_utf8_make_valid(s->str, s->len);
    g_string_free(s, TRUE);
    return valid;
}

/* Zahl, true, false oder null überspringen -> Zahlenwert (sonst 0) */
static double json_scalar(Json *j)
{
    json_spaces(j);
    char *end = NULL;
    double value = g_ascii_strtod(j->p, &end);
    if (end != j->p) {
        j->p = end;
        return value;
    }
    for (const char *const *w = (const char *const[]){"true", "false", "null", NULL}; *w; w++) {
        size_t n = strlen(*w);
        if (!strncmp(j->p, *w, n)) {
            j->p += n;
            return 0;
        }
    }
    j->ok = false;
    return 0;
}

/* beliebigen Wert überspringen */
static void json_skip(Json *j, int depth)
{
    json_spaces(j);
    if (depth > JSON_MAX_DEPTH) j->ok = false;
    if (!j->ok) return;
    if (*j->p == '"') {
        g_free(json_string(j));
    } else if (*j->p == '{' || *j->p == '[') {
        bool object = *j->p == '{';
        char close = object ? '}' : ']';
        j->p++;
        if (json_accept(j, close)) return;
        do {
            if (object) {
                g_free(json_string(j));
                if (!json_accept(j, ':')) j->ok = false;
            }
            json_skip(j, depth + 1);
        } while (j->ok && json_accept(j, ','));
        if (!json_accept(j, close)) j->ok = false;
    } else {
        json_scalar(j);
    }
}

/* ---------------------------------------------------------------------------
 * Suche
 * ------------------------------------------------------------------------- */
void zd_title_free(gpointer data)
{
    ZdTitle *t = data;
    if (!t) return;
    g_free(t->channel);
    g_free(t->topic);
    g_free(t->title);
    g_free(t->description);
    g_free(t->website);
    g_free(t->url_high);
    g_free(t->url_medium);
    g_free(t->url_low);
    g_free(t->url_subtitle);
    g_free(t->name);
    if (t->audio) g_ptr_array_free(t->audio, TRUE);
    if (t->subtitles) g_ptr_array_free(t->subtitles, TRUE);
    g_free(t);
}

static void track_free(gpointer data)
{
    ZdTrack *track = data;
    g_free(track->label);
    g_free(track->lang);
    g_free(track->url);
    g_free(track);
}

static ZdTrack *track_new(const char *label, const char *lang, const char *url)
{
    ZdTrack *track = g_new0(ZdTrack, 1);
    track->label = g_strdup(label);
    track->lang = g_strdup(lang);
    track->url = g_strdup(url);
    return track;
}

/* Text für eine JSON-Zeichenkette schützen */
static void json_append_escaped(GString *out, const char *text)
{
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(out, '\\');
        if ((unsigned char)*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
        else g_string_append_c(out, *p);
    }
}

/* Staffel, Folge und bereinigten Namen aus dem Titel ableiten */
static void title_analyse(ZdTitle *t)
{
    static GRegex *episode_tag, *new_episodes;
    static gsize once;
    if (g_once_init_enter(&once)) {
        episode_tag = g_regex_new(" ?\\(S(\\d+)/E(\\d+)\\)", 0, 0, NULL);
        new_episodes = g_regex_new(" - Neue Folgen?\\b", 0, 0, NULL);
        g_once_init_leave(&once, 1);
    }
    t->season = t->episode = -1;
    GMatchInfo *match = NULL;
    if (g_regex_match(episode_tag, t->title, 0, &match)) {
        char *season = g_match_info_fetch(match, 1), *episode = g_match_info_fetch(match, 2);
        t->season = atoi(season);
        t->episode = atoi(episode);
        g_free(season);
        g_free(episode);
    }
    g_match_info_free(match);
    char *without_tag = g_regex_replace_literal(episode_tag, t->title, -1, 0, "", 0, NULL);
    t->name = g_regex_replace_literal(new_episodes, without_tag, -1, 0, "", 0, NULL);
    g_free(without_tag);
    for (size_t i = 0; i < G_N_ELEMENTS(VARIANTS); i++) {
        char *found = strstr(t->name, VARIANTS[i].tag);
        if (!found) continue;
        memmove(found, found + strlen(VARIANTS[i].tag), strlen(found + strlen(VARIANTS[i].tag)) + 1);
        t->track_label = VARIANTS[i].label;
        t->track_lang = VARIANTS[i].lang;
        break;
    }
    g_strstrip(t->name);

    t->is_variant = strstr(t->title, "Gebärdensprache") != NULL;
    char *topic = g_utf8_strdown(t->topic, -1);
    t->is_film = t->season < 0 && (strstr(topic, "film") || strstr(topic, "fernsehspiel")
                                   || strstr(topic, "kino"));
    g_free(topic);
}

/* Ein Objekt aus "results" lesen */
static ZdTitle *json_read_title(Json *j)
{
    ZdTitle *t = g_new0(ZdTitle, 1);
    if (!json_accept(j, '{')) j->ok = false;
    if (j->ok && !json_accept(j, '}')) {
        do {
            char *key = json_string(j);
            if (!json_accept(j, ':')) j->ok = false;
            char **text = NULL;
            if (!strcmp(key, "channel")) text = &t->channel;
            else if (!strcmp(key, "topic")) text = &t->topic;
            else if (!strcmp(key, "title")) text = &t->title;
            else if (!strcmp(key, "description")) text = &t->description;
            else if (!strcmp(key, "url_website")) text = &t->website;
            else if (!strcmp(key, "url_video_hd")) text = &t->url_high;
            else if (!strcmp(key, "url_video")) text = &t->url_medium;
            else if (!strcmp(key, "url_video_low")) text = &t->url_low;
            else if (!strcmp(key, "url_subtitle")) text = &t->url_subtitle;
            json_spaces(j);
            if (text && *j->p == '"') {
                g_free(*text);
                *text = json_string(j);
            } else if (!strcmp(key, "timestamp")) {
                t->timestamp = (gint64)json_scalar(j);
            } else if (!strcmp(key, "duration")) {
                t->duration = (int)json_scalar(j);
            } else {
                json_skip(j, 2);
            }
            g_free(key);
        } while (j->ok && json_accept(j, ','));
        if (!json_accept(j, '}')) j->ok = false;
    }
    char **fields[] = {&t->channel, &t->topic, &t->title, &t->description, &t->website,
                       &t->url_high, &t->url_medium, &t->url_low, &t->url_subtitle};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++)
        if (!*fields[i]) *fields[i] = g_strdup("");
    title_analyse(t);
    return t;
}

/* "results"-Liste lesen und an out anhängen */
static void json_read_results(Json *j, GPtrArray *out)
{
    if (!json_accept(j, '[')) {
        j->ok = false;
        return;
    }
    if (json_accept(j, ']')) return;
    do {
        g_ptr_array_add(out, json_read_title(j));
    } while (j->ok && json_accept(j, ','));
    if (!json_accept(j, ']')) j->ok = false;
}

/* Antwort der API auswerten -> false bei Fehler */
static bool parse_response(const char *text, GPtrArray *out, int *total, char **error)
{
    Json j = {text, true};
    *total = 0;
    if (!json_accept(&j, '{')) j.ok = false;
    while (j.ok) {
        char *key = json_string(&j);
        if (!json_accept(&j, ':')) j.ok = false;
        if (!strcmp(key, "result") && json_accept(&j, '{')) {
            while (j.ok) {
                char *inner = json_string(&j);
                if (!json_accept(&j, ':')) j.ok = false;
                if (!strcmp(inner, "results")) {
                    json_read_results(&j, out);
                } else if (!strcmp(inner, "queryInfo") && json_accept(&j, '{')) {
                    while (j.ok) {
                        char *field = json_string(&j);
                        if (!json_accept(&j, ':')) j.ok = false;
                        if (!strcmp(field, "totalResults")) *total = (int)json_scalar(&j);
                        else json_skip(&j, 3);
                        g_free(field);
                        if (!json_accept(&j, ',')) break;
                    }
                    if (!json_accept(&j, '}')) j.ok = false;
                } else {
                    json_skip(&j, 2);
                }
                g_free(inner);
                if (!json_accept(&j, ',')) break;
            }
            if (!json_accept(&j, '}')) j.ok = false;
        } else if (!strcmp(key, "err")) {
            json_spaces(&j);
            if (*j.p == 'n') {
                json_scalar(&j);
            } else if (j.ok) {
                *error = g_strdup("Die Mediathek-Suche meldet einen Fehler.");
                g_free(key);
                return false;
            }
        } else {
            json_skip(&j, 1);
        }
        g_free(key);
        if (!json_accept(&j, ',')) break;
    }
    if (!j.ok) *error = g_strdup("Die Antwort der Mediathek-Suche ist nicht lesbar.");
    return j.ok;
}

/* Eine Seite der Suche abrufen */
static bool fetch_page(const char *text, const char *field, int min_seconds, int offset,
                       GPtrArray *out, int *total, char **error)
{
    GString *body = g_string_new("{\"queries\":[{\"fields\":[\"channel\"],\"query\":\"zdf\"}");
    if (text && *text) {
        g_string_append_printf(body, ",{\"fields\":[%s],\"query\":\"", field);
        json_append_escaped(body, text);
        g_string_append(body, "\"}");
    }
    g_string_append_printf(body, "],\"sortBy\":\"timestamp\",\"sortOrder\":\"desc\","
                           "\"future\":false,\"offset\":%d,\"size\":%d", offset, PAGE_SIZE);
    if (min_seconds > 0) g_string_append_printf(body, ",\"duration_min\":%d", min_seconds);
    g_string_append_c(body, '}');
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "-H",
                    "Content-Type: text/plain", "--data-binary", body->str, API_URL, NULL};
    char *reply = NULL, *messages = NULL;
    int rc = run_capture(argv, &reply, &messages);
    bool ok = false;
    if (rc == 0 && reply) {
        ok = parse_response(reply, out, total, error);
    } else {
        *error = g_strdup_printf("Die Mediathek-Suche ist nicht erreichbar (%s).",
                                 messages && *messages ? g_strstrip(messages) : "curl-Fehler");
    }
    g_string_free(body, TRUE);
    g_free(reply);
    g_free(messages);
    return ok;
}

/* Alle Seiten einer Anfrage abrufen, begrenzt auf SEARCH_MAX */
static bool fetch_all(const char *text, const char *field, int min_seconds, GPtrArray *out,
                      const volatile gint *cancel, bool *truncated, char **error)
{
    int total = 0;
    for (int offset = 0; offset < SEARCH_MAX; offset += PAGE_SIZE) {
        if (cancel && g_atomic_int_get(cancel)) return true;
        guint before = out->len;
        if (!fetch_page(text, field, min_seconds, offset, out, &total, error)) return false;
        if (out->len == before || offset + PAGE_SIZE >= total) return true;
    }
    if (truncated) *truncated = true;
    return true;
}

/* Eintrag anzeigen? (Filter, die die API nicht kennt) */
static bool title_matches(const ZdTitle *t, const ZdQuery *query)
{
    if (g_ascii_strncasecmp(t->channel, "ZDF", 3)) return false;
    if (!*t->url_high && !*t->url_medium && !*t->url_low) return false;
    if (t->duration < query->min_minutes * 60) return false;
    if (query->hide_variants && t->is_variant) return false;
    if (query->kind == ZD_KIND_FILMS && !t->is_film) return false;
    if (query->kind == ZD_KIND_SERIES && t->season < 0) return false;
    return true;
}

/* Schlüssel, unter dem die Fassungen eines Titels zusammenfinden */
static char *variant_key(const ZdTitle *t)
{
    char *lower = g_utf8_strdown(t->name, -1);
    char *key = g_strdup_printf("%s\n%s\n%d\n%d", t->topic, lower, t->season, t->episode);
    g_free(lower);
    return key;
}

/* Untertitel eines Eintrags aufnehmen (WebVTT liegt neben der XML-Datei) */
static void add_subtitle(ZdTitle *main_title, const ZdTitle *source)
{
    if (!*source->url_subtitle) return;
    const char *label = strstr(source->url_subtitle, "_eng_") ? "Englisch" : "Deutsch";
    const char *lang = strstr(source->url_subtitle, "_eng_") ? "eng" : "deu";
    for (guint i = 0; i < main_title->subtitles->len; i++) {
        const ZdTrack *known = main_title->subtitles->pdata[i];
        if (!strcmp(known->label, label)) return;
    }
    char *url = g_strdup(source->url_subtitle);
    if (g_str_has_suffix(url, ".xml")) strcpy(url + strlen(url) - 4, ".vtt");
    g_ptr_array_add(main_title->subtitles, track_new(label, lang, url));
    g_free(url);
}

/* Sprachfassungen in ihrer Hauptfassung aufgehen lassen; die übernommenen Einträge
 * werden in raw zu NULL. Fehlt die Hauptfassung, bleibt die Fassung ein eigener Eintrag. */
static void merge_variants(GPtrArray *raw)
{
    GHashTable *mains = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < raw->len; i++) {
        ZdTitle *t = raw->pdata[i];
        t->audio = g_ptr_array_new_with_free_func(track_free);
        t->subtitles = g_ptr_array_new_with_free_func(track_free);
        g_ptr_array_add(t->audio, track_new(t->track_label ? t->track_label : "Deutsch",
                                            t->track_lang ? t->track_lang : "deu", ""));
        add_subtitle(t, t);
        if (!t->track_label && !t->is_variant) g_hash_table_insert(mains, variant_key(t), t);
    }
    for (guint i = 0; i < raw->len; i++) {
        ZdTitle *t = raw->pdata[i];
        if (!t->track_label) continue;
        char *key = variant_key(t);
        ZdTitle *main_title = g_hash_table_lookup(mains, key);
        g_free(key);
        if (!main_title || ABS(main_title->duration - t->duration) > VARIANT_MAX_DIFFERENCE)
            continue;
        bool known = false;
        for (guint n = 0; n < main_title->audio->len; n++)
            known |= !strcmp(((ZdTrack *)main_title->audio->pdata[n])->label, t->track_label);
        if (!known)
            g_ptr_array_add(main_title->audio, track_new(t->track_label, t->track_lang,
                                                         zd_title_url(t, ZD_QUALITY_LOW)));
        add_subtitle(main_title, t);
        zd_title_free(t);
        raw->pdata[i] = NULL;
    }
    g_hash_table_destroy(mains);
}

GPtrArray *zd_search(const ZdQuery *query, const volatile gint *cancel, bool *truncated,
                     char **error)
{
    GPtrArray *raw = g_ptr_array_new_with_free_func(zd_title_free);
    const char *text = query->text ? query->text : "";
    const char *both = "\"topic\",\"title\"";
    int min_seconds = query->min_minutes * 60;
    bool ok = true;
    if (truncated) *truncated = false;
    if (!*text && query->kind == ZD_KIND_FILMS) {
        for (size_t i = 0; ok && i < G_N_ELEMENTS(FILM_TOPICS); i++)
            ok = fetch_all(FILM_TOPICS[i], "\"topic\"", min_seconds, raw, cancel, truncated, error);
    } else if (!*text && query->kind == ZD_KIND_SERIES) {
        ok = fetch_all("", both, MAX(min_seconds, BROWSE_MIN_SECONDS), raw, cancel, truncated,
                       error);
    } else {
        ok = fetch_all(text, both, min_seconds, raw, cancel, truncated, error);
    }
    if (!ok) {
        g_ptr_array_free(raw, TRUE);
        return NULL;
    }

    merge_variants(raw);

    /* Filtern; dieselbe Videodatei kann unter mehreren Rubriken stehen */
    GPtrArray *result = g_ptr_array_new_with_free_func(zd_title_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < raw->len; i++) {
        ZdTitle *t = raw->pdata[i];
        if (!t) continue;
        const char *url = zd_title_url(t, ZD_QUALITY_HIGH);
        if (!title_matches(t, query) || !g_hash_table_add(seen, (char *)url)) continue;
        raw->pdata[i] = NULL;
        g_ptr_array_add(result, t);
    }
    g_hash_table_destroy(seen);
    g_ptr_array_free(raw, TRUE);
    return result;
}

const char *zd_title_url(const ZdTitle *t, ZdQuality quality)
{
    const char *order[3][3] = {{t->url_high, t->url_medium, t->url_low},
                               {t->url_medium, t->url_low, t->url_high},
                               {t->url_low, t->url_medium, t->url_high}};
    for (int i = 0; i < 3; i++)
        if (*order[quality][i]) return order[quality][i];
    return "";
}

gint64 zd_title_estimate(const ZdTitle *t, ZdQuality quality)
{
    static const double KBIT_PER_SECOND[] = {6660, 3360, 808};
    return (gint64)(t->duration * KBIT_PER_SECOND[quality] * 1000 / 8);
}

/* ---------------------------------------------------------------------------
 * Pfade
 * ------------------------------------------------------------------------- */
/* Text als Datei- oder Ordnernamen nutzbar machen */
static char *safe_name(const char *text)
{
    GString *s = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (strchr("/\\:*?\"<>|", *p)) g_string_append_c(s, *p == ':' ? ' ' : '-');
        else if ((unsigned char)*p >= 0x20) g_string_append_c(s, *p);
    }
    char *name = g_string_free(s, FALSE);
    g_strstrip(name);
    while (*name == '.') memmove(name, name + 1, strlen(name));
    size_t length = strlen(name);
    while (length && (name[length - 1] == '.' || name[length - 1] == ' ')) name[--length] = 0;
    if (length > NAME_MAX_BYTES) {
        const char *end = g_utf8_find_prev_char(name, name + NAME_MAX_BYTES + 1);
        name[end ? end - name : NAME_MAX_BYTES] = 0;
        g_strchomp(name);
    }
    if (!*name) {
        g_free(name);
        return g_strdup("Unbenannt");
    }
    return name;
}

char *zd_target_path(const ZdTitle *t, const char *dir)
{
    char *topic = safe_name(t->topic), *name = safe_name(t->name), *file, *path;
    if (t->season >= 0) {
        char *season = g_strdup_printf("Staffel %02d", t->season);
        file = g_strdup_printf("%s - S%02dE%02d - %s.mp4", topic, t->season, t->episode, name);
        path = g_build_filename(dir, "Serien", topic, season, file, NULL);
        g_free(season);
    } else if (t->is_film) {
        file = g_strconcat(name, ".mp4", NULL);
        path = g_build_filename(dir, "Filme", file, NULL);
    } else {
        /* Sendungen ohne Folgenangabe heißen oft gleich – das Datum macht sie eindeutig */
        char date[16] = "";
        time_t when = (time_t)t->timestamp;
        struct tm tm;
        localtime_r(&when, &tm);
        strftime(date, sizeof date, "%Y-%m-%d", &tm);
        file = g_strdup_printf("%s - %s.mp4", date, name);
        path = g_build_filename(dir, topic, file, NULL);
    }
    g_free(topic);
    g_free(name);
    g_free(file);
    return path;
}

/* Eindeutiger Name eines Titels, unabhängig von Qualität und Speicherort. Folgen zählen nach
 * Staffel und Folge, weil das ZDF die Titel nachträglich ändert ("- Neue Folgen"). */
static char *title_key(const ZdTitle *t)
{
    char *topic = g_utf8_strdown(t->topic, -1), *name = g_utf8_strdown(t->name, -1), *key;
    if (t->season >= 0) {
        key = g_strdup_printf("%s|S%dE%d", topic, t->season, t->episode);
    } else if (t->is_film) {
        key = g_strdup_printf("film|%s", name);
    } else {
        key = g_strdup_printf("%s|%s|%" G_GINT64_FORMAT, topic, name, t->timestamp / 86400);
    }
    g_free(topic);
    g_free(name);
    return key;
}

/* ---------------------------------------------------------------------------
 * Verlauf
 *
 * Merkt sich jede fertige Datei in ~/.zdf-downloader/verlauf.tsv. So bleibt eine Folge
 * auch dann als geladen erkennbar, wenn der Speicherort inzwischen ein anderer ist.
 * -------------------------------------------------------------------------*/
typedef struct {
    char *key;
    ZdHistoryEntry e;
} History;

static void history_free(gpointer data)
{
    History *h = data;
    g_free(h->key);
    g_free(h->e.topic);
    g_free(h->e.title);
    g_free(h->e.path);
    g_free(h);
}

static char *history_path(void)
{
    return g_build_filename(zd_app_dir(), HISTORY_FILE, NULL);
}

/* Zeile der Verlaufsdatei bauen */
static void history_append_line(GString *out, const History *h)
{
    const char *fields[] = {h->key, h->e.topic, h->e.title, h->e.path};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++) {
        char *escaped = g_strescape(fields[i], NULL);
        g_string_append_printf(out, "%s\t", escaped);
        g_free(escaped);
    }
    g_string_append_printf(out, "%d\t%d\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\n", h->e.season,
                           h->e.episode, h->e.size, h->e.finished);
}

/* Verlauf laden; spätere Zeilen ersetzen frühere mit demselben Schlüssel */
static void history_load(void)
{
    q.history = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, history_free);
    char *path = history_path(), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **f = g_strsplit(lines[i], "\t", -1);
            if (g_strv_length(f) == 8) {
                History *h = g_new0(History, 1);
                h->key = g_strcompress(f[0]);
                h->e.topic = g_strcompress(f[1]);
                h->e.title = g_strcompress(f[2]);
                h->e.path = g_strcompress(f[3]);
                h->e.season = atoi(f[4]);
                h->e.episode = atoi(f[5]);
                h->e.size = g_ascii_strtoll(f[6], NULL, 10);
                h->e.finished = g_ascii_strtoll(f[7], NULL, 10);
                g_hash_table_replace(q.history, h->key, h);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(path);
    g_free(text);
}

/* Fertigen Auftrag eintragen; q.lock wird gehalten */
static void history_add(const Job *j)
{
    char *result = result_path(j->target);
    History *h = g_new0(History, 1);
    h->key = g_strdup(j->key);
    h->e.topic = g_strdup(j->topic);
    h->e.title = g_strdup(j->title);
    h->e.path = g_strdup(file_size(result) > 0 ? result : j->target);
    h->e.season = j->season;
    h->e.episode = j->episode;
    h->e.size = MAX(file_size(h->e.path), 0);
    h->e.finished = g_get_real_time() / G_USEC_PER_SEC;
    g_free(result);

    GString *line = g_string_new(NULL);
    history_append_line(line, h);
    char *path = history_path();
    FILE *file = fopen(path, "a");
    if (file) {
        fputs(line->str, file);
        fclose(file);
    } else {
        zd_log("Verlauf lässt sich nicht schreiben: %s", g_strerror(errno));
    }
    g_free(path);
    g_string_free(line, TRUE);
    g_hash_table_replace(q.history, h->key, h);
}

/* Verlauf komplett neu schreiben; q.lock wird gehalten */
static void history_save(void)
{
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, q.history);
    while (g_hash_table_iter_next(&iter, NULL, &value)) history_append_line(out, value);
    char *path = history_path();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

static int history_compare(gconstpointer a, gconstpointer b)
{
    const ZdHistoryEntry *x = a, *y = b;
    int order = g_utf8_collate(x->topic, y->topic);
    if (!order) order = x->season - y->season;
    if (!order) order = x->episode - y->episode;
    return order ? order : g_utf8_collate(x->title, y->title);
}

ZdHistoryEntry *zd_history_snapshot(int *count)
{
    g_mutex_lock(&q.lock);
    *count = (int)g_hash_table_size(q.history);
    ZdHistoryEntry *entries = g_new0(ZdHistoryEntry, MAX(*count, 1));
    GHashTableIter iter;
    gpointer value;
    int n = 0;
    g_hash_table_iter_init(&iter, q.history);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const History *h = value;
        entries[n] = h->e;
        entries[n].topic = g_strdup(h->e.topic);
        entries[n].title = g_strdup(h->e.title);
        entries[n].path = g_strdup(h->e.path);
        n++;
    }
    g_mutex_unlock(&q.lock);
    /* außerhalb der Sperre: die Platte kann schlafen */
    for (int i = 0; i < n; i++) entries[i].exists = file_size(entries[i].path) >= 0;
    qsort(entries, (size_t)n, sizeof *entries, (int (*)(const void *, const void *))history_compare);
    return entries;
}

void zd_history_snapshot_free(ZdHistoryEntry *entries, int count)
{
    for (int i = 0; i < count; i++) {
        g_free(entries[i].topic);
        g_free(entries[i].title);
        g_free(entries[i].path);
    }
    g_free(entries);
}

static gboolean history_has_path(gpointer key, gpointer value, gpointer path)
{
    (void)key;
    return !strcmp(((History *)value)->e.path, path);
}

void zd_history_forget(const char *path)
{
    g_mutex_lock(&q.lock);
    if (g_hash_table_foreach_remove(q.history, history_has_path, (gpointer)path)) history_save();
    g_mutex_unlock(&q.lock);
}

ZdStatus zd_title_status(const ZdTitle *t, const char *dir, char **path)
{
    char *target = zd_target_path(t, dir), *key = title_key(t), *known = NULL;
    bool queued = false, recorded = false;
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len && !queued; i++) {
        const Job *j = q.jobs->pdata[i];
        queued = !strcmp(j->key, key) && j->state != ZD_JOB_DONE && j->state != ZD_JOB_FAILED;
    }
    const History *h = g_hash_table_lookup(q.history, key);
    if (h) {
        known = g_strdup(h->e.path);
        recorded = true;
    }
    g_mutex_unlock(&q.lock);

    char *result = result_path(target), *part = g_strconcat(target, ".part", NULL);
    const char *found = NULL;
    if (known && file_size(known) >= 0) found = known;
    else if (file_size(result) > 0) found = result;
    else if (file_size(target) > 0) found = target;

    ZdStatus status = queued ? ZD_STATUS_QUEUED
                    : found ? ZD_STATUS_DONE
                    : file_size(part) >= 0 ? ZD_STATUS_PARTIAL
                    : recorded ? ZD_STATUS_GONE : ZD_STATUS_MISSING;
    if (path) *path = g_strdup(found);
    g_free(target);
    g_free(key);
    g_free(known);
    g_free(result);
    g_free(part);
    return status;
}

/* ---------------------------------------------------------------------------
 * Titelbilder
 * ---------------------------------------------------------------------------
 * Die Suchdaten enthalten keine Bilder. Die Seite des Titels bei zdf.de nennt eines im
 * Feld "thumbnailUrl"; es landet einmal geladen im Bilderordner. Eine leere
 * Datei merkt sich, dass die Seite kein Bild hat. Der Ordner ist einstellbar.
 * ------------------------------------------------------------------------- */
static GMutex cover_lock;
static char *cover_dir;         /* NULL = Standardordner */

/* Adresse des Bildes in der kleinen Größe; NULL = die Seite nennt keines */
static char *cover_url(const char *page)
{
    static const char KEY[] = "\"thumbnailUrl\":\"";
    const char *start = strstr(page, KEY);
    if (!start) return NULL;
    start += strlen(KEY);
    const char *end = strchr(start, '"');
    if (!end) return NULL;
    char *url = g_strndup(start, (gsize)(end - start)), *result = NULL;
    char *tilde = strrchr(url, '~');
    if (tilde && g_str_has_prefix(url, COVER_URL_PREFIX) && !strpbrk(url, " \\'")) {
        const char *query = strchr(tilde, '?');
        *tilde = 0;
        result = g_strconcat(url, "~" COVER_SIZE, query ? query : "", NULL);
    }
    g_free(url);
    return result;
}

/* Bild von der Seite website nach path holen */
static void cover_download(const char *website, const char *path)
{
    char *page_argv[] = {"curl", "-sS", "-f", "-L", "-m", "20", "-A", USER_AGENT,
                         (char *)website, NULL};
    char *reply = NULL, *messages = NULL;
    int rc = run_capture(page_argv, &reply, &messages);
    char *url = rc == 0 && reply ? cover_url(reply) : NULL;
    if (url) {
        char *tmp = g_strconcat(path, ".part", NULL);
        char *image_argv[] = {"curl", "-sS", "-f", "-L", "-m", "20", "-A", USER_AGENT, "-o", tmp,
                              url, NULL};
        char *image_messages = NULL;
        if (run_capture(image_argv, NULL, &image_messages) != 0 || g_rename(tmp, path) != 0)
            g_unlink(tmp);
        g_free(image_messages);
        g_free(tmp);
    } else if (rc == 0) {
        g_file_set_contents(path, "", 0, NULL);
    }
    g_free(url);
    g_free(reply);
    g_free(messages);
}

void zd_set_cover_dir(const char *dir)
{
    g_mutex_lock(&cover_lock);
    g_free(cover_dir);
    cover_dir = g_strdup(dir);
    g_mutex_unlock(&cover_lock);
}

char *zd_default_cover_dir(void)
{
    return g_build_filename(zd_app_dir(), "bilder", NULL);
}

char *zd_cover_file(const char *website)
{
    if (!g_str_has_prefix(website, COVER_PAGE_PREFIX)) return NULL;
    g_mutex_lock(&cover_lock);
    char *dir = cover_dir ? g_strdup(cover_dir) : zd_default_cover_dir();
    g_mutex_unlock(&cover_lock);
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, website, -1);
    char *name = g_strconcat(hash, ".jpg", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_mkdir_with_parents(dir, 0755);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) cover_download(website, path);
    g_free(dir);
    g_free(hash);
    g_free(name);
    if (file_size(path) > 0) return path;
    g_free(path);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * IMDb
 * ---------------------------------------------------------------------------
 * Die Vorschlagsliste von IMDb führt direkt zur Titelseite. Sie ist nur ein Wegweiser: Passt
 * kein Treffer sicher, öffnet sich die IMDb-Suche, damit nie ein falscher Titel erscheint.
 * ------------------------------------------------------------------------- */
/* Wert von "key":" im Bereich start..end */
static char *json_text_between(const char *start, const char *end, const char *key)
{
    const char *value = g_strstr_len(start, end - start, key);
    if (!value) return NULL;
    value += strlen(key);
    const char *close = g_strstr_len(value, end - value, "\"");
    return close ? g_strndup(value, (gsize)(close - value)) : NULL;
}

/* Gehört der Treffer label zum Titel name? Ein Titel wie "Wind River - Tod im Schnee" passt zu
 * "Wind River", aber nur am Anfang und bis zu einem Wortende. */
static bool imdb_label_fits(const char *label, const char *name)
{
    char *a = g_utf8_strdown(label, -1), *b = g_utf8_strdown(name, -1);
    size_t length = strlen(a);
    bool fits = length >= 3 && !strncmp(a, b, length) && (!b[length] || strchr(" :-", b[length]));
    g_free(a);
    g_free(b);
    return fits;
}

/* Titelkennung aus der Vorschlagsliste; NULL = kein sicherer Treffer */
static char *imdb_pick(const char *reply, const char *name, bool is_film)
{
    static const char KEY[] = "\"id\":\"tt";
    char *result = NULL;
    for (const char *p = strstr(reply, KEY); p && !result;) {
        const char *next = strstr(p + 1, KEY);
        const char *end = next ? next : p + strlen(p);
        char *id = json_text_between(p, end, "\"id\":\"");
        char *label = json_text_between(p, end, "\"l\":\"");
        char *kind = json_text_between(p, end, "\"qid\":\"");
        bool type_fits = kind && (is_film ? !strcmp(kind, "movie") || !strcmp(kind, "tvMovie")
                                          : !strcmp(kind, "tvSeries") ||
                                            !strcmp(kind, "tvMiniSeries"));
        if (id && label && type_fits && imdb_label_fits(label, name) &&
            strspn(id, "t0123456789") == strlen(id))
            result = g_strdup(id);
        g_free(id);
        g_free(label);
        g_free(kind);
        p = next;
    }
    return result;
}

char *zd_imdb_url(const char *name, bool is_film)
{
    /* Untertitel wie "- Tod im Schnee" kennt IMDb meist nicht */
    char *query = g_strdup(name);
    char *cut = strstr(query, " - ");
    if (cut) *cut = 0;
    if ((cut = strstr(query, ": "))) *cut = 0;
    char *lower = g_utf8_strdown(query, -1);
    char *escaped = g_uri_escape_string(lower, NULL, FALSE);
    char *api = g_strdup_printf(IMDB_API "%s.json?includeVideos=0", escaped);
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "15", "-A", USER_AGENT, api, NULL};
    char *reply = NULL, *messages = NULL, *id = NULL;
    if (run_capture(argv, &reply, &messages) == 0 && reply) id = imdb_pick(reply, name, is_film);
    char *full = g_uri_escape_string(name, NULL, FALSE);
    char *url = id ? g_strdup_printf("https://www.imdb.com/title/%s/", id)
                   : g_strdup_printf("https://www.imdb.com/find/?q=%s", full);
    zd_log("IMDb: %s -> %s", name, url);
    g_free(query);
    g_free(lower);
    g_free(escaped);
    g_free(api);
    g_free(reply);
    g_free(messages);
    g_free(id);
    g_free(full);
    return url;
}

/* ---------------------------------------------------------------------------
 * Warteschlange: Speichern und Laden
 * ------------------------------------------------------------------------- */
/* Zusatzspuren für die Warteschlange in eine Zeile packen */
static char *extras_to_text(const Job *j)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        g_string_append_printf(s, "%s%d\x1f%s\x1f%s\x1f%s", i ? "\x1e" : "", e->subtitle, e->label,
                               e->lang, e->url);
    }
    return g_string_free(s, FALSE);
}

static void extra_free(gpointer data)
{
    Extra *e = data;
    g_free(e->label);
    g_free(e->lang);
    g_free(e->url);
    g_free(e);
}

/* Zusatzspuren aus der Warteschlange lesen */
static void extras_from_text(Job *j, const char *text)
{
    char **records = g_strsplit(text, "\x1e", -1);
    for (int i = 0; records[i]; i++) {
        char **f = g_strsplit(records[i], "\x1f", -1);
        if (g_strv_length(f) == 4) {
            Extra *e = g_new0(Extra, 1);
            e->subtitle = atoi(f[0]);
            e->label = g_strdup(f[1]);
            e->lang = g_strdup(f[2]);
            e->url = g_strdup(f[3]);
            g_ptr_array_add(j->extras, e);
        }
        g_strfreev(f);
    }
    g_strfreev(records);
}

static char *queue_path(void)
{
    return g_build_filename(g_app_dir, "warteschlange.tsv", NULL);
}

/* Warteschlange sichern (q.lock gehalten) */
static void queue_save(void)
{
    if (!q.persist) return;
    GString *out = g_string_new(QUEUE_HEADER "\n");
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->remove_requested) continue;
        /* unterbrochene Arbeit gilt beim nächsten Start als wartend */
        ZdJobState state = j->state == ZD_JOB_DOWNLOADING ? ZD_JOB_WAITING
                         : j->state == ZD_JOB_CONVERTING ? ZD_JOB_CONVERT_WAITING : j->state;
        char *title = g_strescape(j->title, NULL), *url = g_strescape(j->url, NULL);
        char *target = g_strescape(j->target, NULL);
        char *message = g_strescape(j->message ? j->message : "", NULL);
        char *extras = extras_to_text(j), *extras_text = g_strescape(extras, NULL);
        char *label = g_strescape(j->main_label, NULL), *lang = g_strescape(j->main_lang, NULL);
        char *key = g_strescape(j->key, NULL), *topic = g_strescape(j->topic, NULL);
        g_string_append_printf(out, "%d\t%d\t%d\t%d\t%d\t%d\t%" G_GINT64_FORMAT
                               "\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%d\t%d\n",
                               state, j->convert, j->keep_original, j->encoder, j->gpu_quality,
                               j->cpu_rf, j->total, title, url, target, message, j->skip_main_audio,
                               label, lang, extras_text, key, topic, j->season, j->episode);
        g_free(key);
        g_free(topic);
        g_free(extras);
        g_free(extras_text);
        g_free(label);
        g_free(lang);
        g_free(title);
        g_free(url);
        g_free(target);
        g_free(message);
    }
    char *path = queue_path();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

static void job_free(Job *j)
{
    g_free(j->title);
    g_free(j->url);
    g_free(j->target);
    g_free(j->message);
    g_free(j->main_label);
    g_free(j->main_lang);
    g_free(j->key);
    g_free(j->topic);
    g_ptr_array_free(j->extras, TRUE);
    g_free(j);
}

/* Warteschlange vom letzten Mal laden; offene Aufträge starten pausiert */
static void queue_load(void)
{
    char *path = queue_path(), *text = NULL;
    bool found = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (!found) return;
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    if (!lines[0] || strcmp(lines[0], QUEUE_HEADER)) {
        g_strfreev(lines);
        return;
    }
    for (int i = 1; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", -1);
        if (g_strv_length(f) == 19) {
            Job *j = g_new0(Job, 1);
            j->id = ++q.next_id;
            j->state = (ZdJobState)CLAMP(atoi(f[0]), ZD_JOB_WAITING, ZD_JOB_FAILED);
            j->convert = atoi(f[1]);
            j->keep_original = atoi(f[2]);
            j->encoder = atoi(f[3]) == ZD_ENCODER_CPU ? ZD_ENCODER_CPU : ZD_ENCODER_GPU;
            j->gpu_quality = atoi(f[4]);
            j->cpu_rf = atoi(f[5]);
            j->total = g_ascii_strtoll(f[6], NULL, 10);
            j->title = g_strcompress(f[7]);
            j->url = g_strcompress(f[8]);
            j->target = g_strcompress(f[9]);
            j->message = *f[10] ? g_strcompress(f[10]) : NULL;
            j->skip_main_audio = atoi(f[11]);
            j->main_label = g_strcompress(f[12]);
            j->main_lang = g_strcompress(f[13]);
            j->key = g_strcompress(f[15]);
            j->topic = g_strcompress(f[16]);
            j->season = atoi(f[17]);
            j->episode = atoi(f[18]);
            j->extras = g_ptr_array_new_with_free_func(extra_free);
            char *extras = g_strcompress(f[14]);
            extras_from_text(j, extras);
            g_free(extras);
            j->paused = j->state == ZD_JOB_WAITING || j->state == ZD_JOB_CONVERT_WAITING;
            j->remaining = -1;
            char *part = g_strconcat(j->target, ".part", NULL);
            j->have = MAX(file_size(part), 0);
            g_free(part);
            if (j->state == ZD_JOB_WAITING && j->total > 0)
                j->progress = (double)j->have / j->total;
            g_ptr_array_add(q.jobs, j);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
}

/* Auftrag nach Nummer suchen (q.lock gehalten) */
static Job *job_by_id(guint id)
{
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->id == id) return j;
    }
    return NULL;
}

/* Muss nach dem Download noch ffmpeg ran? */
static bool needs_processing(const Job *j)
{
    return j->convert || j->extras->len > 0 || j->skip_main_audio;
}

/* Dateien der Zusatzspuren löschen */
static void remove_extra_files(const Job *j)
{
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        char *part = g_strconcat(path, ".part", NULL);
        g_unlink(path);
        g_unlink(part);
        g_free(path);
        g_free(part);
    }
}

/* halbfertige Dateien eines Auftrags löschen */
static void remove_partial_files(const Job *j)
{
    char *part = g_strconcat(j->target, ".part", NULL), *tmp = temp_path(j->target);
    g_unlink(part);
    g_unlink(tmp);
    remove_extra_files(j);
    g_free(part);
    g_free(tmp);
}

/* Auftrag aus der Liste nehmen und freigeben (q.lock gehalten, kein Worker aktiv) */
static void job_discard(Job *j)
{
    g_ptr_array_remove(q.jobs, j);
    job_free(j);
}

/* Worker meldet sich ab (q.lock gehalten) */
static void worker_finish(Job *j)
{
    j->worker = false;
    j->speed = 0;
    j->remaining = -1;
    if (j->remove_requested) {
        remove_partial_files(j);
        job_discard(j);
    }
    q.workers--;
    queue_save();
    g_cond_broadcast(&q.worker_done);
    g_cond_signal(&q.wake);
}

/* Auftrag als fehlgeschlagen markieren (q.lock gehalten) */
static void job_fail(Job *j, const char *message)
{
    zd_log("FEHLER %s: %s", j->title, message);
    j->state = ZD_JOB_FAILED;
    g_free(j->message);
    j->message = g_strdup(message);
}

/* soll der Worker aufhören? (q.lock gehalten) */
static bool job_interrupted(const Job *j)
{
    return j->paused || j->remove_requested || q.quitting;
}

/* ---------------------------------------------------------------------------
 * Download
 * ------------------------------------------------------------------------- */
/* Tempolimit je Download in Bytes/s, 0 = unbegrenzt (q.lock gehalten) */
static gint64 rate_per_download(void)
{
    if (q.rate_limit <= 0) return 0;
    int active = 0;
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->state == ZD_JOB_DOWNLOADING && !j->paused) active++;
    }
    return MAX(q.rate_limit / MAX(active, 1), 16 * 1024);
}

/* Weicht das Limit so stark ab, dass curl neu starten muss? */
static bool rate_changed(gint64 running, gint64 wanted)
{
    if (running == wanted) return false;
    if (!running || !wanted) return true;
    return llabs(running - wanted) > running * RATE_TOLERANCE;
}

/* Größe der Datei beim Server erfragen -> Bytes, 0 = Fehler (*error gesetzt) */
static gint64 remote_size(const char *url, char **error)
{
    char *argv[] = {"curl", "-sSIL", "-m", "30", "-A", USER_AGENT, (char *)url, NULL};
    char *out = NULL, *messages = NULL;
    int rc = run_capture(argv, &out, &messages);
    int status = 0;
    gint64 size = 0;
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!strncmp(lines[i], "HTTP/", 5)) {
            const char *space = strchr(lines[i], ' ');
            status = space ? atoi(space + 1) : 0;
            size = 0;                               /* Weiterleitung: letzter Block zählt */
        } else if (!g_ascii_strncasecmp(lines[i], "content-length:", 15)) {
            size = g_ascii_strtoll(lines[i] + 15, NULL, 10);
        }
    }
    g_strfreev(lines);
    if (rc != 0 || status == 0)
        *error = g_strdup("Der Server ist nicht erreichbar.");
    else if (status == 403)
        *error = g_strdup("Zugriff verweigert – das Video ist nur aus Deutschland abrufbar.");
    else if (status == 404 || status == 410)
        *error = g_strdup("Das Video ist nicht mehr in der Mediathek.");
    else if (status != 200 || size <= 0)
        *error = g_strdup_printf("Unerwartete Antwort des Servers (HTTP %d).", status);
    g_free(out);
    g_free(messages);
    return *error ? 0 : size;
}

/* curl für die .part-Datei starten -> Prozessnummer, 0 = Fehler */
static pid_t start_curl(const char *url, const char *part, gint64 rate)
{
    char *rate_text = g_strdup_printf("%" G_GINT64_FORMAT, rate);
    char *argv[32];
    int n = 0;
    char *fixed[] = {"curl", "-sS", "-f", "-L", "-C", "-", "--connect-timeout", "20",
                     "--speed-limit", "1024", "--speed-time", "60", "-A", USER_AGENT, NULL};
    for (int i = 0; fixed[i]; i++) argv[n++] = fixed[i];
    if (rate > 0) {
        argv[n++] = "--limit-rate";
        argv[n++] = rate_text;
    }
    argv[n++] = "-o";
    argv[n++] = (char *)part;
    argv[n++] = (char *)url;
    argv[n] = NULL;
    GPid pid = 0;
    bool ok = g_spawn_async(NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD
                            | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                            child_setup, NULL, &pid, NULL);
    g_free(rate_text);
    return ok ? pid : 0;
}

/* Warten, bis curl endet oder unterbrochen wird; Fortschritt nachführen. base = Bytes
 * der schon fertigen Dateien des Auftrags.
 * -> Exit-Code von curl, -1 = unterbrochen, -2 = Tempolimit geändert */
static int watch_curl(Job *j, pid_t pid, const char *part, gint64 rate, gint64 base)
{
    gint64 last_size = MAX(file_size(part), 0);
    double last_time = now_seconds();
    for (;;) {
        g_usleep(POLL_MS * 1000);
        int status = 0;
        bool ended = waitpid(pid, &status, WNOHANG) == pid;
        gint64 size = MAX(file_size(part), 0);
        double now = now_seconds();

        g_mutex_lock(&q.lock);
        j->have = base + size;
        j->progress = j->total ? (double)j->have / j->total : 0;
        if (now - last_time >= 0.8) {
            double current = MAX(size - last_size, 0) / (now - last_time);
            j->speed = j->speed > 0 ? 0.7 * j->speed + 0.3 * current : current;
            j->remaining = j->speed > 1 ? (j->total - j->have) / j->speed : -1;
            last_size = size;
            last_time = now;
        }
        bool interrupted = job_interrupted(j);
        bool new_rate = rate_changed(rate, rate_per_download());
        g_mutex_unlock(&q.lock);

        if (ended) return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
        if (interrupted || new_rate) {
            stop_child(pid);
            return interrupted ? -1 : -2;
        }
    }
}

/* Wartezeit vor dem nächsten Versuch, die eine Pause sofort beendet */
static void retry_wait(Job *j)
{
    for (int waited = 0; waited < RETRY_WAIT_MS; waited += POLL_MS) {
        g_mutex_lock(&q.lock);
        bool interrupted = job_interrupted(j);
        g_mutex_unlock(&q.lock);
        if (interrupted) return;
        g_usleep(POLL_MS * 1000);
    }
}

/* Platz für die restlichen Bytes vorhanden? */
static bool enough_space(const char *path, gint64 needed)
{
    char *dir = g_path_get_dirname(path);
    struct statvfs st;
    bool ok = statvfs(dir, &st) != 0
              || (gint64)st.f_bavail * (gint64)st.f_frsize >= needed + MIN_FREE_SPACE;
    g_free(dir);
    return ok;
}

/* Eine Datei fertig laden -> true = vollständig unter path; false = unterbrochen oder
 * Fehler (*error gesetzt) */
static bool download_file(Job *j, const char *url, const char *path, gint64 size, gint64 base,
                          char **error)
{
    char *part = g_strconcat(path, ".part", NULL);
    int failures = 0;
    gint64 progress_mark = -1;
    bool complete = false;
    for (;;) {
        gint64 have = file_size(part);
        if (have > size) {                          /* Datei passt nicht zur Quelle */
            g_unlink(part);
            have = 0;
        }
        if (have == size) {
            complete = true;
            break;
        }
        g_mutex_lock(&q.lock);
        bool interrupted = job_interrupted(j);
        gint64 rate = rate_per_download();
        g_mutex_unlock(&q.lock);
        if (interrupted) break;

        pid_t pid = start_curl(url, part, rate);
        if (!pid) {
            *error = g_strdup("curl lässt sich nicht starten.");
            break;
        }
        int rc = watch_curl(j, pid, part, rate, base);
        if (rc == -1) break;
        if (rc == -2) continue;
        gint64 current = MAX(file_size(part), 0);
        if (rc == 0 && current == size) continue;   /* oben als fertig erkannt */
        if (rc == 33) {                             /* Server kann nicht fortsetzen */
            g_unlink(part);
            continue;
        }
        if (rc == 23) {
            *error = g_strdup("Die Datei lässt sich nicht schreiben (Laufwerk voll?).");
            break;
        }
        if (current > progress_mark) failures = 0;  /* es ging voran: neue Versuche */
        progress_mark = current;
        zd_log("Download unterbrochen (curl %d): %s", rc, j->title);
        if (++failures >= MAX_FAILURES) {
            *error = g_strdup_printf("Download abgebrochen (curl-Fehler %d). "
                                     "Er lässt sich später fortsetzen.", rc);
            break;
        }
        retry_wait(j);
    }
    if (complete && g_rename(part, path) != 0) {
        *error = g_strdup("Die fertige Datei lässt sich nicht umbenennen.");
        complete = false;
    }
    g_free(part);
    return complete;
}

/* Untertitel laden; ein Fehler bricht den Auftrag nicht ab (die Spur fehlt dann) */
static void download_subtitle(Job *j, const Extra *e, const char *path)
{
    char *part = g_strconcat(path, ".part", NULL);
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "-o", part,
                    e->url, NULL};
    if (run_capture(argv, NULL, NULL) == 0) g_rename(part, path);
    else zd_log("Untertitel nicht verfügbar: %s (%s)", j->title, e->url);
    g_unlink(part);
    g_free(part);
}

/* Größe mit bis zu drei Versuchen erfragen, 0 = Fehler */
static gint64 remote_size_retry(Job *j, const char *url, char **error)
{
    gint64 size = 0;
    for (int attempt = 0; attempt < 3 && !size; attempt++) {
        g_clear_pointer(error, g_free);
        size = remote_size(url, error);
        if (size || !strstr(*error, "erreichbar")) break;
        retry_wait(j);
    }
    return size;
}

/* Worker: alle Dateien eines Auftrags herunterladen. Die Angaben im Job ändern sich nicht,
 * solange worker gesetzt ist, und dürfen deshalb ohne Sperre gelesen werden. */
static gpointer download_thread(gpointer data)
{
    Job *j = data;
    char *result = result_path(j->target), *dir = g_path_get_dirname(j->target), *error = NULL;
    bool processing = needs_processing(j);
    bool already_there = file_size(result) > 0;
    bool complete = already_there;
    guint count = 1 + j->extras->len;
    gint64 *sizes = g_new0(gint64, count);
    gint64 total = 0;

    if (!complete && g_mkdir_with_parents(dir, 0755) != 0)
        error = g_strdup_printf("Ordner lässt sich nicht anlegen: %s", g_strerror(errno));

    /* Größen aller Videodateien erfragen, damit der Fortschritt von Anfang an stimmt */
    if (!complete && !error) {
        gint64 missing = 0;
        for (guint i = 0; i < count && !error; i++) {
            const Extra *e = i ? j->extras->pdata[i - 1] : NULL;
            if (e && e->subtitle) continue;
            sizes[i] = remote_size_retry(j, e ? e->url : j->url, &error);
            total += sizes[i];
            char *path = e ? extra_path(j->target, i - 1, false) : g_strdup(j->target);
            char *part = g_strconcat(path, ".part", NULL);
            missing += file_size(path) == sizes[i] ? 0 : sizes[i] - MAX(file_size(part), 0);
            g_free(path);
            g_free(part);
        }
        if (!error && !enough_space(j->target, missing))
            error = g_strdup("Zu wenig freier Platz am Speicherort.");
    }
    if (!complete && !error) {
        g_mutex_lock(&q.lock);
        j->total = total;
        g_mutex_unlock(&q.lock);
        gint64 base = 0;
        complete = true;
        for (guint i = 0; i < count && complete; i++) {
            const Extra *e = i ? j->extras->pdata[i - 1] : NULL;
            char *path = e ? extra_path(j->target, i - 1, e->subtitle) : g_strdup(j->target);
            if (e && e->subtitle) {
                if (file_size(path) <= 0) download_subtitle(j, e, path);
            } else if (file_size(path) != sizes[i]) {
                complete = download_file(j, e ? e->url : j->url, path, sizes[i], base, &error);
            }
            base += sizes[i];
            g_free(path);
        }
    }

    g_mutex_lock(&q.lock);
    if (error) {
        job_fail(j, error);
    } else if (complete) {
        j->have = j->total;
        j->progress = 0;
        j->state = processing && !already_there ? ZD_JOB_CONVERT_WAITING : ZD_JOB_DONE;
        if (already_there) {
            g_free(j->message);
            j->message = g_strdup("War schon vorhanden");
        }
        if (j->state == ZD_JOB_DONE) {
            j->progress = 1;
            history_add(j);
        }
        zd_log("Download fertig: %s", j->title);
    } else {
        j->state = ZD_JOB_WAITING;                  /* pausiert, abgebrochen oder Programmende */
    }
    worker_finish(j);
    g_mutex_unlock(&q.lock);
    g_free(result);
    g_free(dir);
    g_free(sizes);
    g_free(error);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Umwandlung
 * ------------------------------------------------------------------------- */
/* erstes Render-Gerät, NULL = keines (Ergebnis statisch) */
static const char *render_device(void)
{
    static char device[32];
    for (int i = 128; i < 136 && !*device; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
        if (g_access(path, R_OK | W_OK) == 0) snprintf(device, sizeof device, "%s", path);
    }
    return *device ? device : NULL;
}

bool zd_gpu_available(void)
{
    const char *device = render_device();
    if (!device) return false;
    char *argv[] = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-vaapi_device",
                    (char *)device, "-f", "lavfi", "-i", "color=black:s=320x240:d=0.2",
                    "-vf", "format=p010,hwupload", "-c:v", "av1_vaapi", "-f", "null", "-", NULL};
    return run_capture(argv, NULL, NULL) == 0;
}

/* Videocodec und Dauer lesen -> false bei Fehler */
static bool media_info(const char *path, char **codec, double *duration)
{
    char *argv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                    "stream=codec_name:format=duration", "-of", "default=noprint_wrappers=1",
                    (char *)path, NULL};
    char *out = NULL;
    bool ok = run_capture(argv, &out, NULL) == 0;
    *codec = NULL;
    *duration = 0;
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!strncmp(lines[i], "codec_name=", 11)) *codec = g_strdup(lines[i] + 11);
        if (!strncmp(lines[i], "duration=", 9)) *duration = g_ascii_strtod(lines[i] + 9, NULL);
    }
    g_strfreev(lines);
    g_free(out);
    return ok && *codec && *duration > 0;
}

/* ffmpeg-Fortschritt aus dem Puffer lesen; verarbeitete Zeilen entfernen */
static void parse_progress(GString *buffer, double *seconds, double *factor)
{
    char *newline;
    while ((newline = strchr(buffer->str, '\n'))) {
        *newline = 0;
        if (!strncmp(buffer->str, "out_time_us=", 12) || !strncmp(buffer->str, "out_time_ms=", 12)) {
            double value = g_ascii_strtod(buffer->str + 12, NULL);
            if (value > 0) *seconds = value / 1e6;
        } else if (!strncmp(buffer->str, "speed=", 6)) {
            double value = g_ascii_strtod(buffer->str + 6, NULL);
            if (value > 0) *factor = value;
        }
        g_string_erase(buffer, 0, newline + 1 - buffer->str);
    }
}

/* Pipe lesen, ohne zu blockieren */
static void drain_pipe(int fd, GString *buffer)
{
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) g_string_append_len(buffer, chunk, n);
}

/* Argument an die Befehlszeile anhängen */
static void arg_add(GPtrArray *argv, const char *text)
{
    g_ptr_array_add(argv, g_strdup(text));
}

/* Mehrere feste Argumente anhängen (NULL-terminiert) */
static void args_add(GPtrArray *argv, const char *const *texts)
{
    for (; *texts; texts++) arg_add(argv, *texts);
}

/* Name und Sprache der Ausgabespur setzen; kind = 'a' oder 's' */
static void add_track_metadata(GPtrArray *argv, char kind, int index, const char *label,
                               const char *lang)
{
    char *stream = g_strdup_printf("-metadata:s:%c:%d", kind, index);
    char *title = g_strdup_printf("title=%s", label), *language = g_strdup_printf("language=%s", lang);
    args_add(argv, (const char *const[]){stream, language, stream, title, NULL});
    g_free(stream);
    g_free(title);
    g_free(language);
}

/* ffmpeg-Befehl bauen: Bild aus der Hauptdatei, dazu alle vorhandenen Spuren.
 * copy = Bild unverändert übernehmen, sonst AV1 mit dem gewählten Encoder. */
static GPtrArray *build_ffmpeg_cmd(const Job *j, bool copy, ZdEncoder encoder, const char *device,
                                   const char *tmp)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    args_add(a, (const char *const[]){"ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel",
                                      "error", "-progress", "pipe:1", "-nostats", NULL});
    if (!copy && encoder == ZD_ENCODER_GPU)
        args_add(a, (const char *const[]){"-vaapi_device", device, NULL});
    args_add(a, (const char *const[]){"-i", j->target, NULL});

    GPtrArray *maps = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *metadata = g_ptr_array_new_with_free_func(g_free);
    args_add(maps, (const char *const[]){"-map", "0:v:0", NULL});
    int inputs = 1, audio = 0, subtitles = 0;
    if (!j->skip_main_audio) {
        args_add(maps, (const char *const[]){"-map", "0:a?", NULL});
        add_track_metadata(metadata, 'a', audio++, j->main_label, j->main_lang);
    }
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        if (file_size(path) > 0) {
            args_add(a, (const char *const[]){"-i", path, NULL});
            char *map = g_strdup_printf("%d:%c:0", inputs++, e->subtitle ? 's' : 'a');
            args_add(maps, (const char *const[]){"-map", map, NULL});
            g_free(map);
            if (e->subtitle) add_track_metadata(metadata, 's', subtitles++, e->label, e->lang);
            else add_track_metadata(metadata, 'a', audio++, e->label, e->lang);
        }
        g_free(path);
    }
    for (guint i = 0; i < maps->len; i++) arg_add(a, maps->pdata[i]);
    args_add(a, (const char *const[]){"-map_metadata", "0", NULL});

    char quality[16];
    if (copy) {
        args_add(a, (const char *const[]){"-c:v", "copy", NULL});
    } else if (encoder == ZD_ENCODER_GPU) {
        snprintf(quality, sizeof quality, "%d", j->gpu_quality);
        args_add(a, (const char *const[]){"-vf", "format=p010,hwupload", "-c:v", "av1_vaapi",
                                          "-rc_mode", "CQP", "-global_quality:v", quality,
                                          "-g:v", GOP_FRAMES, NULL});
    } else {
        snprintf(quality, sizeof quality, "%d", j->cpu_rf);
        args_add(a, (const char *const[]){"-vf", "format=yuv420p10le", "-c:v", "libsvtav1",
                                          "-preset", SVT_PRESET, "-crf", quality, "-svtav1-params",
                                          "tune=1", "-g:v", GOP_FRAMES, NULL});
    }
    args_add(a, (const char *const[]){"-c:a", "copy", "-c:s", "srt", NULL});
    for (guint i = 0; i < metadata->len; i++) arg_add(a, metadata->pdata[i]);
    args_add(a, (const char *const[]){"-max_muxing_queue_size", "4096", "-f", "matroska", tmp,
                                      NULL});
    g_ptr_array_add(a, NULL);
    g_ptr_array_free(maps, TRUE);
    g_ptr_array_free(metadata, TRUE);
    return a;
}

/* ffmpeg ausführen -> 0 = fertig, -1 = unterbrochen, sonst Fehler (die letzten
 * ffmpeg-Meldungen landen in *messages) */
static int run_encoder(Job *j, bool copy, ZdEncoder encoder, const char *tmp, double duration,
                       char **messages)
{
    const char *device = render_device();
    if (!copy && encoder == ZD_ENCODER_GPU && !device) {
        *messages = g_strdup("Kein Render-Gerät gefunden.");
        return 1;
    }
    GPtrArray *command = build_ffmpeg_cmd(j, copy, encoder, device, tmp);
    char **argv = (char **)command->pdata;

    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, child_setup,
                                  GINT_TO_POINTER(1), &pid, NULL, &out_fd, &err_fd, NULL)) {
        *messages = g_strdup("ffmpeg lässt sich nicht starten.");
        g_ptr_array_free(command, TRUE);
        return 1;
    }
    g_ptr_array_free(command, TRUE);
    fcntl(out_fd, F_SETFL, O_NONBLOCK);
    fcntl(err_fd, F_SETFL, O_NONBLOCK);
    GString *progress = g_string_new(NULL), *errors = g_string_new(NULL);
    double seconds = 0, factor = 0;
    bool stopped = false, interrupted = false;
    int status = 0, rc;
    for (;;) {
        struct pollfd fds[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
        poll(fds, 2, POLL_MS);
        drain_pipe(out_fd, progress);
        drain_pipe(err_fd, errors);
        parse_progress(progress, &seconds, &factor);
        if (errors->len > 2000) g_string_erase(errors, 0, errors->len - 2000);

        g_mutex_lock(&q.lock);
        bool end = j->remove_requested || q.quitting, pause = j->paused;
        j->progress = duration > 0 ? MIN(seconds / duration, 1.0) : 0;
        j->remaining = factor > 0 ? (duration - seconds) / factor : -1;
        g_mutex_unlock(&q.lock);

        if (end) {
            interrupted = true;
            stop_child(pid);
            break;
        }
        if (pause != stopped) {
            kill(pid, pause ? SIGSTOP : SIGCONT);
            stopped = pause;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            drain_pipe(err_fd, errors);
            break;
        }
    }
    close(out_fd);
    close(err_fd);
    g_string_free(progress, TRUE);
    if (interrupted) rc = -1;
    else rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    *messages = g_string_free(errors, FALSE);
    if (rc > 0) g_strstrip(*messages);
    return rc;
}

/* Bytes, die das Ergebnis unterbieten muss, damit AV1 Platz spart */
static gint64 comparable_size(const Job *j, double duration)
{
    gint64 size = file_size(j->target);
    for (guint i = 0; i < j->extras->len; i++)
        if (!((const Extra *)j->extras->pdata[i])->subtitle)
            size += (gint64)(duration * AUDIO_BYTES_PER_SECOND);
    return size;
}

/* Wie viele Spuren wurden wirklich zusammengeführt? */
static char *tracks_note(const Job *j)
{
    int audio = j->skip_main_audio ? 0 : 1, subtitles = 0, missing = 0;
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        if (file_size(path) <= 0) missing++;
        else if (e->subtitle) subtitles++;
        else audio++;
        g_free(path);
    }
    if (audio <= 1 && !subtitles && !missing) return g_strdup("");
    return g_strdup_printf(" · %d Tonspur%s, %d Untertitel%s", audio, audio == 1 ? "" : "en",
                           subtitles, missing ? " (nicht alle verfügbar)" : "");
}

/* Worker: Spuren zusammenführen und auf Wunsch nach AV1 umwandeln; Ergebnis prüfen */
static gpointer convert_thread(gpointer data)
{
    Job *j = data;
    char *tmp = temp_path(j->target), *result = result_path(j->target);
    char *error = NULL, *note = NULL, *codec = NULL, *messages = NULL;
    double duration = 0;
    bool done = false, interrupted = false, copy = !j->convert, av1_useless = false;

    if (!media_info(j->target, &codec, &duration))
        error = g_strdup("Die heruntergeladene Datei ist nicht lesbar.");
    while (!error && !done && !interrupted) {
        g_mutex_lock(&q.lock);
        j->merging = copy;
        j->progress = 0;
        g_mutex_unlock(&q.lock);
        g_clear_pointer(&messages, g_free);
        int rc = run_encoder(j, copy, j->encoder, tmp, duration, &messages);
        if (rc > 0 && !copy && j->encoder == ZD_ENCODER_GPU) {
            zd_log("GPU-Umwandlung fehlgeschlagen, versuche CPU: %s (%s)", j->title, messages);
            g_clear_pointer(&messages, g_free);
            rc = run_encoder(j, false, ZD_ENCODER_CPU, tmp, duration, &messages);
        }
        if (rc < 0) {
            interrupted = true;
            break;
        }
        if (rc > 0) {
            error = g_strdup_printf("ffmpeg ist fehlgeschlagen: %s",
                                    messages && *messages ? messages : "unbekannter Fehler");
            break;
        }
        char *out_codec = NULL;
        double out_duration = 0;
        bool readable = media_info(tmp, &out_codec, &out_duration);
        bool right_codec = readable && (copy || !strcmp(out_codec, "av1"));
        double tolerance = MAX(2.0, duration * 0.005);
        g_free(out_codec);
        if (!right_codec || ABS(out_duration - duration) > tolerance) {
            error = g_strdup("Das Ergebnis ist unvollständig – die MP4-Datei bleibt.");
        } else if (!copy && file_size(tmp) >= comparable_size(j, duration)) {
            if (j->extras->len || j->skip_main_audio) {
                copy = true;                /* AV1 bringt nichts: Spuren trotzdem vereinen */
                av1_useless = true;
                continue;
            }
            note = g_strdup("AV1 wäre nicht kleiner – die MP4-Datei bleibt");
            done = true;
        } else if (g_rename(tmp, result) != 0) {
            error = g_strdup("Das Ergebnis lässt sich nicht ablegen.");
        } else {
            char *tracks = tracks_note(j), *after = zd_format_size(file_size(result));
            if (copy) {
                note = g_strdup_printf("%s%s%s", after, tracks,
                                       av1_useless ? " · AV1 wäre nicht kleiner" : "");
            } else {
                char *before = zd_format_size(comparable_size(j, duration));
                note = g_strdup_printf("AV1: %s statt %s%s", after, before, tracks);
                g_free(before);
            }
            g_free(tracks);
            g_free(after);
            /* ohne AV1 steckt die MP4-Datei vollständig in der MKV-Datei */
            if (!j->keep_original || !j->convert) g_unlink(j->target);
            remove_extra_files(j);
            done = true;
        }
    }
    g_unlink(tmp);

    g_mutex_lock(&q.lock);
    j->merging = false;
    if (error) {
        job_fail(j, error);
    } else if (done) {
        j->state = ZD_JOB_DONE;
        j->progress = 1;
        g_free(j->message);
        j->message = g_strdup(note);
        history_add(j);
        zd_log("Verarbeitung fertig: %s (%s)", j->title, note);
    } else if (interrupted) {
        j->state = ZD_JOB_CONVERT_WAITING;
        j->progress = 0;
        if (q.quitting) j->paused = true;
    }
    worker_finish(j);
    g_mutex_unlock(&q.lock);
    g_free(tmp);
    g_free(result);
    g_free(error);
    g_free(note);
    g_free(codec);
    g_free(messages);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Verteiler: startet Worker, solange Plätze frei sind
 * ------------------------------------------------------------------------- */
static gpointer dispatcher_thread(gpointer data)
{
    (void)data;
    g_mutex_lock(&q.lock);
    while (!q.quitting) {
        int downloads = 0, conversions = 0;
        for (guint i = 0; i < q.jobs->len; i++) {
            Job *j = q.jobs->pdata[i];
            if (j->worker && j->state == ZD_JOB_DOWNLOADING) downloads++;
            if (j->worker && j->state == ZD_JOB_CONVERTING) conversions++;
        }
        bool started = false;
        for (guint i = 0; i < q.jobs->len; i++) {
            Job *j = q.jobs->pdata[i];
            if (j->paused || j->worker) continue;
            GThreadFunc worker = NULL;
            if (j->state == ZD_JOB_WAITING && downloads < q.max_downloads) {
                j->state = ZD_JOB_DOWNLOADING;
                downloads++;
                worker = download_thread;
            } else if (j->state == ZD_JOB_CONVERT_WAITING && conversions < q.max_conversions) {
                j->state = ZD_JOB_CONVERTING;
                conversions++;
                worker = convert_thread;
            }
            if (!worker) continue;
            j->worker = true;
            q.workers++;
            started = true;
            g_thread_unref(g_thread_new("auftrag", worker, j));
        }
        if (started) queue_save();
        g_cond_wait_until(&q.wake, &q.lock, g_get_monotonic_time() + G_TIME_SPAN_SECOND);
    }
    g_mutex_unlock(&q.lock);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Öffentliche Schnittstelle
 * ------------------------------------------------------------------------- */
void zd_init(bool persist)
{
    g_app_dir = g_build_filename(g_get_home_dir(), ".zdf-downloader", NULL);
    g_log_path = g_build_filename(g_app_dir, "protokoll.log", NULL);
    g_mkdir_with_parents(g_app_dir, 0755);
    q.jobs = g_ptr_array_new();
    q.persist = persist;
    q.max_downloads = 2;
    q.max_conversions = 1;
    history_load();
    if (persist) queue_load();
    q.dispatcher = g_thread_new("verteiler", dispatcher_thread, NULL);
}

void zd_shutdown(void)
{
    g_mutex_lock(&q.lock);
    q.quitting = true;
    /* Downloads gelten danach als pausiert, damit sie nicht von selbst weiterlaufen */
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->state == ZD_JOB_DOWNLOADING) j->paused = true;
    }
    g_cond_broadcast(&q.wake);
    while (q.workers > 0) g_cond_wait(&q.worker_done, &q.lock);
    queue_save();
    g_mutex_unlock(&q.lock);
    g_thread_join(q.dispatcher);
}

bool zd_have_tools(char **missing)
{
    const char *tools[] = {"curl", "ffmpeg", "ffprobe"};
    for (size_t i = 0; i < G_N_ELEMENTS(tools); i++) {
        char *path = g_find_program_in_path(tools[i]);
        bool found = path != NULL;
        g_free(path);
        if (!found) {
            if (missing) *missing = g_strdup(tools[i]);
            return false;
        }
    }
    return true;
}

/* Steht label in der NULL-terminierten Liste? */
static bool label_listed(const char *const *list, const char *label)
{
    return list && g_strv_contains(list, label);
}

static Extra *extra_new(bool subtitle, const ZdTrack *track)
{
    Extra *e = g_new0(Extra, 1);
    e->subtitle = subtitle;
    e->label = g_strdup(track->label);
    e->lang = g_strdup(track->lang);
    e->url = g_strdup(track->url);
    return e;
}

guint zd_queue_add(const ZdTitle *t, const ZdJobOptions *o)
{
    char *target = zd_target_path(t, o->dir);
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *other = q.jobs->pdata[i];
        if (!strcmp(other->target, target) && other->state != ZD_JOB_DONE) {
            g_mutex_unlock(&q.lock);
            g_free(target);
            return 0;
        }
    }
    Job *j = g_new0(Job, 1);
    j->id = ++q.next_id;
    j->title = t->season >= 0 ? g_strdup_printf("%s – S%02dE%02d – %s", t->topic, t->season,
                                                t->episode, t->name)
                              : g_strdup(t->name);
    j->url = g_strdup(zd_title_url(t, o->quality));
    j->target = target;
    j->key = title_key(t);
    j->topic = g_strdup(t->is_film ? "Filme" : t->topic);
    j->season = t->season;
    j->episode = t->episode;
    const ZdTrack *main_audio = t->audio->pdata[0];
    j->main_label = g_strdup(main_audio->label);
    j->main_lang = g_strdup(main_audio->lang);
    /* Ein Titel, der keine der gewählten Sprachen hat, behält seinen Originalton */
    bool other_audio = false;
    for (guint i = 1; i < t->audio->len; i++)
        other_audio |= !label_listed(o->skip_audio, ((ZdTrack *)t->audio->pdata[i])->label);
    j->skip_main_audio = other_audio && label_listed(o->skip_audio, main_audio->label);
    j->extras = g_ptr_array_new_with_free_func(extra_free);
    for (guint i = 1; i < t->audio->len; i++) {
        const ZdTrack *track = t->audio->pdata[i];
        if (!label_listed(o->skip_audio, track->label))
            g_ptr_array_add(j->extras, extra_new(false, track));
    }
    for (guint i = 0; i < t->subtitles->len; i++) {
        const ZdTrack *track = t->subtitles->pdata[i];
        if (!label_listed(o->skip_subtitles, track->label))
            g_ptr_array_add(j->extras, extra_new(true, track));
    }
    j->convert = o->convert;
    j->keep_original = o->keep_original;
    j->encoder = o->encoder;
    j->gpu_quality = o->gpu_quality;
    j->cpu_rf = o->cpu_rf;
    j->state = ZD_JOB_WAITING;
    j->remaining = -1;
    g_ptr_array_add(q.jobs, j);
    guint id = j->id;
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
    return id;
}

ZdJobView *zd_queue_snapshot(int *count)
{
    g_mutex_lock(&q.lock);
    ZdJobView *views = g_new0(ZdJobView, MAX(q.jobs->len, 1));
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        views[i] = (ZdJobView){j->id, j->state, j->paused, j->merging, g_strdup(j->title),
                               g_strdup(j->target), g_strdup(j->message), j->have, j->total,
                               j->speed, j->progress, j->remaining};
    }
    *count = (int)q.jobs->len;
    g_mutex_unlock(&q.lock);
    return views;
}

void zd_queue_snapshot_free(ZdJobView *views, int count)
{
    for (int i = 0; i < count; i++) {
        g_free(views[i].title);
        g_free(views[i].target);
        g_free(views[i].message);
    }
    g_free(views);
}

/* Pausenzustand eines Auftrags setzen (q.lock gehalten) */
static void job_set_paused(Job *j, bool paused)
{
    if (j->state == ZD_JOB_DONE || j->state == ZD_JOB_FAILED) return;
    j->paused = paused;
    if (paused && j->state == ZD_JOB_DOWNLOADING) j->speed = 0;
}

void zd_job_set_paused(guint id, bool paused)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j) job_set_paused(j, paused);
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}

void zd_queue_set_paused(bool paused)
{
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len; i++) job_set_paused(q.jobs->pdata[i], paused);
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}

void zd_job_retry(guint id)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j && j->state == ZD_JOB_FAILED) {
        j->state = ZD_JOB_WAITING;  /* fertige Dateien überspringt der Download von selbst */
        j->paused = false;
        g_clear_pointer(&j->message, g_free);
        queue_save();
        g_cond_signal(&q.wake);
    }
    g_mutex_unlock(&q.lock);
}

void zd_job_remove(guint id)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j) {
        j->remove_requested = true;
        if (!j->worker) {
            remove_partial_files(j);
            job_discard(j);
        }
        queue_save();
    }
    g_mutex_unlock(&q.lock);
}

void zd_queue_remove_finished(void)
{
    g_mutex_lock(&q.lock);
    for (guint i = q.jobs->len; i-- > 0;) {
        Job *j = q.jobs->pdata[i];
        if (j->state == ZD_JOB_DONE && !j->worker) job_discard(j);
    }
    queue_save();
    g_mutex_unlock(&q.lock);
}

void zd_set_rate_limit(gint64 bytes_per_second)
{
    g_mutex_lock(&q.lock);
    q.rate_limit = MAX(bytes_per_second, 0);
    g_mutex_unlock(&q.lock);
}

void zd_set_parallel(int downloads, int conversions)
{
    g_mutex_lock(&q.lock);
    q.max_downloads = CLAMP(downloads, 1, ZD_MAX_DOWNLOADS);
    q.max_conversions = CLAMP(conversions, 1, ZD_MAX_CONVERSIONS);
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}
