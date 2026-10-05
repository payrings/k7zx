// Tests for the Z80 loader routine tables and the runtime patcher.
#include "test_harness.h"

#include "core/loader.h"
#include "core/settings.h"
#include "core/texts.h"

#include <fstream>
#include <iterator>

using namespace k7zx;

namespace {

std::size_t codeSize(Method m) { return CodeSizes::primary(m); }

}  // namespace

TEST("every method has a distinct, non-trivial loader") {
    // Over the surviving techniques only: Method values 13-19 were removed in
    // step 15 and keep their slots as reserved placeholders, so the raw enum
    // range is no longer the set of techniques.
    for (Method method : Settings::turboMethods()) {
        if (method == kRayo) continue;  // builds its own loader (rayo.cpp)
        const std::size_t n = codeSize(method);
        CHECK(n > 32);
        // Hand-assembled Z80: starts with DI (0xF3) or a documented prologue.
        CHECK(data::codes[method][0] != 0x00);
    }
}

TEST("loader tables are copied by their real size, not a fixed 360 bytes") {
    // The original did `memcpy(loaderRoutine, codigo, 360)` from arrays that are all
    // much shorter, reading past the end of each table.
    for (Method method : Settings::turboMethods()) CHECK(codeSize(method) < 360);
    // The body length is the NUL-terminated run from offset 5, exactly as the
    // original's strlen(loaderRoutine + 5).  For routines that contain an embedded
    // zero (the Raudo threshold table, for instance) the loader code is
    // truncated there -- which is why the original overrides routineLength with
    // an explicit constant for those methods.
    LoaderRoutine r;
    r.loadMethod(kShavingsRaudo);
    CHECK(r.bodyLength() > 8);
    CHECK(r.bodyLength() <= CodeSizes::raudo - 5);
    // Nothing beyond the table may be non-zero.
    for (std::size_t i = CodeSizes::raudo; i < LoaderRoutine::kCapacity; ++i) CHECK_EQ(int(r[i]), 0);

    // Methods that do not override the length need a zero-free body.
    for (int m : {kRom, kMilks, kFsk, kShavingsSlow, kShavingsDelta, kUltra, kFi, kFiQ,
                  kManchester, kManchesterDif, kEscurrido}) {
        LoaderRoutine q;
        q.loadMethod(static_cast<Method>(m));
        CHECK(q.bodyLength() > 32);
    }
}

TEST("extra (override) routines load and are patchable") {
    LoaderRoutine r;
    for (int e = kRomMini; e <= kNpuR; ++e) {
        r.loadExtra(static_cast<ExtraRoutine>(e));
        CHECK(r.bodyLength() > 8);
    }
    // The Raudo 48 kHz variant is used instead of the primary routine.
    r.loadExtra(kRaudo48000);
    CHECK(r.bodyLength() > 8);
    CHECK(r.bodyLength() <= CodeSizes::raudo48000 - 5);
}

TEST("code extra table covers every ExtraRoutine index") {
    for (int e = 0; e < 8; ++e) {
        const auto* p = data::codes_extra[e];
        CHECK(p != nullptr);
        bool anyNonZero = false;
        for (std::size_t i = 0; i < 8; ++i) anyNonZero = anyNonZero || p[i] != 0;
        CHECK(anyNonZero);
    }
}

TEST("multi-block dispatchers are long enough for seven blocks") {
    // k7zx 4.3 strcpy'd these into a 70 byte buffer; the Manchester one is 66.
    CHECK(CodeSizes::multi_machester <= 70);
    CHECK(CodeSizes::multi_milks <= 80);
    CHECK(CodeSizes::multi_slow <= 80);
    // Each dispatcher must hold a complete entry for seven blocks.
    CHECK(1 + 8 * 7 + 1 <= 80);
    CHECK(1 + 6 * 7 + 1 <= 80);
    CHECK(4 + 7 + 1 <= 80);
}

TEST("snapshot tail code fits its declared length") {
    CHECK_EQ(CodeSizes::load_snap_blocks, 20u);
    CHECK_EQ(CodeSizes::load_snap_blocks_milks, 14u);
    CHECK_EQ(CodeSizes::load_snap_blocks_manchester, 24u);
}

