// k7zx 5.0 - modern C++ port
//
// Settings persistence and the method/speed compatibility tables.
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace k7zx {
namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

/// ASCII lower case, for the case-insensitive key handling described below.
std::string lower(const std::string& s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

int toInt(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return fallback;
    if (v < -1000000L || v > 1000000L) return fallback;  // out of any sane range
    return static_cast<int>(v);
}

bool toBool(const std::string& s, bool fallback) {
    if (s.empty()) return fallback;
    return s == "1" || s == "true" || s == "True" || s == "yes" || s == "on";
}

/// A tiny INI reader/writer over std::map.
///
/// Keys are folded to lower case. Windows INI keys are case-insensitive, and
/// the original relied on that: it *read* `Files.Lame` but *wrote* `Files.lame`,
/// which was invisible on Windows and would silently lose the setting here.
/// Section names are folded too, for the same reason.
class IniFile {
public:
    explicit IniFile(const std::string& path) {
        std::ifstream in(path);
        std::string line, section;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string value = trim(line.substr(eq + 1));
            // Undo the quoting save() applies, so a value with a '=' or a
            // newline in it round-trips.
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                std::string unquoted;
                for (std::size_t i = 1; i + 1 < value.size(); ++i) {
                    const char ch = value[i];
                    if (ch == '\\' && i + 2 < value.size() && (value[i + 1] == 'n' ||
                        value[i + 1] == 'r' || value[i + 1] == '"' || value[i + 1] == '\\')) {
                        const char esc = value[++i];
                        unquoted += (esc == 'n') ? '\n' : (esc == 'r') ? '\r' : esc;
                    } else {
                        unquoted += ch;
                    }
                }
                value = unquoted;
            }
            data_[section + "." + trim(line.substr(0, eq))] = value;
        }
        ok_ = in.good() || !data_.empty();
    }

    bool ok() const { return ok_; }

    /// An exact match wins; failing that, the first key that differs only in
    /// case.  Exact-first keeps a file that has both `Lame=` and `lame=` honest,
    /// and case-insensitive-fallback is what makes a file the Windows original
    /// wrote readable here.
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        auto it = data_.find(key);
        if (it != data_.end()) return it->second;
        const std::string want = lower(key);
        for (const auto& kv : data_)
            if (lower(kv.first) == want) return kv.second;
        return fallback;
    }
    /// Distinguishes a key that is present with an empty value from one that is
    /// absent, which get() alone cannot: both return "".
    /// Same case-insensitive lookup as get(), so a differently-cased key counts
    /// as present rather than as absent-and-therefore-defaulted.
    bool has(const std::string& key) const {
        if (data_.find(key) != data_.end()) return true;
        const std::string want = lower(key);
        for (const auto& kv : data_)
            if (lower(kv.first) == want) return true;
        return false;
    }
    int getInt(const std::string& key, int fallback) const { return toInt(get(key), fallback); }
    bool getBool(const std::string& key, bool fallback) const { return toBool(get(key), fallback); }

    void set(const std::string& key, const std::string& value) { data_[key] = value; }
    void erase(const std::string& key) {
        data_.erase(key);
        for (auto it = data_.begin(); it != data_.end();)
            it = (lower(it->first) == lower(key)) ? data_.erase(it) : std::next(it);
    }

    bool save(const std::string& path) const {
        std::ofstream out(path);
        if (!out) return false;
        // Group keys by section, preserving insertion-independent ordering.
        std::map<std::string, std::vector<std::pair<std::string, std::string>>> sections;
        for (const auto& [k, v] : data_) {
            const auto dot = k.find('.');
            const std::string sec = (dot == std::string::npos) ? std::string() : k.substr(0, dot);
            const std::string name = (dot == std::string::npos) ? k : k.substr(dot + 1);
            sections[sec].emplace_back(name, v);
        }
        for (const auto& [sec, entries] : sections) {
            if (!sec.empty()) out << "[" << sec << "]\n";
            for (const auto& [k, v] : entries) {
                // Quote anything that would otherwise break the file: a newline
                // in a path turned into a spurious extra key on the next read.
                const bool needsQuote =
                    v.empty() || v.find_first_of("\n\r=") != std::string::npos;
                out << k << "=";
                if (needsQuote) {
                    out << '"';
                    for (const char ch : v) {
                        if (ch == '"' || ch == '\\') out << '\\';
                        if (ch == '\n') { out << "\\n"; continue; }
                        if (ch == '\r') { out << "\\r"; continue; }
                        out << ch;
                    }
                    out << '"';
                } else {
                    out << v;
                }
                out << "\n";
            }
            out << "\n";
        }
        return static_cast<bool>(out);
    }

