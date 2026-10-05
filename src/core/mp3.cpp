// k7zx 5.0 - modern C++ port
//
// mp3 output plumbing.  See mp3.h.
#include "mp3.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <exception>

#include "byteorder.h"

namespace k7zx {

std::string defaultMp3Encoder() {
    if (const char* env = std::getenv("K7ZX_MP3_ENCODER")) {
        if (*env) return env;
    }
    return "lame";
}

bool haveProgram(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    std::size_t offset = 0;
    while (offset <= p.size()) {
        const auto colon = p.find(':', offset);
        const std::string dir =
            p.substr(offset, colon == std::string::npos ? colon : colon - offset);
        if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if (colon == std::string::npos) break;
        offset = colon + 1;
    }
    return false;
}

int runCommand(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    // waitpid() can report an error -- most usefully EINTR -- and the return
    // value used to be ignored, so `status` stayed 0 and WIFEXITED(0) made a
    // failed run look like a clean exit with code 0.
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int wavSampleRate(const std::string& path) {
    try {
        const std::vector<std::uint8_t> d = readFile(path);
        const ByteReader r(d);
        // "RIFF".... "WAVE" "fmt " <size> then the format chunk: audio format
        // (2), channels (2), sample rate (4).
        if (d.size() < 12 + 24) return 0;
        if (std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
            return 0;
        if (std::memcmp(d.data() + 12, "fmt ", 4) != 0) return 0;
        return static_cast<int>(r.le32(24));
    } catch (const std::exception&) {
        return 0;
    }
}

bool encodeMp3(const std::string& wav, const std::string& mp3, const std::string& encoder,
               int bitrate, std::string& error) {
    const std::string enc = encoder.empty() ? defaultMp3Encoder() : encoder;
    if (!haveProgram(enc)) {
        error = "the mp3 encoder '" + enc + "' was not found on PATH";
        return false;
    }
    const bool resample = wavSampleRate(wav) < kResampleBelowHz;
    std::vector<std::string> args{enc, "-m", "m", "-f", "-b", std::to_string(bitrate)};
    if (resample) {
        args.emplace_back("--resample");
        args.emplace_back("44.1");
    }
    args.emplace_back(wav);
    args.emplace_back(mp3);
    if (runCommand(args) == 0) return true;

    // Fall back to ffmpeg if the first encoder was unhappy with the file.
    if (haveProgram("ffmpeg")) {
        // -ar matters: ffmpeg clamps 320 kbps to 160 below the MPEG-1 rates
        // rather than refusing, which would silently halve the bitrate while
        // the report still said 320.
        std::vector<std::string> alt{"ffmpeg", "-y", "-loglevel", "error", "-i", wav};
        if (resample) {
            alt.emplace_back("-ar");
            alt.emplace_back("44100");
        }
        alt.emplace_back("-b:a");
        alt.emplace_back(std::to_string(bitrate) + "k");
        alt.emplace_back(mp3);
        if (runCommand(alt) == 0) return true;
    }
    error = "the mp3 encoder '" + enc + "' failed";
    return false;
}

}  // namespace k7zx