// In rutinas.cpp the dispatcher and tail tables call into the relocated loader
// through `slow [19]`, `milks [19]` and `machester [19]+2` -- the low byte of
// the loader's own `jp pilot`.  A transcription that turned those into the
// literal 19 (0x13) sent every call to 0xFF13, where nothing is loaded.
TEST("dispatcher and tail tables call the relocated loaders' entry points") {
    const std::uint8_t slowEntry = data::codes[kShavingsSlow][19];
    const std::uint8_t milksEntry = data::codes[kMilks][19];
    const std::uint8_t manEntry = static_cast<std::uint8_t>(data::codes[kManchester][19] + 2);
    CHECK_EQ(int(slowEntry), 0x3d);
    CHECK_EQ(int(milksEntry), 0x56);
    CHECK_EQ(int(manEntry), 0x3f);

    // multi_slow: PUSH HL, then 8 x (LD HL,nn / CALL $FF3D), then NUL.
    CHECK_EQ(int(data::multi_slow[0]), 0xe5);
    for (int b = 0; b < 8; ++b) {
        const std::uint8_t* e = data::multi_slow + 1 + 6 * b;
        CHECK_EQ(int(e[0]), 0x21);
        CHECK_EQ(int(e[3]), 0xcd);
        CHECK_EQ(int(e[4]), int(slowEntry));
        CHECK_EQ(int(e[5]), 0xff);
    }
    // multi_machester: PUSH HL, then 8 x (LD HL,nn / LD A,$58 / CALL $FF3F).
    CHECK_EQ(int(data::multi_machester[0]), 0xe5);
    for (int b = 0; b < 8; ++b) {
        const std::uint8_t* e = data::multi_machester + 1 + 8 * b;
        CHECK_EQ(int(e[0]), 0x21);
        CHECK_EQ(int(e[3]), 0x3e);
        CHECK_EQ(int(e[5]), 0xcd);
        CHECK_EQ(int(e[6]), int(manEntry));
        CHECK_EQ(int(e[7]), 0xff);
    }
    CHECK_EQ(int(data::multi_machester[CodeSizes::multi_machester - 1]), 0);
    // multi_milks: PUSH HL / LD HL,$FF56 / PUSH HL...
    CHECK_EQ(int(data::multi_milks[2]), int(milksEntry));
    CHECK_EQ(int(data::load_snap_blocks[7]), int(slowEntry));
    CHECK_EQ(int(data::load_snap_blocks_milks[4]), int(milksEntry));
    CHECK_EQ(int(data::load_snap_blocks_manchester[9]), int(manEntry));
    CHECK_EQ(int(data::lprint_code[12]), int(milksEntry));
    CHECK_EQ(int(data::lprint_code[18]), int(milksEntry));
}

TEST("system variables and BASIC stub are complete") {
    // SYSTEM_VARS is copied into memory at 23552 and must not read past itself.
    CHECK_EQ(CodeSizes::SYSTEM_VARS, 203u);
    CHECK_EQ(CodeSizes::BAS, 52u);
    CHECK_EQ(CodeSizes::multi_128, 76u);
    CHECK_EQ(CodeSizes::deco_snap, 64u);
}

TEST("every loading technique keeps the name the original gave it") {
    // These strings are the interface: the original's `tecnica[]` and its
    // MaNonTrppo captions.  Renaming or dropping a technique is not allowed,
    // so they are pinned here.
    // tecnica[] in SettingdForm.cpp.
    static const char* const kExpected[] = {
        "ROM",      "Milks", "FSK",         "S. Slow", "S. Delta", "S. Raudo",
        "Ultra",    "NPU",   "Fi",          "Fi Quadruple", "Manchester",
        "Man. diferencial", "Escurrido"};
    // The radio captions in MainForm.dfm, which spell two of them out in full.
    static const char* const kCaptions[] = {
        "ROM",          "Milks",        "FSK",           "Shavings Slow",
        "Shavings Delta", "Shavings Raudo", "Ultra",     "NPU",
        "Fi",           "Fi Quadruple", "Manchester",    "Man. diferencial",
        "Escurrido"};
    for (int m = kRom; m <= kEscurrido; ++m) {
        CHECK_EQ(std::string(texts::methodName(static_cast<Method>(m))),
                 std::string(kExpected[m]));
        CHECK_EQ(std::string(texts::methodCaption(static_cast<Method>(m))),
                 std::string(kCaptions[m]));
    }
}

