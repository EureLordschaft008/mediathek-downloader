# Mediathek Downloader 1.3.0

Lädt Filme und Serien aus den Mediatheken von ZDF, ARD und 3sat – einzelne Folgen, ganze Staffeln
oder eine Serie mit allen Staffeln auf einmal. Als Programm mit grafischer Oberfläche
(`mediathek-downloader`, Aufbau und Optik wie beim AV1-Converter) und als Terminal-Version
(`mediathek-downloader-cli`, z. B. für lange Läufe in tmux).

Bis Version 1.2 hieß das Programm ZDF-Downloader. Beim ersten Start zieht es Einstellungen, Verlauf,
Warteschlange und Zwischenspeicher aus `~/.zdf-downloader` nach `~/.mediathek-downloader` um.

## Installieren

Über die Paketquelle [VPRepo](https://github.com/EureLordschaft008/VPRepo):

```bash
sudo apt install mediathek-downloader
```

Oder direkt aus der Paketdatei:

```bash
sudo apt install ./mediathek-downloader_1.3.0-1_amd64.deb
```

Das Programm steht danach im Startmenü unter **Unterhaltungsmedien**. Es braucht `curl` und
`ffmpeg`; `apt` installiert beides mit.

## Was es kann

- **Mediatheken** – Suche und Vorschau laufen wahlweise über eine, mehrere oder alle Mediatheken.
  Aus „Alle“ heraus wählt ein Klick genau eine, weitere Klicks nehmen andere dazu. Die Auswahl gilt
  für beide Seiten und bleibt gespeichert.
- **Vorschau** – die Startseite zeigt neue Filme und Serien als Kacheln mit Titelbild. Ein Häkchen
  merkt einen Film vor, bei einer Serie alle fehlenden Folgen. Ein Klick auf die Kachel öffnet
  Bild, Beschreibung und alle Staffeln und Folgen zum einzelnen Auswählen. Die Auswahl gilt
  zusammen mit der Seite Suche; „Herunterladen“ startet alles auf einmal.
- **Kategorien** – Knöpfe über den Kacheln filtern nach Genres wie Komödie, Drama,
  Krimi oder Thriller, dazu die Wahl zwischen Filmen und Serien. Beim ersten Start sammelt das
  Programm die Kategorien aller Titel im Hintergrund, das dauert einige Minuten. Danach kommen sie
  aus dem Zwischenspeicher, nur neue Titel werden nachgeholt.
- **Details beim Überfahren** – ruht die Maus kurz auf einer Kachel, zeigt ein Popup Altersfreigabe,
  Länge, Datum, Kategorien, IMDb-Bewertung und Beschreibung. Dieselben Angaben stehen in der
  Detailansicht.
- **IMDb** – der gelbe Knopf an Filmen und Serien öffnet die IMDb-Seite im Browser. Passt kein
  Treffer sicher, öffnet sich die IMDb-Suche zum Titel, und es erscheint keine Bewertung. So zeigt
  das Programm nie die Bewertung eines falschen Titels. Das trifft vor allem deutsche Titel, die bei
  IMDb anders heißen.
- **Suche** – nach Filmen und Serien, wahlweise nur Filme oder nur Serien. Treffer stehen als Baum
  Serie → Staffel → Folge; jede Ebene hat ein Häkchen. Eine Serie oder Staffel wählt nur aus, was
  noch fehlt. Eine schon geladene Folge lässt sich nicht aus Versehen doppelt laden.
- **Was ist schon da?** – Jede Folge trägt ihren Stand: *Geladen*, *In Warteschlange*, *Angefangen*
  oder *Datei fehlt*. Serien und Staffeln zeigen „2 von 265 geladen“. Der Filter oben blendet alles
  außer den fehlenden oder den geladenen Titeln aus.
- **Bibliothek** – alles Geladene nach Sendung, mit Größe, Datum, Abspielen und Ordner öffnen. Das
  Lupensymbol sucht die fehlenden Folgen einer Serie.
- **Sprachen und Untertitel** – Gibt es mehrere Tonspuren oder Untertitel, fragt das Programm vor
  dem Download. Standard: alles laden. Alles landet zusammen in einer MKV-Datei.
- **Pause** – einzeln oder alle. Auch nach dem Beenden oder einem Absturz geht es an derselben
  Stelle weiter; offene Downloads warten beim nächsten Start pausiert auf einen Klick.
- **Tempolimit** – gilt für alle Downloads zusammen und sofort, auch mitten im Download.
- **AV1** – auf Wunsch wandelt das Programm jedes Video nach dem Download um, mit den Einstellungen
  des AV1-Converters. Das Ergebnis wird geprüft; erst dann verschwindet die MP4-Datei.
- **Speicherort** – frei wählbar, Standard ist `~/Videos/Mediathek`.
- **Zwischenspeicher** – Titelbilder, Kategorien, FSK und IMDb-Zuordnung bleiben dauerhaft liegen
  und werden nicht neu geladen. Die Film- und Folgenlisten der Vorschau gelten sechs Stunden; der
  Knopf „Vorschau neu laden“ holt sie sofort frisch. Der Ordner ist frei wählbar, Standard ist
  `~/.mediathek-downloader/bilder`. Halbfertige Downloads (`.part`) bleiben beim Speicherort.

## Wo die Dateien landen

```
<Speicherort>/Serien/Löwenzahn mit Fritz Fuchs/Staffel 45/Löwenzahn mit Fritz Fuchs - S45E11 - Tierärzte - Hilfe für Keks.mkv
<Speicherort>/Filme/Air - Der große Wurf.mkv
```

Eine Folge gilt als geladen, sobald sie im Verlauf steht und die Datei noch da ist – egal in welcher
Qualität oder an welchem Speicherort. Wird eine Datei verschoben, zeigt das Programm *Datei fehlt*;
der Eintrag lässt sich in der Bibliothek entfernen, die Folge dann neu laden.

Einstellungen, Warteschlange, Verlauf und Protokoll liegen in `~/.mediathek-downloader`.

## Terminal-Version

```bash
mediathek-downloader-cli --serien --staffel 26 --nur-fehlende "Löwenzahn"
```

```bash
mediathek-downloader-cli --serien --staffel 26 --laden alle --ziel ~/Videos/Mediathek --limit 10 --av1 gpu "Löwenzahn"
```

Ohne `--laden` zeigt das Programm nur die Trefferliste mit dem Stand jedes Titels. Strg+C pausiert;
derselbe Befehl macht dort weiter. Alle Optionen: `mediathek-downloader-cli --help`. Die Warteschlange
der Oberfläche nutzt die Terminal-Version nicht, den Verlauf schon.

## Qualität und AV1

| Stufe | Bild | Größe je 90 min |
|---|---|---|
| Hoch (Standard) | 1080p, 50 Bilder/s, H.264 | ca. 4,5 GB |
| Mittel | kleiner als 1080p | ca. 2,3 GB |
| Niedrig | für unterwegs | ca. 0,5 GB |

| | GPU (Standard) | CPU |
|---|---|---|
| Encoder | `av1_vaapi`, Qualität 64 | SVT-AV1, Preset 6, RF 35 |
| Tempo | sehr schnell | sehr viel langsamer |
| Ton, Untertitel | unverändert | unverändert |

Scheitert die GPU, übernimmt die CPU. Wird das Ergebnis nicht kleiner, bleibt die MP4-Datei.

## Grenzen

- **Kein 4K.** Die Suche liefert höchstens 1080p. 4K-Fassungen gibt es nur für wenige Titel und
  nur über die Datenquelle des ZDF selbst, die das Programm nicht nutzt.
- **Geoblocking.** Manche Titel liegen auf Adressen, die nur aus Deutschland erreichbar sind.
- **Mediathek-Fristen.** Geladen werden kann nur, was gerade online steht. Die Serie zeigt also nur
  die Folgen, die es derzeit gibt.
- **Staffeln stimmen nur so weit wie die Angaben.** Ohne `(S18/E05)` im Titel steht eine Sendung
  unter „Einzelne Sendungen“.
- **Fassungen mit Gebärdensprache** sind eigene Einträge, weil die Einblendung fest im Bild ist.
  Sie sind ausgeblendet und lassen sich in den Einstellungen einschalten.
- **Titelbilder** kommen von den Seiten der Mediatheken. Nicht jeder Titel hat eines; dann erscheint ein
  Platzhalter. Eine Serie zeigt das Bild ihrer neuesten Folge.
- **Kategorien** stammen beim ZDF von dessen Seiten. ARD und 3sat nennen keine; dort leitet das
  Programm sie aus Stichwörtern in Sendungsname und Beschreibung ab, zum Beispiel Krimi bei „Tatort“.
  Das ist gröber, und viele Titel haben dann keine Kategorie. Sie erscheinen nur unter „Alle“. Eine
  Serie übernimmt die Kategorien ihrer neuesten Folge.
- **Staffeln bei der ARD:** Nennt ein Titel nur „Folge 5“, zählt er zu Staffel 1. Serien ganz ohne
  Nummern wie der Tatort stehen unter „Einzelne Sendungen“.
- **Andere Sender:** ARTE und die Dritten sind nicht dabei, nur was unter „ARD“ geführt wird.
- **Die Suche** läuft über [MediathekViewWeb](https://mediathekviewweb.de), nicht über die Sender. Ist
  dieser Dienst nicht erreichbar, gibt es keine Treffer.

## Rechtliches

Information courtesy of IMDb (https://www.imdb.com). Used with permission. Die Bewertungen stammen
aus der [offiziellen Datendatei](https://developer.imdb.com/non-commercial-datasets/) von IMDb, die
nur für den privaten, nicht kommerziellen Gebrauch freigegeben ist. Sie wird wöchentlich neu geladen
und belegt etwa 25 MB im Zwischenspeicher.

Das Programm lädt frei zugängliche Dateien ohne Kopierschutz; es umgeht keine technischen
Schutzmaßnahmen. Kopien zum privaten Gebrauch sind nach § 53 UrhG grundsätzlich erlaubt. Die Dateien
bleiben urheberrechtlich geschützt: nicht weitergeben, nicht veröffentlichen. Das ist keine
Rechtsberatung.

## Neue Version veröffentlichen

`VERSION` im Makefile und `MD_VERSION` in `src/md_core.h` erhöhen, committen und den Tag `v<Version>`
pushen. GitHub Actions baut dann das Paket, legt das Release an und bringt es nach VPRepo.

## Aus dem Quelltext bauen

Gebraucht werden `gcc`, `make`, `libgtk-3-dev` und `dpkg-deb`.

```bash
make deb
```

| Datei | Aufgabe |
|---|---|
| `src/md_core.c` | Engine: Suche, Warteschlange, Download, Umwandlung, Verlauf |
| `src/md_gui.c` | grafische Oberfläche (GTK 3) |
| `src/md_cli.c` | Terminal-Version |
| `data/` | Starter, Symbol, Paketbeschreibung |
