// k7zx 5.0 - modern C++ port
//
// User-facing strings, transcribed from the original source's textos.cpp
// (called texts.cpp here).
#include "texts.h"

#include "settings.h"  // for Settings::turboIndex

namespace k7zx {
namespace texts {
namespace {

// The original's `tecnica[]`, verbatim -- this is the list shown in the
// options dialog's "Encoding technic" combo.  The trailing "?" is the
// placeholder it used for the combo's unused tail.
// Slots 13-19 were the low-rate techniques and Veloz, both removed; the
// values are reserved so a saved `Ops.Method` cannot drift onto another
// technique.  They render as the "?" the original used for its unused tail.
const char* const kMethodNames[kMethodCount + 1] = {
    "ROM",         "Milks",         "FSK",          "S. Slow",     "S. Delta",
    "S. Raudo",    "Ultra",         "NPU",          "Fi",          "Fi Quadruple",
    "Manchester",  "Man. diferencial", "Escurrido",
    "?",           "?",             "?",            "?",           "?",
    "?",           "?",
    "Rayo",        "?"};

// The main window spells the two "Shavings" variants out in full; these are the
// radio button captions from MainForm.dfm.  The original was inconsistent
// here, so both spellings are kept rather than normalised away.
// Indexed by Settings::turboIndex(), NOT by the raw Method value: the enum is
// not contiguous across the high-speed techniques -- Rayo, added by this port,
// sits after the gap left by the removed techniques -- so indexing by
// Method would run off the end.
static const char* const kMethodCaptions[] = {
    "ROM",             "Milks",         "FSK",             "Shavings Slow",
    "Shavings Delta",  "Shavings Raudo", "Ultra",          "NPU",
    "Fi",              "Fi Quadruple",  "Manchester",      "Man. diferencial",
    "Escurrido",       "Rayo"};
static_assert(sizeof kMethodCaptions / sizeof kMethodCaptions[0] == 14,
              "kMethodCaptions must hold exactly one entry per turbo technique");

// info_tecnica[] in the original SettingdForm.cpp -- the short tag k7zx
// embeds in a generated file name to mark the speed.
// Slots 13-19 are reserved (removed techniques); see the note on
// kMethodNames above.  They keep the original's "_???_" fallback.
const char* const kMethodTags[kMethodCount + 1] = {
    "_ROM_", "_MLK_", "_FSK_", "_SSL_", "_SDE_", "_SRA_", "_ULT_", "_NPU_", "_FI__",
    "_FIQ_", "_MAN_", "_MDF_", "_ESC_", "_???_", "_???_", "_???_", "_???_", "_???_",
    "_???_", "_???_", "_RAY_", "_???_"};

// info_kbps[] in SettingdForm.cpp -- indexed by samples-per-bit value.
const char* const kSamplesTags[] = {"?",     "?",     "?",     "?",     "1.33", "1.25",
                                    "1.50",  "1.75",  "2.00",  "2.25",  "2.50", "2.75",
                                    "3.00",  "?",     "3.50",  "?",     "4.00", "?",
                                    "?",     "?",     "5.00",  "?",     "?",    "?",
                                    "6.00",  "?",     "?",     "?",     "7.00", "?",
                                    "?",     "?",     "8.00",  "?",     "?",    "?",
                                    "?",     "?"};

// nom_esquema[] -- scheme tags appended to generated file names.
const char* const kSchemeTags[4] = {"_SNAP_", "_UNBL_", "_VBLO_", "_ORIG_"};

constexpr const char* t_rom =
    "One bit is encoded by the length between a rising edge and the "
    "following falling edge (the length of a positive pulse). If length of negative "
    "pulse is equal to positive one, data is encoded in the reverse wav too. This is the "
    "technical used by the loading routine in spectrum ROM \r\n"
    " If length of negative pulse is constant, the load will "
    "be faster. The code for the loading routine is the simplest.";

constexpr const char* t_milks =
    "Same as ROM but a positive pulse encodes 2 bit at a time. Only"
    " 3.5 sample/bit will work with both direct and reverse polarity.\r\n Milks loading routine "
    "is the only one in k7zx that includes a header with address and size of each block."
    ": So it can emulate LOAD \"\" CODE.";

constexpr const char* t_fsk =
    "One bit is encoded by the length between two falling edges. It "
    "works reliably in a wide range of speeds.";

constexpr const char* t_slow =
    "Same as FSK but a cycle encodes 2 bit  at a time. "
    "Cubic  or Saw waveform is preferred depending on Spectrum model";

constexpr const char* t_delta =
    "Same as FSK but a cycle encodes 2 bit at a time. "
    "The length of positive pulse is constant (1 or 2 samples). Because of that it "
    "will work with direct and reverse polarity and additional code for detecting "
    "polarity is not needed.";

constexpr const char* t_raudo =
    "Same as FSK but a cycle encodes 2 bit  at a time. "
    "A more efficient code in the loading routine allows 27,428 bps!! into a real"
    " Spectrum.\r\n No multi block , no control checksum,";

constexpr const char* t_ultra =
    "Negative pulse encodes one bit and positive pulse encodes "
    "following bit. This technical requires a fine adjustment of volume and select the "
    " better wave form (0,3,4). But it's worth it : 22,050 bps can be achieved";

constexpr const char* t_npu = "Same as Ultra but encodes 2 bits for pulse.\r\n ";

constexpr const char* t_fi =
    "One bit is encoded by the difference between the lengths of the positive"
    " pulse and negative pulse of a cycle. Cycle length is constant. The same routine in memory "
    "can load data at different speeds. All others routines in k7zx have parameters to be adjusted"
    "   for each speed.";

constexpr const char* t_fiq = "Same as Fi but encodes 2 bits at a time.";

constexpr const char* t_manchester =
    "One bit is encoded by low and high levels synchronised with"
    " a clock frequency. See details in wikipedia\r\n Better Wave form: 4 equal energy";

constexpr const char* t_man_dif =
    "One bit is encoded by low and high levels synchronised with"
    " a clock frequency. See details in wikipedia\r\n Better Wave form: 4 equal energy";

constexpr const char* escurridoHelp =
    "It works as Shavings Delta but more complicated to be faster ";

constexpr const char* t_none = "???";
// Not in k7zx: added by this port alongside the k7zx 4.3 techniques.

constexpr const char* c_rayo =
    "Added by this port (not part of k7zx 4.3). The signal of Shavings Raudo -- two "
    "bits per wave cycle, decoded from the full cycle so the EAR input's threshold "
    "offset cannot skew it -- with what Raudo lacks: a checksum and end check, so a "
    "bad load stops with \"R Tape loading error\" instead of crashing; automatic "
    "polarity; immunity to a key held down while loading; the commonest bit pairs "
    "sent as the shortest cycles; and optional LZ compression, expanded in place "
    "by the loader, which on typical games halves the loading time.\r\n"
    "2.25 or 2.75 samples/bit at 44100 or 48000 Hz; always loads as one block; "
    "48K snapshots and tapes.";

// Slots 13-18 were the low-rate techniques and 19 was Veloz; both removed in
// port, so they fall back to t_none.
const char* const kExplanations[kMethodCount] = {
    t_rom,   t_milks, t_fsk,     t_slow,       t_delta,       t_raudo,
    t_ultra, t_npu,  t_fi,      t_fiq,        t_manchester,  t_man_dif,
    escurridoHelp, t_none, t_none, t_none, t_none, t_none, t_none, t_none, c_rayo};

constexpr const char* c_normal =
    " Tap and Tzx files (not snapshots) are converted to "
    "the original wav file just as many other well known utilities (tap2wav"
    ", tzx2wav.. ) do.\r\nBut the wave form in the .wav file is not a "
    "square one. It is  a polynomial (cubic) function ( x*(x-P1)*(x-2*P1)).\r\n"
    "With this wave form it is possible to convert the wave file to a mp3 file with notable"
    " compression rates   For instance 32k  Manic Miner results a 8,200 KB wav (8 bits mono). \r\n"
    " And this wav can be compressed to a  559 KB mp3 file";

constexpr const char* c_hi =
    " Tap , tzx , sna and z80 files are converted to a wav "
    "file that it's loaded  into a real Spectrum in few seconds (for instance normal "
    "length 190 seconds of Manic Miner can be loaded in 9,5 s.). \r\n"
    " Different encoding techniques can be used. Fsk at 5 , S.Slow at 3.5 and S.Raudo at 2.75"
    "  samples/bit have the best speed/reliability ratio\r\n"
    " (Tap an Tzx files can be converted only if they contain blocks of data with their headers"
    " They should be also contain correct CLEAR and USR parameters. But if not, they can "
    " be fixed manually. Up to 7 blocks can be loaded with  \"many blocks\" option)";


// Mode 2 was the low-rate "Ma non troppo" mode, removed.  The slot
// is kept so the array stays indexed by ConversionMode; the removed blurb reads
// as the normal-mode one, which is what an out-of-range mode falls back to.
const char* const kConversionModes[kConvertModeCount] = {c_normal, c_hi, c_normal};

}  // namespace

const char* methodName(Method m) {
    return (m >= 0 && m < kMethodCount) ? kMethodNames[m] : kMethodNames[kMethodCount];
}

const char* methodCaption(Method m) {
    // Never returns null. It used to: the array held 13 captions but was
    // indexed by Method, so techniques past that point read off its end.
    const int i = Settings::turboIndex(m);
    return (i >= 0 && static_cast<std::size_t>(i) < sizeof kMethodCaptions / sizeof kMethodCaptions[0])
               ? kMethodCaptions[i]
               : methodName(m);
}

const char* methodTag(Method m) {
    return (m >= 0 && m < kMethodCount) ? kMethodTags[m] : kMethodTags[kMethodCount];
}

const char* schemeTag(Scheme s) {
    return (s >= 0 && s < 4) ? kSchemeTags[s] : kSchemeTags[0];
}

const char* samplesTag(int samplesPerBit) {
    constexpr int kLast = static_cast<int>(sizeof(kSamplesTags) / sizeof(kSamplesTags[0])) - 1;
    return (samplesPerBit >= 0 && samplesPerBit <= kLast) ? kSamplesTags[samplesPerBit] : "?";
}

const char* explanation(Method m) {
    return (m >= 0 && m < kMethodCount) ? kExplanations[m] : t_none;
}

const char* conversionMode(int mode) {
    return (mode >= 0 && mode < kConvertModeCount) ? kConversionModes[mode] : c_normal;
}

}  // namespace texts
}  // namespace k7zx
