#include "i18n.h"

namespace ao4ui {
namespace {

Lang g_lang = Lang::De;

// UTF-8, { German, English }.
const char* const kTexts[T_Count][2] = {
    {"Army of Two: The Devil's Cartel - Nativer PC-Port (Testversion)", "Army of Two: The Devil's Cartel - Native PC port (test build)"},
    {"Spielen", "Play"},
    {"Einstellungen", "Settings"},
    {"Profile", "Profiles"},
    {"Hilfe", "Help"},
    {"Ersteinrichtung", "First-time setup"},
    {"Dieser Port enthält keine Spieldaten. Bitte wähle deine eigene ISO von Army of Two: The Devil's Cartel "
     "(Xbox 360, Europa, En/De/It). Die Spieldateien werden einmalig lokal entpackt (ca. 7,3 GB).",
     "This port contains no game data. Please select your own ISO of Army of Two: The Devil's Cartel "
     "(Xbox 360, Europe, En/De/It). The game files are extracted locally once (about 7.3 GB)."},
    {"ISO-Datei", "ISO file"},
    {"Durchsuchen...", "Browse..."},
    {"Prüfen", "Check"},
    {"Installieren", "Install"},
    {"Unterstützte Version erkannt (Europa, En/De/It).", "Supported version detected (Europe, En/De/It)."},
    {"Diese Version wird nicht unterstützt. Der Port läuft nur mit der Europa-Version (En/De/It).",
     "This version is not supported. The port only runs with the Europe version (En/De/It)."},
    {"Entpacke Spieldateien...", "Extracting game files..."},
    {"Einrichtung abgeschlossen.", "Setup complete."},
    {"Abbrechen", "Cancel"},
    {"Freier Speicher auf dem Ziellaufwerk", "Free space on the target drive"},
    {"Einzelspieler", "Single player"},
    {"Splitscreen-Koop (2 Spieler, 1 PC)", "Split-screen co-op (2 players, 1 PC)"},
    {"Online/LAN-Koop: Spiel hosten", "Online/LAN co-op: host game"},
    {"Online/LAN-Koop: Spiel beitreten", "Online/LAN co-op: join game"},
    {"Spiel starten", "Start game"},
    {"Spiel läuft...", "Game is running..."},
    {"Spielerplatz 1", "Player slot 1"},
    {"Spielerplatz 2", "Player slot 2"},
    {"Deine Netzwerkadresse (an den Mitspieler weitergeben)", "Your network address (give it to your partner)"},
    {"Adresse des Hosts", "Host address"},
    {"Im Spiel Koop/Splitscreen wählen, Spieler 2 drückt Start. Welche Geräte zu welchem Spieler gehören, steht unten.",
     "Choose co-op/split screen in the game, player 2 presses Start. Which device belongs to which player is set below."},
    {"Nur LAN oder virtuelles LAN (Hamachi, Tailscale, ZeroTier). Im Spiel: MISSIONEN > Xbox LIVE.",
     "LAN or virtual LAN only (Hamachi, Tailscale, ZeroTier). In the game: MISSIONS > Xbox LIVE."},
    {"Spiel-Korrekturen", "Game fixes"},
    {"Grafik", "Graphics"},
    {"Ton", "Audio"},
    {"Steuerung", "Controls"},
    {"Sprache", "Language"},
    {"Vollbild", "Fullscreen"},
    {"Fenstergröße", "Window size"},
    {"Auflösungs-Skalierung", "Resolution scale"},
    {"Bildfilter", "Output filter"},
    {"Anisotrope Filterung", "Anisotropic filtering"},
    {"VSync", "VSync"},
    {"60 FPS (experimentell)", "60 FPS (experimental)"},
    {"Echte 60 Bilder pro Sekunde mit korrigiertem Spieltempo. Bekanntes Problem: gelegentlich bleibt ein Gegner in Deckung untätig.",
     "Real 60 frames per second with corrected game speed. Known issue: occasionally an enemy stays idle in cover."},
    {"Tonausgabe", "Audio output"},
    {"Stumm", "Mute"},
    {"Maus-Empfindlichkeit", "Mouse sensitivity"},
    {"Maus Y-Achse umkehren", "Invert mouse Y axis"},
    {"Tastenbelegung: config/input-bindings.json. Im Spiel: F4 = Menü, F9 = Bravo-Modus, F6 = Partner-Ausrüstung.",
     "Key bindings: config/input-bindings.json. In game: F4 = menu, F9 = Bravo mode, F6 = partner gear."},
    {"Speichern", "Save"},
    {"Gespeichert.", "Saved."},
    {"Neues Profil", "New profile"},
    {"Umbenennen", "Rename"},
    {"Löschen", "Delete"},
    {"Name", "Name"},
    {"Spielstände", "Save games"},
    {"Profil wird verwendet und kann nicht gelöscht werden.", "Profile is in use and cannot be deleted."},
    {"Log-Ordner öffnen", "Open log folder"},
    {"Logs für Fehlerbericht packen (ZIP)", "Pack logs for a bug report (ZIP)"},
    {"Liesmich öffnen", "Open readme"},
    {"Version", "Version"},
    {"Gilt ab dem nächsten Spielstart.", "Applies from the next game start."},
    {"Port-Menü", "Port menu"},
    {"Weiterspielen (F4)", "Resume (F4)"},
    {"Sofort wirksam", "Applies immediately"},
    {"Sprache des Launchers", "Launcher language"},
    {"Spielsprache (Text und Sprachausgabe)", "Game language (text and speech)"},
    {"XUID", "XUID"},
    {"Die Spielstände bleiben im Ordner profiles erhalten.", "The save games stay in the profiles folder."},
    {"(kein Profil)", "(no profile)"},
    {"Beim ersten Hosten fragt Windows nach der Firewall-Freigabe (privates Netzwerk, TCP 42124 + Spiel).",
     "When hosting for the first time Windows asks for firewall access (private network, TCP 42124 + game)."},
    {"Spiel wurde beendet.", "Game exited."},
    {"Spieldateien neu einrichten", "Set up game files again"},
    {"Spielordner", "Game folder"},
    {"Behebt Fehler des Originalspiels.", "Fixes bugs of the original game."},
    {"Log-ZIP erstellt", "Log ZIP created"},
};

}  // namespace

void SetLanguage(Lang lang) { g_lang = lang; }
Lang GetLanguage() { return g_lang; }
const char* Tr(Text id) {
  if (id < 0 || id >= T_Count) return "?";
  return kTexts[id][g_lang == Lang::En ? 1 : 0];
}

}  // namespace ao4ui
