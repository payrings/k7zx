// k7zx-cli - headless front end for the k7zx conversion engine.
//
// Same options the GUI exposes, scriptable for batch conversion.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "core/convert.h"
#include "core/mp3.h"
#include "core/settings.h"
#include "core/texts.h"

namespace {

void usage() {
    std::printf(
        "k7zx 5.0 (modern C++ port) - ZX Spectrum tape file to audio converter\n"
        "\n"
        "Usage:\n"
        "  k7zx-cli [options] <input.tap|tzx|sna|z80|sbb|hex> [output]\n"
        "\n"
        "Conversion mode:\n"
        "  -m, --mode MODE       normal | hispeed             (default: hispeed)\n"
        "\n"
        "High speed encoding (hispeed mode):\n"
        "  -t, --method NAME     rom milks fsk slow delta raudo ultra npu fi fiq\n"
        "                        manchester manchester-dif escurrido rayo\n"
        "\n"
        "                        raudo always loads as one block, as in k7zx 4.3\n"
        "                        raudo profiles 0-3 are safer, 4-7 reckless\n"
        "                        For MP3 output or cassette recording use -t fsk -s 5.00\n"
        "  -s, --spb SAMPLES     samples per bit, e.g. 2.75 or 275 (default: 2.75)\n"
        "  -r, --rate HZ         44100 or 48000              (default: 48000)\n"
        "  -w, --waveform NAME   square ramp cubic continuous energy parabola delta\n"
        "  -c, --scheme NAME     one | many | original       (default: many)\n"
        "      --compress         keep LZ compression for Rayo (the default)\n"
        "      --stereo          write 16-bit stereo instead of 8-bit mono\n"
        "      --invert          invert the wave polarity\n"
        "      --invert-right    invert the right channel (stereo only)\n"
        "      --final-tone      append the decaying closing tone\n"
        "      --accelerate      use the fast BASIC loader\n"
        "      --no-checksum     do not install the tape error handler\n"
        "      --no-loader       do not synthesise the BASIC loader program\n"
        "      --kolmogorov      optimise Delta timings from the payload statistics\n"
        "      --no-compress     send Rayo data uncompressed (default: compressed)\n"
        "\n"
        "Other:\n"
        "      --clear ADDR      override the CLEAR address (decimal or 0x hex)\n"
        "      --usr ADDR        override the USR entry point\n"
        "      --name NAME       program name recorded in the loader\n"
        "      --blocks LIST     comma separated block indices, or 'all'/'none'\n"
        "      --poke ADDR=VALUE poke a byte before loading; repeatable. Base 0, so\n"
        "                       0x5ce5 works as well as 23781\n"
        "      --emulate         write a TZX direct-recording file instead of a WAV\n"
        "      --mp3             also encode the result to mp3\n"
        "      --mp3-encoder PROG  encoder to run: lame, or $K7ZX_MP3_ENCODER,\n"
        "                       with ffmpeg as a fallback. Implies --mp3\n"
        "      --mp3-bitrate KBPS  bit rate to ask the encoder for (default 320).\n"
        "                       Implies --mp3\n"
        "  -v, --verbose         report the loaded blocks and the chosen settings\n"
        "  -q, --quiet           only report errors\n"
        "  -h, --help            this message\n"
        "      --version         print the version\n"
        "\n"
        "Settings are read from the file shown by --config-path\n"
        "(default: $XDG_CONFIG_HOME/k7zx/k7zx.ini). k7zx-cli never writes it;\n"
        "only k7zx, the graphical program, saves changes.\n");
}

/// Everything the command line can say, as tri-state: an empty optional means
/// "the user did not ask for this", so the stored configuration keeps it.  The
/// fields used to carry the documented defaults and were copied over the
/// settings file unconditionally, so a flag that was never given still won --
/// a user with 44.1 kHz, stereo and Shavings Slow saved in k7zx.ini got 48 kHz,
/// mono and Raudo from k7zx-cli while the GUI used their own values.
///
/// With no configuration file the result is unchanged, because every default
/// here matches the one in Settings.
struct Opt {
    std::string input, output;
    std::optional<int> mode, method, spb, rate, waveform, scheme;
    std::optional<bool> stereo, invert, invertRight, finalTone;
    std::optional<bool> accelerate, checksum, loader, kolmogorov, compress;
    bool emulate = false, verbose = false, quiet = false;
    int clearAddr = -1, usrAddr = -1;
    std::string name;
    std::string blocks = "all";
    std::string configPath;
    std::vector<std::pair<unsigned, unsigned>> pokes;  // --poke ADDR=VALUE
    bool usePokes = false;                            // --poke given at least once
    bool mp3 = false;                                 // --mp3
    std::string mp3Encoder;                           // --mp3's encoder, or --mp3-encoder
    int mp3Bitrate = 0;                               // --mp3-bitrate, 0 = the default
};

bool parseMethod(const std::string& s, int& out) {
    static const struct { const char* name; int m; } kTable[] = {
        {"rom", k7zx::kRom},         {"milks", k7zx::kMilks},
        {"fsk", k7zx::kFsk},         {"slow", k7zx::kShavingsSlow},
        {"delta", k7zx::kShavingsDelta}, {"raudo", k7zx::kShavingsRaudo},
        {"ultra", k7zx::kUltra},     {"npu", k7zx::kNpu},
        {"fi", k7zx::kFi},           {"fiq", k7zx::kFiQ},
        {"manchester", k7zx::kManchester}, {"manchester-dif", k7zx::kManchesterDif},
        {"escurrido", k7zx::kEscurrido},   {"rayo", k7zx::kRayo},
    };
    for (const auto& e : kTable)
        if (s == e.name) { out = e.m; return true; }
    return false;
}

/// True for the names the command line used to accept and no longer does, so
/// that a stale script fails with advice rather than "unknown method".  The
/// low-rate techniques and Veloz were removed in step 15; the CLI is the only
/// place that still needs to recognise them by name.
bool isRemovedMethod(const std::string& s) {
    static const char* const kRemoved[] = {"veloz",    "andante", "agitato",      "vivace",
                                           "presto",   "maestoso", "scherzando"};
    for (const char* n : kRemoved)
        if (s == n) return true;
    return false;
}

bool parseWaveform(const std::string& s, int& out) {
    static const struct { const char* n; int v; } table[] = {
        {"square", k7zx::kSquare},   {"ramp", k7zx::kRamp},
        {"cubic", k7zx::kCubic},     {"continuous", k7zx::kZeroContinuous},
        {"energy", k7zx::kEqualEnergy}, {"parabola", k7zx::kParabola},
        {"delta", k7zx::kDelta}};
    for (const auto& e : table)
        if (s == e.n) { out = e.v; return true; }
    return false;
}

/// Accepts "2.75", "275" or "2,75".
// Delegates to the core, so the 1.33 special case is covered by the unit tests
// rather than living only here.
bool parseSpb(const std::string& s, int& out) { return k7zx::Settings::parseSamplesPerBit(s, out); }

int parseAddr(const std::string& s) {
    return static_cast<int>(std::strtol(s.c_str(), nullptr, 0));
}

}  // namespace

