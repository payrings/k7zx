// k7zx GUI - audio playback.
#include "player.h"

#include <fcntl.h>
#include <glib.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace k7zx {
namespace {

bool haveProgram(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    std::size_t offset = 0;
    while (offset <= p.size()) {
        const auto colon = p.find(':', offset);
        const std::string dir = p.substr(offset, colon == std::string::npos ? colon : colon - offset);
        if (!dir.empty()) {
            const std::string full = dir + "/" + name;
            if (::access(full.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        offset = colon + 1;
    }
    return false;
}

struct Backend {
    const char* program;
};

// paplay/pw-play understand raw wav on PipeWire/PulseAudio; aplay needs ALSA
// (and only handles what the ALSA sink accepts); mpv and ffplay are fallbacks.
const Backend kBackends[] = {
    {"paplay"}, {"pw-play"}, {"mpv"}, {"ffplay"}, {"aplay"}};

/// How long a player is given to go away on its own after SIGTERM, and after
/// SIGKILL.  Both bounds are there so Stop cannot freeze the interface; the
/// self-test treats anything over 500 ms as a failure.  A player normally
/// answers SIGTERM in a millisecond or two, so neither is normally reached.
constexpr int kTermGraceMs = 150;
constexpr int kKillGraceMs = 150;
constexpr int kPollStepMs = 5;

/// Has this child terminated?
///
/// Deliberately neither kill(pid, 0) nor waitpid().  kill() cannot tell a
/// zombie from a running process, so it kept reporting "alive" long after the
/// player had gone and Stop always burned its whole timeout.  waitpid() would
/// answer correctly but would steal the exit status from GLib's child watch,
/// which is the only thing that reaps the process -- and a child nobody reaps
/// is a zombie for the life of the program.  waitid() with WNOWAIT reports the
/// exit and leaves the child waitable, so GLib still collects it.
bool exited(pid_t pid) {
    if (pid <= 0) return true;
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT | WNOHANG) != 0)
        return true;  // not a child any more, so it cannot be running
    return info.si_pid != 0;
}

/// Wait up to `ms` for `pid` to terminate.  Returns true if it did.
bool waitGone(pid_t pid, int ms) {
    for (int waited = 0; waited < ms; waited += kPollStepMs) {
        if (exited(pid)) return true;
        struct timespec ts { 0, static_cast<long>(kPollStepMs) * 1000 * 1000 };
        ::nanosleep(&ts, nullptr);
    }
    return exited(pid);
}

}  // namespace

Player::Player() = default;

Player::~Player() {
    // No main loop runs any more, so the child watch will never fire and GLib
    // will never reap.  Kill what is left and collect it here; SIGKILL cannot
    // be caught or ignored, so this returns as soon as the kernel has torn the
    // process down.
    for (const auto& child : children_) {
        const pid_t pid = child.first;
        if (::kill(-pid, SIGKILL) != 0) ::kill(pid, SIGKILL);
    }
    for (const auto& child : children_) {
        const pid_t pid = child.first;
        int status = 0;
        for (int i = 0; i < 200 && ::waitpid(pid, &status, WNOHANG) == 0; ++i) {
            struct timespec ts { 0, kPollStepMs * 1000 * 1000L };
            ::nanosleep(&ts, nullptr);
        }
    }
    children_.clear();
    childPid_ = -1;
    playing_ = false;
}

bool Player::play(const std::string& path) {
    stop();
    backend_.clear();
    // player.h documents error_ as "empty if none"; without this a single
    // failed play left its message up for every later successful one.
    error_.clear();

    if (::access(path.c_str(), R_OK) != 0) {
        error_ = "cannot read '" + path + "'";
        return false;
    }

    std::vector<std::string> argv;
    for (const Backend& b : kBackends) {
        if (!haveProgram(b.program)) continue;
        backend_ = b.program;
        argv.emplace_back(b.program);
        if (std::strcmp(b.program, "mpv") == 0) {
            argv.emplace_back("--really-quiet");
            argv.emplace_back("--no-video");
        } else if (std::strcmp(b.program, "ffplay") == 0) {
            argv.emplace_back("-nodisp");
            argv.emplace_back("-autoexit");
            argv.emplace_back("-loglevel");
            argv.emplace_back("quiet");
        }
        argv.push_back(path);
        break;
    }

    if (argv.empty()) {
        // Nothing known installed: ask the desktop to open it.
        backend_ = "xdg-open";
        argv = {"xdg-open", path};
    }

    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        error_ = "could not start the audio player";
        return false;
    }
    if (pid == 0) {
        // Child: detach from the terminal so the GUI stays responsive.
        setsid();
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    playing_ = true;
    childPid_ = pid;
    // GLib's child watch is what reaps this process; it takes the pid as an
    // argument, so each player gets its own watch.
    children_[pid] = Glib::signal_child_watch().connect(
        [this, pid](Glib::Pid, int status) { onChildExited(pid, status); }, static_cast<GPid>(pid));
    return true;
}

void Player::onChildExited(pid_t pid, int status) {
    children_.erase(pid);  // destroys the watch, which is what collected it
    if (pid != childPid_) {
        // A player that had already been stopped; stop() has done the
        // bookkeeping, so there is nothing to announce.
        playing_ = false;
        return;
    }
    childPid_ = -1;
    playing_ = false;
    // 127 is what the child _exit()s with when execvp() fails, so a backend
    // that has gone missing between the PATH scan and the fork is reported
    // rather than shown as a clean finish.
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        error_ = "could not run '" + backend_ + "'";
    else
        error_.clear();
    finished_.emit();
}

void Player::stop() {
    if (childPid_ <= 0) {
        playing_ = false;
        return;
    }
    const pid_t pid = static_cast<pid_t>(childPid_);
    // The child called setsid(), so it leads its own process group and
    // signalling the negative pid reaches the player and anything it spawned.
    // Without this the audio simply kept playing after Stop.  The child calls
    // setsid() *after* fork(), so for a moment there is no process group with
    // this id and kill(-pid, ...) fails with ESRCH; fall back to the pid itself.
    if (::kill(-pid, SIGTERM) != 0) ::kill(pid, SIGTERM);
    if (!waitGone(pid, kTermGraceMs)) {
        if (::kill(-pid, SIGKILL) != 0) ::kill(pid, SIGKILL);
        // Bounded even after SIGKILL: an uninterruptible child must not be able
        // to hang the interface.  If it really is stuck, children_ keeps the
        // watch armed and GLib reaps it whenever the kernel lets it go.
        waitGone(pid, kKillGraceMs);
    }
    playing_ = false;
    // Only claim the child is gone once the kernel says so.  Until GLib's
    // watch has collected it, children_ still holds the pid, and hasChild()
    // stays honest about a process still being outstanding.
    if (exited(pid)) childPid_ = -1;
}

}  // namespace k7zx