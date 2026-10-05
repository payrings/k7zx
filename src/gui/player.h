// k7zx GUI - audio playback.
//
// The original used the Win32 sndPlaySound API.  On Linux this shells out to
// whichever of paplay / pw-play / aplay / mpv / ffplay is present; if none is,
// the file is handed to the desktop's default handler.
#ifndef K7ZX_PLAYER_H
#define K7ZX_PLAYER_H

#include <glibmm.h>
#include <gtkmm.h>

#include <map>
#include <string>
#include <sys/types.h>

namespace k7zx {

class Player {
public:
    Player();
    ~Player();

    /// Start playing `path`.  Returns false if no player could be started.
    bool play(const std::string& path);
    void stop();
    bool isPlaying() const { return playing_; }

    /// True while a player process has been spawned and not yet reaped.
    bool hasChild() const { return childPid_ > 0; }

    /// The player process id, or 0 when nothing is running.  The self-test
    /// takes this before stop() and then asks the OS whether the process is
    /// really gone.
    int childPid() const { return childPid_; }

    /// Emitted when playback finishes or is interrupted.
    sigc::signal<void>& finished() { return finished_; }

    /// Which backend was used for the last play() call, for the status bar.
    const std::string& backend() const { return backend_; }

    /// Description of the last failure, empty if none.
    const std::string& error() const { return error_; }

private:
    void onChildExited(pid_t pid, int status);

    /// Every player process spawned and not yet collected, and the child watch
    /// that will collect it.  GLib's watch is the only thing that reaps a child,
    /// so it has to stay connected until the process is really gone -- the old
    /// code disconnected it before waiting and then gave up, leaving a permanent
    /// zombie whenever the wait timed out.
    std::map<pid_t, sigc::connection> children_;
    sigc::signal<void> finished_;
    std::string backend_;
    std::string error_;
    bool playing_ = false;
    int childPid_ = -1;
};

}  // namespace k7zx

#endif  // K7ZX_PLAYER_H