TEST("method and scheme tags are defined for every value") {
    for (Method m : Settings::turboMethods()) {
        CHECK(std::string(texts::methodName(m)) != "???");
        CHECK(std::string(texts::methodTag(m)).size() > 1);
        CHECK(std::string(texts::explanation(m)).size() >= 3);
    }
    // The reserved values 13-19 are gone but keep their slots, so an out-of-range
    // `Ops.Method` still gets a printable placeholder rather than reading past
    // the tables.
    for (int m = 13; m <= 19; ++m) {
        CHECK_EQ(std::string(texts::methodName(static_cast<Method>(m))), std::string("?"));
        CHECK(std::string(texts::methodTag(static_cast<Method>(m))).size() > 1);
        CHECK_EQ(Settings::turboIndex(static_cast<Method>(m)), -1);
    }
    for (int s = 0; s < 4; ++s) CHECK(std::string(texts::schemeTag(static_cast<Scheme>(s))).size() > 1);
    for (int mode = 0; mode < kConvertModeCount; ++mode)
        CHECK(std::string(texts::conversionMode(mode)).size() > 20);
}

// --- legacy INI migration ---------------------------------------------------
// The original wrote four settings under keys the first version of this port
// never read, so an existing k7zx.ini silently lost them. These are the keys
// whose absence was a real behaviour change.

namespace {

Settings loadFrom(const char* body) {
    const std::string path = "/tmp/k7zx-legacy-test.ini";
    std::ofstream out(path);
    out << body;
    out.close();
    Settings s;
    loadSettings(path, s);
    return s;
}

}  // namespace

TEST("legacy Ops.Basic selects the original-loader scheme") {
    // The scheme choices were radio buttons, so Basic=1 means the user picked
    // the original loader.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBasic=1\r\n").scheme),
             static_cast<int>(kOriginalLoader));
    // Basic=0 on its own says only "not the original loader".  A real 4.3 file
    // always carried Ops.Bloques alongside it, so with that key absent the
    // default stands; see the Bloques test for the combinations that do occur.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBasic=0\r\n").scheme),
             static_cast<int>(kManyBlocks));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBasic=0\r\nBloques=0\r\n").scheme),
             static_cast<int>(kOneBlock));
}

TEST("legacy Ops.MetodoHI restores the high-speed technique") {
    // MetodoHI=4 is Shavings Delta.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nMetodoHI=4\r\n").method),
             static_cast<int>(kShavingsDelta));
}

TEST("legacy Ops.Mode 2 loads as high speed with FSK at 5.00") {
    // Mode 2 was the low-rate "Ma non troppo" mode, removed in step 15. An old
    // config that still names it must load as high speed with the replacement
    // the removal notes recommend, not crash and not go quiet.  5.00 is one of
    // the FSK speeds that survives every analogue channel condition (step 16).
    const Settings s = loadFrom("[Ops]\r\nMode=2\r\n");
    CHECK_EQ(s.conversionMode, static_cast<int>(kConvertHiSpeed));
    CHECK_EQ(s.method, static_cast<int>(kFsk));
    CHECK_EQ(s.samplesPerBit, static_cast<int>(kS5_00));
    // ...and the legacy Spanish spelling behaves the same way.
    const Settings legacy = loadFrom("[Ops]\r\nConversor=2\r\n");
    CHECK_EQ(legacy.conversionMode, static_cast<int>(kConvertHiSpeed));
    CHECK_EQ(legacy.method, static_cast<int>(kFsk));
    CHECK_EQ(legacy.samplesPerBit, static_cast<int>(kS5_00));
}

TEST("an out-of-range Ops.Mode loads as high speed with FSK at 5.00") {
    for (const char* v : {"-1", "3", "99"}) {
        const std::string body = std::string("[Ops]\r\nMode=") + v + "\r\n";
        const Settings s = loadFrom(body.c_str());
        CHECK_EQ(s.conversionMode, static_cast<int>(kConvertHiSpeed));
        CHECK_EQ(s.method, static_cast<int>(kFsk));
        CHECK_EQ(s.samplesPerBit, static_cast<int>(kS5_00));
    }
}