private:
    std::map<std::string, std::string> data_;
    bool ok_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Method / speed compatibility
// ---------------------------------------------------------------------------
const std::vector<Method>& Settings::turboMethods() {
    // kRom..kEscurrido are contiguous, then Rayo, which is not: 13-19 were
    // removed from this port.
    static const std::vector<Method> kList = {
        kRom,  kMilks, kFsk,  kShavingsSlow, kShavingsDelta, kShavingsRaudo, kUltra,
        kNpu,  kFi,    kFiQ,  kManchester,   kManchesterDif, kEscurrido,   kRayo};
    return kList;
}

bool Settings::parseSamplesPerBit(const std::string& text, int& out) {
    std::string t;
    for (char ch : text) {
        if (ch == ',') t += '.';
        else if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
            if (ch == '.') {
                // atof stops at the first '.', so "2.5.5" would be read as 2.5
                // and accepted.  Reject a second one instead of guessing.
                if (t.find('.') != std::string::npos) return false;
            }
            t += ch;
        }
        else return false;  // reject anything that is not a plain number
    }
    if (t.empty()) return false;

    if (t.find('.') == std::string::npos) {
        // Integer forms: "3" means 3.00, "275" means 2.75. Normalise to decimal
        // so that "133" reaches the same 1.33 special case as "1.33" does.
        const long whole = std::strtol(t.c_str(), nullptr, 10);
        if (whole > 0 && whole < 10) t = std::to_string(whole) + ".0";
        else if (whole >= 100 && whole <= 999)
            t = std::to_string(whole / 100) + "." +
                std::string(1, static_cast<char>('0' + (whole / 10) % 10)) +
                std::string(1, static_cast<char>('0' + whole % 10));
        else return false;
    }

    const double v = std::atof(t.c_str());
    if (!(v > 0.0)) return false;
    // 1.33 is not a quarter-integer, so scaling by four cannot express it: it
    // has its own enum slot. Without this, "1.33" rounded to 1.25 and
    // Escurrido's slowest speed was unreachable.
    if (v > 1.30 && v < 1.36) { out = kS1_33; return true; }
    const int scaled = static_cast<int>(v * 4.0 + 0.5);
    // The character filter above accepts several '.' ("2.5.5") and atof stops
    // at the first one, and nothing stopped a value like 3.33 rounding to 3.25
    // -- a SamplesPerBit no technique offers.  Such a value is worse than an
    // error: the loader's threshold table is patched per speed, so the tape it
    // produced had none and could not load.  k7zx-cli already checked this at
    // the call site; doing it here covers every caller, including `k7zx --play`.
    static const int kOffered[] = {kS8_00, kS7_00, kS6_00, kS5_00, kS4_00, kS3_50, kS3_00,
                                   kS2_75, kS2_50, kS2_25, kS2_00, kS1_75, kS1_50, kS1_25,
                                   kS1_33};
    for (const int k : kOffered) {
        if (k == scaled) {
            out = scaled;
            return true;
        }
    }
    return false;
}

bool Settings::methodAllowsSamplesPerBit(Method m, int spb) {
    const std::vector<int>& v = allowedSamplesPerBit(m);
    for (int x : v)
        if (x == spb) return true;
    return false;
}

int Settings::turboIndex(Method m) {
    const std::vector<Method>& v = turboMethods();
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] == m) return static_cast<int>(i);
    return -1;
}

