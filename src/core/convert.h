// k7zx 5.0 - modern C++ port
//
// One-call façade over the readers and the two converters.  The GUI and the
// command line tool both go through this so they cannot drift apart.
#ifndef K7ZX_CONVERT_H
#define K7ZX_CONVERT_H

#include <memory>
#include <string>
#include <vector>

#include "defs.h"
#include "settings.h"
#include "zxcode.h"
#include "zxfiles.h"
#include "zxwav.h"

namespace k7zx {

/// Result of a conversion.
struct ConversionResult {
    bool ok = false;
    std::string outputPath;
    double duration = -1;   ///< seconds of audio
    std::string errors;
    std::string warnings;
};

/// A loaded tape image plus the derived audio/loader options.
class Converter {
public:
    Converter();

    /// Read `path` (dispatches on the extension).  Returns false and fills
    /// errorMessage() if the file cannot be read or parsed.
    bool load(const std::string& path);
    bool isLoaded() const { return loaded_; }
    TapeImage& image() { return *image_; }
    const TapeImage& image() const { return *image_; }
    const std::string& errorMessage() const { return error_; }

    /// Override the CLEAR / USR values recovered from the file.  0 keeps the
    /// value the reader found.
    void setClearAddress(unsigned v) { image_->setClearN(v); }
    void setUsrAddress(unsigned v) { image_->setUsrN(v); }
    void setProgramName(const std::string& n) { image_->setPrgName(n); }

    /// Select which blocks take part in the conversion.
    void selectAllBlocks(bool on);
    void setBlockSelected(int i, bool on);
    int blockCount() const { return image_->blockCount(); }

    /// Derive AudioOptions/LoaderOptions from the current Settings.
    void applySettings(const Settings& settings);

    /// Access the derived audio options, which the GUI displays.
    AudioOptions& audioOptions() { return audio_; }
    const AudioOptions& audioOptions() const { return audio_; }

    /// Convert the loaded image to `outPath`.  `normalMode` selects the
    /// verbatim tap/tzx replay instead of the turbo loader.
    ConversionResult convert(const std::string& outPath, bool normalMode = false);
    
    /// Build the output file name the original would have used, given the
    /// name the user typed in the "output" box (or the input name).  The tags
    /// come from the options the conversion will actually use, not from the
    /// controls, and they are only added when the "info in output file name"
    /// option is set -- so the single-file path, the batch dialog and
    /// k7zx-cli all produce the same name for the same conversion.
    std::string suggestedOutputName(const std::string& base) const;

    /// The path most recently passed to load().
    const std::string& inputPath() const { return inputPath_; }


    /// The loading scheme a conversion will actually use.  k7zx 4.3 did not
    /// let every technique use every scheme: its settings form disabled the
    /// "many blocks" radio for Shavings Raudo and the "original loader" radio
    /// for everything except Milks, and MainForm then substituted the scheme
    /// that was still available (see MainForm.cpp, where `esquema` is set).
    /// This reproduces that substitution so the GUI, the CLI and batch mode
    /// cannot write a tape whose loader was never designed for its scheme.
    Scheme effectiveScheme() const;

private:
    std::unique_ptr<TapeImage> image_;
    AudioOptions audio_;
    LoaderOptions loader_;
    bool loaded_ = false;
    /// Whether suggestedOutputName() appends the technique / scheme / speed tags.
    bool infoInFileName_ = false;
    std::string error_;
    std::string inputPath_;
};

}  // namespace k7zx

#endif  // K7ZX_CONVERT_H