TEST("Ops.Method 13-19 fall back to the default high-speed technique") {
    // 13-18 were the low-rate techniques, 19 was Veloz; all removed in step 15.
    for (int v = 13; v <= 19; ++v) {
        const std::string body = "[Ops]\r\nMethod=" + std::to_string(v) + "\r\n";
        const Settings s = loadFrom(body.c_str());
        CHECK_EQ(s.method, static_cast<int>(kShavingsRaudo));
    }
    // The legacy key behaves the same way.
    CHECK_EQ(loadFrom("[Ops]\r\nMetodoMA=3\r\nMetodoHI=16\r\n").method,
             static_cast<int>(kShavingsRaudo));
    // Out-of-range values are caught the same way.
    CHECK_EQ(loadFrom("[Ops]\r\nMethod=250\r\n").method, static_cast<int>(kShavingsRaudo));
    // A technique that still exists is left alone.
    CHECK_EQ(loadFrom("[Ops]\r\nMethod=2\r\n").method, static_cast<int>(kFsk));
}

TEST("Ops.MusicMethod is ignored on read and no longer written") {
    // It held the low-rate technique chosen in the removed mode.
    const std::string path = "/tmp/k7zx-legacy-test.ini";
    CHECK(saveSettings(path, Settings{}));
    std::ifstream in(path);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("MusicMethod") == std::string::npos);
    // Reading a config that still carries it must not disturb anything.
    const Settings s = loadFrom("[Ops]\r\nMusicMethod=3\r\nMethod=2\r\n");
    CHECK_EQ(s.method, static_cast<int>(kFsk));
}

TEST("legacy info-in-filename keys are both honoured") {
    // The original read Ops.InfoinFile in the player form and Ops.Info in the
    // main form, with conflicting defaults.
    CHECK_EQ(loadFrom("[Ops]\r\nInfoinFile=1\r\n").infoInFileName, true);
    CHECK_EQ(loadFrom("[Ops]\r\nInfo=1\r\n").infoInFileName, true);
    // The new key still wins when present.
    CHECK_EQ(loadFrom("[Ops]\r\nInfo=0\r\n[Files]\r\nInfo=1\r\n").infoInFileName, true);
}

TEST("legacy Files.Lame restores the mp3 encoder path in either case") {
    // The original read `Files.Lame` but wrote `Files.lame`; the difference was
    // invisible on Windows, where INI keys are case-insensitive.
    CHECK_EQ(loadFrom("[Files]\r\nLame=/opt/lame\r\n").lamePath, std::string("/opt/lame"));
    CHECK_EQ(loadFrom("[Files]\r\nlame=/opt/lame2\r\n").lamePath, std::string("/opt/lame2"));
    CHECK_EQ(loadFrom("[Files]\r\nLamePath=/opt/new\r\nLame=/opt/old\r\n").lamePath,
             std::string("/opt/new"));
}

TEST("INI keys and section names are read whatever their case") {
    // Windows INI keys are case-insensitive and the original depended on it, so
    // a k7zx.ini it wrote could carry any spelling.  The reader compared keys
    // byte for byte, so on Linux such a file was silently ignored -- and the
    // program quietly fell back to its defaults.  Anything that rewrites the
    // file (an INI library, an editor, a config manager) can lowercase it too.
    CHECK_EQ(static_cast<int>(loadFrom("[ops]\r\nMODE=0\r\n").conversionMode),
             static_cast<int>(kConvertNormal));
    CHECK_EQ(static_cast<int>(loadFrom("[OPS]\r\nmode=0\r\n").conversionMode),
             static_cast<int>(kConvertNormal));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nmethod=8\r\n").method),
             static_cast<int>(kFi));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nMETHOD=8\r\n").method),
             static_cast<int>(kFi));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nSamplesPerBit=20\r\n").samplesPerBit),
             static_cast<int>(kS5_00));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nsamplesperbit=20\r\n").samplesPerBit),
             static_cast<int>(kS5_00));
    CHECK_EQ(loadFrom("[Files]\r\nlamepath=/opt/x\r\n").lamePath, std::string("/opt/x"));
    CHECK(loadFrom("[Ops]\r\nINVERT=1\r\n").invert);
    CHECK(loadFrom("[Ops]\r\nInVeRt=1\r\n").invert);
    CHECK(loadFrom("[Ops]\r\nFINAL=1\r\n").finalTone);
    CHECK(loadFrom("[Ops]\r\nFINAL=1\r\n").stereo == false);
}