std::vector<int> Settings::allowedSamplesPerBit(Method m) {
    // The original encoded this as a 15-entry row of markers per method, one
    // column per samples-per-bit setting ordered 8.00 .. 1.25:
    //   'x' hidden; 'g', 'o', 'r' were drawn green, yellow and red
    //   (MPBCmbBxDrawItem) -- good, marginal, risky.  Any column that is not 'x' was selectable, so the lists
    // below are exactly the non-'x' columns of each row, transcribed from
    // SettingdForm::MetodoCmbBxChange.
    switch (m) {
        case kRom:            return {kS3_00, kS2_50};
        case kMilks:          return {kS3_50, kS2_75, kS2_50, kS2_25, kS1_75};
        case kFsk:            return {kS8_00, kS7_00, kS6_00, kS5_00, kS4_00, kS3_00, kS2_50};
        case kShavingsSlow:   return {kS4_00, kS3_50, kS3_00, kS2_50};
        case kShavingsDelta:  return {kS3_50, kS3_00, kS2_75, kS2_25, kS1_75};
        case kShavingsRaudo:  return {kS2_75, kS2_50, kS2_25, kS1_75};
        // Ultra and NPU: columns 6/8/10/12 and 8/10/11/14.  Column 10 is 2.00;
        // an earlier transcription read it as 2.25, a speed neither patcher
        // has a case for, so it produced an unpatched (unloadable) loader
        // while hiding the 2.00 the original offered.
        case kUltra:          return {kS3_00, kS2_50, kS2_00, kS1_50};
        case kNpu:            return {kS2_50, kS2_00, kS1_75, kS1_25};
        case kFi:             return {kS8_00, kS7_00, kS6_00, kS5_00, kS4_00, kS3_00};
        case kFiQ:            return {kS3_50, kS3_00, kS2_75, kS2_50, kS2_25, kS2_00, kS1_75};
        case kManchester:
        case kManchesterDif:  return {kS4_00, kS3_00, kS2_00};
        case kEscurrido:      return {kS2_50, kS2_25, kS2_00, kS1_50, kS1_33};
        // Added by this port: shortest cycle 4 or 3 samples (see rayo.h).
        case kRayo:           return {kS2_75, kS2_25};
        default:              return {kS3_50, kS3_00, kS2_75, kS2_50, kS2_25};
    }
}

int samplesToBps(int sampleRate, int samplesPerBit) {
    if (samplesPerBit == kS1_33) return (sampleRate * 3) / 4;
    if (samplesPerBit <= 0) return 0;
    return (sampleRate * 4) / samplesPerBit;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
std::string defaultSettingsPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        base = std::string(home) + "/.config";
    }
    return base + "/k7zx/k7zx.ini";
}

static int atoiOr(const std::string& text, int fallback) {
    const int v = toInt(text, fallback);
    return v == fallback && text.find_first_of("0123456789") == std::string::npos ? fallback : v;
}

std::string& settingsWarnings() {
    static std::string w;
    return w;
}

