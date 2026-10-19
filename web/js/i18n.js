// German/English translations. Provider values and measurements remain data.
'use strict';
// One German/English text table. Provider names, IDs and device measurements are data.
const W = Object.fromEntries(`
Positive ganze Vereinskennung von 1 bis %. Ohne Verein separat wählen.|Positive integer club ID from 1 to %. Select no club separately.
JSON-Daten sind ungültig. Eingabe prüfen und erneut versuchen.|JSON data is invalid. Check the input and retry.
Übertragung der Anfrage wurde unterbrochen. Verbindung prüfen und erneut versuchen.|The request transfer was interrupted. Check the connection and retry.
JSON-Byteposition: %|JSON byte offset: %
Alias|Alias
Gespeicherte Vereinsnamen konnten nicht gelesen werden. Unter Wettbewerbe die Namenssammlung erneut öffnen und bei Bedarf die Sicherung importieren. Anbieterwerte bleiben sichtbar.|Saved club names could not be read. Under Competitions, reopen the names collection and import its backup if needed. Provider values remain visible.
Vollständiger Alias|Complete alias
Verein aus Vorlage|Club from catalogue
Spieldetails|Match details
Letzter erfolgreicher Abruf|Last successful request
Vereinsnamen verwalten|Manage club names
Originalname|Original name
Vollständiger Originalname|Complete original name
Sprache|Language
Originalnamen des Anbieters|Provider original names
Ruft vollständige Originalnamen für den gespeicherten Wettbewerb ab. Der Abruf zählt zum Anbieterlimit. Nur eine ausdrückliche Übernahme ändert den Entwurf; Speichern übernimmt ihn dauerhaft.|Fetches complete original names for the saved competition. The request counts against provider limits. Only explicit adoption changes the draft; Save makes it persistent.
Originalnamen abrufen|Fetch original names
Geänderte Wettbewerbsquelle zuerst speichern.|Save the changed competition source first.
In Verein übernehmen|Adopt into club
Bitte wählen|Please choose
Neuer Verein|New club
Originalwerte in Entwurf übernehmen|Adopt original values into draft
Keine Teams gemeldet|No teams reported
Übersicht|Overview
Fehler|Error
Warnung|Warning
Bestätigung|Confirmation
Status erneut prüfen|Check status again
Gespeichert. Die Nachprüfung ist noch offen. Status erneut prüfen; Speichern muss nicht wiederholt werden.|Saved. Verification is pending. Check status again; there is no need to save again.
Speichern wurde nicht bestätigt. Änderungen bleiben im Entwurf.|Saving was not confirmed. Changes stay in the draft.
Status geprüft. Noch offene Änderungen können gespeichert werden.|Status checked. Remaining changes can be saved.
Wettbewerbe|Competitions
Anzeige|Display
Bilder|Images
Einstellungen|Settings
System|System
Anmelden|Log in
Abmelden|Log out
Abbrechen|Cancel
Schließen|Close
Weiter|Next
Zurück|Back
Übernehmen|Apply
Speichern|Save
Speichert …|Saving …
Verwerfen|Discard
Gespeichert|Saved
Nicht gesetzt|Not set
Ändern|Change
Entfernen|Remove
Behalten|Keep
Wird beim Speichern entfernt|Will be removed when saved
Leeres Feld behält den gespeicherten Wert.|An empty field keeps the stored value.
Liste zeigen|Show list
Liste ausblenden|Hide list
Änderungen nicht gespeichert|unsaved changes
Fehler verhindern das Speichern|errors prevent saving
Änderungen verworfen.|Changes discarded.
Ungültiger Wert|Invalid value
Wert fehlt|Value required
Ungültiger Base64-WLAN-Name|Invalid base64 Wi-Fi name
WLAN-Name als Base64: höchstens 32 Originalbytes. Base64 direkt bearbeiten oder ein gefundenes Netzwerk auswählen.|Wi-Fi name in base64: at most 32 original bytes. Edit base64 directly or choose a discovered network.
Wert außerhalb des erlaubten Bereichs|Value outside the allowed range
Zu viele Einträge|Too many entries
Text ist zu lang|Text is too long
Wert wird benötigt|Value required
Doppelter Eintrag|Duplicate entry
Werte passen nicht zusammen|Values conflict
Mindestens einen Bildschirm einschalten|Enable at least one screen
Falscher Datentyp|Wrong value type
Unbekanntes Feld|Unknown field
Erlaubter Bereich|Allowed range
Höchstens % Byte UTF-8|At most % UTF-8 bytes
Gerät nicht erreichbar. Es wird erneut versucht …|Device unreachable. Retrying …
Erneut versuchen|Retry
Schema|Schema
Status|Status
Sitzung|Session
Zum Ändern anmelden|Log in to make changes
Passwort|Password
Passwort wurde abgelehnt.|Password rejected.
Zugriff über diese Adresse nicht erlaubt. Öffne die lokale Geräteadresse.|Access through this address is not allowed. Open the local device address.
Das Gerät testet ein Update. Änderungen sind nach dem Test wieder möglich.|The device is testing an update. Changes are available after the test.
Das Gerät ist beschäftigt. Warte kurz und versuche es erneut.|The device is busy. Wait briefly and retry.
Der Abruf wurde durch eine andere Anfrage geändert. Erneut versuchen.|Another request changed the result. Retry.
Speicher reicht nicht aus.|Not enough memory.
Bildspeicher ist voll.|Image storage is full.
Datei oder Wert ist zu groß.|File or value is too large.
Datei passt nicht zum Gerät oder ist beschädigt.|File is incompatible with this device or damaged.
Zeitüberschreitung. Erneut versuchen.|Operation timed out. Retry.
Nicht verfügbar|Unavailable
Aktion momentan nicht möglich.|Action is not possible right now.
Verbindung fehlgeschlagen.|Connection failed.
Antwort des Geräts ist nicht lesbar.|Device response is unreadable.
Automatisch|Automatic
Hell|Light
Dunkel|Dark
Verbunden|Connected
Einrichtung|Setup
Verbinde …|Connecting …
Kein WLAN|No WiFi
Netz nicht gefunden|Network not found
Passwort falsch?|Wrong password?
Setup-Netz aktiv|Setup network on
WLAN getrennt|WiFi disconnected
Eigenes Spiel|Your match
Konferenz|Conference
Tabelle|Table
Wappen und Diashow|Crest and slideshow
Display jetzt|Display now
Daten werden geladen|Loading data
Wartet auf Anfragelimit bis %|Waiting for the request limit until %
Noch keine Daten|No data yet
Keine eigenen Spiele verfügbar|No matches for your club available
Anstoß|Kickoff
Unbekannt|Unknown
Geplant|Scheduled
Live|Live
Halbzeit|Half-time
Verlängerung|Extra time
Elfmeterschießen|Penalties
Beendet|Finished
Verschoben|Postponed
Abgesagt|Cancelled
Abgebrochen|Abandoned
Unterbrochen|Suspended
Daten abrufen|Refresh data
Nächster Bildschirm|Next screen
IP-Adresse anzeigen|Show IP address
Abruf vorgemerkt.|Refresh queued.
Aktion an das Display gesendet.|Action sent to the display.
Datenstand|Data state
Datenalter|Data age
Aktuell|Current
Veraltet|Stale
Abruf läuft|Refresh in progress
Nächster Abruf|Next refresh
jetzt|now
in % s|in % s
um %|at %
Spiel läuft|Match live
Vor dem Anpfiff|Before kickoff
Nach dem Abpfiff|After the final whistle
Kein Spiel im Zeitfenster|No match window open
Spielfenster beginnt|Match window opens
Tageslimit knapp: Abrufe seltener oder ausgelassen|Daily limit tight: requests less often or skipped
Tageslimit erreicht|Daily limit reached
Neuer Versuch nach einem Fehler|Retry after an error
Wartet auf WLAN und Uhrzeit|Waiting for WiFi and time
Update-Test läuft|Update test running
Spiele|matches
Quelle|Source
Gespeicherte Einstellungen waren ungültig. Das Gerät nutzt Startwerte; Speichern überschreibt die gespeicherten Einstellungen.|Stored settings were invalid. The device uses initial values; saving overwrites the stored settings.
Gespeicherte Einstellungen sind beschädigt. Das Gerät nutzt Startwerte; Speichern überschreibt die gespeicherten Einstellungen.|Stored settings are damaged. The device uses initial values; saving overwrites the stored settings.
Gespeicherte Einstellungen konnten nicht gelesen werden. Das Gerät nutzt Startwerte; Speichern ist bis zum Zurücksetzen gesperrt.|Stored settings could not be read. The device uses initial values; saving is blocked until a reset.
Einstellungsspeicher nicht verfügbar. Das Gerät nutzt Startwerte; Änderungen werden nicht gespeichert.|Settings storage unavailable. The device uses initial values; changes are not saved.
Gespeicherte Einstellungen überschreiben?|Overwrite stored settings?
Überschreiben und speichern|Overwrite and save
Einrichtung abschließen|Complete setup
WLAN unter Einstellungen auswählen und speichern.|Select and save WiFi under Settings.
Wettbewerb und Verein auswählen und speichern.|Select and save a competition and club.
Auf gültige Uhrzeit und den ersten Datenabruf warten.|Wait for a valid clock and the first data refresh.
Zeit noch ungültig|Clock not valid yet
Tagesbudget verbraucht.|Daily request budget used up.
WLAN-Verbindung verloren.|WiFi connection lost.
Wettbewerb oder Saison beim Anbieter nicht gefunden.|Competition or season not found at the provider.
Verbindung zum Anbieter fehlgeschlagen.|Connection to provider failed.
Anbieterantwort unvollständig oder unlesbar.|Provider response incomplete or unreadable.
Anbieterantwort zu groß für das Gerät.|Provider response too large for the device.
Anbieter nicht aktiviert.|Provider not enabled.
Schlüssel oder Freigabe unter Einstellungen speichern.|Save the key or opt-in under Settings.
Neue Abrufe ab 0:00 UTC; Tageslimit des Anbieters unter Einstellungen prüfen.|Requests resume at 0:00 UTC; check the provider daily limit under Settings.
Gerät war zu lange beschäftigt.|Device was busy for too long.
Kurz warten und erneut versuchen.|Wait briefly and try again.
WLAN-Empfang prüfen.|Check the WiFi signal.
Uhrzeit noch ungültig.|Clock not valid yet.
Internetzugang prüfen; die Uhrzeit kommt vom Zeitserver.|Check internet access; the clock comes from the time server.
Einstellungen während des Abrufs geändert.|Settings changed during the request.
Erneut versuchen.|Try again.
Anbieter begrenzt Anfragen.|Provider is rate limiting requests.
Später erneut versuchen.|Try again later.
Anbieter hat den Zugang abgelehnt.|Provider denied access.
Gespeicherten Schlüssel unter Einstellungen prüfen.|Check the saved key under Settings.
Wettbewerb bearbeiten und neu auswählen.|Edit the competition and select it again.
Anbieter hat einen Fehler gemeldet.|Provider returned an error.
Später erneut versuchen; bleibt der Fehler, einen anderen Anbieter wählen.|Try again later; if it persists, choose another provider.
Internetzugang prüfen.|Check internet access.
Bleibt der Fehler, einen anderen Anbieter wählen.|If it persists, choose another provider.
Einen anderen Anbieter für diesen Wettbewerb wählen.|Choose another provider for this competition.
Bleibt der Fehler, Gerät neu starten und Fehlercode melden.|If it persists, restart the device and report the error code.
Abruf fehlgeschlagen.|Request failed.
Bleibt der Fehler, Fehlercode melden.|If it persists, report the error code.
Anbieter trägt keine Ergebnisse ein.|Provider does not enter results.
Anbieter meldet keine laufende Saison.|Provider reports no current season.
Saison im Wettbewerb prüfen oder auf die neue Saison warten.|Check the competition season or wait for the new season.
Abrufzähler konnte nicht gespeichert werden.|Request counter could not be stored.
Gerät neu starten; bleibt der Fehler, Fehlercode melden.|Restart the device; if it persists, report the error code.
Fehlercode|Error code
Stand|As of
Gestört|Failing
Nicht gepflegt|Not maintained
Unvollständig|Incomplete
Nicht alle Daten passen in den Gerätespeicher.|Not all data fit into the device memory.
Weniger Wettbewerbe oder Wettbewerbe mit weniger Mannschaften auswählen.|Select fewer competitions or competitions with fewer teams.
Betroffen|Affected
Spielliste|Match list
Mannschaftsnamen|Team names
Spielereignisse|Match events
Andere aktivierte Anbieter:|Other enabled providers:
Unter Einstellungen einen weiteren Anbieter aktivieren.|Enable another provider under Settings.
Aktion fehlgeschlagen. Seite neu laden und erneut versuchen.|Action failed. Reload the page and try again.
WLAN-Suche fehlgeschlagen. Erneut suchen; bleibt der Fehler, Gerät neu starten.|WiFi scan failed. Scan again; if it persists, restart the device.
Updatezustand ungeklärt.|Update state unresolved.
Update fehlgeschlagen.|Update failed.
Die gewählte Firmware läuft bereits; kein Neustart erkannt.|The chosen firmware is already running; no restart detected.
Version unter System prüfen; für ein Update eine andere Firmware-Datei wählen.|Check the version under System; choose another firmware file for an update.
Nach dem Neustart läuft eine andere als die gewählte Firmware.|After the restart a different firmware than the chosen one is running.
Version unter System prüfen und Update wiederholen.|Check the version under System and repeat the update.
Update nicht innerhalb von 2 Minuten abgeschlossen: kein Neustart oder keine Antwort des Geräts.|Update not completed within 2 minutes: no restart or no answer from the device.
Seite neu laden, Version unter System prüfen und Update bei Bedarf wiederholen.|Reload the page, check the version under System and repeat the update if needed.
Neue Firmware läuft noch im Test; die Bestätigung blieb aus.|The new firmware is still in its test; the confirmation did not arrive.
Einige Minuten warten und Seite neu laden.|Wait a few minutes and reload the page.
Bildspeicher formatiert; die bereinigten Bildverweise konnten nicht gespeichert werden. Einstellungen speichern, damit die Verweise entfernt werden.|Image storage was formatted; the cleaned image references could not be saved. Save the settings to remove the references.
Wettbewerb hinzufügen|Add competition
Bearbeiten|Edit
Löschen|Delete
Nach oben|Move up
Nach unten|Move down
Aktiv|Active
Die Wettbewerbsreihenfolge bestimmt den Vorrang eigener Spiele und die Reihenfolge innerhalb jeder Sportart.|Competition order determines own-match priority and the order within each sports type.
Bildschirme dieses Wettbewerbs|Screens of this competition
Auf keinem Bildschirm|On no screen
„Eigenes Spiel“ braucht einen Verein.|“Your match” needs a club.
Zeigt je Wettbewerb mit eingeschaltetem „Spieltag“ einen Spieltag mit Ergebnis oder Anstoßzeit, in der Reihenfolge unter „Wettbewerbe“. Die Seiten wechseln alle 8 s, danach folgt der nächste Wettbewerb. Spiele je Seite wie „Spiele gleichzeitig“ bei „Konferenz“. Hoch und Runter blättern seitenweise, nach der ersten oder letzten Seite zum vorigen oder nächsten gespeicherten Spieltag, dann zum vorigen oder nächsten Wettbewerb.|Shows one matchday of every competition with “Matchday” switched on, with result or kickoff time, in the order under “Competitions”. Pages change every 8 s, then the next competition follows. Matches per page as “Matches visible at once” under “Conference”. Up and Down page; past the first or last page they move to the previous or next stored matchday, then to the previous or next competition.
Gilt auch für „Spieltag“ (Spiele je Seite).|Also used by “Matchday” (matches per page).
Tabelle weiterschalten nach|Advance the table after
Ohne eigenen Verein blättert die Tabelle nach dieser Zeit eine Seite weiter; nach der letzten Seite folgt die Tabelle des nächsten Wettbewerbs. Mit eigenem Verein folgt nach dieser Zeit der nächste Wettbewerb.|Without an own club the table turns a page after this time; after the last page the next competition's table follows. With an own club the next competition follows after this time.
Saison|Season
Keine|None
Kurznamen|Short names
Höchstens % Bytes.|At most % bytes.
Anbieter|Provider
Wettbewerb|Competition
Verein|Club
Zusammenfassung|Summary
Ohne Schlüssel, Spiele und Tabellen; keine Live-Minuten.|No key, fixtures and tables; no live minutes.
Schlüssel erforderlich, Spiele, Tabellen und Live-Angaben; Tagesbudget.|Key required, fixtures, tables and live details; daily budget.
Inoffizielle Schnittstelle; Freigabe erforderlich, Live-Angaben je Wettbewerb.|Unofficial interface; opt-in required, live details vary by competition.
Schlüssel erforderlich, Spiele und Tabellen; Live-Angaben je Tarif.|Key required, fixtures and tables; live details vary by plan.
Unter Einstellungen aktivieren und speichern.|Enable and save under Settings.
Weitere laden|Load more
Listen laden|Load lists
Suchen|Search
Tippen filtert die geladene Liste; Enter oder „Suchen“ durchsucht die ganze Liste des Anbieters.|Typing filters the loaded list; Enter or “Search” searches the provider's whole list.
Suche|Search
Land|Country
Keine Treffer.|No results.
Wettbewerb-ID|Competition ID
Verein-ID|Club ID
Experte|Expert
Saison überschreiben|Override season
Leer: laufende Saison. Eine andere Saison wird mit dem Wettbewerb gespeichert.|Empty: current season. Another season is saved with the competition.
laufende|current
Leer: laufende Saison, das Gerät ermittelt sie selbst.|Empty: current season, determined by the device.
Automatik|Automatic
Wenn kein Spiel läuft|Outside matchday
Wenn andere Spiele laufen|Other matches active
Wenn das eigene Spiel läuft|Your match active
Nach Tastendruck zurück zur Automatik|Return to automatic selection after button press
0 = nie zurück.|0 = never return.
Vorlauf vor dem ersten Anstoß|Before first kickoff
Nachlauf nach dem letzten Abpfiff|After final whistle
Bildschirme|Screens
Anzeigen|Enabled
Hintergrundbild|Background image
Bild|Image
Standard-Hintergrund|Default background
Standard-Wappen|Default crest
Farben|Colours
Grundfarbe|Base colour
Text|Text
Akzent|Accent
Textgröße|Text size
Spiele gleichzeitig|Matches visible at once
Tabellenzeilen|Table rows
Bildwechsel|Slide interval
Das Wappen erscheint auf dem Bildschirm „Wappen und Diashow“ und kann auch als Bild in der Diashow stehen. Das Wappen selbst ersetzt du unter „Bilder“. Reihenfolge = Abspielfolge.|The crest appears on the “Crest and slideshow” screen and can also be a slideshow image. Replace the crest itself under “Images”. Order = playback order.
Wappen|Crest
Zur Diashow hinzufügen|Add to slideshow
Leere Diashow zeigt das Wappen.|An empty slideshow shows the crest.
Helligkeit und Ausrichtung|Brightness and orientation
Ausrichtung|Orientation
Helligkeit der Displaybeleuchtung von 0 bis 100 %. 0 schaltet die Beleuchtung aus.|Display backlight brightness from 0 to 100 %. 0 switches the backlight off.
Ändert im eingestellten Zeitfenster ausschließlich die Displaybeleuchtung. Farben, Hintergrund und Bilder bleiben unverändert. Benötigt eine gültige Gerätezeit.|Changes only the display backlight during the configured time window. Colours, background and images stay unchanged. Requires a valid device clock.
Täglicher Beginn und Ende in der lokalen Gerätezeit. Die Startzeit ist eingeschlossen, die Endzeit nicht. Gleiche Zeiten ergeben kein Nachtfenster.|Daily start and end in the device's local time. The start is included, the end is excluded. Equal times give an empty night window.
Tageshelligkeit|Day brightness
Nachtmodus|Night mode
Nacht ab|Night starts
Nacht bis|Night ends
Abdunkeln auf|Dim to
Drehung|Rotation
IP-Adresse auf dem Display|IP address on display
Nach dem Start einblenden für|Show after startup for
0 = dauerhaft.|0 = permanent.
Unten statt oben|Bottom instead of top
Im Einrichtungsmodus dauerhaft zeigen|Show permanently in setup mode
Tastenverhalten beim Blättern|Button scrolling behaviour
Scroll-Schritt bei gehaltener Taste|Scroll step while button held
Scroll-Richtung zurücksetzen nach|Reset scroll direction after
Bilder liegen auf dem Gerät und können mehrfach verwendet werden. Verwendungen werden unter Anzeige gespeichert. Fehlende Dateien behalten ihren Bildplatz, solange sie ausgewählt sind.|Images are stored on the device and can be reused. Save their uses under Display. Missing files keep their image slot while selected.
belegt|used
von|of
Höchstens % je Bild|At most % per image
Nicht verwendet|Unused
Bild %|Image %
Datei fehlt|File missing
Dateien|Files
Freie Bildplätze: %|Free image slots: %
Keine freien Bildplätze. Bilder ersetzen oder löschen.|No free image slots. Replace or delete images.
Ersetzen|Replace
Neues Bild|New image
Foto oder Grafik zuschneiden|Crop a photo or graphic
Standardbild|Stock image
Wird verwendet als|Used as
Danach erscheint dort das Standardbild. Die Datei wird sofort gelöscht.|The stock image will be used there. The file is deleted immediately.
Bild gelöscht.|Image deleted.
Bild gespeichert. Verwendung unter Anzeige festlegen.|Image saved. Set its uses under Display.
eingebaut|built in
Ersetzt · %|Replaced · %
Aus Bibliothek übernehmen|Take from library
Werksbild wiederherstellen|Restore factory image
Wiederherstellen|Restore
Das Bild wird auf dem Gerät kopiert und zählt zum Speicher. Die Bibliothek bleibt unverändert.|The image is copied on the device and counts towards the storage. The library stays unchanged.
Danach erscheint wieder das Werksbild. Die eigene Datei wird sofort gelöscht.|The factory image is shown again. Your file is deleted immediately.
Standardbild ersetzt.|Stock image replaced.
Werksbild wiederhergestellt.|Factory image restored.
Bild bearbeiten|Image editor
Zurück zur Übersicht|Back to gallery
Zuschnitt in Geräteauflösung. Ziehen verschiebt, Mausrad oder zwei Finger zoomen.|Crop at device resolution. Drag to move; mouse wheel or two fingers zoom.
Datei|File
Zoom|Zoom
Einpassen|Fit
Füllen|Fill
Deckkraft|Opacity
Beispieltext einblenden (wird nicht gespeichert)|Show sample text (not saved)
Datei kann nicht gelesen werden. Das bisherige Bild bleibt erhalten.|File cannot be decoded. The previous image is kept.
Wähle eine Bilddatei.|Choose an image file.
Größe wird berechnet …|Calculating size …
JPEG-Qualität|JPEG quality
Frei für dieses Bild|Available for this image
Bild auch bei Qualität 30 % zu groß. Weniger Details, stärker zoomen oder Deckkraft senken.|Image is too large even at 30% quality. Use fewer details, zoom further or reduce opacity.
Bild überschreitet die Speicherquote einschließlich Reserve.|Image exceeds storage quota including reserve.
Hochladen|Upload
WLAN|WiFi
WLAN-Name|WiFi name
WLAN-Passwort|WiFi password
Netzwerke suchen|Scan networks
Suche läuft …|Scanning …
Offen|Open
Keine Netzwerke gefunden.|No networks found.
WLAN-Name übernommen. Passwort prüfen, dann speichern.|WiFi name selected. Check the password, then save.
Gerätename|Hostname
Passwort für das Setup-WLAN|Setup WiFi password
Externe Antenne verwenden|Use external antenna
Nur aktivieren, wenn eine Antenne am Stecker angeschlossen ist.|Enable only if an antenna is connected to the socket.
Datenanbieter|Data providers
Schlüssel|Key
Höchstens Abrufe pro Tag|Maximum requests per day
ESPN verwenden|Use ESPN
ESPN ist eine inoffizielle Schnittstelle. Freigabe erfolgt ausdrücklich.|ESPN is an unofficial interface. Enabling it is an explicit opt-in.
Zeit und Sprache|Time and language
Zeitzone|Time zone
Zeitserver|Time server
Sprache von Display und Oberfläche|Display and interface language
Deutsch|German
Zugang|Access
Ohne Admin-Passwort kann jeder im WLAN Einstellungen ändern. Lesen bleibt mit Passwort möglich.|Without an admin password anyone on WiFi can change settings. Reading stays available with a password.
Admin-Passwort|Admin password
Demo-Modus|Demo mode
Zeigt erfundene Spiele statt echter Daten.|Shows fictional matches instead of real data.
Status regelmäßig in die serielle Konsole schreiben|Write status regularly to serial console
Statusintervall|Status interval
Live-Protokoll im Browser erlauben|Enable live browser diagnostics
Demo und Protokolle|Demo and diagnostics
Firmware|Firmware
Version|Version
Gerät|Device
Einstellungsformat|Settings format
Build-Kennung|Build identity
Update installieren|Install update
Passende firmware.bin wählen. Profil, Speicherlayout, Format und Größe werden geprüft; die Firmware entscheidet abschließend.|Choose compatible firmware.bin. Profile, layout, format and size are checked; firmware has the final say.
Datei geprüft|File checked
Update|Update
Übertragen|Transferring
Gerät startet neu, warte auf Antwort …|Device restarting, waiting for response …
Neue Firmware läuft im Test. Bestätigung abwarten.|New firmware is in trial. Waiting for confirmation.
Update bestätigt.|Update confirmed.
Gerät ist zur vorherigen Firmware zurückgekehrt.|Device returned to the previous firmware.
Sicherung|Backup
Die Sicherung enthält Einstellungen, Wettbewerbe, Anzeigeprogramme und die vollständige Namenssammlung; Bilder sind nicht enthalten.|The backup contains settings, competitions, screen programmes and the complete names collection; images are not included.
Passwörter und Schlüssel einschließen|Include passwords and keys
Diese Datei enthält Geheimnisse. Sicher aufbewahren.|This file contains secrets. Store it securely.
Einstellungen exportieren|Export settings
Einstellungen importieren|Import settings
Neustart|Restart
Gerät neu starten|Restart device
Das Display ist kurz aus. Die Seite verbindet sich danach erneut.|The display goes dark briefly. The page reconnects afterwards.
Gerät ist wieder erreichbar.|Device is reachable again.
Wartung|Maintenance
Werkseinstellungen unvollständig; Daten können bereits geändert sein. Erneut zurücksetzen. Bei unlesbarem Speicher die vollständige Formatierung auswählen.|Factory reset incomplete; data may already have changed. Retry reset. Select full formatting if storage is unreadable.
Einstellungsspeicher|Settings storage
Abrufzähler|Request counters
Formatierung|Formatting
Eigene Bilder|User images
Gespeichert wird der UTF-8-Präfix: „%“.|The UTF-8 prefix saved will be: “%”.
Bildspeicher reparieren|Repair image storage
Vollständige Reparatur erfordert bestätigte Werkseinstellungen. WLAN, Zugang, Abrufzähler, gespeicherte Vereinsnamen und alle Dateien werden gelöscht. Standardbilder benötigen danach USB-uploadfs.|Full repair requires a confirmed factory reset. WiFi, access, request counters, saved club names and all files are deleted. Stock images need USB uploadfs afterwards.
Reparieren|Repair
Bildspeicher repariert. Standardbilder mit USB-uploadfs wiederherstellen.|Image storage repaired. Restore stock images via USB uploadfs.
Alle eigenen Bilder löschen|Delete all user images
Jede Datei der Bibliothek und jede Auswahl einer fehlenden Datei wird einzeln gelöscht. Wappen und Standard-Hintergrund zeigen danach wieder die Werksbilder. Teilfehler bleiben sichtbar.|Each library file and each selection of a missing file is deleted individually. Crest and default background show their factory images again. Partial failures remain visible.
Werkseinstellungen|Factory reset
Zurücksetzen|Reset
Löscht Einstellungen einschließlich WLAN und Zugang, Abrufzähler und gespeicherte Vereinsnamen. Danach startet der Einrichtungsmodus. Die unveränderliche Namensvorlage der Firmware bleibt verfügbar.|Deletes settings including WiFi and access, request counters and saved club names. Setup mode starts afterwards. The immutable firmware names preset remains available.
Eigene Bilder ebenfalls löschen|Also delete user images
Dateispeicher vollständig formatieren (auch Standardbilder)|Format file storage completely (including stock images)
Zum Bestätigen Gerätenamen eingeben|Type hostname to confirm
Nach dem Reset mit dem Setup-WLAN verbinden und diese Adresse öffnen.|After reset, join setup WiFi and open this address.
Diagnose|Diagnostics
Gerätewerte|Device values
Uhrzeit gültig|Clock valid
Signalstärke|Signal strength
Läuft seit|Uptime
Letzter Neustart|Last reset
Freier interner Speicher|Free internal memory
Kleinster freier Speicher seit Start|Minimum free memory since boot
Kleinster freier Speicher im Messintervall|Minimum free memory in measurement interval
Kleinster freier Speicher|Minimum free memory
Firmware-Speicherplatz|Firmware storage
Größter freier Block|Largest free block
Bildspeicher|Image storage
Abrufe seit Neustart|Requests since restart
Update-Test|Update trial
Aufgaben und freie Stack-Minima|Tasks and minimum free stack
Live-Protokoll|Live diagnostics
Unter Debug aktivieren und speichern.|Enable under Debug and save.
Stream geschlossen. Erneut öffnen.|Stream closed. Reopen it.
Stream verbinden|Connect stream
WLAN oder Gerätename geändert|WiFi or hostname changed
Die Funkverbindung wird neu aufgebaut. Falls die Seite getrennt wird, öffne eine der folgenden Adressen.|The wireless connection restarts. If disconnected, open one of these addresses.
Speichern und verbinden|Save and reconnect
Änderung|Change
Geheimnis wird geändert|Secret will be changed
Geheimnis wird entfernt|Secret will be removed
Anmeldung abgebrochen.|Login cancelled.
Maximale Listenlänge erreicht.|Maximum list length reached.
Saison: nur Ziffern, außer OpenLigaDB genau vier.|Season: digits only; exactly four except OpenLigaDB.
IDs: ohne Steuerzeichen und ohne Leerzeichen am Anfang oder Ende.|IDs: no control characters and no space at the start or end.
WLAN-Passwort: 8–63 ASCII-Zeichen oder 64 Hex-Ziffern; leer für offenes WLAN.|WiFi password: 8–63 ASCII characters or 64 hex digits; empty for open WiFi.
Setup-Passwort: 8–63 ASCII-Zeichen; leer für offenes WLAN.|Setup password: 8–63 ASCII characters; empty for open WiFi.
Gerätename: Buchstaben, Ziffern und Bindestrich, ohne Bindestrich am Rand.|Hostname: letters, digits and hyphens, with no hyphen at either end.
Zeitserver: gültige DNS-Namen oder IPv4-Adresse.|Time server: a valid DNS name or IPv4 address.
API-Schlüssel: nur sichtbare ASCII-Zeichen ohne Leerzeichen.|API key: visible ASCII characters without spaces.
Daten werden geladen. Der erste Abruf kann länger dauern.|Loading data. The first refresh can take longer.
Netzwerkänderung gespeichert. Neue Adresse öffnen, wenn die Verbindung abbricht.|Network change saved. Open the new address if disconnected.
Bildvorschau nicht verfügbar.|Image preview unavailable.
Standardbilder benötigen danach USB-uploadfs.|Stock images need USB uploadfs afterwards.
Stromversorgung eingeschaltet|Power on
Externer Neustart-Pin|External reset pin
Software-Neustart|Software restart
Programmabsturz|Program crash
Überwachung der Unterbrechungen|Interrupt watchdog
Überwachung der Aufgaben|Task watchdog
Weitere Überwachung|Other watchdog
Aufwachen aus Tiefschlaf|Wake from deep sleep
Versorgungsspannung zu niedrig|Supply voltage too low
Fehler im dauerhaften Chip-Speicher|eFuse error
Störung der Stromversorgung|Power glitch
Prozessor blockiert|CPU lockup
Hintergrundbilder auf allen Bildschirmen|Background images on all screens
Eigenes Spiel in der Konferenz anheften|Pin your match in the conference
Schematische Vorschau aus den Gerätedaten, kein Bildschirmfoto.|Schematic preview from the device data, not a screenshot.
Die Statuspille unten zeigt das Gerät nur % s nach jeder Änderung.|The device shows the status pill at the bottom only for % s after each change.
Beispielwerte, weil das Gerät sie nicht meldet: %.|Example values because the device does not report them: %.
Bilddatei fehlt.|Image file is missing.
Bild nicht lesbar.|Image unreadable.
Bilddatei nicht nutzbar.|Image file unusable.
Bildprüfung hat zu lange gedauert.|Image validation took too long.
Bildquelle konnte nicht übernommen werden.|Image source could not be applied.
Bild konnte beim Zeichnen nicht gelesen werden.|Image could not be read while drawing.
Bild kann nicht angezeigt werden.|Image cannot be displayed.
Bibliotheksbild|Library image
Ersatzdatei|Replacement file
Werksbild|Factory image
Kein Bild|No image
Ersatz aktiv|Fallback active
Kein verwendbares Bild; Fußballinhalte bleiben sichtbar.|No usable image; football content remains visible.
Unter Bilder das gewünschte Bild ersetzen oder das Werksbild wiederherstellen.|Under Images, replace the desired image or restore the factory image.
Werksbilder mit USB-uploadfs wiederherstellen; dabei können eigene Bilder verloren gehen.|Restore factory images with USB-uploadfs; this can erase your own images.
Web prüfen|Check Web UI
Im Web Fehler prüfen|Check error in Web
Bild ersetzen|Replace image
Bilddarstellung|Image display
Wird geprüft|Being checked
Kein verwendbares Bild|No usable image
Bild nicht verfügbar|Image unavailable
In Weboberfläche ersetzen|Replace in Web UI
Gerät neu starten. Bleibt der Fehler, Fehlercode melden.|Restart the device. If the error remains, report the error code.
Ereigniszeile|Event line
Spielzeilen|Match rows
Startbildschirm|Start screen
Name des Setup-WLANs|Setup WiFi name
Spieltag|Matchday
Von Hand gewählt · zurück zur Automatik % s nach dem letzten Tastendruck.|Selected manually · back to automatic % s after the last button press.
Von Hand gewählt · bleibt bis zum nächsten Tastendruck.|Selected manually · stays until the next button press.
Einrichtung: Es ist noch kein aktiver Wettbewerb gespeichert.|Setup: no active competition is saved yet.
Demo-Modus: Das eigene Spiel wird simuliert.|Demo mode: your match is simulated.
Das eigene Spiel läuft oder liegt im Vorlauf oder Nachlauf.|Your match is running or within the time before or after it.
Spiele der Wettbewerbe laufen oder liegen im Vorlauf oder Nachlauf.|Matches of the competitions are running or within the time before or after them.
Kein Spiel läuft.|No match is running.
Nächstes eigenes Spiel: %|Your next match: %
ausgeschaltet|off
Dieser Bildschirm ist ausgeschaltet. Das Display zeigt stattdessen „%“.|This screen is off. The display shows “%” instead.
Zeigt das Wappen und die Bilder der Diashow (unten).|Shows the crest and the slideshow images (below).
Oben für alle Bildschirme ausgeschaltet. Die Auswahl bleibt erhalten.|Switched off above for all screens. The selection is kept.
Zeigt das eigene Spiel auf jeder Seite der Konferenz als erste Zeile, solange es angezeigt wird. Ausgeschaltet steht es als normale Zeile in der Akzentfarbe zwischen den anderen Spielen.|Shows your match as the first row on every page of the conference while it is shown. When off, it appears as a normal row in the accent colour among the other matches.
Zur Auswahl stehen nur eingeschaltete Bildschirme.|Only screens that are switched on can be selected.
Für das eigene Spiel gilt die nächste Auswahl.|Your own match uses the next choice.
Schaltet die Hintergrundbilder von „Eigenes Spiel“, „Konferenz“, „Spieltag“ und „Tabelle“ gemeinsam ein oder aus. Ausgeschaltet zeigen diese Bildschirme nur die Grundfarbe; die Auswahl je Bildschirm bleibt erhalten.|Switches the background images of “Your match”, “Conference”, “Matchday” and “Table” on or off together. When off, these screens show only their base colour; each screen keeps its selection.
Schematische Vorschau mit Beispielwerten in den Farben und mit dem Bild des Bildschirms.|Schematic preview with example values in the screen's colours and image.
Diashow ist voll (%). Entferne zuerst ein Bild.|Slideshow is full (%). Remove an image first.
Höchstens % Wettbewerbe. Entferne zuerst einen.|At most % competitions. Remove one first.
„%“ entfernen?|Remove “%”?
Wirksam nach „Speichern“.|Takes effect after “Save”.
Wähle einen aktivierten Anbieter.|Choose an enabled provider.
Wähle zuerst einen Wettbewerb aus der Liste oder gib unter „Experte“ eine ID ein.|Choose a competition from the list first, or enter an ID under “Expert”.
Wähle zuerst einen Verein aus der Liste oder gib unter „Experte“ eine ID ein.|Choose a club from the list first, or enter an ID under “Expert”.
Höchstens so viele Abrufe je Tag; der Tag wechselt um 00:00 UTC. Reicht der Rest nicht bis zum Tageswechsel, ruft das Gerät zuerst Live-Tabelle, Tabelle, Spielplan und Live-Stände seltener oder gar nicht ab, das eigene Spiel zuletzt; sind die Abrufe aufgebraucht, ruft es den Anbieter erst nach dem Tageswechsel wieder ab. 0 = keine Tagesgrenze.|At most this many requests per day; the day changes at 00:00 UTC. If the remaining requests would run out before midnight, the device reduces or pauses live-table, table, fixture and other live-score requests in that order, preserving requests for your match longest. Once the daily cap is reached, requests resume after midnight UTC. 0 = no daily cap.
Nur zur Fehlersuche: Das Gerät schreibt seinen Zustand über das USB-Kabel an einen angeschlossenen Computer.|For troubleshooting only: the device writes its state over the USB cable to a connected computer.
Abstand zwischen zwei Statuszeilen.|Interval between two status lines.
Nur zur Fehlersuche: Erlaubt unter „System“ das Live-Protokoll des Geräts im Browser. Ausgeschaltet lehnt das Gerät diese Verbindung ab.|For troubleshooting only: allows the live device log in the browser under “System”. When off, the device refuses this connection.
So viele Minuten vor dem Anstoß gilt ein Spiel als laufend: Das Display wechselt automatisch, und das Gerät ruft häufiger ab.|For this many minutes before kick-off a match counts as running: the display switches automatically and the device fetches more often.
So viele Minuten nach dem Abpfiff gilt ein Spiel noch als laufend.|For this many minutes after the final whistle a match still counts as running.
Nur Tabelle: Abstand der Blätterschritte, solange eine Taste gehalten wird.|Table only: interval between scroll steps while a button is held.
So viele Sekunden nach dem letzten Richtungswechsel blättert eine Blättern-Taste wieder abwärts. 0 = nie zurücksetzen.|This many seconds after the last change of direction, a scroll button scrolls down again. 0 = never reset.
Konferenz-Modus|Conference mode
Angezeigter Spieltag|Matchday shown
Aktueller Spieltag|Current matchday
Nächster Spieltag|Next matchday
Aktueller Spieltag: der laufende oder als Nächstes beginnende Spieltag; ein beendeter bleibt bis zum Ende des Folgetags stehen. Nächster Spieltag: wechselt schon um Mitternacht nach dem letzten Anstoß zum folgenden Spieltag; ein Wettbewerb ohne gespeicherten kommenden Spieltag wird übersprungen. Zu einem Spieltag gehören Spiele, deren Anstöße weniger als 48 Stunden auseinanderliegen.|Current matchday: the running or next starting matchday; a finished one stays until the end of the following day. Next matchday: moves on to the following matchday at midnight after the last kickoff; a competition without a stored upcoming matchday is skipped. A matchday holds matches whose kickoffs are less than 48 hours apart.
Alle Seiten|All pages
Eine Seite|One page
Alle Seiten: alle Spiele in Anstoßreihenfolge; die Seiten wechseln alle 8 s. Eine Seite: laufende Spiele mit dem jüngsten Tor zuerst, torlose nach Anstoß, dann beendete, dann nicht begonnene Spiele.|All pages: all matches in kick-off order; pages change every 8 s. One page: running matches with the latest goal first, goalless ones by kick-off, then finished, then not started matches.
Eigenes Spiel den ganzen Spieltag zeigen|Show your match for the whole matchday
Zeigt das eigene Spiel ab dem Anstoß in der Konferenz, solange dort Spiele desselben Spieltags laufen, auch nach dem Abpfiff. Ausgeschaltet gilt für das eigene Spiel dieselbe Regel wie für alle Spiele.|Shows your match in the conference from kick-off while matches of the same matchday are shown there, also after the final whistle. When off, your match follows the same rule as every match.
Tasten|Buttons
Taste %|Button %
Pflichttaste|Main button
Hoch|Up
Runter|Down
Blättern mit Richtungswechsel|Scroll with direction change
Jede Taste ist abschaltbar. Genau eine Taste ist die Pflichttaste: kurz = nächster Bildschirm, doppelt = vorheriger Bildschirm, lang = zurück zur Automatik.|Every button can be switched off. Exactly one button is the main button: short = next screen, double = previous screen, long = back to automatic.
Konferenz und Spieltag: vorherige Seite. Tabelle: nach oben blättern; gehalten wiederholt.|Conference and matchday: previous page. Table: scroll up; repeats while held.
Konferenz und Spieltag: nächste Seite. Tabelle: nach unten blättern; gehalten wiederholt.|Conference and matchday: next page. Table: scroll down; repeats while held.
Gehalten: blättert in der aktuellen Richtung (Konferenz und Spieltag seitenweise, Tabelle zeilenweise). Loslassen wechselt die Richtung; Start ist abwärts.|While held: scrolls in the current direction (conference and matchday by page, table by row). Releasing changes the direction; it starts downwards.
Genau eine eingeschaltete Taste muss die Pflichttaste sein.|Exactly one active button must be the main button.
Höchstens Abrufe im Zeitfenster|Maximum requests per time window
Zeitfenster|Time window
Abrufintervalle|Request intervals
Gesamtlimit|Overall limit
Kennung|Identifier
Spielplan|Fixtures
Live-Tabelle|Live table
Ereignisse eigenes Spiel|Your match events
Ohne Verein|Without club
Details|Details
Nicht gespeichert|Not saved
Kein eigenes Spiel; die Spiele erscheinen in der Konferenz, die Tabelle ohne markierte Zeile.|No own match; the matches appear in the conference, the table without a highlighted row.
Noch keine Wettbewerbe. Wähle Anbieter, Wettbewerb und optional einen Verein.|No competitions yet. Choose a provider, competition and optionally a club.
Noch nicht gespeichert (neu oder geändert). Drücke unten „Speichern“: Erst dann ruft das Gerät diesen Wettbewerb ab.|Not saved yet (new or changed). Press “Save” below: only then does the device query this competition.
Einstellungen stammen aus einer älteren Firmware und wurden verworfen. Bitte neu einrichten.|Settings come from an older firmware and were discarded. Please set them up again.
Gespeicherte Wettbewerbe überschreiben?|Overwrite stored competitions?
Höchstens so viele Abrufe innerhalb des Zeitfensters, z. B. 10 Abrufe je 60 s. Das Gerät überschreitet sie in keinem Zeitfenster. Beide Werte 0 = keine Grenze.|At most this many requests within the time window, e.g. 10 requests per 60 s. The device never exceeds it in any window. Both values 0 = no limit.
Länge des Zeitfensters. 0 nur zusammen mit 0 Abrufen.|Length of the time window. 0 only together with 0 requests.
Antennenschalter|Antenna switch
Netzwerkbasis|Network stack
WLAN-Schnittstelle|WiFi interface
Setup-Schnittstelle|Setup interface
Setup-IP|Setup IP
WLAN-Treiber|WiFi driver
WLAN-Konfiguration|WiFi configuration
WLAN-Ereignisse|WiFi events
WLAN-Start|WiFi start
Setup-DNS|Setup DNS
Namensdienst|Local name service
Web-Ankündigung|Web announcement
Netzwerkstart gestört|Network startup failed
Netzwerkdienst|Network service
Lokale Netzwerkdienste|Local network services
Lokaler Netzwerkdienst konnte nicht gestartet oder aktualisiert werden.|A local network service could not be started or updated.
Automatischer neuer Versuch. Bleibt der Fehler, Gerät neu starten und Fehlercode melden.|The device retries automatically. If the error persists, restart it and report the error code.
Im Browser die IP-Adresse des Geräts verwenden.|Use the device's IP address in the browser.
IP im Browser öffnen|Open IP in browser
IP nutzen|Use IP
neuer Versuch|retrying
fehlgeschlagen|failed
gestört|failed
Wird gestartet|Starting
Neuer Versuch|Retry at
Neuer Versuch läuft|Retrying
Bildschirmfoto|Screenshot
Bildschirmfoto herunterladen|Download screenshot
Bildschirmfoto wird erstellt …|Creating screenshot …
Hilfe zum Bildschirmfoto|Screenshot help
Zeichnet den aktuellen Bildschirm einmal neu und lädt das Ergebnis als PNG herunter. Die Anzeige kann dabei bis zu 5 s pausieren. Die PNG-Datei bleibt ohne zusätzliche Drehung in der aktuellen Leserichtung des Displays. Außerhalb eines runden Displays bleibt das Bild transparent. Erfasst die neu gezeichnete Ausgabe, keine Fehler im LCD selbst. Anmeldung wie bei anderen Geräteaktionen; ohne Admin-Passwort automatisch. Es wird keine Datei auf dem Gerät gespeichert.|Redraws the current screen once and downloads the result as a PNG. The display can pause for up to 5 s while recording. The PNG stays in the display's current reading orientation without an additional rotation. The area outside a round display stays transparent. Captures the redrawn output, not faults in the LCD itself. Uses the same login as other device actions; automatic without an admin password. No file is stored on the device.
Bildschirmfoto hat zu lange gedauert. WLAN-Empfang prüfen und erneut versuchen.|The screenshot took too long. Check WiFi reception and try again.
Bildschirmfoto wurde nicht vollständig übertragen. WLAN-Verbindung prüfen und erneut herunterladen.|The screenshot was not transferred completely. Check the WiFi connection and download it again.
Bildschirmfoto ist unvollständig oder hat ein unbekanntes Format. Seite neu laden und erneut versuchen; bleibt der Fehler, Fehlercode melden.|The screenshot is incomplete or uses an unknown format. Reload the page and try again; if it persists, report the error code.
Der Browser konnte keine PNG-Datei erstellen. Seite neu laden oder einen anderen Browser verwenden.|The browser could not create a PNG file. Reload the page or use another browser.
Das Display ist noch nicht bereit oder unterstützt kein Bildschirmfoto. Kurz warten und erneut versuchen; bleibt der Fehler, Fehlercode melden.|The display is not ready or does not support screenshots. Wait briefly and try again; if it persists, report the error code.
Das Gerät führt gerade eine andere aufwendige Aktion aus. Kurz warten und das Bildschirmfoto erneut herunterladen.|The device is handling another demanding operation. Wait briefly and download the screenshot again.
Für das Bildschirmfoto reicht der Gerätespeicher gerade nicht. Andere Vorgänge abwarten und erneut versuchen; bleibt der Fehler, Gerät neu starten und Fehlercode melden.|The device does not currently have enough memory for the screenshot. Wait for other operations and try again; if it persists, restart the device and report the error code.
Das Gerät konnte kein Bildschirmfoto erstellen. Erneut versuchen; bleibt der Fehler, Gerät neu starten und Fehlercode melden.|The device could not create a screenshot. Try again; if it persists, restart the device and report the error code.
Ja|Yes
Nein|No
Hilfe|Help
Einzelspiel|Single match
Andere Spiele|Other matches
Ruhe|Rest
Saisonpause|Season break
Phase|Phase
Phasen|Phases
Bildschirmart|Screen type
Darstellung|Appearance
Freigeben|Enable
Besuchszeit|Visit duration
Gemeinsam|Shared
Wirksam|Effective
Ablauf nach Spielsituation|Sequence by match situation
Gemeinsame Reihenfolge|Common order
Zeiten und Sportansichten|Timing and sports views
Manuelle Rückkehrzeit|Manual return time
Eigene Spiele: Vorlauf|Own matches: before kickoff
Eigene Spiele: Nachlauf|Own matches: after finish
Andere Spiele: Vorlauf|Other matches: before kickoff
Andere Spiele: Nachlauf|Other matches: after finish
Automatische Saisonpause|Automatic season break
Pausenschwelle in Tagen|Break threshold in days
Eigenes Spiel anheften|Pin own match
Eigene Sonderbehandlung|Own-match special treatment
Farben, Hintergründe und Unterseiten|Colours, backgrounds and subpages
Hintergrundbilder auf allen Sportansichten|Background images on all sports views
Unterseitenpunkte|Subpage dots
Bilderfolge und Bilderrahmen|Image sequence and picture frame
Bilderfolge|Images
Bilderrahmen|Picture frame
Wappen im Bilderrahmen|Include crest in picture frame
Abweichungen je Wettbewerb|Competition overrides
Gemeinsame Regeln verwenden|Use shared rules
Je Wettbewerb aufteilen|Split by competition
Zuerst einen Wettbewerb hinzufügen.|Add a competition first.
Bilder und Wappen laufen bei Freigabe automatisch.|Enabled images and crest run automatically.
Die Besuchszeit muss ein ganzzahliges Vielfaches der Bildwechselzeit sein.|Visit duration must be a whole-number multiple of the image interval.
Die Anzeigeeinstellungen konnten nicht geladen werden. Seite neu laden. [WEB S02]|Screen settings could not be loaded. Reload the page. [WEB S02]
Anzeige neu laden|Reload screen settings
Verwirft nur den Anzeigeentwurf. Andere noch offene Änderungen bleiben erhalten.|Discards only the screen settings draft. Other pending changes are retained.
Automatische Fußballabfragen sind im Bilderrahmen pausiert.|Automatic football requests are paused in picture frame mode.
Die tatsächliche Geräteausgabe kannst du als Bildschirmfoto herunterladen.|Download a screenshot to inspect the actual device output.
Keine Bibliotheksbilder ausgewählt. Das Wappen ist eine eigene Bildschirmart.|No library images selected. The crest is a separate screen type.
Eine gemeinsame Reihenfolge, fünf Phasen. Es gilt die höchste passende Phase: eigenes Spiel, andere Spiele, Spieltag, Ruhe, optionale Saisonpause.|One common order and five phases. The highest applicable phase wins: own match, other matches, matchday, rest, optional season break.
Automatische Einträge folgen dieser Reihenfolge; Wettbewerbe innerhalb einer Sportart folgen ihrer Reihenfolge unter Wettbewerbe. Phasenwechsel und Rückkehr zur Automatik starten beim ersten geeigneten Eintrag.|Automatic entries follow this order; competitions within a sports type follow the Competitions page order. A phase change or return to automatic starts at the first eligible entry.
Freigabe für diese Phase. Ausgeschaltete Einträge erscheinen weder automatisch noch manuell. Gespeicherte Zeiten und Abweichungen bleiben erhalten.|Enables this entry in this phase. Disabled entries are unavailable in automatic and manual operation. Stored durations and overrides are retained.
Automatisch: wird in dieser Phase in der gemeinsamen Artenfolge besucht. Ausgeschaltet bleibt ein freigegebener Sporteintrag über Weiter/Zurück erreichbar. Wappen und Bilder werden dabei übersprungen. Die gespeicherte Besuchszeit gilt auch für automatische Unterseiten eines nur manuell erreichbaren Eintrags, bis du selbst Auf/Ab benutzt.|Automatic: visited in the common type order in this phase. When automatic is off, an enabled sports entry remains reachable with Next/Previous. Manual sports navigation skips crest and images. Manual-only entries retain their stored duration for automatic subpages until you press Up/Down.
Besuchszeit in Sekunden: 15 bis 43.200. Sportansichten erhalten mindestens 15 Sekunden insgesamt und 5 Sekunden je Unterseite. Auch bei nur manuell erreichbaren Sporteinträgen wirkt die gespeicherte Zeit, bis du selbst Auf/Ab benutzt.|Visit duration in seconds: 15 to 43,200. Sports views receive at least 15 seconds in total and 5 seconds per subpage. Manual-only sports entries also use their stored duration until you press Up/Down.
Besuch der Bilderfolge: 15 bis 43.200 Sekunden und ein ganzzahliges Vielfaches der Bildwechselzeit. Die Bildwechselzeit gilt getrennt.|Image-sequence visit: 15 to 43,200 seconds and a whole-number multiple of the image interval. The image interval is separate.
Aufteilen erlaubt eigene Freigabe, Automatik und Besuchszeit je Wettbewerb. „Gemeinsam“ erbt dieses eine Feld. Zurück zu gemeinsamen Regeln schaltet Abweichungen nur aus; ihre gespeicherten Werte bleiben erhalten.|Splitting enables competition-specific eligibility, automatic participation and visit duration. Shared inherits that one field. Returning to shared rules disables overrides while retaining their stored values.
Nach dem letzten Tastendruck zurück zur Automatik. 0 hält die manuelle Anzeige dauerhaft; auch im Bilderrahmen. Im Bilderrahmen beginnt danach das betrachtete Bild mit voller Bildzeit.|Returns to automatic after the last button press. 0 holds the manual view indefinitely, including picture frame mode. Picture playback then resumes from the viewed image with a full image interval.
Vor- und Nachlauf in Minuten: 0 bis 180. Eigene Sonderbehandlung gilt nur bei ausgewähltem Verein und eingeschalteter Sonderbehandlung dieses Wettbewerbs.|Before/after windows in minutes: 0 to 180. Own-match special treatment requires a selected club and enabled special treatment for that competition.
Verwendet die Saisonpausen-Phase nur bei einem vollständigen, frischen Nachweis aller aktiven Wettbewerbe. Unbekannte oder unvollständige Abdeckung beweist keine Pause.|Uses the season-break phase only with complete, fresh evidence for every active competition. Unknown or incomplete coverage does not prove a break.
Die nachgewiesene Zeitspanne zwischen Anstößen muss größer als diese Zahl mal 24 Stunden sein. Sommerzeit ändert die Grenze nicht.|The proven interval between kickoffs must exceed this number times 24 hours. Daylight saving does not change the threshold.
Gemeinsame Höchstzahl je Unterseite von Konferenz und Spieltag, mindestens 2. Beim Anheften zählt das eigene Spiel mit. Übrige Partien werden auf dieselbe Seitenzahl verteilt; kleine letzte Seiten werden soweit möglich aufgefüllt. Die Obergrenze hängt vom Display ab.|Shared maximum per Conference and Matchday page, at least 2. The pinned own match counts. Remaining fixtures keep the same page count; small final pages are filled where possible. The upper limit depends on the display.
Tabellenzeilen je Unterseite. Die Obergrenze hängt vom Display ab.|Table rows per subpage. The upper limit depends on the display.
Alle Seiten zeigt alle geeigneten Spiele. Eine Seite bevorzugt laufende Spiele und jüngste Tore innerhalb der vorhandenen Plätze.|All pages shows all eligible matches. One page prioritises running matches and recent goals within the available rows.
Heftet das eigene Spiel in seiner Konferenz an, solange es dort ausgewählt ist. Ohne Anheften bleibt die normale Reihenfolge erhalten.|Pins the own match in its conference while it is selected there. With pinning off, the ordinary order applies.
Aktueller Spieltag behält einen beendeten Spieltag bis Ende des Folgetags seines lokalen Spielendes. Nächster Spieltag nutzt den vorhandenen folgenden Spieltag; fehlende Daten werden nicht erfunden.|Current matchday retains a finished matchday through the day after its local finish date. Next matchday uses an available following matchday; missing data is not invented.
Gemeinsame Freigabe der Hintergrundbilder für die vier Sportansichten. Einzelne Bildauswahlen bleiben erhalten.|Shared background-image switch for the four sports views. Individual image choices are retained.
Zeit je Bild in Sekunden: 3 bis 3.600. Besuche der Bilderfolge müssen ein ganzzahliges Vielfaches davon sein.|Time per image in seconds: 3 to 3,600. Image-sequence visits must be a whole-number multiple of this interval.
Nimmt das separate Wappen zusätzlich zu den ausgewählten Bibliotheksbildern in den Bilderrahmen auf.|Includes the separate crest alongside the selected library images in picture frame mode.
Hintergrundbild für diese Ansicht. Die gemeinsame Hintergrund-Freigabe gilt zusätzlich; die Bildauswahl bleibt beim Ausschalten erhalten.|Background image for this view. The shared background switch also applies; the selected image is retained when disabled.
Bild aus der Bibliothek oder der gemeinsame Standard-Hintergrund.|Library image or the shared default background.
Grundfarbe hinter den Inhalten.|Base colour behind the content.
Farbe der Texte und Ergebnisse.|Colour of text and scores.
Farbe für Hervorhebungen und den eigenen Verein.|Colour of highlights and the own club.
Skaliert Texte innerhalb der vorhandenen Schriftgrößen. Die tatsächliche Lesbarkeit wird auf dem Gerät geprüft.|Scales text within the available font sizes. Actual readability is checked on the device.
Zeigt Punkte für vorhandene Unterseiten dieser Bildschirmart. Die Punkte stellen keine Wettbewerbe oder automatische Artenfolge dar.|Shows dots for existing subpages of this screen type. Dots do not represent competitions or the automatic type sequence.
Bilderrahmen zeigt ausschließlich Wappen und ausgewählte Bilder. Nach dem Speichern starten keine automatischen Fußballabfragen oder Fortsetzungen mehr; eine bereits gesendete Anfrage wird noch abgeschlossen. Profile und Daten bleiben erhalten. Beim Verlassen werden nur fällige Abrufe innerhalb der Anbieterlimits nachgeholt. Der Wettbewerbsassistent darf ausdrücklich gestartete Einrichtungslisten weiter laden.|Picture frame mode shows only the crest and selected images. After saving, no automatic football requests or continuations start; a request already sent is allowed to finish. Profiles and data are retained. Leaving the mode resumes only due requests within provider limits. Explicitly requested setup lists remain available in the competition wizard.
Bevorzugt Spiele des ausgewählten Vereins in der eigenen Phase und in den zugehörigen Ansichten. Ohne Sonderbehandlung gelten dieselben Regeln wie für andere Spiele; der Verein bleibt für Zuordnung und Tabellenmarkierung erhalten.|Prioritises the selected club's matches in the own phase and related views. With special treatment off, ordinary match rules apply; club identity and table highlighting are retained.
Demo-Szenarien|Demo scenarios
Szenario|Scenario
Schritt|Step
Gemeinsame Regeln|Common rules
Debug|Debug
Demo|Demo
Protokolle|Logs
Manuelle Anzeige|Manual view
Screenshot zusätzlich auf Übersicht anzeigen|Also show screenshot on Overview
Zeigt den Screenshot-Button zusätzlich auf der Übersicht. Hier unter Debug bleibt er immer erreichbar.|Also shows the screenshot button on Overview. It is always available here under Debug.
Demo wird vorbereitet. Status aktualisieren.|Demo is being prepared. Refresh the status.
Zeigt simulierte Spiele statt echter Daten. Nach dem Speichern sind die Demo-Steuerungen verfügbar.|Shows simulated matches instead of real data. Save to make Demo controls available.
Automatik weiterschalten|Advance automatic display
Zur Automatik zurückkehren|Return to automatic display
Automatische Einträge: %|Automatic entries: %
Wechselt zur nächsten automatischen Unterseite oder zum nächsten Bild, danach zur nächsten Bildschirmart oder zum nächsten Wettbewerb. Eine manuelle Anzeige kehrt zuerst zur Automatik zurück. Gespeicherte Zeiten und Demo-Schritt bleiben erhalten.|Moves to the next automatic page or image, then the next screen type or competition. A manual view first returns to automatic display. Saved times and the Demo step remain unchanged.
Erlaubt den Live-Protokollstream im Browser. Ausschalten lehnt diese Verbindung ab; Screenshot und Demo bleiben unabhängig.|Allows the live log stream in the browser. Switching it off rejects that connection; screenshot and Demo remain independent.
Live-Protokoll im Browser erlauben aktivieren und speichern.|Enable Allow live logs in browser and save.
Rückkehr zur Automatik angefordert.|Return to automatic display requested.
Automatischer Wechsel angefordert.|Automatic change requested.
Bildposition|Image position
Horizontal (px)|Horizontal (px)
Vertikal (px)|Vertical (px)
0 zentriert das Bild. Positive Werte verschieben nach rechts oder unten, negative nach links oder oben; Pixel beziehen sich auf die Geräteauflösung.|0 centres the image. Positive values move right or down, negative values left or up; pixels refer to the device resolution.
Eine gültige Pixelzahl eingeben. Die bisherige Position bleibt erhalten.|Enter a valid pixel value. The previous position is retained.
Gespeichertes Bild konnte nicht geladen werden. Wähle eine lokale Bilddatei; das gespeicherte Bild bleibt erhalten.|The stored image could not be loaded. Select a local image file; the stored image is retained.
Szenarien laden|Load scenarios
Schritt anzeigen|Show step
Vorheriger Schritt|Previous step
Nächster Schritt|Next step
Schritt wird übernommen …|Applying step …
Aktualisieren|Refresh
Demo-Modus zuerst einschalten und speichern.|Enable and save demo mode first.
Demo-Schritt noch nicht bestätigt. Aktualisieren und erneut prüfen.|Demo step has not been confirmed. Refresh and check again.
Steuert simulierte Spieldaten und eine separate Testzeit. Die ersten beiden gespeicherten Wettbewerbe liefern Reihenfolge und Anzeige-Regeln. Fehlt eine gespeicherte Zuordnung, gelten die gemeinsamen Regeln. Systemuhr und echte Anbieter werden nicht verändert; Szenarien werden nicht gespeichert.|Controls simulated match data and a separate test clock. Saved screen rules remain effective. System time and real providers are unchanged; scenarios are not persisted.
Die Demo ersetzt keine echten Anbieterantworten oder die Prüfung auf dem Gerät. Eine leere Automatik oder deaktivierte Ansicht bleibt auch im Szenario leer oder deaktiviert.|The demo does not replace real provider responses or device checks. An empty automatic sequence or a disabled view remains empty or disabled in the scenario.
Ob die Bildverweise gespeichert wurden, ist unklar. Gerät neu starten und Zustand prüfen; nicht erneut löschen.|Whether the image references were saved is unknown. Restart the device and check its state; do not delete again.
Bildverweise wurden gespeichert. Die Datei wurde nicht gelöscht; das Löschen kann erneut versucht werden.|Image references were saved. The file was not deleted; deletion can be tried again.
Speicherergebnis unklar. Nicht erneut speichern. Status prüfen; bei unklarem Speicherzustand Gerät neu starten.|Save outcome is unknown. Do not save again. Check status; restart the device if persistence remains uncertain.
Speichern wurde abgelehnt. Alle Änderungen bleiben im Entwurf.|Save was rejected. All changes remain in the draft.
Speicherergebnis noch unklar. Gerät neu starten und Status erneut prüfen.|Save outcome remains unknown. Restart the device and check status again.
Speicherergebnis zuerst prüfen.|Check the save outcome first.
Ungültige Namenssammlung. Format, Kennungen und doppelte Zuordnungen prüfen.|Invalid names collection. Check format, IDs and duplicate bindings.
Vereinsnamen|Club names
Namensänderungen bleiben im gemeinsamen Entwurf. Erst Speichern übernimmt sie. Vereinsgruppen werden dadurch nicht verändert.|Name changes remain in the shared draft until Save. Club groups stay unchanged.
Kurzname|Short name
Kennungen und Aliasse|IDs and aliases
Namensvorlage laden|Load names catalogue
Aus Vorlage übernehmen|Adopt from catalogue
Kurznamen-Ausnahmen|Short-name exceptions
In Entwurf übernehmen|Use in draft
Namenssammlung importieren|Import names collection
Namenssammlung exportieren|Export names collection
Ersetzt die Namenssammlung im Entwurf. Erst Speichern übernimmt sie.|Replaces the names collection in the draft. Save applies it.
Import lädt Konfiguration und Namen in den gemeinsamen Entwurf. Erst Speichern übernimmt sie.|Import loads configuration and names into the shared draft. Save applies them.
Alias hinzufügen|Add alias
Alias entfernen|Remove alias
Nur ein eigenes Einzelspiel|Only one own single match
Begrenzt eigene Einzelspiele auf den fokussierten Wettbewerb. Ausgeschaltet laufen weitere eigene Spiele nach der Wettbewerbs- und Gruppenpriorität in derselben Folge.|Limits own single matches to the focused competition. When off, further own matches follow competition and group priority in the same sequence.
Vereinsgruppe|Club group
Vereinsname|Club name
Gruppe bearbeiten|Edit group
Aus Gruppe lösen|Remove from group
Wettbewerbe über Verein gruppieren|Group competitions by club
Vereinsgruppe anlegen|Create club group
Gruppe auflösen|Dissolve group
Mindestens zwei Wettbewerbe mit ausgewähltem Verein wählen.|Select at least two competitions with a selected club.
Keine weitere Gruppe verfügbar.|No further group is available.
Eine Vereinsgruppe benötigt einen ausgewählten Verein.|A club group requires a selected club.
Diese Zuordnung bleibt bei einem Vereinswechsel bestehen. Prüfe die zusammengehörigen Wettbewerbe vor dem Übernehmen.|This membership stays when the club changes. Check the related competitions before applying.
Wähle die Wettbewerbe desselben Vereins ausdrücklich aus. Die Gruppe erhält die höchste Priorität ihrer Mitglieder; die Mitglieder behalten ihre Reihenfolge. Namen verändern keine Gruppierung.|Select competitions of the same club explicitly. The group takes its highest member priority; members keep their order. Names do not change grouping.
Die Auswahl ändert die Gruppen im Entwurf. Übernehmen bestätigt den Entwurf; erst Speichern auf der Seite übernimmt ihn auf dem Gerät.|The selection changes only the displayed group memberships. Apply places the changes in the draft; Save persists them.
Konfiguration wurde zwischenzeitlich geändert. Status aktualisieren und den Entwurf prüfen. [WEB S01]|Configuration changed in the meantime. Refresh status and check the draft. [WEB S01]
Kein Metadatenwert|No metadata value
Name des Anbieters verwenden|Use provider label
Anbieterwert verwenden entfernt die eigene Vorgabe. Ein eigener leerer Wert bleibt ausdrücklich leer.|Use provider label removes the override. An explicitly empty custom value stays empty.
`.trim().split('\n').map(line => line.split('|')));