// --- the high-speed technique list ------------------------------------------
// The Method enum is not contiguous across the high-speed techniques: values
// 13-19 were removed in step 15 and Rayo sits at 20. The list is therefore
// maintained explicitly in Settings::turboMethods(), and every accessor that
// indexes an array by the raw Method has to be checked against that.

TEST("the bit-rate keys are not written, because the rate is fixed") {
    // The original had no user-facing bit-rate control: its MPBCmbBx combo was
    // the samples-per-bit selector, and 256 kbps was hard-coded. A fresh
    // config must not gain the per-mode keys, even though an old one that still
    // carries them round-trips unchanged like any other unrecognised key.
    const std::string path = "/tmp/k7zx-legacy-test.ini";
    CHECK(saveSettings(path, Settings{}));
    std::ifstream in(path);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("RatioMA") == std::string::npos);
    CHECK(all.find("RatioNOR") == std::string::npos);
}

TEST("every technique has a usable name, tag, caption and explanation") {
    // Over the surviving techniques; 13-19 are reserved (step 15) and are
    // covered by the placeholder test above.
    for (const Method M : Settings::turboMethods()) {
        CHECK(texts::methodName(M) != nullptr);
        CHECK(std::string(texts::methodName(M)).size() > 0);
        CHECK(texts::methodTag(M) != nullptr);
        CHECK(std::string(texts::methodTag(M)).size() > 1);
        CHECK(texts::explanation(M) != nullptr);
        CHECK(std::string(texts::explanation(M)).size() >= 3);
        // The regression: methodCaption() used to index a 13 entry array by the
        // raw Method, so it returned "Veloz" for Andante and null for the rest.
        CHECK(texts::methodCaption(M) != nullptr);
        CHECK(std::string(texts::methodCaption(M)).size() > 0);
    }
}

TEST("turboMethods lists each high-speed technique exactly once, Rayo included") {
    const std::vector<Method>& v = Settings::turboMethods();
    // The thirteen originals in their original order, then Rayo.
    const Method expect[] = {kRom,           kMilks,           kFsk,
                             kShavingsSlow,  kShavingsDelta,  kShavingsRaudo,
                             kUltra,         kNpu,             kFi,
                             kFiQ,           kManchester,      kManchesterDif,
                             kEscurrido,     kRayo};
    CHECK_EQ(v.size(), sizeof expect / sizeof expect[0]);
    for (std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] == expect[i]);
        // And the index round-trips, which is what the GUI combo relies on.
        CHECK_EQ(Settings::turboIndex(v[i]), static_cast<int>(i));
        CHECK(std::string(texts::methodCaption(v[i])).size() > 0);
    }
    // None of the reserved values 13-19 is in the high-speed list.
    for (int m = 13; m <= 19; ++m) CHECK_EQ(Settings::turboIndex(static_cast<Method>(m)), -1);
}

TEST("every high-speed technique offers at least one speed") {
    for (Method m : Settings::turboMethods())
        CHECK(!Settings::allowedSamplesPerBit(m).empty());
}

TEST("samples-per-bit parsing handles the 1.33 special case") {
    // The bug: parsing scaled by four, which only works for quarter-integers.
    // 1.33 has its own enum slot, so `-s 1.33` used to resolve to 1.25 and
    // Escurrido's slowest speed was unreachable from the command line.
    int v = 0;
    // Both separators are accepted, and the bare integer forms: "3" is 3.00 and
    // "275" is 2.75.
    CHECK(Settings::parseSamplesPerBit("2.75", v)); CHECK_EQ(v, kS2_75);
    CHECK(Settings::parseSamplesPerBit("2,75", v)); CHECK_EQ(v, kS2_75);
    CHECK(Settings::parseSamplesPerBit("275", v));  CHECK_EQ(v, kS2_75);
    CHECK(Settings::parseSamplesPerBit("3", v));    CHECK_EQ(v, kS3_00);
    CHECK(Settings::parseSamplesPerBit("3,5", v));  CHECK_EQ(v, kS3_50);
    CHECK(Settings::parseSamplesPerBit("1.33", v)); CHECK_EQ(v, kS1_33);
    CHECK(Settings::parseSamplesPerBit("133", v));  CHECK_EQ(v, kS1_33);
    // Nonsense is refused rather than silently becoming a valid speed.
    CHECK(!Settings::parseSamplesPerBit("", v));
    CHECK(!Settings::parseSamplesPerBit("0", v));
    CHECK(!Settings::parseSamplesPerBit("-2.5", v));
    CHECK(!Settings::parseSamplesPerBit("2.75x", v));
}

