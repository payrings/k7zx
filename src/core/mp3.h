// k7zx 5.0 - modern C++ port
//
// mp3 output, shared by both front ends.
//
// The original had one checkbox, "LAME mp3 encoder", and one entry box naming
// the program to run; the GUI shelled out to it after writing the WAV. That is
// all this does -- there is no mp3 code here, just the plumbing around an
// external encoder -- but it lived in the GUI, so k7zx-cli could not produce an
// mp3 at all even though every other conversion option was available there.
#ifndef K7ZX_MP3_H
#define K7ZX_MP3_H

#include <string>
#include <vector>

namespace k7zx {

/// The bit rate the -> MP3 button asks for, and the CLI's default.
///
/// GUIDE.md ("MP3, streaming and cassette") measures this: at 320 every
/// technique in the MP3 table comes out of the encoder clean except NPU, which
/// fails even as an uncompressed WAV and so is not the encoder's doing. 256 was
/// never itself measured; it sat between two tested rates and was a guess. The
/// WAV the Spectrum loads is written first and is unaffected either way.
inline constexpr int kMp3Bitrate = 320;

/// MPEG-1 Layer III is needed for a 320 kbps stream. Below 32 kHz the encoder
/// would switch to MPEG-2 Layer III, whose ceiling is 160 kbps, so it has to be
/// told to resample up first. This is the original's `--resample` hint, which it
/// attached to 24/32/48 kHz requests.
///
/// Unreachable in this port: effectiveRate() no longer divides by anything, so
/// the WAV is always 44100 or 48000 Hz, both MPEG-1 rates. The check is kept as
/// a guard in case a lower rate is ever produced again.
inline constexpr int kResampleBelowHz = 32000;

/// The encoder to run when none was named: $K7ZX_MP3_ENCODER, else "lame".
std::string defaultMp3Encoder();

/// Is `name` an executable? A name with a slash is checked directly; otherwise
/// every directory on PATH is.
bool haveProgram(const std::string& name);

/// Run `args` with both output streams sent to /dev/null and wait for it.
/// Returns the exit status, or -1 if it could not be started or was killed.
int runCommand(const std::vector<std::string>& args);

/// The sample rate recorded in a finished WAV, or 0 if it cannot be read.
int wavSampleRate(const std::string& path);

/// Encode `wav` to `mp3` with `encoder` at `bitrate` kbps. Falls back to
/// ffmpeg, which needs -ar because it silently clamps 320 kbps to 160 below the
/// MPEG-1 rates rather than refusing. On failure `error` says why and `wav` is
/// left untouched.
bool encodeMp3(const std::string& wav, const std::string& mp3,
               const std::string& encoder, int bitrate, std::string& error);

}  // namespace k7zx

#endif  // K7ZX_MP3_H