/*
 * ZDF-Downloader – Schnittstelle der Engine (zd_core.c)
 * =====================================================
 * Genutzt von der grafischen Oberfläche (zd_gui.c) und der Terminal-Version
 * (zd_cli.c). Alle Funktionen sind thread-sicher, sofern nichts anderes dabeisteht.
 */
#ifndef ZD_CORE_H
#define ZD_CORE_H

#include <glib.h>
#include <stdbool.h>

#define ZD_VERSION "1.3.0"
#define ZD_MAX_DOWNLOADS 4      /* höchstens so viele Downloads gleichzeitig */
#define ZD_MAX_CONVERSIONS 3    /* höchstens so viele Umwandlungen gleichzeitig */
#define ZD_DEFAULT_GPU_QUALITY 64   /* VAAPI-AV1 (0–255), wie im AV1-Converter */
#define ZD_DEFAULT_CPU_RF 35        /* SVT-AV1 (0–63), wie im AV1-Converter */

typedef enum { ZD_KIND_ALL, ZD_KIND_FILMS, ZD_KIND_SERIES } ZdKind;
/* Mediatheken als Bitmaske */
typedef enum {
    ZD_SOURCE_ZDF = 1, ZD_SOURCE_ARD = 2, ZD_SOURCE_3SAT = 4, ZD_SOURCE_ALL = 7
} ZdSource;
typedef enum { ZD_QUALITY_HIGH, ZD_QUALITY_MEDIUM, ZD_QUALITY_LOW } ZdQuality;
typedef enum { ZD_ENCODER_GPU, ZD_ENCODER_CPU } ZdEncoder;

/* Tonspur oder Untertitel eines Eintrags */
typedef struct {
    char *label;                /* "Deutsch", "Englisch", "Audiodeskription" … */
    char *lang;                 /* ISO 639-2, z. B. "deu" */
    char *url;                  /* Untertitel: WebVTT; Ton: kleinste Datei der Sprache */
} ZdTrack;

/* ein Eintrag der Mediathek; Fassungen in anderen Sprachen sind zusammengefasst */
typedef struct {
    char *channel, *topic, *title, *description, *website;
    char *url_high, *url_medium, *url_low, *url_subtitle;
    char *name;                 /* Titel ohne "(S18/E05)" und "- Neue Folgen" */
    gint64 timestamp;           /* Sendezeit (Unix-Zeit) */
    int duration;               /* Sekunden */
    int season, episode;        /* -1 = keine Angabe */
    int source;                 /* ZD_SOURCE_* */
    bool is_film, is_variant;   /* variant: Gebärdensprache (eigenes Bild) */
    const char *track_label, *track_lang;   /* Sprache laut Titel, NULL = Hauptfassung */
    GPtrArray *audio;           /* ZdTrack*; [0] = Ton der Datei, aus der das Bild kommt */
    GPtrArray *subtitles;       /* ZdTrack* */
} ZdTitle;

/* Antworten der Suche zwischenspeichern? USE liest gespeicherte, REFRESH holt neu und speichert */
typedef enum { ZD_CACHE_OFF, ZD_CACHE_USE, ZD_CACHE_REFRESH } ZdCache;

typedef struct {
    const char *text;           /* Suchbegriff; leer = neueste Einträge */
    ZdKind kind;
    int sources;                /* ZD_SOURCE_*, 0 = alle */
    ZdCache cache;
    int min_minutes;            /* kürzere Einträge (Trailer, Clips) ausblenden */
    bool hide_variants;         /* Fassungen mit Gebärdensprache ausblenden */
} ZdQuery;

/* Wohin und wie ein Eintrag heruntergeladen wird */
typedef struct {
    const char *dir;
    ZdQuality quality;
    bool convert;               /* nach dem Download nach AV1 umwandeln */
    ZdEncoder encoder;
    int gpu_quality, cpu_rf;
    bool keep_original;         /* MP4 nach erfolgreicher Umwandlung behalten */
    /* abgewählte Bezeichnungen (ZdTrack.label), NULL-terminiert; NULL = alles laden */
    const char *const *skip_audio, *const *skip_subtitles;
} ZdJobOptions;

typedef enum {
    ZD_JOB_WAITING,             /* Download wartet (auch: pausiert) */
    ZD_JOB_DOWNLOADING,
    ZD_JOB_CONVERT_WAITING,     /* Download fertig, Umwandlung/Zusammenführen wartet */
    ZD_JOB_CONVERTING,          /* AV1-Umwandlung oder Zusammenführen der Spuren */
    ZD_JOB_DONE,
    ZD_JOB_FAILED
} ZdJobState;

/* Momentaufnahme eines Auftrags (gehört dem Aufrufer) */
typedef struct {
    guint id;
    ZdJobState state;
    bool paused;
    bool merging;               /* CONVERTING: nur Spuren zusammenführen, kein AV1 */
    char *title, *target, *message;
    gint64 have, total;         /* Bytes */
    double speed;               /* Download: Bytes/s */
    double progress;            /* 0..1 der aktuellen Phase */
    double remaining;           /* Sekunden der aktuellen Phase, < 0 = unbekannt */
} ZdJobView;

/* Angaben von der Seite des Titels */
typedef struct {
    char *fsk;                  /* "FSK 12", NULL = keine Angabe */
    char **genres;              /* "Komödie" …, NULL-terminiert */
} ZdPageInfo;