TEST("methodAllowsSamplesPerBit agrees with allowedSamplesPerBit") {
    for (Method m : Settings::turboMethods()) {
        const std::vector<int> ok = Settings::allowedSamplesPerBit(m);
        for (int v : ok) CHECK(Settings::methodAllowsSamplesPerBit(m, v));
        // And a speed the technique does not offer is rejected, which is what
        // stops the loader patcher silently emitting a wave that cannot load.
        for (int v = 4; v <= 32; ++v) {
            bool listed = false;
            for (int x : ok) listed = listed || x == v;
            if (!listed) CHECK(!Settings::methodAllowsSamplesPerBit(m, v));
        }
    }
}

// ---------------------------------------------------------------------------
// Migration from a real k7zx 4.3 k7zx.ini
//
// The 4.3 main window wrote its whole configuration on every exit
// (original/MainForm.cpp:230-264) and read it back with ReadBool for several
// keys the port had been reading as integers, or looking for in the wrong
// section.  Each of these used to migrate to the wrong setting.
// ---------------------------------------------------------------------------

TEST("settings are saved into a config directory that does not exist yet") {
    // The original wrote k7zx.ini next to the executable, where the directory
    // always existed.  This port moved it under XDG_CONFIG_HOME, so on a machine
    // that had never run k7zx the write failed and every change was lost.
    const std::string dir = "/tmp/k7zx-mkdir-test";
    std::remove((dir + "/k7zx/k7zx.ini").c_str());
    std::remove(dir.c_str());
    Settings s;
    s.method = kFsk;
    s.samplesPerBit = kS5_00;
    CHECK(saveSettings(dir + "/k7zx/k7zx.ini", s));

    Settings back;
    CHECK(loadSettings(dir + "/k7zx/k7zx.ini", back));
    CHECK_EQ(static_cast<int>(back.method), static_cast<int>(kFsk));
    CHECK_EQ(back.samplesPerBit, static_cast<int>(kS5_00));
}

TEST("legacy Ops.Bloques is a Bool, not a scheme index") {
    // WriteBool("Ops","Bloques", BloquesRB->Checked): true means MANY blocks.
    // Reading it as Scheme(1) gave kOneBlock, the exact opposite.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBloques=1\r\nBasic=0\r\n").scheme),
             static_cast<int>(kManyBlocks));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBloques=0\r\nBasic=0\r\n").scheme),
             static_cast<int>(kOneBlock));
    // The settings dialog wrote the same key as an integer scheme index.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBloques=2\r\n").scheme),
             static_cast<int>(kManyBlocks));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBloques=3\r\n").scheme),
             static_cast<int>(kOriginalLoader));
}

TEST("legacy Ops.Basic still selects the original loader") {
    // It can only apply when the many/one choice was off, and it must not
    // override an explicit many-blocks setting.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBasic=1\r\n").scheme),
             static_cast<int>(kOriginalLoader));
    // Basic wins: it is the radio the user actually ticked.
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nBloques=1\r\nBasic=1\r\n").scheme),
             static_cast<int>(kOriginalLoader));
    // and the modern spelling is a straight index
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nManyBlocks=3\r\n").scheme),
             static_cast<int>(kOriginalLoader));
}


TEST("legacy Ops.Pokeador is read from the Ops section") {
    // The old lookup prefixed it with "Files.", so no existing config ever had
    // its poke box restored.
    CHECK(loadFrom("[Ops]\r\nPokeador=1\r\n").usePokes);
    CHECK(!loadFrom("[Ops]\r\nPokeador=0\r\n").usePokes);
    CHECK(loadFrom("[Files]\r\nUsePokes=1\r\n").usePokes);
}

