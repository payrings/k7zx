// k7zx 5.0 - modern C++ port
//
// Persistent settings, stored in a plain INI file next to the config
// directory (the original wrote k7zx.ini next to the executable).
#ifndef K7ZX_SETTINGS_H
#define K7ZX_SETTINGS_H

#include <string>
#include <vector>

#include "defs.h"

namespace k7zx {

/// One row of the "pokes" table in the GUI.
struct Poke {
    int address = 0;
    int value = 0;
};

/// Everything the user can configure, plus where they left the directory
/// browser pointing.
struct Settings {
    // --- conversion mode ---------------------------------------------------
    int conversionMode = kConvertHiSpeed;

    // --- audio -------------------------------------------------------------
    int sampleRate = 48000;   ///< 44100 or 48000
    int waveform = kCubic;
    bool invert = false;
    bool invertRight = false;
    bool finalTone = false;
    bool stereo = false;
    bool accelerateBasic = false;

    // --- high speed encoder -------------------------------------------------
    int method = kShavingsRaudo;
    int samplesPerBit = kS2_75;
    bool antiKolmogorov = false;
    bool compress = true;            ///< LZ-compress the data (Rayo only)
    bool controlChecksum = true;
    bool generateLoader = true;
    Scheme scheme = kManyBlocks;
    unsigned clearAddress = 65344;  ///< CLEAR address used when the file has none

    // --- mp3 output --------------------------------------------------------
    bool encodeMp3 = false;          ///< the original's "LAME mp3 encoder" tick box
      // No bit-rate setting: the original hard-coded 256 kbps (this port uses
      // 320; see GUIDE.md) and exposed no control at all.  Its `MPBCmbBx` combo
      // was the samples-per-bit selector, and the per-mode rates came from
      // `RatioMA`/`RatioNOR`, which a fixed rate makes redundant.
      std::string lamePath = "lame";   ///< encoder to shell out to



    // --- GUI state ----------------------------------------------------------
    std::string lastDirectory;
    std::string outputDirectory;
    bool infoInFileName = false;

    /// Window size remembered between runs.  A stored size *larger* than the
    /// layout wants is honoured -- a user who deliberately made the window
    /// bigger keeps it -- while a smaller one is ignored, so the page is never
    /// clipped.  Zero means "no preference".
    int windowWidth = 0;
    int windowHeight = 0;
    bool usePokes = false;
    std::vector<Poke> pokes;

    /// Is the tape-error check meaningful for this method?  Several encoders
    /// embed their own parity, so the synthetic RST 8 handler is dead code.
    static bool methodSupportsChecksumCheck(Method m) {
        // kShavingsRaudo is excluded because the original's help text says it
        // has no control checksum.  kRayo is excluded too: its checksum is built
        // into the loader and is always checked.
        return !(m == kRom || m == kShavingsDelta || m == kShavingsRaudo || m == kUltra ||
                 m == kFiQ || m == kEscurrido || m == kRayo);
    }

    /// Which samples-per-bit values this method can actually run at.  The
    /// original encoded this as a per-method row of 'x'/'g'/'o'/'r' markers
    /// in the combo box, drawn as traffic lights (SettingdForm.cpp,
    /// MPBCmbBxDrawItem): 'g' green, 'o' yellow, 'r' red, 'x' hidden.  The
    /// yellow and red speeds are marginal; several of them do not load even
    /// from k7zx 4.3's own output.  The ranges below were transcribed from
    /// SettingdForm::MetodoCmbBxChange.
    static std::vector<int> allowedSamplesPerBit(Method m);
    /// The techniques offered in the high-speed method list, in presentation
    /// order.  This is deliberately NOT the range kRom..kRayo: the removed
    /// low-rate encoders and Veloz left a gap at 13-19, and Veloz used to sit
    /// after them.  Every place that iterates or indexes the high-speed
    /// technique list must go through this, so that adding a technique cannot
    /// silently leave the GUI, the CLI or the self-test behind.
    static const std::vector<Method>& turboMethods();
    /// Index of `m` within turboMethods(), or -1 if it is not one of them.
    static int turboIndex(Method m);
    /// Parses a samples-per-bit value as written by a user: "2.75", "275" or
    /// "3" all work.  1.33 is special: it has its own enum slot because it is
    /// not a quarter-integer, and scaling by four would round it to 1.25.
    /// Returns false if `text` is not a usable speed.
    static bool parseSamplesPerBit(const std::string& text, int& out);
    /// Is `spb` one of the values this method can actually run at?
    static bool methodAllowsSamplesPerBit(Method m, int spb);

    /// Does the method support the "many blocks" scheme?
    static bool methodSupportsMultiBlock(Method m) { return m != kShavingsRaudo && m != kRayo; }

    /// Does the method offer LZ compression?
    static bool methodSupportsCompression(Method m) { return m == kRayo; }

    /// Does the method support the "as close to the original loader" scheme?
    static bool methodSupportsOriginalLoader(Method m) { return m == kMilks; }
};

/// Load settings from `path`, filling in defaults for anything missing.
/// Returns false if the file could not be read (settings is still populated
/// with defaults).
bool loadSettings(const std::string& path, Settings& settings);

/// Anything loadSettings() had to drop or adjust while migrating an older
/// configuration.  Cleared by each call to loadSettings(); empty when the file
/// needed no help.  Exposed so the front ends can tell the user, instead of
/// silently losing part of their setup.
std::string& settingsWarnings();

/// Write settings to `path`.  Returns false on failure.
bool saveSettings(const std::string& path, const Settings& settings);

/// Default configuration path: $XDG_CONFIG_HOME/k7zx/k7zx.ini (or
/// ~/.config/k7zx/k7zx.ini).
std::string defaultSettingsPath();

/// Bits per second for a given samples-per-bit selector, as shown in the UI.
/// bps = 4 * sampleRate / mpb, with 1.33 bps (mpb == 4) special-cased.
int samplesToBps(int sampleRate, int samplesPerBit);

}  // namespace k7zx

#endif  // K7ZX_SETTINGS_H