/* Angaben von IMDb */
typedef struct {
    double rating;              /* 0 = keine Bewertung */
    int votes, year;            /* year: Erscheinungsjahr, 0 = unbekannt */
} ZdImdbInfo;

/* Stand eines Titels aus Sicht des Nutzers */
typedef enum {
    ZD_STATUS_MISSING,          /* noch nicht geladen */
    ZD_STATUS_PARTIAL,          /* angefangen, aber nicht in der Warteschlange */
    ZD_STATUS_QUEUED,           /* in der Warteschlange */
    ZD_STATUS_DONE,             /* Datei liegt vor */
    ZD_STATUS_GONE              /* laut Verlauf geladen, Datei aber nicht mehr da */
} ZdStatus;

/* ein Eintrag des Verlaufs (gehört dem Aufrufer) */
typedef struct {
    char *topic, *title, *path;
    int season, episode;        /* -1 = keine Angabe */
    gint64 size, finished;      /* Bytes, Unix-Zeit */
    bool exists;                /* liegt die Datei noch dort? */
} ZdHistoryEntry;

/* --- Start und Ende ----------------------------------------------------- */
/* persist: Warteschlange in ~/.zdf-downloader speichern und von dort laden */
void zd_init(bool persist);
/* laufende Arbeit anhalten (.part-Dateien bleiben) und Warteschlange sichern */
void zd_shutdown(void);
const char *zd_app_dir(void);
const char *zd_log_path(void);
void zd_log(const char *format, ...) G_GNUC_PRINTF(1, 2);
bool zd_have_tools(char **missing);     /* curl, ffmpeg, ffprobe vorhanden? */
bool zd_gpu_available(void);            /* AV1-Encoder per VAAPI nutzbar? (kurzer Test) */

/* --- Suche -------------------------------------------------------------- */
/* Liefert ZdTitle* (mit zd_title_free freigeben); NULL bei Fehler (*error gesetzt).
 * cancel: wird zwischen den Seiten geprüft, darf NULL sein. */
GPtrArray *zd_search(const ZdQuery *query, const volatile gint *cancel, bool *truncated,
                     char **error);
void zd_title_free(gpointer title);
const char *zd_source_label(int source);    /* "ZDF", "ARD" … */
const char *zd_title_url(const ZdTitle *title, ZdQuality quality);
gint64 zd_title_estimate(const ZdTitle *title, ZdQuality quality);  /* Bytes, geschätzt */
char *zd_target_path(const ZdTitle *title, const char *dir);        /* Pfad der MP4-Datei */

/* Titelbild der Seite website als Datei holen (blockiert); NULL = keines. Freigeben mit g_free. */
char *zd_cover_file(const char *website);
/* Altersfreigabe und Kategorien von der Seite des Titels (blockiert); NULL = nicht erreichbar */
ZdPageInfo *zd_page_info(const char *website);
void zd_page_info_free(ZdPageInfo *info);
void zd_set_cover_dir(const char *dir);     /* Ordner der Titelbilder; NULL = Standard */
char *zd_default_cover_dir(void);           /* ~/.zdf-downloader/bilder */

/* Adresse der passenden IMDb-Seite, sonst der IMDb-Suche (blockiert). Freigeben mit g_free. */
char *zd_imdb_url(const char *name, bool is_film);
/* Bewertung nachschlagen (blockiert, lädt beim ersten Mal die Datendatei von IMDb);
 * false = kein sicherer Treffer */
bool zd_imdb_info(const char *name, bool is_film, ZdImdbInfo *info);

/* --- Warteschlange ------------------------------------------------------ */
/* -> Auftragsnummer; 0 = schon in der Warteschlange */
guint zd_queue_add(const ZdTitle *title, const ZdJobOptions *options);
ZdJobView *zd_queue_snapshot(int *count);
void zd_queue_snapshot_free(ZdJobView *views, int count);
void zd_job_set_paused(guint id, bool paused);
void zd_queue_set_paused(bool paused);  /* alle offenen Aufträge */
void zd_job_retry(guint id);
void zd_job_remove(guint id);           /* bricht ab und löscht halbfertige Dateien */
void zd_queue_remove_finished(void);    /* fertige Einträge aus der Liste nehmen */
void zd_set_rate_limit(gint64 bytes_per_second);    /* 0 = unbegrenzt, gilt sofort */
void zd_set_parallel(int downloads, int conversions);

/* --- Verlauf: was schon geladen ist ------------------------------------- */
/* Prüft Warteschlange, Verlauf und den Ordner dir. path (darf NULL sein): Datei, falls da.
 * Eine Folge gilt als geladen, egal in welcher Qualität oder an welchem Speicherort. */
ZdStatus zd_title_status(const ZdTitle *title, const char *dir, char **path);
/* sortiert nach Sendung, Staffel, Folge; prüft jede Datei */
ZdHistoryEntry *zd_history_snapshot(int *count);
void zd_history_snapshot_free(ZdHistoryEntry *entries, int count);
void zd_history_forget(const char *path);   /* Eintrag löschen, die Datei bleibt */

/* --- Anzeige ------------------------------------------------------------ */
char *zd_format_size(double bytes);         /* "3,2 GB" */
char *zd_format_time(double seconds);       /* "1 h 05 min" */

#endif