TEST("legacy Ops.Metodo, which the main window wrote, selects the technique") {
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nMetodo=2\r\n").method),
             static_cast<int>(kFsk));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nMetodoHI=2\r\n").method),
             static_cast<int>(kFsk));
    CHECK_EQ(static_cast<int>(loadFrom("[Ops]\r\nMethod=2\r\n").method),
             static_cast<int>(kFsk));
}

TEST("an INI value containing a newline or an equals sign round-trips") {
    // Written unquoted, a newline in a path became a spurious extra key on the
    // next read, and an embedded '=' truncated the value.
    const std::string path = "/tmp/k7zx-quote-test.ini";
    Settings s;
    s.lastDirectory = "/tmp/dir\nOps.Mode=0";
    s.lamePath = "a=b";
    CHECK(saveSettings(path, s));

    Settings back;
    CHECK(loadSettings(path, back));
    CHECK_EQ(back.lastDirectory, s.lastDirectory);
    CHECK_EQ(back.lamePath, s.lamePath);
}

TEST("saving does not resurrect settings that were removed") {
    // saveSettings merges into the existing file, which used to leave every
    // legacy key in place forever -- so a stale MusicMethod or MetodoMA stayed
    // behind, keeping the migration paths alive after the first save.
    const std::string path = "/tmp/k7zx-legacy-stays.ini";
    {
        std::ofstream out(path);
        out << "[Ops]\r\nMusicMethod=3\r\nMetodoMA=16\r\nMetodoHI=2\r\nBasic=1\r\n"
               "Bloques=2\r\nMethod=2\r\n";
    }
    Settings s;
    loadSettings(path, s);
    CHECK(saveSettings(path, s));

    std::ifstream in(path);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("MusicMethod") == std::string::npos);
    CHECK(all.find("MetodoMA") == std::string::npos);
    CHECK(all.find("MetodoHI") == std::string::npos);
    // The keys the port does write are all there, under their English names.
    CHECK(all.find("Method=2") != std::string::npos);
    CHECK(all.find("ManyBlocks=") != std::string::npos);
    CHECK(all.find("MusicMethod") == std::string::npos);
}

TEST("every setting survives a save and reload") {
    // Regression: "Statistical optimisation" was written to the file and then
    // deleted again by the superseded-key cleanup, so the tick box silently
    // reset on every exit.  The round trip is checked for every option the GUI
    // and CLI can set.
    const std::string path = "/tmp/k7zx-roundtrip-all.ini";
    Settings s;
    s.conversionMode = kConvertHiSpeed;
    s.sampleRate = 44100;
    s.waveform = kCubic;
    s.method = kFiQ;
    s.samplesPerBit = kS2_25;
    s.scheme = kManyBlocks;
    s.antiKolmogorov = true;      // the one that was being dropped
    s.compress = false;
    s.controlChecksum = false;
    s.invert = true;
    s.invertRight = true;
    s.finalTone = true;
    s.stereo = true;
    s.accelerateBasic = true;
    s.generateLoader = false;
    s.usePokes = true;
    s.infoInFileName = true;
    s.lamePath = "/opt/lame";
    s.lastDirectory = "/tmp";
    s.outputDirectory = "/tmp/out";
    s.pokes.push_back(Poke{0x8000, 0xAA});
    s.windowWidth = 1600;
    s.windowHeight = 1000;
    CHECK(saveSettings(path, s));

    Settings back;
    CHECK(loadSettings(path, back));
    CHECK_EQ(static_cast<int>(back.conversionMode), static_cast<int>(kConvertHiSpeed));
    CHECK_EQ(back.sampleRate, 44100);
    CHECK_EQ(static_cast<int>(back.waveform), static_cast<int>(kCubic));
    CHECK_EQ(static_cast<int>(back.method), static_cast<int>(kFiQ));
    CHECK_EQ(back.samplesPerBit, static_cast<int>(kS2_25));
    CHECK_EQ(static_cast<int>(back.scheme), static_cast<int>(kManyBlocks));
    CHECK(back.antiKolmogorov);
    CHECK(!back.compress);
    CHECK(!back.controlChecksum);
    CHECK(back.invert);
    CHECK(back.invertRight);
    CHECK(back.finalTone);
    CHECK(back.stereo);
    CHECK(back.accelerateBasic);
    CHECK(!back.generateLoader);
    CHECK(back.usePokes);
    CHECK(back.infoInFileName);
    CHECK_EQ(back.lamePath, std::string("/opt/lame"));
    CHECK_EQ(back.lastDirectory, std::string("/tmp"));
    CHECK_EQ(back.outputDirectory, std::string("/tmp/out"));
    CHECK_EQ(back.pokes.size(), 1u);
    CHECK_EQ(back.pokes[0].address, 0x8000);
    CHECK_EQ(back.pokes[0].value, 0xAA);
    // A deliberately enlarged window has to survive: it used to be written on
    // every exit and then applied with `std::max(h, 0)`, which can never widen
    // a window, so the preference did nothing.
    CHECK_EQ(back.windowWidth, 1600);
    CHECK_EQ(back.windowHeight, 1000);
}

