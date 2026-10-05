// k7zx 5.0 - modern C++ port
//
// The conversion façade shared by the GUI and the CLI.
#include "convert.h"

#include <algorithm>

#include "texts.h"

namespace k7zx {
namespace {

const char* schemeName(Scheme s) {
    switch (s) {
        case kOneBlock: return "one";
        case kManyBlocks: return "many";
        case kOriginalLoader: return "original";
        default: return "snapshot";
    }
}

std::string baseNameOf(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

}  // namespace

Converter::Converter() : image_(std::make_unique<TapeImage>()) {}

bool Converter::load(const std::string& path) {
    error_.clear();
    loaded_ = false;
    inputPath_ = path;
    image_ = std::make_unique<TapeImage>();
    const ReadError e = image_->read(path);
    if (e != ReadError::none) {
        error_ = image_->errors();
        if (error_.empty()) error_ = readErrorText(e);
        return false;
    }
    loaded_ = true;
    return true;
}

void Converter::selectAllBlocks(bool on) {
    auto& t = image_->blocks();
    for (int i = 0; i < image_->blockCount() && i < static_cast<int>(t.size()); ++i) {
        if (t[static_cast<std::size_t>(i)].isTerminator()) break;
        t[static_cast<std::size_t>(i)].selected = on;
    }
}


void Converter::setBlockSelected(int i, bool on) {
    if (i < 0 || i >= image_->blockCount()) return;
    image_->blocks()[static_cast<std::size_t>(i)].selected = on;
}

void Converter::applySettings(const Settings& s) {
    audio_.sampleRate = s.sampleRate;
    audio_.waveform = s.waveform;
    audio_.invert = s.invert;
    audio_.invertRight = s.invertRight;
    audio_.finalTone = s.finalTone;
    audio_.stereo = s.stereo;
    audio_.accelerateBasic = s.accelerateBasic;
    audio_.samplesPerBit = s.samplesPerBit;
    audio_.emulate = false;
    infoInFileName_ = s.infoInFileName;

    switch (s.conversionMode) {
        case kConvertNormal:
            // Verbatim replay: no turbo loader, 44.1 kHz, 8-bit mono.
            audio_.method = kRom;
            audio_.sampleRate = 44100;
            audio_.waveform = kCubic;
            audio_.stereo = false;
            audio_.accelerateBasic = false;
            break;
        case kConvertHiSpeed:
        default:
            audio_.method = s.method;
            break;
    }

    loader_.scheme = s.scheme;
    loader_.generateLoader = s.generateLoader;
    // k7zx 4.3 enabled the tape-error box for Ma non troppo too, and only
    // cleared it for the high-speed techniques that carry no checksum.  The
    // low-rate mode is gone; the rule for the remaining ones is unchanged.
    loader_.controlChecksum =
        s.controlChecksum && s.conversionMode == kConvertHiSpeed &&
        Settings::methodSupportsChecksumCheck(static_cast<Method>(audio_.method));
    loader_.antiKolmogorov = s.antiKolmogorov;
    loader_.compress = s.compress;
    loader_.clearAddress = image_->clearN() ? image_->clearN() : s.clearAddress;
    loader_.usrAddress = image_->usrN();

    // Pokes live on the image, not on the audio options, so they used to be
    // applied only by the single-file GUI path.  With the box ticked, batch
    // conversion and k7zx-cli silently produced tapes without them.  Applying
    // the same (address, value) pair twice is idempotent, so the GUI is
    // unaffected -- but the list has to be rebuilt rather than appended to, or
    // every Convert / Play / Emulate grows it by another copy and the vector
    // never stops growing for as long as the window is open.
    image_->clearPokes();
    if (s.usePokes)
        for (const Poke& p : s.pokes) {
            // Poke holds a wider int than the image does; a value written by
            // hand into k7zx.ini can be out of range, and narrowing it silently
            // would poke a different address than the file names.
            if (p.address < 0 || p.address > 0xffff || p.value < 0 || p.value > 0xff) continue;
            image_->addPoke(static_cast<std::uint16_t>(p.address), static_cast<std::uint8_t>(p.value));
        }
}

ConversionResult Converter::convert(const std::string& outPath, bool normalMode) {
    ConversionResult r;
    r.outputPath = outPath;
    if (!loaded_) {
        r.errors = "no tape image loaded";
        return r;
    }

    if (normalMode) {
        NormalConverter nc(audio_);
        r.ok = nc.convert(inputPath_, outPath);
        r.duration = nc.duration();
        r.errors = nc.errorMessage();
        r.warnings = nc.warnings();
        return r;
    }

    LoaderOptions loader = loader_;
    loader.scheme = effectiveScheme();
    HighSpeedConverter hc(*image_, audio_, loader);
    r.ok = hc.convert(outPath);
    r.duration = hc.duration();
    r.errors = hc.errorMessage();
    r.warnings = image_->warnings() + hc.warnings();
    if (loader.scheme != loader_.scheme && !image_->snap().snapshotType)
        r.warnings += std::string(texts::methodName(static_cast<Method>(audio_.method))) +
                      " cannot use the '" + schemeName(loader_.scheme) + "' scheme; used '" +
                      schemeName(loader.scheme) + "' instead\r\n";
    return r;
}

Scheme Converter::effectiveScheme() const {
    const Method m = static_cast<Method>(audio_.method);
    Scheme s = loader_.scheme;
    // MainForm.cpp: `if (UnBloqueRB->Checked||!BloquesRB->Enabled) _UNBLOQUE_;
    // else if (OriginalRB->Checked&&OriginalRB->Enabled) _CARGADOR_ORIGINAL_;
    // else _VARIOSBLOQUES_;` -- then the original loader falls back to many
    // blocks when no BASIC block is selected.
    if (s == kOneBlock || !Settings::methodSupportsMultiBlock(m)) return kOneBlock;
    if (s == kOriginalLoader) {
        bool anyBasic = false;
        const auto& t = image_->blocks();
        for (int i = 0; i < image_->blockCount() && i < static_cast<int>(t.size()); ++i) {
            const BlockInfo& b = t[static_cast<std::size_t>(i)];
            if (b.isTerminator()) break;
            if (b.selected && b.type == '0') anyBasic = true;
        }
        if (!Settings::methodSupportsOriginalLoader(m) || !anyBasic) return kManyBlocks;
    }
    return s;
}




std::string Converter::suggestedOutputName(const std::string& base) const {
    std::string stem = baseNameOf(base);
    const auto dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);

    std::string out = stem;
    if (infoInFileName_) {
        out += std::string(texts::methodTag(static_cast<Method>(audio_.method))) +
               texts::schemeTag(effectiveScheme()) +
               std::string(texts::samplesTag(audio_.samplesPerBit));
        if (audio_.invert) out += "i";
    }
    return out + ".wav";
}

}  // namespace k7zx