int main(int argc, char** argv) {
    Opt o;
    o.configPath = k7zx::defaultSettingsPath();

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "k7zx-cli: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "--version") { std::printf("k7zx 5.0 (modern C++ port)\n"); return 0; }
        if (a == "-m" || a == "--mode") {
            const std::string v = next("--mode");
            if (v == "normal") o.mode = k7zx::kConvertNormal;
            else if (v == "hispeed" || v == "hi") o.mode = k7zx::kConvertHiSpeed;
            else if (v == "mp3" || v == "music" || v == "lowrate") {
                std::fprintf(stderr,
                             "k7zx-cli: the low-rate mode was removed; for MP3 or tape "
                             "use -t fsk -s 5.00\n");
                return 2;
            } else {
                std::fprintf(stderr, "k7zx-cli: unknown mode '%s'\n", v.c_str());
                return 2;
            }
        } else if (a == "-t" || a == "--method") {
            const std::string v = next("--method");
            int m = 0;
            if (!parseMethod(v, m)) {
                if (isRemovedMethod(v))
                    std::fprintf(stderr,
                                 "k7zx-cli: '%s' was removed; for MP3 or tape use "
                                 "-t fsk -s 5.00\n",
                                 v.c_str());
                else
                    std::fprintf(stderr, "k7zx-cli: unknown method '%s'\n", v.c_str());
                return 2;
            }
            o.method = m;
        } else if (a == "-s" || a == "--spb") {
            int sp = 0;
            if (!parseSpb(next("--spb"), sp)) {
                std::fprintf(stderr, "k7zx-cli: bad samples-per-bit value\n");
                return 2;
            }
            o.spb = sp;
        } else if (a == "-r" || a == "--rate") {
            const int r = std::atoi(next("--rate").c_str());
            if (r != 44100 && r != 48000) {
                std::fprintf(stderr, "k7zx-cli: sample rate must be 44100 or 48000\n");
                return 2;
            }
            o.rate = r;
        } else if (a == "-c" || a == "--scheme") {
            const std::string v = next("--scheme");
            if (v == "one") o.scheme = static_cast<int>(k7zx::kOneBlock);
            else if (v == "many") o.scheme = static_cast<int>(k7zx::kManyBlocks);
            else if (v == "original") o.scheme = static_cast<int>(k7zx::kOriginalLoader);
            else { std::fprintf(stderr, "k7zx-cli: unknown scheme '%s'\n", v.c_str()); return 2; }
        } else if (a == "--stereo") { o.stereo = true;
        } else if (a == "--invert") { o.invert = true;
        } else if (a == "--invert-right") { o.invertRight = true;
        } else if (a == "--final-tone") { o.finalTone = true;
        } else if (a == "--accelerate") { o.accelerate = true;
        } else if (a == "--no-checksum") { o.checksum = false;
        } else if (a == "--no-loader") { o.loader = false;
        } else if (a == "--kolmogorov") { o.kolmogorov = true;
        } else if (a == "--compress") { o.compress = 1;
        } else if (a == "--no-compress") { o.compress = 0;
        } else if (a == "--emulate") { o.emulate = true;
        } else if (a == "--clear") { o.clearAddr = parseAddr(next("--clear"));
        } else if (a == "--usr") { o.usrAddr = parseAddr(next("--usr"));
        } else if (a == "-w" || a == "--waveform") {
            int wf = 0;
            if (!parseWaveform(next("--waveform"), wf)) {
                std::fprintf(stderr, "k7zx-cli: unknown waveform\n");
                return 2;
            }
            o.waveform = wf;
        } else if (a == "--name") { o.name = next("--name");
        } else if (a == "--blocks") { o.blocks = next("--blocks");
        } else if (a == "--poke") {
            const std::string v = next("--poke");
            const std::size_t eq = v.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "k7zx-cli: --poke wants ADDR=VALUE, not '%s'\n", v.c_str());
                return 2;
            }
            // Base 0, so "0x4000" means 16384; atoi would stop at the 'x' and
            // silently poke address 0.
            const long addr = std::strtol(v.substr(0, eq).c_str(), nullptr, 0);
            const long val = std::strtol(v.substr(eq + 1).c_str(), nullptr, 0);
            if (addr < 0 || addr > 0xffff || val < 0 || val > 0xff) {
                std::fprintf(stderr,
                             "k7zx-cli: --poke %s is out of range; the address must be "
                             "0..$ffff and the value 0..$ff\n",
                             v.c_str());
                return 2;
            }
            o.pokes.emplace_back(static_cast<unsigned>(addr), static_cast<unsigned>(val));
        } else if (a == "--mp3") {
            // Takes no value: the encoder has its own option, because a bare
            // word after --mp3 is indistinguishable from the output file name.
            o.mp3 = true;
        } else if (a == "--mp3-encoder") {
            o.mp3Encoder = next("--mp3-encoder");
            o.mp3 = true;  // naming an encoder plainly means you want an mp3
        } else if (a == "--mp3-bitrate") {
            o.mp3Bitrate = std::atoi(next("--mp3-bitrate").c_str());
            if (o.mp3Bitrate < 32 || o.mp3Bitrate > 320) {
                std::fprintf(stderr, "k7zx-cli: --mp3-bitrate must be 32..320\n");
                return 2;
            }
            o.mp3 = true;
        } else if (a == "--config-path") { o.configPath = next("--config-path");
        } else if (a == "-v" || a == "--verbose") { o.verbose = true;
        } else if (a == "-q" || a == "--quiet") { o.quiet = true;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "k7zx-cli: unknown option '%s' (try --help)\n", a.c_str());
            return 2;
        } else {
            positional.push_back(a);
        }
    }

    if (positional.empty()) { usage(); return 2; }
    o.input = positional[0];
    if (positional.size() > 1) o.output = positional[1];

    k7zx::Settings settings;
    k7zx::loadSettings(o.configPath, settings);
    if (!k7zx::settingsWarnings().empty())  // anything dropped while migrating
        std::fprintf(stderr, "k7zx: %s", k7zx::settingsWarnings().c_str());

    // The command line wins, but only where it actually said something: an
    // option nobody passed leaves the stored value alone.
    if (o.mode) settings.conversionMode = *o.mode;
    if (o.rate) settings.sampleRate = *o.rate;
    if (o.waveform) settings.waveform = *o.waveform;
    if (o.scheme) settings.scheme = static_cast<k7zx::Scheme>(*o.scheme);
    if (o.stereo) settings.stereo = *o.stereo;
    if (o.invert) settings.invert = *o.invert;
    if (o.invertRight) settings.invertRight = *o.invertRight;
    if (o.finalTone) settings.finalTone = *o.finalTone;
    if (o.accelerate) settings.accelerateBasic = *o.accelerate;
    if (o.checksum) settings.controlChecksum = *o.checksum;
    if (o.loader) settings.generateLoader = *o.loader;
    if (o.kolmogorov) settings.antiKolmogorov = *o.kolmogorov;
    if (o.compress) settings.compress = *o.compress;
    if (o.spb) settings.samplesPerBit = *o.spb;
    if (o.method) settings.method = *o.method;

    // A technique only works at the speeds it was designed for. The GUI rebuilds
    // its speed list per technique, but the command line has always accepted any
    // value: the loader patcher then hits its `default:` arm, leaves the
    // threshold table unpatched, and writes a wave that cannot load. Refuse it
    // here rather than silently produce one.
    if (settings.conversionMode == k7zx::kConvertHiSpeed) {
        const k7zx::Method m = static_cast<k7zx::Method>(settings.method);
        const std::vector<int>& ok = k7zx::Settings::allowedSamplesPerBit(m);
        bool allowed = false;
        for (int v : ok) allowed = allowed || v == settings.samplesPerBit;
        if (!allowed) {
            std::fprintf(stderr,
                         "k7zx-cli: %s does not support that speed; it offers:",
                         k7zx::texts::methodName(m));
            // Print the speeds the way they are written on the command line --
            // 2.75 -- rather than the raw quarter-integer enum values (11).
            for (std::size_t i = 0; i < ok.size(); ++i) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%.2f", ok[i] / 4.0);
                std::fprintf(stderr, "%s %s", i ? "," : "", buf);
            }
            std::fprintf(stderr, "\n");
            // Refuse outright when the caller is driving, but only warn when the
            // pair came out of a hand-edited k7zx.ini: their file, their call.
            if (o.method || o.spb) return 2;
            std::fprintf(stderr,
                         "k7zx-cli: using it anyway; it will not load. Pass -t and -s "
                         "to choose a valid pair.\n");
            settings.samplesPerBit = ok.front();
        }
    }

    // Only Rayo has an LZ compressor behind --compress / --no-compress. The GUI
    // greys the box out for every other technique; the command line used to
    // accept the flag for all of them and quietly do nothing.
    if (o.compress && *o.compress &&
        !k7zx::Settings::methodSupportsCompression(static_cast<k7zx::Method>(settings.method))) {
        std::fprintf(stderr,
                     "k7zx-cli: %s has no LZ compression (only Rayo does); ignoring "
                     "--compress\n",
                     k7zx::texts::methodName(static_cast<k7zx::Method>(settings.method)));
        settings.compress = false;
    }

    if (!o.pokes.empty()) {
        settings.pokes.clear();
        for (const auto& p : o.pokes) settings.pokes.push_back(k7zx::Poke{int(p.first), int(p.second)});
        settings.usePokes = true;
    }

    k7zx::Converter conv;
    if (!conv.load(o.input)) {
        std::fprintf(stderr, "k7zx-cli: %s\n", conv.errorMessage().c_str());
        return 1;
    }

    const k7zx::TapeImage& img = conv.image();
    if (o.verbose) {
        std::printf("file      : %s\n", o.input.c_str());
        std::printf("type      : %s\n", img.snap().snapshotType == 1   ? "snapshot 48K"
                                              : img.snap().snapshotType == 2 ? "snapshot 128K"
                                                                          : "tape blocks");
        if (img.snap().snapshotType) {
            std::printf("PC/SP     : $%04X / $%04X  bank $%02X\n", img.snap().PC, img.snap().SP,
                        static_cast<unsigned>(img.snap().page) & 0xff);
        }
        std::printf("CLEAR/USR : %u / %u\n", img.clearN(), img.usrN());
        std::printf("blocks    : %d\n", img.blockCount());
        for (int i = 0; i < img.blockCount(); ++i) {
            const auto& t = img.blocks()[static_cast<std::size_t>(i)];
            if (t.isTerminator()) break;
            std::printf("  [%d] type %c  $%04X + %u  %s\n", i, t.type, t.startAddress, t.length,
                        t.selected ? "selected" : "skipped");
        }
        if (!img.warnings().empty()) std::printf("warnings  : %s\n", img.warnings().c_str());
    }

    if (o.clearAddr >= 0) conv.setClearAddress(static_cast<unsigned>(o.clearAddr));
    if (o.usrAddr >= 0) conv.setUsrAddress(static_cast<unsigned>(o.usrAddr));
    if (!o.name.empty()) conv.setProgramName(o.name);

    if (o.blocks == "none") {
        conv.selectAllBlocks(false);
    } else if (o.blocks != "all") {
        conv.selectAllBlocks(false);
        std::string list = o.blocks;
        std::size_t offset = 0;
        while (offset < list.size()) {
            const auto comma = list.find(',', offset);
            const std::string tok = list.substr(offset, comma == std::string::npos ? comma : comma - offset);
            if (!tok.empty()) conv.setBlockSelected(std::atoi(tok.c_str()), true);
            if (comma == std::string::npos) break;
            offset = comma + 1;
        }
    }

    conv.applySettings(settings);
    if (o.emulate) {
        // High speed mode but packaged as a TZX direct recording.
        conv.audioOptions().emulate = true;
    }

    std::string out = o.output;
    if (out.empty()) {
        out = conv.suggestedOutputName(o.input);
        if (o.emulate) {
            out.resize(out.size() - 4);
            out += ".tzx";
        }
    }

    const bool normalMode = settings.conversionMode == k7zx::kConvertNormal;
    const k7zx::ConversionResult r = conv.convert(out, normalMode);

    if (!r.ok) {
        std::fprintf(stderr, "k7zx-cli: conversion failed: %s\n", r.errors.c_str());
        if (!r.warnings.empty()) std::fprintf(stderr, "warnings: %s\n", r.warnings.c_str());
        return 1;
    }

    if (o.mp3) {
        // The WAV is an intermediate here, so it goes next to the mp3 and is
        // removed once the encode has succeeded.  `--mp3 out.mp3` names the mp3;
        // otherwise the WAV's own name with the extension changed does.
        std::string mp3 = o.output;
        if (mp3.empty()) {
            mp3 = out;
            mp3.resize(mp3.size() - 4);
            mp3 += ".mp3";
        } else if (mp3.size() < 4 || mp3.compare(mp3.size() - 4, 4, ".mp3") != 0) {
            // Otherwise a WAV lands in a file called song.mp3, which is worse
            // than saying so.
            std::fprintf(stderr, "k7zx-cli: '%s' does not end in .mp3; writing %s.mp3\n",
                         mp3.c_str(), mp3.c_str());
            mp3 += ".mp3";
        }
        const std::string wav = mp3 + ".wav";
        if (!std::rename(out.c_str(), wav.c_str())) {
            std::string err;
            if (k7zx::encodeMp3(wav, mp3, o.mp3Encoder,
                          o.mp3Bitrate ? o.mp3Bitrate : k7zx::kMp3Bitrate, err)) {
                std::remove(wav.c_str());
                if (!o.quiet) std::printf("%s  (%.1f s of audio)\n", mp3.c_str(), r.duration);
                if (!r.warnings.empty())
                    std::fprintf(stderr, "warnings: %s\n", r.warnings.c_str());
                return 0;
            }
            std::fprintf(stderr, "k7zx-cli: %s\n", err.c_str());
            // Leave the WAV where it was moved to, so the conversion is not lost.
            std::rename(wav.c_str(), out.c_str());
            return 1;
        }
        std::fprintf(stderr, "k7zx-cli: cannot move %s aside to encode it\n", out.c_str());
        return 1;
    }

    if (!o.quiet) {
        std::printf("%s  (%.1f s of audio)\n", r.outputPath.c_str(), r.duration);
        if (!r.warnings.empty()) std::fprintf(stderr, "warnings: %s\n", r.warnings.c_str());
    }
    return 0;
}