TEST("samplesToBps is the authority for every published bit rate") {
    // The guide publishes 59 bit-rate figures and nothing tests this function,
    // so a change to the formula (or to the 1.33 special case) would move every
    // one of them without failing a test.  check_findings.py re-implements the
    // formula in Python and compares the *documents*; this pins the C++ itself.
    // The values are 48000 * 4 / samplesPerBit by integer division, except 1.33,
    // which is not a quarter-integer and has its own rule.
    CHECK_EQ(samplesToBps(48000, kS8_00), 6000);
    CHECK_EQ(samplesToBps(48000, kS7_00), 6857);
    CHECK_EQ(samplesToBps(48000, kS6_00), 8000);
    CHECK_EQ(samplesToBps(48000, kS5_00), 9600);
    CHECK_EQ(samplesToBps(48000, kS4_00), 12000);
    CHECK_EQ(samplesToBps(48000, kS3_50), 13714);
    CHECK_EQ(samplesToBps(48000, kS3_00), 16000);
    CHECK_EQ(samplesToBps(48000, kS2_75), 17454);
    CHECK_EQ(samplesToBps(48000, kS2_50), 19200);
    CHECK_EQ(samplesToBps(48000, kS2_25), 21333);
    CHECK_EQ(samplesToBps(48000, kS2_00), 24000);
    CHECK_EQ(samplesToBps(48000, kS1_75), 27428);
    CHECK_EQ(samplesToBps(48000, kS1_50), 32000);
    CHECK_EQ(samplesToBps(48000, kS1_25), 38400);
    CHECK_EQ(samplesToBps(48000, kS1_33), 36000);   // the special case
    // At 44.1 kHz the guide quotes a second set.
    CHECK_EQ(samplesToBps(44100, kS8_00), 5512);
    CHECK_EQ(samplesToBps(44100, kS5_00), 8820);
    CHECK_EQ(samplesToBps(44100, kS1_25), 35280);
    // Degenerate input must not divide by zero.
    CHECK_EQ(samplesToBps(48000, 0), 0);
    CHECK_EQ(samplesToBps(48000, -3), 0);
}

TEST("the published technique and speed lists match the code") {
    // 61 combinations across 14 techniques.  Nothing else in the suite compares
    // this against the measurement snapshot, so a changed speed list would leave
    // the guide's 61 rating rows quietly describing a different experiment.
    int combinations = 0;
    for (const Method m : Settings::turboMethods()) {
        const std::vector<int>& v = Settings::allowedSamplesPerBit(m);
        CHECK(!v.empty());
        combinations += static_cast<int>(v.size());
        // Every listed speed must also be accepted for that technique, and every
        // enum value that is not a quarter-integer must be the 1.33 slot.
        for (const int spb : v) CHECK(Settings::methodAllowsSamplesPerBit(m, spb));
    }
    CHECK_EQ(combinations, 61);
    CHECK_EQ(Settings::turboMethods().size(), 14u);
    // The reserved values and the out-of-range one stay out of the list.
    for (int m = 13; m <= 19; ++m)
        CHECK_EQ(Settings::turboIndex(static_cast<Method>(m)), -1);
}