bool loadSettings(const std::string& path, Settings& s) {
    if (path.empty()) return false;
    settingsWarnings().clear();
    auto note = [](const std::string& m) { settingsWarnings() += m; };
    IniFile ini(path);
    if (!ini.ok() && ini.get("Ops.Method").empty() && ini.get("Ops.Metodo").empty())
        return false;

    // The original's INI keys were Spanish; the English ones are written now and
    // the old spellings are still read, so an existing config keeps working.
    auto intKey = [&](const char* name, const char* legacy, int fallback) {
        if (!ini.get(std::string("Ops.") + name).empty())
            return ini.getInt(std::string("Ops.") + name, fallback);
        if (!ini.get(std::string("Ops.") + legacy).empty())
            return ini.getInt(std::string("Ops.") + legacy, fallback);
        return fallback;
    };
    auto boolKey = [&](const char* name, const char* legacy, bool fallback) {
        if (!ini.get(std::string("Ops.") + name).empty())
            return ini.getBool(std::string("Ops.") + name, fallback);
        if (!ini.get(std::string("Ops.") + legacy).empty())
            return ini.getBool(std::string("Ops.") + legacy, fallback);
        return fallback;
    };

    s.conversionMode = intKey("Mode", "Conversor", s.conversionMode);
    s.sampleRate = intKey("SampleRate", "Frecuencia", s.sampleRate);
    s.waveform = intKey("Waveform", "Forma", s.waveform);
    s.invert = boolKey("Invert", "Invertida", s.invert);
    s.invertRight = boolKey("InvertRight", "Inv_derecho", s.invertRight);
    s.finalTone = ini.getBool("Ops.Final", s.finalTone);
    s.stereo = ini.getBool("Ops.CD", s.stereo);
    s.accelerateBasic = boolKey("AccelerateBasic", "Acelerar", s.accelerateBasic);
    // Two spellings of the same value: `Ops.MetodoHI` from the settings dialog
    // (original/SettingdForm.cpp:198) and `Ops.Metodo`, which the main window
    // wrote on *every* exit (original/MainForm.cpp:255).  Since MainForm ran
    // last, a real 4.3 config almost always carries `Metodo` -- and it used to
    // be read and then thrown away, so those files silently came back on the
    // default technique.
    {
        int m = s.method;
        if (!ini.get("Ops.Method").empty()) {
            m = ini.getInt("Ops.Method", m);
        } else if (!ini.get("Ops.MetodoHI").empty()) {
            m = ini.getInt("Ops.MetodoHI", m);
        } else if (!ini.get("Ops.Metodo").empty()) {
            m = ini.getInt("Ops.Metodo", m);
        }
        s.method = static_cast<Method>(m);
    }
    // `Ops.MusicMethod` (legacy `Ops.MetodoMA`) held the technique chosen in the
    // removed low-rate "Ma non troppo" mode.  It is ignored on read and no
    // longer written.
    s.samplesPerBit = intKey("SamplesPerBit", "Muestras", s.samplesPerBit);
    s.antiKolmogorov = ini.getBool("Ops.Kolmogorov", s.antiKolmogorov);
    s.compress = ini.getBool("Ops.Compress", s.compress);
    s.controlChecksum = ini.getBool("Ops.control_chksum", s.controlChecksum);
    s.generateLoader = boolKey("GenerateLoader", "Cargador", s.generateLoader);
    // `Ops.Bloques` was written as a *Bool* by the main window on every exit
    // (original/MainForm.cpp:260) and read back with ReadBool, so `1` means
    // many blocks.  The settings dialog wrote the same key as an integer scheme
    // index instead (original/SettingdForm.cpp:205), so both spellings turn up:
    // 2 is many and 3 is the original loader, but 1 is many, not "one block".
    {
        const std::string many = ini.get("Ops.ManyBlocks");
        const std::string old = ini.get("Ops.Bloques");
        if (!many.empty()) {
            s.scheme = static_cast<Scheme>(atoiOr(many, static_cast<int>(s.scheme)));
        } else if (!old.empty()) {
            const int v = atoiOr(old, 0);
            if (v >= 2)
                s.scheme = static_cast<Scheme>(v);
            else
                s.scheme = v ? kManyBlocks : kOneBlock;
        }
        // `Ops.Basic` was the third scheme radio ("as close to the original
        // loader as possible").  The three choices were radio buttons, so Basic
        // true means the user picked the original loader and Bloques was
        // therefore false; it has to win over whatever the default was.
        if (ini.getBool("Ops.Basic", false)) s.scheme = kOriginalLoader;
    }
    s.lastDirectory = ini.get("Files.Path", s.lastDirectory);
    s.outputDirectory = ini.get("Files.Output", s.outputDirectory);
    // The same "Info in file's name" tick box was read from `Ops.Info` by the
    // main form and from `Ops.InfoinFile` by the player form, with conflicting
    // defaults, and whichever form closed last won. Try both.
    if (!ini.get("Files.Info").empty()) {
        s.infoInFileName = ini.getBool("Files.Info", s.infoInFileName);
    } else if (!ini.get("Ops.InfoinFile").empty()) {
        s.infoInFileName = ini.getBool("Ops.InfoinFile", s.infoInFileName);
    } else if (!ini.get("Ops.Info").empty()) {
        s.infoInFileName = ini.getBool("Ops.Info", s.infoInFileName);
    }
    s.encodeMp3 = ini.getBool("Files.Mp3", s.encodeMp3);
    // The original read `Files.Lame` but *wrote* `Files.lame`; the case
    // difference was invisible on Windows, where INI keys are case-insensitive.
    if (!ini.get("Files.LamePath").empty()) {
        // get() returning "" is the "key absent" signal used above, so a
    // deliberately blank LamePath could not round-trip and reverted to "lame".
    if (ini.has("Files.LamePath")) s.lamePath = ini.get("Files.LamePath", s.lamePath);
    } else if (!ini.get("Files.lame").empty()) {
        s.lamePath = ini.get("Files.lame", s.lamePath);
    } else if (!ini.get("Files.Lame").empty()) {
        s.lamePath = ini.get("Files.Lame", s.lamePath);
    }
    // `Ops.Pokeador` (original/MainForm.cpp:263).  filesKey() prefixes with
    // "Files.", so the legacy name was being looked for in the wrong section
    // and the poke box came back unticked for every existing config.
    s.usePokes = ini.getBool("Files.UsePokes", ini.getBool("Ops.Pokeador", s.usePokes));
    // A stored window size is honoured only if it is *larger* than what the
    // layout wants; a smaller one is ignored so the page never gets clipped.
    s.windowWidth = std::clamp(ini.getInt("Form.Width", s.windowWidth), 0, 16384);
    s.windowHeight = std::clamp(ini.getInt("Form.Height", s.windowHeight), 0, 16384);

    s.pokes.clear();
    for (int i = 0; i < 64; ++i) {
        const std::string key = "Pokes." + std::to_string(i);
        const std::string a = ini.get(key + ".addr");
        if (a.empty()) break;
        // The converter narrows these to a 16 bit address and an 8 bit value,
        // so an address like 70000 in the file would become 4464 and poke a
        // completely different place.  Drop it and say so instead.
        const int addr = toInt(a, -1);
        const int value = toInt(ini.get(key + ".value"), -1);
        if (addr < 0 || addr > 0xffff || value < 0 || value > 0xff) {
            note("poke " + std::to_string(i) + " ignored: address " + a + " / value " +
                 std::to_string(value) + " is outside 0..$ffff / 0..$ff\r\n");
            continue;
        }
        s.pokes.push_back(Poke{addr, value});
    }
    if (!ini.get("Pokes.64.addr").empty())
        note("more than 64 pokes in the configuration; the rest were ignored\r\n");

    // Clamp anything nonsensical rather than propagating it into the renderer.
    // Two ranges went away with the low-rate mode and are migrated here so that
    // an existing k7zx.ini keeps working.
    //
    // `Ops.Mode` 2 was the low-rate "Ma non troppo" mode, and anything else
    // outside 0..1 is meaningless.  Both load as high speed with FSK at 5.00
    // samples per bit, the replacement the removal notes recommend.  5.00, 6.00
    // and 7.00 are the FSK speeds that load with no errors in every analogue
    // channel condition on both machines (see the guide's reliability
    // ratings); 4.00 is faster but does not finish at a badly aligned
    // threshold.
    if (s.conversionMode != kConvertNormal && s.conversionMode != kConvertHiSpeed) {
        s.conversionMode = kConvertHiSpeed;
        s.method = kFsk;
        s.samplesPerBit = kS5_00;
    }
    // `Ops.Method` 13-18 were the low-rate techniques and 19 was Veloz.  Any
    // value that is not one of the remaining high-speed techniques falls back
    // to the default one.
    if (Settings::turboIndex(static_cast<Method>(s.method)) < 0) s.method = kShavingsRaudo;
    s.sampleRate = s.sampleRate == 44100 ? 44100 : 48000;
    s.waveform = std::clamp(s.waveform, static_cast<int>(kSquare), static_cast<int>(kDelta));
    s.scheme = static_cast<Scheme>(std::clamp(static_cast<int>(s.scheme),
                                            static_cast<int>(kOneBlock),
                                            static_cast<int>(kOriginalLoader)));
    return true;
}

bool saveSettings(const std::string& path, const Settings& s) {
    // The original wrote k7zx.ini next to the executable, where the directory
    // always existed.  This port moved it under XDG_CONFIG_HOME, and nothing
    // created the `k7zx` subdirectory -- so on any machine that had never run
    // k7zx before, the write failed and every setting the user changed was
    // silently lost on exit.
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::error_code ec;
        std::filesystem::create_directories(path.substr(0, slash), ec);
        // ec is deliberately ignored: if the directory cannot be made the open
        // below reports the real problem.
    }

    if (path.empty()) return false;
    IniFile ini(path);

    ini.set("Ops.Mode", std::to_string(s.conversionMode));
    ini.set("Ops.SampleRate", std::to_string(s.sampleRate));
    ini.set("Ops.Waveform", std::to_string(s.waveform));
    ini.set("Ops.Invert", s.invert ? "1" : "0");
    ini.set("Ops.InvertRight", s.invertRight ? "1" : "0");
    ini.set("Ops.Final", s.finalTone ? "1" : "0");
    ini.set("Ops.CD", s.stereo ? "1" : "0");
    ini.set("Ops.AccelerateBasic", s.accelerateBasic ? "1" : "0");
    ini.set("Ops.Method", std::to_string(s.method));
    ini.set("Ops.SamplesPerBit", std::to_string(s.samplesPerBit));
    ini.set("Form.Width", std::to_string(s.windowWidth));
    ini.set("Form.Height", std::to_string(s.windowHeight));
    ini.set("Ops.Kolmogorov", s.antiKolmogorov ? "1" : "0");
    ini.set("Ops.Compress", s.compress ? "1" : "0");
    ini.set("Ops.control_chksum", s.controlChecksum ? "1" : "0");
    ini.set("Ops.GenerateLoader", s.generateLoader ? "1" : "0");
    ini.set("Ops.ManyBlocks", std::to_string(static_cast<int>(s.scheme)));

    ini.set("Files.Path", s.lastDirectory);
    ini.set("Files.Output", s.outputDirectory);
    ini.set("Files.Info", s.infoInFileName ? "1" : "0");
    ini.set("Files.Mp3", s.encodeMp3 ? "1" : "0");
    ini.set("Files.LamePath", s.lamePath);
    ini.set("Files.UsePokes", s.usePokes ? "1" : "0");

    // Replace the poke list wholesale.
    for (std::size_t i = 0; i < 64; ++i) {
        const std::string key = "Pokes." + std::to_string(i);
        if (i < s.pokes.size()) {
            ini.set(key + ".addr", std::to_string(s.pokes[i].address));
            ini.set(key + ".value", std::to_string(s.pokes[i].value));
        } else {
            ini.set(key + ".addr", "");
        }
    }
    // The old file was loaded first so that keys this port knows nothing about
    // survive a save, but that also kept every *superseded* key alive forever --
    // MusicMethod and MetodoMA for the removed low-rate mode, and the Spanish
    // spellings now written in English -- which in turn kept their migration
    // paths alive after the first save.  Drop exactly those, and nothing else.
    static const char* const kSuperseded[] = {
        "Ops.MusicMethod", "Ops.MetodoMA", "Ops.MetodoHI", "Ops.Metodo",
        "Ops.Conversor",   "Ops.Frecuencia", "Ops.Muestras", "Ops.Invertida",
        // MAJOR: "Ops.Kolmogorov" used to be in this list.  It is not a superseded
        // Spanish spelling -- it is the *current* key for "Statistical
        // optimisation", written above and read back above, so erasing it here
        // meant the tick box could never persist.
        "Ops.Inv_derecho", "Ops.Acelerar",   "Ops.Cargador",
        "Ops.Info",        "Ops.Pokeador",   "Ops.Bloques",  "Ops.Basic",
        "Ops.Divisor",
        "Ops.Forma",       "Files.lame",     "Files.Lame",   "Files.Pokeador"};
    for (const char* key : kSuperseded) ini.erase(key);

    return ini.save(path);
}

}  // namespace k7zx
