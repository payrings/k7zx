// k7zx GUI - the main window, laid out to match the original MainForm.
//
// The original is a dialog with two tabs (Settings / About).  Settings carries
// a browser row (directories | files | file info), the loading-technique and
// wave controls, and a bottom block of check boxes, transport buttons, the
// output directory, the LAME mp3 options and the "-> WAV" / "-> MP3" buttons.
#include "mainwindow.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "batchdialog.h"
#include "core/byteorder.h"
#include "core/mp3.h"
#include "core/texts.h"

namespace k7zx {
namespace {

const char* const kModeNames[] = {"Normal conversion", "High speed (turbo loader)"};

/// Where --self-test puts the conversions its batch steps really perform.  Not
/// the user's output folder, and removed again when the run finishes.
const char* const kSelfTestOutDir = "/tmp/k7zx-selftest-out";

// Waveform names.  The comment above used to claim these were "as MainForm.cpp
// spells them in English", but "rampa" and "parabola" are the original's
// Spanish and were left lower-cased among the English ones.  k7zx-cli has
// always called the same waveform "ramp", so the GUI now agrees with it.
const char* const kWaveformNamesOrig[] = {"Square", "Ramp",           "Cubic",
                                          "Continuous Compensate", "E Compensate",
                                          "Parabola", "Delta"};

/// Height of the loading-method / scheme / wave row.  Sized from the tallest
/// group (Wave properties: 277 px of content plus ~19 px of frame decoration)
/// so that none of the groups needs a scroll bar.
constexpr int kOptionRowHeight = 306;

/// Minimum width of the settings page, so a window narrower than the design
/// scrolls sideways rather than squashing the groups into slivers.  Before
/// k7zx 5.0 this was a bare 1500, which forced a 1516 px window over content
/// that only ever needed 912 -- nearly 600 px of dead space on every row.
/// The value below is the layout's design width: the browser row and the
/// option row both fit inside it, and the expanding children take up the
/// slack, so the window is exactly as wide as it has to be.
constexpr int kPageMinWidth = 1115;

/// Minimum width for the drop-downs in the four option groups.  Without this a
/// combo reports a natural width of a few pixels, the frame collapses around
/// it and the text is clipped -- which is what "High speed (turbo loader)"
/// and "b.p.s = frequency / (samples per bit)" were doing.
constexpr int kComboMinWidth = 210;

/// The original hard-coded 256 kbps and offered no bit-rate control at all: its
/// `MPBCmbBx` combo was the samples-per-bit selector, not an encoder rate. The
/// turbo techniques put 69-89% of their energy above 7 kHz, which is the first
/// band a lossy encoder throws away, so a low rate silently damages the very
/// signal the mp3 is meant to preserve.
///
/// 320 kbps rather than 256, because 320 is the highest rate the matrix in
/// GUIDE.md ("MP3, streaming and cassette") actually measures, and it is the
/// highest rate MPEG-1 Layer III allows -- so it is the last rate before the
/// ceiling starts costing something. At 320 every technique in that table comes
/// out of the encoder clean except NPU, which fails even as an uncompressed
/// WAV and so is not the encoder's doing. 256 was never itself measured; it sat
/// between two tested rates and was a guess. The WAV the Spectrum loads is
/// written first and is never affected either way.
std::string spbLabel(int mpb) {
    char buf[16];
    if (mpb == kS1_33)
        std::snprintf(buf, sizeof buf, "1.33");
    else
        std::snprintf(buf, sizeof buf, "%.2f", mpb / 4.0);
    return buf;
}

std::string baseName(const std::string& p) {
    const auto s = p.find_last_of("/\\");
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

std::string dirNameOf(const std::string& p) {
    const auto s = p.find_last_of("/\\");
    return (s == std::string::npos) ? std::string() : p.substr(0, s);
}

std::string lowerExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = name.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool isTapeFile(const std::string& name) {
    const std::string ext = lowerExtension(name);
    return ext == ".tap" || ext == ".tzx" || ext == ".sna" || ext == ".z80" || ext == ".sbb" ||
           ext == ".hex";
}

bool isDirectory(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool statFile(const std::string& p, unsigned long long& size, std::time_t& when) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    size = static_cast<unsigned long long>(st.st_size);
    when = st.st_mtime;
    return true;
}

std::string formatSize(unsigned long long bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.0f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.0f kB", bytes / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", bytes);
    return buf;
}

std::string formatTime(std::time_t t) {
    char buf[32];
    std::tm tmv{};
    ::localtime_r(&t, &tmv);
    std::strftime(buf, sizeof buf, "%d %b %H:%M", &tmv);
    return buf;
}

std::string fileKind(const std::string& ext) {
    if (ext == ".tap") return "TAP";
    if (ext == ".tzx") return "TZX";
    if (ext == ".sna") return "SNA";
    if (ext == ".z80") return "Z80";
    if (ext == ".sbb") return "SBB";
    if (ext == ".hex") return "HEX";
    return "?";
}

std::vector<std::string> listTapeFiles(const std::string& dir) {
    std::vector<std::string> out;
    try {
        Glib::Dir d(dir);
        for (Glib::Dir::iterator it = d.begin(), end = d.end(); it != end; ++it) {
            const std::string name = *it;
            if (isTapeFile(name) && !isDirectory(dir + "/" + name)) out.push_back(name);
        }
    } catch (const Glib::Error&) {
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> listSubdirs(const std::string& dir) {
    std::vector<std::string> out;
    try {
        Glib::Dir d(dir);
        for (Glib::Dir::iterator it = d.begin(), end = d.end(); it != end; ++it) {
            const std::string name = *it;
            if (name.empty() || name[0] == '.') continue;
            if (isDirectory(dir + "/" + name)) out.push_back(name);
        }
    } catch (const Glib::Error&) {
    }
    std::sort(out.begin(), out.end());
    return out;
}

const std::vector<std::pair<std::string, std::string>>& places() {
    static const std::vector<std::pair<std::string, std::string>> p = [] {
        const std::string h = Glib::get_home_dir();
        std::vector<std::pair<std::string, std::string>> v;
        v.emplace_back("Home", h);
        for (const char* d : {"Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos"})
            v.emplace_back(d, Glib::build_filename(h, d));
        v.emplace_back("File system", "/");
        return v;
    }();
    return p;
}

bool copyFileBytes(const std::string& from, const std::string& to) {
    // No remove() first: rename() replaces atomically on POSIX, and removing
    // the target up front means a failed rename has already destroyed the
    // user's previous WAV.
    if (std::rename(from.c_str(), to.c_str()) == 0) return true;
    try {
        const std::vector<std::uint8_t> data = readFile(from);
        writeFile(to, data.data(), data.size());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool publishOutput(const std::string& tmp, const std::string& out) {
    return copyFileBytes(tmp, out);
}

double wavDuration(const std::string& p) {
    try {
        const std::vector<std::uint8_t> d = readFile(p);
        if (d.size() < 44) return 0;
        const ByteReader r(d);
        const std::uint32_t rate = r.le32(24);
        const int align = r.le16(32) ? r.le16(32) : 1;
        std::size_t dataOff = 0, dataLen = 0, offset = 12;
        while (offset + 8 <= d.size()) {
            const char* id = reinterpret_cast<const char*>(d.data() + offset);
            const std::uint32_t sz = r.le32(offset + 4);
            if (std::memcmp(id, "data", 4) == 0) {
                dataOff = offset + 8;
                dataLen = std::min<std::size_t>(sz, d.size() - dataOff);
                break;
            }
            offset += 8 + sz + (sz & 1);
        }
        if (!rate || !align || !dataLen) return 0;
        return static_cast<double>(dataLen / align) / rate;
    } catch (const std::exception&) {
        return 0;
    }
}

Gtk::Frame* frameScroll(const char* label, Gtk::Box*& out) {
    auto* frame = Gtk::manage(new Gtk::Frame(label));
    frame->set_shadow_type(Gtk::SHADOW_NONE);
    auto* scroller = Gtk::manage(new Gtk::ScrolledWindow());
    scroller->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    scroller->set_hexpand(true);
    scroller->set_vexpand(true);
    out = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 3));
    out->set_border_width(4);
    scroller->add(*out);
    frame->add(*scroller);
    return frame;
}

// --- column records ---------------------------------------------------------
struct DirColumns : public Gtk::TreeModel::ColumnRecord {
    DirColumns() {
        add(text);
        add(fullpath);
    }
    Gtk::TreeModelColumn<Glib::ustring> text;
    Gtk::TreeModelColumn<Glib::ustring> fullpath;
};
struct FileColumns : public Gtk::TreeModel::ColumnRecord {
    FileColumns() {
        add(name);
        add(size);
        add(type);
        add(accessed);
    }
    Gtk::TreeModelColumn<Glib::ustring> name;
    Gtk::TreeModelColumn<Glib::ustring> size;
    Gtk::TreeModelColumn<Glib::ustring> type;
    Gtk::TreeModelColumn<Glib::ustring> accessed;
};
struct BlockColumns : public Gtk::TreeModel::ColumnRecord {
    BlockColumns() {
        add(checked);
        add(text);
    }
    // `bool`, not a string: append_column_editable() only produces a
    // CellRendererToggle for a bool column.  With a Glib::ustring column it
    // silently produced a CellRendererText, the dynamic_cast below never
    // matched, signal_toggled() was never connected, and the column could not
    // be used at all -- clicking it started a text edit on "yes"/"no".
    Gtk::TreeModelColumn<bool> checked;
    Gtk::TreeModelColumn<Glib::ustring> text;
};
struct PokeColumns : public Gtk::TreeModel::ColumnRecord {
    PokeColumns() {
        add(address);
        add(value);
    }
    Gtk::TreeModelColumn<Glib::ustring> address;
    Gtk::TreeModelColumn<Glib::ustring> value;
};

std::shared_ptr<DirColumns> dirCols() {
    static auto c = std::make_shared<DirColumns>();
    return c;
}
std::shared_ptr<FileColumns> fileCols() {
    static auto c = std::make_shared<FileColumns>();
    return c;
}
std::shared_ptr<BlockColumns> blockCols() {
    static auto c = std::make_shared<BlockColumns>();
    return c;
}
std::shared_ptr<PokeColumns> pokeCols() {
    static auto c = std::make_shared<PokeColumns>();
    return c;
}

}  // namespace

/// The high-speed technique list is not a contiguous enum range -- values
/// 13-19 were removed in step 15 and Rayo, added by this port, sits at 20 --
/// so row <-> method conversion goes through Settings.
static int turboRow(Method m) {
      const int i = Settings::turboIndex(m);
      return i < 0 ? 0 : i;
}
static Method turboMethodFromRow(int row) {
      const std::vector<Method>& v = Settings::turboMethods();
      if (row < 0 || static_cast<std::size_t>(row) >= v.size()) return kRom;
      return v[static_cast<std::size_t>(row)];
}

// ===========================================================================
// Construction
// ===========================================================================
void MainWindow::installInfoIconCss() {
    static const char* const kCss =
        "button.info-icon {\n"
        "  padding: 0;\n"
        "  min-width: 17px; min-height: 17px;\n"
        "  border-radius: 9px;\n"
        "  font-size: 10px; font-weight: bold;\n"
        "  color: #ffffff;\n"
        "  background-image: none;\n"
        "  background-color: #5b5b5b;\n"
        "  border: 1px solid #7a7a7a;\n"
        "}\n"
        "button.info-icon:hover {\n"
        "  background-color: #3584e4;\n"
        "  border-color: #3584e4;\n"
        "}\n"
        "button.info-icon:active { background-color: #2a6fc9; }\n"
        // The popup frame.  Colours come from the theme (@theme_bg_color,
        // @borders) so it reads correctly in light and dark themes; the window
        // itself is left transparent so only the rounded frame is painted.
        "window.k7zx-info-window {\n"
        "  background-color: transparent;\n"
        "  border: none;\n"
        "  box-shadow: none;\n"
        "}\n"
        "frame.k7zx-info-frame {\n"
        "  background-color: @theme_bg_color;\n"
        "  border: 1px solid alpha(@borders, 0.75);\n"
        "  border-radius: 7px;\n"
        "}\n"
        "frame.k7zx-info-frame:hover { border-color: alpha(@borders, 1.0); }\n";
    auto css = Gtk::CssProvider::create();
    css->load_from_data(kCss);
    Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), css,
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

Gtk::Button* MainWindow::makeInfoButton(Glib::RefPtr<Gtk::Window>& popup, Gtk::Button& button,
                                        Glib::RefPtr<Gtk::Label>& text, const char* tip) {
    // A Gtk::Popover looks like the right widget but will not keep a size: it is
    // only given a layout pass once, so after the first show/hide it comes back
    // allocated 1x1 with an empty box.  A plain undecorated popup window keeps
    // its allocation across hide/show, so the description stays readable.
    button.set_label("i");
    button.set_relief(Gtk::RELIEF_NONE);
    button.set_focus_on_click(false);
    button.set_tooltip_text(tip);
    button.get_style_context()->add_class("info-icon");

    // A wrapping Label, not a ScrolledWindow: a popover sizes itself to its
    // child, and a scrolled window collapses to a sliver there, which left the
    // description unreadable.  A label sizes to its wrapped text.
    text = Glib::RefPtr<Gtk::Label>(new Gtk::Label());
    text->set_line_wrap(true);
    text->set_line_wrap_mode(Pango::WRAP_WORD_CHAR);
    text->set_max_width_chars(56);
    text->set_xalign(0);
    text->set_yalign(0);
    text->set_selectable(true);
    text->set_margin_start(10);
    text->set_margin_end(10);
    text->set_margin_top(8);
    text->set_margin_bottom(8);

    popup = Glib::RefPtr<Gtk::Window>(new Gtk::Window(Gtk::WINDOW_POPUP));
    popup->set_decorated(false);
    popup->set_keep_above(true);
    popup->set_resizable(false);
    popup->set_skip_taskbar_hint(true);
    popup->set_type_hint(Gdk::WINDOW_TYPE_HINT_TOOLTIP);
    popup->set_transient_for(*this);
    popup->set_modal(false);
    auto* frame = Gtk::manage(new Gtk::Frame());
    frame->set_shadow_type(Gtk::SHADOW_NONE);
    frame->get_style_context()->add_class("k7zx-info-frame");
    frame->add(*text.get());
    popup->get_style_context()->add_class("k7zx-info-window");
    popup->add(*frame);
    popup->signal_key_press_event().connect(
        [popup](GdkEventKey* e) {
            if (e->keyval == GDK_KEY_Escape || e->keyval == GDK_KEY_Return ||
                e->keyval == GDK_KEY_KP_Enter) {
                popup->hide();
                return true;
            }
            return false;
        });
    // Let it lay out once so the size is right, then put it away: neither popup
    // may be visible at start-up.
    popup->show_all();
    popup->hide();
    button.signal_clicked().connect([this, &button] {
        Glib::RefPtr<Gtk::Window> target = &button == &modeInfoButton_   ? modeInfoPopup_
                                           : &button == &methodInfoButton_ ? methodInfoPopup_
                                                                         : Glib::RefPtr<Gtk::Window>();
        if (!target) return;
        Glib::RefPtr<Gtk::Window> other = (&button == &modeInfoButton_) ? methodInfoPopup_
                                                                      : modeInfoPopup_;
        if (other) other->hide();
        if (target->is_visible()) {
            target->hide();
        } else {
            // Place it just under the (i) that was clicked.
            int bx = 0, by = 0;
            get_position(bx, by);
            int ix = 0, iy = 0;
            button.get_window()->get_origin(ix, iy);
            target->set_position(Gtk::WIN_POS_NONE);
            target->move(ix + static_cast<int>(button.get_width()), iy);
            target->show();
            target->grab_focus();
        }
    });
    return &button;
}

bool MainWindow::onWindowButtonPress(GdkEventButton* event) {
    if (!event) return false;
    auto isIcon = [&](Gtk::Button& b) {
        if (!b.get_realized() || !b.get_window()) return false;
        return event->window == b.get_window()->gobj();
    };
    if (isIcon(modeInfoButton_) || isIcon(methodInfoButton_)) return false;
    if (modeInfoPopup_) modeInfoPopup_->hide();
    if (methodInfoPopup_) methodInfoPopup_->hide();
    return false;
}

MainWindow::MainWindow(const std::string& configPath) {
    installInfoIconCss();
    set_title("k7zx 5.0");
    set_border_width(4);
    // A provisional default only.  Once the page has been laid out and
    // measured, set_default_size() is called again with the computed size, so
    // realisation has nothing to disagree with: previously this stayed 1360x880
    // and whichever size GTK happened to honour at realisation decided the
    // outcome.
    set_default_size(1360, 880);

    // The path is a constructor argument, not something set afterwards: the
    // constructor loads the settings and applies them to the controls, and
    // loadSettings() leaves `settings` untouched when the file does not exist.
    // Setting it later therefore did not undo the user's real settings -- which
    // is how --self-test came to drive the window with, and save over, the real
    // k7zx.ini.
    configPath_ = configPath.empty() ? defaultSettingsPath() : configPath;
    loadSettings(configPath_, settings_);

    std::string tempDir = Glib::get_user_runtime_dir();
    if (tempDir.empty()) tempDir = Glib::get_tmp_dir();
    tempWav_ = tempDir + "/k7zx-temp.wav";
    tempTzx_ = tempDir + "/k7zx-temp.tzx";
    tempMp3_ = tempDir + "/k7zx-temp.mp3";

    converter_ = std::make_unique<Converter>();
    player_.finished().connect(sigc::mem_fun(*this, &MainWindow::onPlayerFinished));

    // The original window is a tabbed dialog, so the notebook is the window's
    // only child: a Gtk::Bin can hold exactly one widget.
    buildNotebook();
    signal_button_press_event().connect(sigc::mem_fun(*this, &MainWindow::onWindowButtonPress));
    show_all();

    int sw = 0, sh = 0;
    if (Gdk::Display::get_default()) {
        auto screen = Gdk::Display::get_default()->get_default_screen();
        if (screen) {
            Gdk::Rectangle geo;
            screen->get_monitor_geometry(0, geo);
            sw = geo.get_width();
            sh = geo.get_height();
        }
    }
    // Width comes from the page itself, so the window is exactly as wide as the
    // controls need -- no magic constant to keep in step with the layout.  Every
    // group is on show at all times, so this number is stable.
    for (int i = 0; i < 40; ++i)
        if (!Glib::MainContext::get_default()->iteration(false)) break;
    Gtk::Requisition pageMin, pageNat;
    settingsPage_->get_preferred_size(pageMin, pageNat);
      const int naturalW = std::max(pageMin.width, pageNat.width) + 16;
      int w = std::min(std::max(900, naturalW), sw > 0 ? sw : naturalW);
      // Height comes from the content too, the same way the width does, so
      // removing a row makes the window genuinely shorter instead of leaving a
      // band of dead space where it used to be. The saved height is only a
      // preference for when the content wants *more* room than the screen has,
      // and for a user who has deliberately made the window taller.
      const int contentH = pageMin.height + 16 + 40;
      int h = contentH;
      // A size remembered from a previous run is honoured only when it is larger
      // than what the content needs; a smaller one would clip the page, so it is
      // ignored.  This used to read `std::max(h, 0)`, which can never widen
      // anything, so the stored height did nothing at all.
if (settings_.windowHeight > contentH) h = std::max(h, settings_.windowHeight);
if (settings_.windowWidth > naturalW) w = std::max(w, settings_.windowWidth);
if (sh > 0) h = std::min(h, sh);
      h = std::min(std::max(h, 560), sh > 0 ? sh : h);
      // The remembered width is honoured above, so it has to be clamped to the
      // screen here too -- the height path already was.  Widen the window on a
      // large monitor, start k7zx on a small one, and the window was created
      // wider than the screen: `move()` then pins its left edge to 0 and the
      // right-hand groups are off-screen and unreachable.
      if (sw > 0) w = std::min(w, sw);
naturalWidth_ = naturalW;
    // As well as resize(): the default size is what GTK honours at
    // realisation, and it was left at the provisional 1360x880 above, so the
    // two could disagree.
    set_default_size(std::max(900, w), h);
      resize(std::max(900, w), h);
      move(std::max(0, (sw - w) / 2), std::max(0, (sh - h) / 3));

    applySettingsToControls();

    currentDir_ = settings_.lastDirectory;
    if (currentDir_.empty() || !isDirectory(currentDir_)) currentDir_ = Glib::get_home_dir();
    dirPathEntry_.set_text(currentDir_);
    reloadDirectory();
    reloadFiles();
    updateSensitivity();
    setStatus("ready");
}

MainWindow::~MainWindow() = default;

void MainWindow::buildNotebook() {
    auto* book = Gtk::manage(new Gtk::Notebook());
    book->set_border_width(4);
    add(*book);

    auto* pageScroll = Gtk::manage(new Gtk::ScrolledWindow());
    pageScroll->set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    settingsPage_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));
      // A floor for the page width, so a window narrower than the design scrolls
      // rather than squashing the groups into slivers.  See kPageMinWidth: this
      // used to be a bare 1500 and wasted ~600 px on every row.
      settingsPage_->set_size_request(kPageMinWidth, -1);
    pageScroll->add(*settingsPage_);
    aboutPage_ = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 4));

    book->append_page(*pageScroll, "Settings");
    book->append_page(*aboutPage_, "About");
    settingsPage_->show_all();
    pageScroll->show_all();
    aboutPage_->show_all();

    buildBrowserRow();
    buildOptionRow();
    buildBottomRow();
    buildAboutPage();
}

// --- directories | files | file info ---------------------------------------
void MainWindow::buildBrowserRow() {
    auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    row->set_size_request(-1, 250);
    settingsPage_->pack_start(*row, Gtk::PACK_SHRINK);

    Gtk::Box* dbox = nullptr;
    dirFrame_ = frameScroll("Directories", dbox);
    dirFrame_->set_size_request(210, -1);
    dirPathEntry_.signal_activate().connect([this] {
        const std::string d = dirPathEntry_.get_text();
        if (isDirectory(d)) {
            currentDir_ = d;
            reloadDirectory();
            // The Files list, currentFile_, the saved lastDirectory and "Convert
            // all..." all follow currentDir_.  Without this the entry and the
            // tree showed the new folder while every conversion still used the
            // previous one.
            reloadFiles();
        } else {
            dirPathEntry_.set_text(currentDir_);
        }
    });
    dbox->pack_start(dirPathEntry_, Gtk::PACK_SHRINK);

    dirModel_ = Gtk::TreeStore::create(*dirCols());
    dirTree_.set_model(dirModel_);
    dirTree_.append_column("", dirCols()->text);
    dirTree_.signal_row_activated().connect([this](const Gtk::TreeModel::Path& p,
                                                   Gtk::TreeViewColumn*) {
        onDirectoryActivated(p);
    });
    dirTree_.get_selection()->signal_changed().connect(
        sigc::mem_fun(*this, &MainWindow::onDirectorySelected));
    dirTree_.get_selection()->set_mode(Gtk::SELECTION_BROWSE);
    auto* dscroll = Gtk::manage(new Gtk::ScrolledWindow());
    dscroll->set_size_request(200, -1);
    dscroll->add(dirTree_);
    dbox->pack_start(*dscroll, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Box* fbox = nullptr;
    fileFrame_ = frameScroll("Files", fbox);
    fileFrame_->set_size_request(370, -1);
    fileStore_ = Gtk::ListStore::create(*fileCols());
    fileView_.set_model(fileStore_);
    fileView_.append_column("Name", fileCols()->name);
    fileView_.append_column("Size", fileCols()->size);
    fileView_.append_column("Type", fileCols()->type);
    fileView_.append_column("Accessed", fileCols()->accessed);
    fileView_.get_column(0)->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
    fileView_.get_column(0)->set_fixed_width(150);
    fileView_.get_column(1)->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
    fileView_.get_column(1)->set_fixed_width(70);
    fileView_.get_column(1)->set_alignment(1.0);
    fileView_.get_column(2)->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
    fileView_.get_column(2)->set_fixed_width(45);
    fileView_.get_column(3)->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
    fileView_.get_column(3)->set_fixed_width(85);
    fileView_.signal_cursor_changed().connect([this] { onFileSelected(); });
    fileView_.signal_row_activated().connect([this](const Gtk::TreeModel::Path& p,
                                                   Gtk::TreeViewColumn*) { onFileActivated(p); });
    auto* fscroll = Gtk::manage(new Gtk::ScrolledWindow());
    fscroll->set_size_request(370, -1);
    fscroll->add(fileView_);
    fbox->pack_start(*fscroll, Gtk::PACK_EXPAND_WIDGET);

    Gtk::Box* ibox = nullptr;
    infoFrame_ = frameScroll("File info", ibox);
    infoFrame_->set_size_request(320, -1);

    auto* grid = Gtk::manage(new Gtk::Grid());
    grid->set_column_spacing(6);
    grid->set_row_spacing(1);
    auto addInfo = [&](int row, const char* caption, Gtk::Label& value) {
        auto* cap = Gtk::manage(new Gtk::Label(caption, Gtk::ALIGN_START));
        cap->set_xalign(0);
        grid->attach(*cap, 0, row, 1, 1);
        value.set_xalign(0);
        grid->attach(value, 1, row, 1, 1);
    };
    addInfo(0, "Program:", programLabel_);
    addInfo(1, "CLEAR:", clearLabel_);
    addInfo(2, "RANDOMIZE USR:", usrLabel_);
    ibox->pack_start(*grid, Gtk::PACK_SHRINK);

    auto* blocksCaption = Gtk::manage(new Gtk::Label("Blocks:", Gtk::ALIGN_START));
    blocksCaption->set_xalign(0);
    ibox->pack_start(*blocksCaption, Gtk::PACK_SHRINK);

    blockStore_ = Gtk::ListStore::create(*blockCols());
    blockView_.set_model(blockStore_);
    // Built explicitly rather than through append_column_editable(), whose
    // renderer choice depends on the model column's C++ type: a Glib::ustring
    // column silently gets a CellRendererText, and the toggle handler below
    // then never connects at all.
    {
        Gtk::TreeViewColumn* checkColumn = Gtk::manage(new Gtk::TreeViewColumn(""));
        auto* toggle = Gtk::manage(new Gtk::CellRendererToggle());
        toggle->signal_toggled().connect(
            [this](const Glib::ustring& p) { onBlockToggled(Gtk::TreeModel::Path(p)); });
        checkColumn->pack_start(*toggle, false);
        checkColumn->add_attribute(toggle->property_active(), blockCols()->checked);
        blockView_.append_column(*checkColumn);
    }
    blockView_.append_column("", blockCols()->text);
    blockView_.get_column(0)->set_clickable(true);
    blockView_.get_column(0)->set_fixed_width(28);
    blockView_.get_column(1)->set_expand(true);
    auto* bscroll = Gtk::manage(new Gtk::ScrolledWindow());
    bscroll->set_size_request(-1, 150);
    bscroll->add(blockView_);
    ibox->pack_start(*bscroll, Gtk::PACK_EXPAND_WIDGET);

    blockWarning_.set_text("");
    blockWarning_.set_line_wrap(true);
    blockWarning_.set_xalign(0);
    ibox->pack_start(blockWarning_, Gtk::PACK_SHRINK);

    row->pack_start(*dirFrame_, Gtk::PACK_SHRINK);
    row->pack_start(*fileFrame_, Gtk::PACK_SHRINK);
    row->pack_start(*infoFrame_, Gtk::PACK_EXPAND_WIDGET);
}

// --- technique / speed / scheme / wave --------------------------------------
void MainWindow::buildOptionRow() {
    // Tall enough for the tallest group (Wave properties, 277 px of content plus
    // ~19 px of frame decoration) so that none of them needs a scroll bar.
    auto* row = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    row->set_size_request(-1, kOptionRowHeight);
    settingsPage_->pack_start(*row, Gtk::PACK_SHRINK);

    Gtk::Box* mbox = nullptr;
    methodFrame_ = frameScroll("Loading method", mbox);
    auto smallLabel = [](const char* t) -> Gtk::Label* {
        auto* l = Gtk::manage(new Gtk::Label(t, Gtk::ALIGN_START));
        l->set_xalign(0);
        return l;
    };

    auto* modeRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    for (const char* n : kModeNames) modeCombo_.append(n);
    modeCombo_.set_active(std::clamp(settings_.conversionMode, 0, 1));
    modeCombo_.set_hexpand(true);
    modeCombo_.set_size_request(kComboMinWidth, -1);
    modeCombo_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onSettingsChanged));
    modeRow->pack_start(modeCombo_, Gtk::PACK_EXPAND_WIDGET);
    makeInfoButton(modeInfoPopup_, modeInfoButton_, modeInfoText_,
                   "What does this conversion mode do?");
    modeRow->pack_start(modeInfoButton_, Gtk::PACK_SHRINK);
    mbox->pack_start(*modeRow, Gtk::PACK_SHRINK);

    auto* methodRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    for (Method m : Settings::turboMethods()) methodCombo_.append(texts::methodCaption(m));
    methodCombo_.set_active(turboRow(static_cast<Method>(settings_.method)));
    methodCombo_.set_hexpand(true);
    methodCombo_.set_size_request(kComboMinWidth, -1);
    methodCombo_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onQuickMethodChanged));
    methodRow->pack_start(methodCombo_, Gtk::PACK_EXPAND_WIDGET);
    makeInfoButton(methodInfoPopup_, methodInfoButton_, methodInfoText_,
                   "How this technique encodes the tape");
    methodRow->pack_start(methodInfoButton_, Gtk::PACK_SHRINK);
    mbox->pack_start(*methodRow, Gtk::PACK_SHRINK);

    mbox->pack_start(*smallLabel("Samples per bit"), Gtk::PACK_SHRINK);
    // Changing the speed only changes a setting; it must not go through
    // onQuickMethodChanged(), which rebuilds this combo and restores the
    // previous selection.
    spbCombo_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onSettingsChanged));
    mbox->pack_start(spbCombo_, Gtk::PACK_SHRINK);

    bpsLabel_.set_xalign(0);
    bpsLabel_.set_line_wrap(true);
    mbox->pack_start(bpsLabel_, Gtk::PACK_SHRINK);

    Gtk::Box* sbox = nullptr;
    schemeFrame_ = frameScroll("Loading scheme", sbox);
    oneBlockRadio_.set_group(schemeGroup_);
    oneBlockRadio_.set_label("All in one block");
    oneBlockRadio_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::updateSensitivity));
    sbox->pack_start(oneBlockRadio_, Gtk::PACK_SHRINK);
    manyBlocksRadio_.set_group(schemeGroup_);
    manyBlocksRadio_.set_label("Many blocks");
    manyBlocksRadio_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::updateSensitivity));
    sbox->pack_start(manyBlocksRadio_, Gtk::PACK_SHRINK);
    originalRadio_.set_group(schemeGroup_);
    originalRadio_.set_label("As the original loader");
    originalRadio_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::updateSensitivity));
    sbox->pack_start(originalRadio_, Gtk::PACK_SHRINK);
    loaderCheck_.set_label("Generate the BASIC loader");
    sbox->pack_start(loaderCheck_, Gtk::PACK_SHRINK);
    checksumCheck_.set_label("Verify tape loading error");
    sbox->pack_start(checksumCheck_, Gtk::PACK_SHRINK);
    kolmogorovCheck_.set_label("Statistical optimisation");
    sbox->pack_start(kolmogorovCheck_, Gtk::PACK_SHRINK);
    compressCheck_.set_label("Compress (Rayo)");
    compressCheck_.set_tooltip_text("Rayo only: LZ-compress the data and expand it in place after loading");
    sbox->pack_start(compressCheck_, Gtk::PACK_SHRINK);

    Gtk::Box* wbox = nullptr;
    waveFrame_ = frameScroll("Wave properties", wbox);
    wbox->pack_start(*smallLabel("Sampling frequency"), Gtk::PACK_SHRINK);
    rateCombo_.append("44100 Hz");
    rateCombo_.append("48000 Hz");
    rateCombo_.set_active(settings_.sampleRate == 44100 ? 0 : 1);
    rateCombo_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onSettingsChanged));
    wbox->pack_start(rateCombo_, Gtk::PACK_SHRINK);


    wbox->pack_start(*smallLabel("Waveform"), Gtk::PACK_SHRINK);
    for (const char* n : kWaveformNamesOrig) waveformCombo_.append(n);
    waveformCombo_.set_active(std::clamp(settings_.waveform, 0, 6));
    waveformCombo_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onSettingsChanged));
    wbox->pack_start(waveformCombo_, Gtk::PACK_SHRINK);

    invertCheck_.set_label("Invert wave");
    wbox->pack_start(invertCheck_, Gtk::PACK_SHRINK);
    stereoCheck_.set_label("Stereo (16 bit)");
    stereoCheck_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::updateSensitivity));
    wbox->pack_start(stereoCheck_, Gtk::PACK_SHRINK);

    invertRightCheck_.set_label("Reverse right channel");
    invertRightCheck_.set_tooltip_text("Invert the right channel (stereo only)");
    invertRightCheck_.set_active(settings_.invertRight);
    invertRightCheck_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::onSettingsChanged));
    wbox->pack_start(invertRightCheck_, Gtk::PACK_SHRINK);
    finalToneCheck_.set_label("Final tone");
    wbox->pack_start(finalToneCheck_, Gtk::PACK_SHRINK);
    accelerateCheck_.set_label("Accelerated BASIC loader");
    wbox->pack_start(accelerateCheck_, Gtk::PACK_SHRINK);


    methodFrame_->set_size_request(kComboMinWidth + 18, -1);
    schemeFrame_->set_size_request(230, -1);
    waveFrame_->set_size_request(215, -1);
    row->pack_start(*methodFrame_, Gtk::PACK_EXPAND_WIDGET);
    row->pack_start(*schemeFrame_, Gtk::PACK_SHRINK);
    row->pack_start(*waveFrame_, Gtk::PACK_SHRINK);
    row->set_homogeneous(false);
}

// --- checkboxes, transport, output folder, mp3 -----------------------------
void MainWindow::buildBottomRow() {
    // The "Info in output file name" tick now sits with the LAME options
    // below, and the transport buttons with the other buttons above.

    auto* outRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto* outLabel = Gtk::manage(new Gtk::Label("Output directory", Gtk::ALIGN_START));
    outLabel->set_xalign(0);
    outRow->pack_start(*outLabel, Gtk::PACK_SHRINK);
    outputEntry_.set_text(settings_.outputDirectory);
    outputEntry_.set_hexpand(true);
    outRow->pack_start(outputEntry_, Gtk::PACK_EXPAND_WIDGET);
    auto* browse = Gtk::manage(new Gtk::Button("..."));
    browse->set_tooltip_text("Output folder");
    browse->signal_clicked().connect([this] {
        Gtk::FileChooserDialog dlg(*this, "Output folder", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dlg.add_button("_Select", Gtk::RESPONSE_OK);
        if (!outputEntry_.get_text().empty()) dlg.set_current_folder(outputEntry_.get_text());
        if (dlg.run() == Gtk::RESPONSE_OK) outputEntry_.set_text(dlg.get_filename());
    });
    outRow->pack_start(*browse, Gtk::PACK_SHRINK);
    settingsPage_->pack_start(*outRow, Gtk::PACK_SHRINK);

    auto* lameRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    infoNameCheck_.set_label("Info in output file name");
    infoNameCheck_.set_active(settings_.infoInFileName);
    lameRow->pack_start(infoNameCheck_, Gtk::PACK_SHRINK);
    lameCheck_.set_label("LAME mp3 encoder");
    lameCheck_.set_active(settings_.encodeMp3);
    lameCheck_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::updateSensitivity));
    lameRow->pack_start(lameCheck_, Gtk::PACK_SHRINK);
    auto* brLabel = Gtk::manage(new Gtk::Label("bitrate", Gtk::ALIGN_START));
    brLabel->set_xalign(0);
    brLabel->set_text(std::to_string(kMp3Bitrate) + " kbps, fixed");
    brLabel->set_tooltip_text(
        "The original hard-coded 256 kbps; this uses 320, the highest rate the "
        "guide's lossy-codec table measures and the ceiling of MPEG-1 Layer III. "
        "The turbo techniques put most of their energy above 7 kHz, which is "
        "exactly the band a lossy encoder discards first, so a lower rate "
        "silently damages the signal it is meant to preserve.");
    lameRow->pack_start(*brLabel, Gtk::PACK_SHRINK);
    settingsPage_->pack_start(*lameRow, Gtk::PACK_SHRINK);

    auto* encRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto* encLabel = Gtk::manage(new Gtk::Label("Encoder", Gtk::ALIGN_START));
    encLabel->set_xalign(0);
    encRow->pack_start(*encLabel, Gtk::PACK_SHRINK);
    lameEntry_.set_text(settings_.lamePath);
    lameEntry_.set_hexpand(true);
    lameEntry_.set_tooltip_text("mp3 encoder to run");
    encRow->pack_start(lameEntry_, Gtk::PACK_EXPAND_WIDGET);
    settingsPage_->pack_start(*encRow, Gtk::PACK_SHRINK);

    // --- one row for every button, as the layout puts them ------------------
    // Conversion on the left, transport on the right.  Previously they were
    // split across two rows, which read as two separate groups.

    auto* goRow = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    wavButton_.set_label("-> WAV");
    wavButton_.set_tooltip_text("Write the WAV");
    wavButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onConvertWav));
    goRow->pack_start(wavButton_, Gtk::PACK_SHRINK);
    mp3Button_.set_label("-> MP3");
    mp3Button_.set_tooltip_text("Write a WAV, then encode it to mp3");
    mp3Button_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onConvertMp3));
    goRow->pack_start(mp3Button_, Gtk::PACK_SHRINK);
    allButton_.set_label("Convert all...");
    allButton_.set_tooltip_text("Convert every file in the folder");
    allButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onConvertAll));
    goRow->pack_start(allButton_, Gtk::PACK_SHRINK);
    // The gap that pushes the transport buttons to the right hand end.
    goRow->pack_start(*Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 0)),
                      Gtk::PACK_EXPAND_WIDGET);
    playButton_.set_label("Play");
    playButton_.set_tooltip_text("Play the current conversion");
    playButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onPlay));
    goRow->pack_start(playButton_, Gtk::PACK_SHRINK);
    stopButton_.set_label("Stop");
    stopButton_.set_sensitive(false);
    stopButton_.set_tooltip_text("Stop playback");
    stopButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onStop));
    goRow->pack_start(stopButton_, Gtk::PACK_SHRINK);
    emuButton_.set_label("->Emu");
    emuButton_.set_tooltip_text("Write a TZX direct recording instead of a WAV");
    emuButton_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::onEmulate));
    goRow->pack_start(emuButton_, Gtk::PACK_SHRINK);
    settingsPage_->pack_start(*goRow, Gtk::PACK_SHRINK);
}

void MainWindow::buildAboutPage() {
    auto* box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
    box->set_border_width(12);
    aboutPage_->pack_start(*box, Gtk::PACK_EXPAND_WIDGET);

    auto* title = Gtk::manage(new Gtk::Label("k7zx 5.0", Gtk::ALIGN_START));
    title->set_xalign(0);
    box->pack_start(*title, Gtk::PACK_SHRINK);

    auto* text = Gtk::manage(new Gtk::Label(
        "ZX Spectrum tape file to audio converter.\n\n"
        "A modern C++ port of k7zx 4.3 by Francisco Villa, originally written for\n"
        "Borland C++Builder 6\n"
          "on Windows by Francisco Villa.\n"
          "Thanks, as the original did, to Antonio Villena, Black Hole and the\n"
          "people of es.comp.sistemas.sinclair.\n\n"
        "Reads .tap, .tzx, .sna, .z80, .sbb and .hex files.",
        Gtk::ALIGN_START));
    text->set_xalign(0);
    text->set_line_wrap(true);
    box->pack_start(*text, Gtk::PACK_SHRINK);

    auto* sep = Gtk::manage(new Gtk::Separator(Gtk::ORIENTATION_HORIZONTAL));
    box->pack_start(*sep, Gtk::PACK_SHRINK);

    auto* grid = Gtk::manage(new Gtk::Grid());
    grid->set_column_spacing(8);
    grid->set_row_spacing(4);
    auto* cap = Gtk::manage(new Gtk::Label("Overrides", Gtk::ALIGN_START));
    cap->set_xalign(0);
    grid->attach(*cap, 0, 0, 2, 1);
    auto addRow = [&](int row, const char* text, Gtk::Widget& w) {
        auto* l = Gtk::manage(new Gtk::Label(text, Gtk::ALIGN_START));
        l->set_xalign(0);
        grid->attach(*l, 0, row, 1, 1);
        w.set_hexpand(true);
        grid->attach(w, 1, row, 1, 1);
    };
    clearEntry_.set_width_chars(8);
    clearEntry_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onClearChanged));
    usrEntry_.set_width_chars(8);
    usrEntry_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onUsrChanged));
    nameEntry_.signal_changed().connect(sigc::mem_fun(*this, &MainWindow::onNameChanged));
    addRow(1, "CLEAR:", clearEntry_);
    addRow(2, "USR:", usrEntry_);
    addRow(3, "Program name:", nameEntry_);
    box->pack_start(*grid, Gtk::PACK_SHRINK);

    pokeCheck_.set_label("Do pokes before loading");
    pokeCheck_.signal_toggled().connect(sigc::mem_fun(*this, &MainWindow::onPokeToggled));
    box->pack_start(pokeCheck_, Gtk::PACK_SHRINK);

    Glib::RefPtr<Gtk::ListStore> pokeStore = Gtk::ListStore::create(*pokeCols());
    // Gtk::manage(), not the stack: this widget is handed to pscroll, which
    // outlives this function.  A stack-allocated child added to a container is
    // finalised when the C++ object goes out of scope (glibmm's ~ObjectBase
    // unrefs the GObject), so GTK unparents it and the ScrolledWindow is left
    // with no child -- the "Add" button below then filled a list nobody could
    // see, and the pokes were still saved on exit.
    Gtk::TreeView* pokeView = Gtk::manage(new Gtk::TreeView(pokeStore));
    pokeView->append_column_editable("Address", pokeCols()->address);
    pokeView->append_column_editable("Value", pokeCols()->value);
    pokeView->get_column(0)->set_expand(true);
    auto* pscroll = Gtk::manage(new Gtk::ScrolledWindow());
    pscroll->set_size_request(-1, 90);
    pscroll->add(*pokeView);
    box->pack_start(*pscroll, Gtk::PACK_EXPAND_WIDGET);

    auto* addRow2 = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 4));
    addRow2->set_border_width(4);
    auto* addr = Gtk::manage(new Gtk::Entry());
    addr->set_placeholder_text("address");
    auto* val = Gtk::manage(new Gtk::Entry());
    val->set_placeholder_text("value");
    auto* add = Gtk::manage(new Gtk::Button("Add"));
    add->signal_clicked().connect([this, pokeStore, addr, val] {
        // Base 0: the guide documents "0x4000", and atoi stops at the 'x', so
        // "0x4000" used to be accepted as address 0 and poked the wrong place.
        const int a = static_cast<int>(std::strtol(addr->get_text().c_str(), nullptr, 0));
        const int v = static_cast<int>(std::strtol(val->get_text().c_str(), nullptr, 0));
        if (a < 0 || a > 0xffff || v < 0 || v > 0xff) return;
        Gtk::TreeModel::Row row = *(pokeStore->append());
        row[pokeCols()->address] = std::to_string(a);
        row[pokeCols()->value] = std::to_string(v);
        settings_.pokes.push_back(Poke{a, v});
        addr->set_text("");
        val->set_text("");
    });
    addRow2->pack_start(*addr, Gtk::PACK_EXPAND_WIDGET);
    addRow2->pack_start(*val, Gtk::PACK_EXPAND_WIDGET);
    addRow2->pack_start(*add, Gtk::PACK_SHRINK);
    box->pack_start(*addRow2, Gtk::PACK_SHRINK);
}

// ===========================================================================
// Browsing
// ===========================================================================
void MainWindow::reloadDirectory() {
    if (!dirModel_) return;
    dirModel_->clear();

    for (const auto& [name, path] : places()) {
        if (!isDirectory(path)) continue;
        Gtk::TreeModel::Row row = *(dirModel_->append());
        row[dirCols()->text] = name;
        row[dirCols()->fullpath] = path;
        for (const auto& sub : listSubdirs(path)) {
            Gtk::TreeModel::Row child = *(dirModel_->append(row.children()));
            child[dirCols()->text] = sub;
            child[dirCols()->fullpath] = path + "/" + sub;
        }
    }
    // And the tree the user is actually in, if it is not under /.
    if (!currentDir_.empty() && currentDir_[0] == '/') {
        Gtk::TreeModel::Row row = *(dirModel_->append());
        row[dirCols()->text] = currentDir_;
        row[dirCols()->fullpath] = currentDir_;
        for (const auto& sub : listSubdirs(currentDir_)) {
            Gtk::TreeModel::Row child = *(dirModel_->append(row.children()));
            child[dirCols()->text] = sub;
            child[dirCols()->fullpath] = currentDir_ + "/" + sub;
        }
        dirTree_.expand_to_path(dirModel_->get_path(row));
    }
}

void MainWindow::onDirectoryActivated(const Gtk::TreeModel::Path& path) {
    if (!dirModel_ || path.empty()) return;
    Gtk::TreeModel::iterator it = dirModel_->get_iter(path);
    if (!it) return;
    const Glib::ustring dir = (*it)[dirCols()->fullpath];
    if (!isDirectory(dir)) return;
    currentDir_ = dir;
    dirPathEntry_.set_text(dir);
    reloadDirectory();
    reloadFiles();
    setStatus("browsing " + dir);
}

void MainWindow::onDirectorySelected() {
    if (!dirTree_.get_selection()) return;
    Gtk::TreeModel::iterator it = dirTree_.get_selection()->get_selected();
    if (!it) return;
    const Glib::ustring dir = (*it)[dirCols()->fullpath];
    if (!isDirectory(dir)) return;
    if (dir == currentDir_) return;
    currentDir_ = dir;
    dirPathEntry_.set_text(dir);
    reloadFiles();
}

void MainWindow::reloadFiles() {
    fileStore_->clear();
    fileList_ = listTapeFiles(currentDir_);
    for (const auto& name : fileList_) {
        const std::string full = currentDir_ + "/" + name;
        unsigned long long size = 0;
        std::time_t when = 0;
        // statFile()'s result used to be discarded, so a file that vanished or
        // became unreadable between the listing and the stat was shown as
        // "0 B" / "01 Jan 1970 00:00" -- and stayed selectable.
        if (!statFile(full, size, when)) continue;
        Gtk::TreeModel::Row row = *(fileStore_->append());
        row[fileCols()->name] = name;
        row[fileCols()->size] = formatSize(size);
        row[fileCols()->type] = fileKind(lowerExtension(name));
        row[fileCols()->accessed] = formatTime(when);
    }
    // Deliberately no "select the first row" here.  GTK's select() emits
    // `changed` on the selection but not `cursor-changed` on the view, so the
    // call this replaces was a no-op; making it live would recurse, because
    // onFileSelected() -> openPath() -> reloadFiles() -> select again.  The
    // row for an already-loaded file is highlighted in openPath() instead,
    // where selecting it must *not* trigger a reload.
}

void MainWindow::onFileSelected() {
    if (!fileView_.get_selection()) return;
    Gtk::TreeModel::iterator it = fileView_.get_selection()->get_selected();
    if (!it) return;
    const Glib::ustring name = (*it)[fileCols()->name];
    if (name.empty()) return;
    openPath(currentDir_ + "/" + name);
}

void MainWindow::onFileActivated(const Gtk::TreeModel::Path& path) {
    if (fileView_.get_selection() && fileStore_) {
        Gtk::TreeModel::iterator it = fileStore_->get_iter(path);
        if (it) fileView_.get_selection()->select(it);
    }
    onFileSelected();
    onConvertWav();
}

void MainWindow::refreshFileInfo() {
    const bool loaded = converter_ && converter_->isLoaded();
    if (!loaded) {
        programLabel_.set_text("");
        clearLabel_.set_text("0");
        usrLabel_.set_text("0");
        blockStore_->clear();
        blockWarning_.set_text("");
        return;
    }
    const TapeImage& img = converter_->image();
    programLabel_.set_text(img.prgName());
    clearLabel_.set_text(std::to_string(img.clearN()));
    usrLabel_.set_text(std::to_string(img.usrN()));

    // The original shows a red line when a block would clobber $ff3c.
    std::string warn;
    for (int i = 0; i < img.blockCount(); ++i) {
        const auto& z = img.blocks()[static_cast<std::size_t>(i)];
        if (z.isTerminator()) break;
        if (static_cast<unsigned>(z.startAddress) + z.length > kMaxAddress) {
            warn = "Block overrides address $ff3c";
            break;
        }
    }
    if (warn.empty() && img.warnings().find("$ff3c") != std::string::npos)
        warn = "Block overrides address $ff3c";
    blockWarning_.set_text(warn);
}

void MainWindow::refreshBlockList() {
    if (!blockStore_) return;
    blockStore_->clear();
    if (!converter_ || !converter_->isLoaded()) return;
    const auto& t = converter_->image().blocks();
    const int n = converter_->blockCount();
    for (int i = 0; i < n; ++i) {
        const auto& z = t[static_cast<std::size_t>(i)];
        if (z.isTerminator()) break;
        Gtk::TreeModel::Row row = *(blockStore_->append());
        row[blockCols()->checked] = z.selected;
        char addr[8];
        std::snprintf(addr, sizeof addr, "%04X", z.startAddress);
        if (z.type == '0')
            row[blockCols()->text] = std::to_string(i) + " - Basic auto: " +
                                     std::to_string(z.auto_run) + " long: " +
                                     std::to_string(z.length) + " vars: " +
                                     std::to_string(z.param2);
        else if (z.type == '5')
            row[blockCols()->text] =
                std::to_string(i) + " - Bytes dir: " + std::string(addr) +
                " long: " + std::to_string(z.length) + " (no header)";
        else
            row[blockCols()->text] = std::to_string(i) + " - Bytes dir: " + std::string(addr) +
                                     " long: " + std::to_string(z.length);
    }
}

void MainWindow::setConfigPath(const std::string& path) {
    configPath_ = path;
    loadSettings(configPath_, settings_);
    if (!settingsWarnings().empty())
        std::fprintf(stderr, "k7zx: %s", settingsWarnings().c_str());
}

void MainWindow::openPath(const std::string& path) {
    currentFile_ = path;
    // Keep the browser in step with the file, so the Files list shows what is
    // actually loaded rather than whatever folder was last browsed.
    const std::string dir = dirNameOf(path);
    if (!dir.empty() && isDirectory(dir) && dir != currentDir_) {
        currentDir_ = dir;
        dirPathEntry_.set_text(dir);
        reloadDirectory();
    }
    reloadFiles();
    if (fileStore_) {
        const std::string want = baseName(path);
        for (Gtk::TreeModel::Row r : fileStore_->children()) {
            if (r[fileCols()->name] == want) {
                fileView_.get_selection()->select(fileStore_->get_iter(fileStore_->get_path(r)));
                break;
            }
        }
    }
    converter_ = std::make_unique<Converter>();
    if (!converter_->load(path)) {
        showReport("Could not read the file",
                   converter_->errorMessage() +
                       "\n\nSupported formats: .tap .tzx .sna .z80 .sbb .hex");
        updateSensitivity();
        return;
    }
    const TapeImage& img = converter_->image();
    clearLabel_.set_text(std::to_string(img.clearN()));
    usrLabel_.set_text(std::to_string(img.usrN()));
    programLabel_.set_text(img.prgName());
    clearEntry_.set_text(std::to_string(img.clearN()));
    usrEntry_.set_text(std::to_string(img.usrN()));
    nameEntry_.set_text(img.prgName());
    converter_->selectAllBlocks(true);
    refreshBlockList();
    refreshFileInfo();
    setStatus(baseName(path) + " loaded");
    updateSensitivity();
}

void MainWindow::onBlockToggled(const Gtk::TreeModel::Path& p) {
    if (!converter_ || !converter_->isLoaded() || !blockStore_) return;
    Gtk::TreeModel::iterator it = blockStore_->get_iter(p);
    if (!it) return;
    const Gtk::TreePath path = blockStore_->get_path(*it);
    if (path.empty()) return;
    // The cell renderer has already written the new value into the model; read it
    // rather than inverting, which made the checkbox and the converter
    // disagree.
    converter_->setBlockSelected(path.front(), (*it)[blockCols()->checked]);
    updateSensitivity();
}

void MainWindow::onClearChanged() {
    if (converter_ && converter_->isLoaded())
        converter_->setClearAddress(
            static_cast<unsigned>(std::atoi(clearEntry_.get_text().c_str())));
}

void MainWindow::onUsrChanged() {
    if (converter_ && converter_->isLoaded())
        converter_->setUsrAddress(static_cast<unsigned>(std::atoi(usrEntry_.get_text().c_str())));
}

void MainWindow::onNameChanged() {
    if (converter_ && converter_->isLoaded()) converter_->setProgramName(nameEntry_.get_text());
}

void MainWindow::onPokeToggled() {
    settings_.usePokes = pokeCheck_.get_active();
    if (!converter_ || !converter_->isLoaded()) return;
    converter_->image().clearPokes();
    if (settings_.usePokes)
        for (const Poke& p : settings_.pokes)
            converter_->image().addPoke(static_cast<std::uint16_t>(p.address),
                                        static_cast<std::uint8_t>(p.value));
}

// ===========================================================================
// Settings
// ===========================================================================
void MainWindow::applySettingsToControls() {
    modeCombo_.set_active(std::clamp(settings_.conversionMode, 0, 1));
    methodCombo_.set_active(turboRow(static_cast<Method>(settings_.method)));
    rateCombo_.set_active(settings_.sampleRate == 44100 ? 0 : 1);
    waveformCombo_.set_active(std::clamp(settings_.waveform, 0, 6));
    invertCheck_.set_active(settings_.invert);
    stereoCheck_.set_active(settings_.stereo);
    invertRightCheck_.set_active(settings_.invertRight);
    finalToneCheck_.set_active(settings_.finalTone);
    accelerateCheck_.set_active(settings_.accelerateBasic);
    loaderCheck_.set_active(settings_.generateLoader);
    checksumCheck_.set_active(settings_.controlChecksum);
    kolmogorovCheck_.set_active(settings_.antiKolmogorov);
    compressCheck_.set_active(settings_.compress);
    infoNameCheck_.set_active(settings_.infoInFileName);
    lameCheck_.set_active(settings_.encodeMp3);
    lameEntry_.set_text(settings_.lamePath);
    pokeCheck_.set_active(settings_.usePokes);
    outputEntry_.set_text(settings_.outputDirectory);
    oneBlockRadio_.set_active(settings_.scheme == kOneBlock);
    manyBlocksRadio_.set_active(settings_.scheme == kManyBlocks);
    originalRadio_.set_active(settings_.scheme == kOriginalLoader);
    onQuickMethodChanged();
    updateSensitivity();
}

void MainWindow::collectSettings() {
    settings_.conversionMode = std::clamp(modeCombo_.get_active_row_number(), 0, 1);
    settings_.method = turboMethodFromRow(methodCombo_.get_active_row_number());
    // Read the speed straight from the combo: this is the value Convert, Play
    // and Emulate actually use, so it must not depend on a signal arriving.
    const int spbRow = spbCombo_.get_active_row_number();
    if (spbRow >= 0 && spbRow < static_cast<int>(spbValues_.size()))
        settings_.samplesPerBit = spbValues_[static_cast<std::size_t>(spbRow)];
    settings_.sampleRate = (rateCombo_.get_active_row_number() == 0) ? 44100 : 48000;
    settings_.waveform = std::clamp(waveformCombo_.get_active_row_number(), 0, 6);
    settings_.invert = invertCheck_.get_active();
    settings_.stereo = stereoCheck_.get_active();
    // Remembered so a deliberately enlarged window survives a restart; see the
    // layout code for how it is applied.
    settings_.windowWidth = get_width();
    settings_.windowHeight = get_height();

    // Every option Convert / Play / Emulate actually use has to be read here.
    // invertRight was written only by onSettingsChanged(), which is connected
    // to the four combos, so ticking "Reverse right channel" and then pressing
    // "-> WAV" used the previous value unless some other control happened to
    // fire first.
    settings_.invertRight = invertRightCheck_.get_active();
    settings_.finalTone = finalToneCheck_.get_active();
    settings_.accelerateBasic = accelerateCheck_.get_active();
    settings_.generateLoader = loaderCheck_.get_active();
    settings_.controlChecksum = checksumCheck_.get_active();
    settings_.antiKolmogorov = kolmogorovCheck_.get_active();
    settings_.compress = compressCheck_.get_active();
    settings_.infoInFileName = infoNameCheck_.get_active();
    settings_.encodeMp3 = lameCheck_.get_active();
    settings_.lamePath = lameEntry_.get_text();
    settings_.outputDirectory = outputEntry_.get_text();
    settings_.lastDirectory = currentDir_;

    if (originalRadio_.get_active())
        settings_.scheme = kOriginalLoader;
    else if (manyBlocksRadio_.get_active())
        settings_.scheme = kManyBlocks;
    else
        settings_.scheme = kOneBlock;
}

void MainWindow::onQuickMethodChanged() {
    if (updating_) return;
    updating_ = true;
    const Method m = turboMethodFromRow(methodCombo_.get_active_row_number());
    spbValues_ = Settings::allowedSamplesPerBit(static_cast<Method>(m));

    spbCombo_.remove_all();
    for (int v : spbValues_) spbCombo_.append(spbLabel(v));

    int row = 0;
    for (std::size_t i = 0; i < spbValues_.size(); ++i) {
        if (spbValues_[i] == settings_.samplesPerBit) {
            row = static_cast<int>(i);
            break;
        }
    }
    spbCombo_.set_active(row);
    if (methodInfoText_) methodInfoText_->set_text(texts::explanation(static_cast<Method>(m)));
    updating_ = false;
    onSettingsChanged();
}

void MainWindow::onSettingsChanged() {
    if (updating_) return;
    updating_ = true;
    const int spbRow = spbCombo_.get_active_row_number();
    if (spbRow >= 0 && spbRow < static_cast<int>(spbValues_.size()))
        settings_.samplesPerBit = spbValues_[static_cast<std::size_t>(spbRow)];
    settings_.sampleRate = (rateCombo_.get_active_row_number() == 0) ? 44100 : 48000;
    settings_.waveform = std::clamp(waveformCombo_.get_active_row_number(), 0, 6);
    settings_.invert = invertCheck_.get_active();
    settings_.stereo = stereoCheck_.get_active();
    settings_.invertRight = invertRightCheck_.get_active();
    settings_.finalTone = finalToneCheck_.get_active();
    settings_.accelerateBasic = accelerateCheck_.get_active();
    bpsLabel_.set_text("b.p.s = frequency / (samples per bit)\n" +
                       std::to_string(samplesToBps(settings_.sampleRate,
                                                    settings_.samplesPerBit)) + " bps");
    if (modeInfoText_)
        modeInfoText_->set_text(
            texts::conversionMode(std::clamp(modeCombo_.get_active_row_number(), 0, 1)));
    updating_ = false;
    updateSensitivity();
}

void MainWindow::updateSensitivity() {
    const bool loaded = converter_ && converter_->isLoaded();
    const bool isSnapshot = loaded && converter_->image().snap().snapshotType != 0;
    const int mode = std::clamp(modeCombo_.get_active_row_number(), 0, 1);
    const Method m = turboMethodFromRow(methodCombo_.get_active_row_number());
    const bool hispeed = mode == kConvertHiSpeed;

    methodCombo_.set_sensitive(hispeed);
    spbCombo_.set_sensitive(hispeed);
    // As in k7zx 4.3, Shavings Raudo does not allow many blocks
    // (Converter::effectiveScheme enforces that as well).
    manyBlocksRadio_.set_sensitive(!isSnapshot &&
                                   (hispeed && Settings::methodSupportsMultiBlock(m)));
    originalRadio_.set_sensitive(!isSnapshot && hispeed &&
                                 Settings::methodSupportsOriginalLoader(m));
    oneBlockRadio_.set_sensitive(!isSnapshot);
    checksumCheck_.set_sensitive(hispeed && Settings::methodSupportsChecksumCheck(m));
    kolmogorovCheck_.set_sensitive(m == kShavingsDelta && hispeed);
    compressCheck_.set_sensitive(hispeed && Settings::methodSupportsCompression(m));
    emuButton_.set_sensitive(loaded && hispeed);
    pokeCheck_.set_sensitive(loaded);

    invertRightCheck_.set_sensitive(stereoCheck_.get_active());
    lameEntry_.set_sensitive(lameCheck_.get_active());
    if (lameCheck_.get_active())
        lameEntry_.set_tooltip_text(haveProgram(lameEntry_.get_text())
                                        ? "mp3 encoder to run"
                                        : "not found on PATH: " + lameEntry_.get_text());
    else
        lameEntry_.set_tooltip_text("mp3 encoder to run");

    wavButton_.set_sensitive(loaded);
    mp3Button_.set_sensitive(loaded);
    playButton_.set_sensitive(loaded && !player_.isPlaying());
    allButton_.set_sensitive(!currentDir_.empty());
    stopButton_.set_sensitive(player_.isPlaying());
}

// ===========================================================================
// Actions
// ===========================================================================
std::string MainWindow::outputPathFor(const std::string& input, const char* extension) const {
    std::string name = converter_->suggestedOutputName(input);
    // suggestedOutputName() gives the ".wav" form; only the extension differs.
    if (std::strcmp(extension, ".wav") != 0) {
        name.resize(name.size() - 4);
        name += extension;
    }
    const Glib::ustring outDir = outputEntry_.get_text();
    // dirNameOf() is empty for a bare file name, and "" + "/" + name is a path
    // in the filesystem root -- "k7zx test.tap" then tried to write
    // /test_SRA__VBLO__2.75.wav.  Same answer as the input when it has no
    // directory of its own: the current working directory.
    std::string dir = outDir.empty() ? dirNameOf(input) : outDir.raw();
    if (dir.empty()) return name;
    return dir + "/" + name;
}

bool MainWindow::encodeMp3(const std::string& wav, const std::string& mp3,
                           std::string& error) const {
    const Glib::ustring enc = lameEntry_.get_text();
    return k7zx::encodeMp3(wav, mp3, enc.raw(), k7zx::kMp3Bitrate, error);
}

void MainWindow::onConvertWav() {
    if (!converter_ || !converter_->isLoaded()) return;
    collectSettings();
    converter_->applySettings(settings_);

    const bool normalMode = settings_.conversionMode == kConvertNormal;
    const ConversionResult r = converter_->convert(tempWav_, normalMode);
    if (!r.ok) {
        showReport("Conversion failed", r.errors);
        return;
    }

    const std::string out = outputPathFor(currentFile_, ".wav");
    if (!publishOutput(tempWav_, out)) {
        showReport("Could not write the output file", out);
        return;
    }
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.1f", r.duration);
    std::string body = out + "\n\nDuration: " + std::string(secs) + " seconds";
    if (!r.warnings.empty()) body += "\n\nWarnings:\n" + r.warnings;
    showReport("Conversion finished", body);
    setStatus("wrote " + baseName(out));
}

void MainWindow::onConvertMp3() {
    if (!converter_ || !converter_->isLoaded()) return;
    collectSettings();
    converter_->applySettings(settings_);

    const bool normalMode = settings_.conversionMode == kConvertNormal;
    const ConversionResult r = converter_->convert(tempWav_, normalMode);
    if (!r.ok) {
        showReport("Conversion failed", r.errors);
        return;
    }

    const std::string out = outputPathFor(currentFile_, ".mp3");
    std::string error;
    if (!encodeMp3(tempWav_, tempMp3_, error)) {
        showReport("mp3 encoding failed", error);
        return;
    }
    if (!publishOutput(tempMp3_, out)) {
        showReport("Could not write the output file", out);
        return;
    }
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.1f", r.duration);
    std::string body = out + "\n\nDuration: " + std::string(secs) + " seconds\n" +
                       std::to_string(kMp3Bitrate) + " kbps";
    // The WAV path reports these; the mp3 path used to swallow them, so a user
    // converting to mp3 was told "finished" with no hint the tape had changed.
    if (!r.warnings.empty()) body += "\n\nWarnings:\n" + r.warnings;
    showReport("Conversion finished", body);
    setStatus("wrote " + baseName(out));
}

void MainWindow::onEmulate() {
    if (!converter_ || !converter_->isLoaded()) return;
    collectSettings();
    converter_->applySettings(settings_);
    converter_->audioOptions().emulate = true;

    const ConversionResult r = converter_->convert(tempTzx_, false);
    converter_->audioOptions().emulate = false;
    if (!r.ok) {
        showReport("Emulation failed", r.errors);
        return;
    }
    const std::string out = outputPathFor(currentFile_, ".tzx");
    if (!publishOutput(tempTzx_, out)) {
        showReport("Could not write the output file", out);
        return;
    }
    setStatus("wrote " + baseName(out));
    showReport("TZX written",
               out + "\n\nOpen it with a Spectrum emulator to load the tape.");
}

void MainWindow::onPlay() {
    if (!converter_ || !converter_->isLoaded()) return;
    collectSettings();
    converter_->applySettings(settings_);

    const bool normalMode = settings_.conversionMode == kConvertNormal;
    const ConversionResult r = converter_->convert(tempWav_, normalMode);
    if (!r.ok) {
        showReport("Conversion failed", r.errors);
        return;
    }
    if (!player_.play(tempWav_)) {
        showReport("Could not play", player_.error());
        return;
    }
    stopButton_.set_sensitive(true);
    playButton_.set_sensitive(false);
    setStatus("playing via " + player_.backend());
}

void MainWindow::onStop() {
    player_.stop();
    stopButton_.set_sensitive(false);
    playButton_.set_sensitive(converter_ && converter_->isLoaded());
    setStatus("stopped");
}

void MainWindow::onPlayerFinished() {
    stopButton_.set_sensitive(false);
    playButton_.set_sensitive(converter_ && converter_->isLoaded());
    setStatus("playback finished");
}

void MainWindow::onConvertAll() {
    collectSettings();
    if (currentDir_.empty()) return;
    std::vector<std::string> files;
    for (const auto& f : listTapeFiles(currentDir_)) files.push_back(currentDir_ + "/" + f);
    if (files.empty()) {
        showReport("Nothing to do", "No tape or snapshot files found in " + currentDir_);
        return;
    }
    BatchDialog dlg(*this, files, settings_.outputDirectory, settings_);
    dlg.run();
}



void MainWindow::showReport(const std::string& title, const std::string& body) {
    if (quiet_) {
        std::fprintf(stderr, "%s: %s\n", title.c_str(), body.c_str());
        return;
    }
    Gtk::MessageDialog dlg(*this, title, false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
    dlg.set_secondary_text(body);
    dlg.run();
}

void MainWindow::setStatus(const std::string& text) {
    status_.pop();
    status_.push(text);
}

bool MainWindow::on_delete_event(GdkEventAny*) {
    player_.stop();
    collectSettings();
    saveSettings(configPath_, settings_);
    return false;
}

// ===========================================================================
// Headless convert + play
// ===========================================================================
int MainWindow::convertAndPlayHeadless(const std::string& path, const std::string& method,
                                      int samplesPerBit, const std::string& scheme,
                                      int sampleRate) {
    Settings settings;
    loadSettings(defaultSettingsPath(), settings);

    struct { const char* n; int v; } map[] = {
        {"rom", kRom},           {"milks", kMilks},           {"fsk", kFsk},
        {"slow", kShavingsSlow}, {"delta", kShavingsDelta},  {"raudo", kShavingsRaudo},
        {"ultra", kUltra},       {"npu", kNpu},              {"fi", kFi},
        {"fiq", kFiQ},           {"manchester", kManchester}, {"manchester-dif", kManchesterDif},
        {"escurrido", kEscurrido},
        // Rayo is missing from this table, so `-t rayo` was silently ignored
        // and the tape was written with whatever technique was stored.
        {"rayo", kRayo}};
    for (const auto& e : map)
        if (method == e.n) settings.method = e.v;
    if (samplesPerBit > 0) {
        // The CLI refuses a speed the chosen technique does not offer; without
        // this check `k7zx --play -t rom -s 5.00` happily wrote a tape whose
        // loader threshold table was never patched, i.e. one that cannot load.
        if (!Settings::methodAllowsSamplesPerBit(static_cast<Method>(settings.method),
                                              samplesPerBit)) {
            std::fprintf(stderr, "k7zx: %s does not support that speed\n",
                         texts::methodName(static_cast<Method>(settings.method)));
            return 2;
        }
        settings.samplesPerBit = samplesPerBit;
    }
    if (sampleRate == 44100 || sampleRate == 48000) settings.sampleRate = sampleRate;
    if (scheme == "one") settings.scheme = kOneBlock;
    else if (scheme == "many") settings.scheme = kManyBlocks;
    else if (scheme == "original") settings.scheme = kOriginalLoader;

    Converter conv;
    if (!conv.load(path)) {
        std::fprintf(stderr, "k7zx: %s\n", conv.errorMessage().c_str());
        return 1;
    }
    conv.applySettings(settings);

    const std::string out = Glib::get_tmp_dir() + "/k7zx-play.wav";
    const ConversionResult r = conv.convert(out, settings.conversionMode == kConvertNormal);
    if (!r.ok) {
        std::fprintf(stderr, "k7zx: conversion failed: %s\n", r.errors.c_str());
        return 1;
    }
    std::printf("wrote %s  (%.1f s)\n", out.c_str(), r.duration);

    Player player;
    if (!player.play(out)) {
        std::fprintf(stderr, "k7zx: %s\n", player.error().c_str());
        return 1;
    }
    std::printf("playing via %s\n", player.backend().c_str());
    while (player.isPlaying()) Glib::MainContext::get_default()->iteration(true);
    return 0;
}

// ===========================================================================
// GUI smoke test
// ===========================================================================
int MainWindow::runSelfTest(const std::string& path) {
    quiet_ = true;
    // The batch steps below perform real conversions.  They used to write them
    // wherever settings_.outputDirectory pointed -- the user's own folder when
    // the self-test ran against their real k7zx.ini -- and never removed them.
    // Give them a directory of their own and clear it at the end.
    const std::string savedOutputDir = settings_.outputDirectory;
    settings_.outputDirectory = kSelfTestOutDir;
    outputEntry_.set_text(settings_.outputDirectory);
    int problems = 0;
    auto note = [&problems](const std::string& what) {
        std::fprintf(stderr, "selftest: %s\n", what.c_str());
        ++problems;
    };
    auto info = [](const std::string& what) {
        std::fprintf(stderr, "selftest: expected: %s\n", what.c_str());
    };
    // An expected outcome: reported, but not a failure.  Without this the
    // self-test "failed" on 3 of its 8 fixtures purely because Rayo refuses
    // 128K snapshots and tapes with no loadable blocks, which it does on
    // purpose and with a message saying what to use instead.
    show();
    auto pump = [] {
        for (int i = 0; i < 40; ++i)
            if (!Glib::MainContext::get_default()->iteration(false)) break;
    };
    pump();

    // --- browsing ---------------------------------------------------------
    openPath(path);
    if (!converter_ || !converter_->isLoaded()) {
        note("could not load " + path);
        return problems;
    }
    pump();
    refreshBlockList();
    refreshFileInfo();
    updateSensitivity();
    pump();
    if (converter_->blockCount() > 0 && blockStore_->children().empty())
        note("block list stayed empty although the image has blocks");
    if (programLabel_.get_text().empty()) note("file info did not show a program name");

    for (int i = 0; i < converter_->blockCount() + 2; ++i) {
        converter_->setBlockSelected(i, i % 2 == 0);
        onBlockToggled(Gtk::TreeModel::Path(std::to_string(i)));
        pump();
    }
    converter_->selectAllBlocks(true);
    refreshBlockList();

    for (const char* d : {"/tmp", "/", "/nonexistent-directory-xyz"}) {
        onDirectoryActivated(Gtk::TreeModel::Path());  // an empty path is ignored
        currentDir_ = d;
        dirPathEntry_.set_text(d);
        reloadDirectory();
        reloadFiles();
        pump();
    }
    currentDir_ = dirNameOf(path);
    reloadDirectory();
    reloadFiles();
    pump();

    // --- picking a different speed must actually take effect --------------
    {
        methodCombo_.set_active(kShavingsRaudo);
        onQuickMethodChanged();
        const int rows = static_cast<int>(spbValues_.size());
        if (rows < 2) note("expected several speeds for Raudo");
        for (int row = 0; row < rows; ++row) {
            spbCombo_.set_active(row);
            const int want = spbValues_[static_cast<std::size_t>(row)];
            if (settings_.samplesPerBit != want)
                note("speed combo row " + std::to_string(row) + " did not apply (want " +
                     std::to_string(want) + ", have " + std::to_string(settings_.samplesPerBit) + ")");
            if (spbCombo_.get_active_row_number() != row)
                note("speed combo snapped back from row " + std::to_string(row));
            collectSettings();
            if (settings_.samplesPerBit != want)
                note("collectSettings() lost the speed " + std::to_string(want));
            converter_->applySettings(settings_);
            if (converter_->audioOptions().samplesPerBit != want)
                note("the converter did not receive the speed " + std::to_string(want));
        }
    }

    // --- every technique / speed / scheme through the real handlers -------
    for (Method m : Settings::turboMethods()) {
        methodCombo_.set_active(turboRow(m));
        onQuickMethodChanged();
        updateSensitivity();
        for (std::size_t row = 0; row < spbValues_.size(); ++row) {
            spbCombo_.set_active(static_cast<int>(row));
            onSettingsChanged();
        }
        for (int sch = kOneBlock; sch <= kOriginalLoader; ++sch) {
            if (sch == kOneBlock) oneBlockRadio_.set_active(true);
            else if (sch == kManyBlocks) manyBlocksRadio_.set_active(true);
            else originalRadio_.set_active(true);
            onSettingsChanged();
            collectSettings();
            converter_->applySettings(settings_);
            pump();
            const ConversionResult res = converter_->convert(tempWav_, false);
            if (!res.ok) {
                // Rayo cannot carry every combination -- a 128K snapshot or a
                // tape with no loadable blocks is refused on purpose, with a
                // message telling the user what to use instead.  That is the
                // documented behaviour, not a self-test failure.
                const TapeImage& img = converter_->image();
                const bool refusedOnPurpose =
                    m == kRayo &&
                    (img.snap().snapshotType == 2 || img.blockCount() == 0);
                if (refusedOnPurpose)
                    info("refused as documented: method " + std::to_string(m) + " scheme " +
                         std::to_string(sch));
                else
                    note("convert failed for method " + std::to_string(m) + " scheme " +
                         std::to_string(sch));
            }
            pump();
        }
    }

    // --- the emulator output ---------------------------------------------
    methodCombo_.set_active(kShavingsRaudo);
    onQuickMethodChanged();
    onSettingsChanged();
    collectSettings();
    converter_->applySettings(settings_);
    converter_->audioOptions().emulate = true;
    if (!converter_->convert(tempTzx_, false).ok) note("emulate failed");
    converter_->audioOptions().emulate = false;

    // --- mp3 encoding ----------------------------------------------------
    if (haveProgram("lame")) {
        methodCombo_.set_active(kShavingsRaudo);
        onQuickMethodChanged();
        onSettingsChanged();
        collectSettings();
        converter_->applySettings(settings_);
        if (converter_->convert(tempWav_, false).ok) {
            std::string error;
            if (!encodeMp3(tempWav_, tempMp3_, error)) note("mp3 encoding failed: " + error);
            else if (wavDuration(tempWav_) <= 0) note("the wav used for mp3 is empty");
        }
    }

      // --- every control must actually be in the window --------------------
      // This check exists because a control was once created and wired up but
      // never packed into the page: invisible, with its getter permanently at
      // the default. Anything the self-test toggles must be realised, visible,
      // mapped and parented, or it does not exist as far as a user is concerned.
      {
          struct { const char* n; Gtk::Widget* w; } must[] = {
              {"mode", &modeCombo_},             {"method", &methodCombo_},
              {"spb", &spbCombo_},               {"waveform", &waveformCombo_},
              {"rate", &rateCombo_},             {"info", &infoNameCheck_},
              {"lame", &lameCheck_},             {"compress", &compressCheck_},
              {"kolmogorov", &kolmogorovCheck_}, {"checksum", &checksumCheck_},
              {"loader", &loaderCheck_},         {"stereo", &stereoCheck_},
              {"invert", &invertCheck_},         {"invertRight", &invertRightCheck_},
              {"finalTone", &finalToneCheck_},   {"accelerate", &accelerateCheck_},
              {"wav", &wavButton_},              {"mp3", &mp3Button_},
              {"all", &allButton_},              {"play", &playButton_},
              {"stop", &stopButton_},            {"emu", &emuButton_}};
          for (auto& m : must) {
              if (m.w->get_parent() == nullptr)
                  note(std::string(m.n) + " has no parent: it is not in the layout");
              else if (!m.w->get_realized() || !m.w->get_visible() || !m.w->get_mapped())
                  note(std::string(m.n) + " is in the layout but not on screen");
          }
      }

      // --- the batch dialog, including closing it while a step is pending ---
    for (int round = 0; round < 3; ++round) {
        BatchDialog dlg(*this, {path}, settings_.outputDirectory, settings_);
        dlg.show();
        pump();
        dlg.response(Gtk::RESPONSE_CLOSE);
        pump();
    }
    {
        std::vector<std::string> many{path, path};
        BatchDialog dlg(*this, many, settings_.outputDirectory, settings_);
        dlg.show();
        pump();
        dlg.startBatch();
        pump();
        dlg.response(Gtk::RESPONSE_CLOSE);
        pump();
    }

    // --- the player ------------------------------------------------------
    player_.play("/tmp/k7zx-does-not-exist.wav");
    if (player_.isPlaying()) note("player claimed to play a missing file");

    {
        methodCombo_.set_active(kShavingsRaudo);
        onQuickMethodChanged();
        onSettingsChanged();
        collectSettings();
        converter_->applySettings(settings_);
        if (converter_->convert(tempWav_, false).ok && player_.play(tempWav_)) {
            if (!player_.hasChild()) note("play() reported success but spawned nothing");
            pump();
            if (!player_.isPlaying()) note("play() returned but the player is not running");
            if (wavDuration(tempWav_) < 1.0)
                note("the stop test needs audio longer than a second to be meaningful");

            const int pid = player_.childPid();
            const auto t0 = std::chrono::steady_clock::now();
            player_.stop();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (player_.isPlaying()) note("stop() left the player marked as playing");
            if (player_.hasChild()) note("stop() left the player process running");
            if (ms > 500)
                note("stop() took " + std::to_string(ms) +
                     " ms; it waited for playback to finish instead of stopping it");
            if (pid > 0 && ::kill(pid, 0) == 0)
                note("stop() returned but the player process " + std::to_string(pid) +
                     " is still alive");
        } else {
            note("could not start playback for the stop test");
        }
        // The destructor must clean up too, for "close the window mid-playback".
        player_.play(tempWav_);
        pump();
    }

    // --- rapid re-selection ----------------------------------------------
    for (int i = 0; i < 5; ++i) {
        openPath(path);
        pump();
    }

    // --- the two controls folded in from the old options dialog ----------
    // "Reverse right channel" used to live in a second
    // window; they are now part of the main window and must actually work.
    {

        stereoCheck_.set_active(true);
        invertRightCheck_.set_active(true);
        updateSensitivity();
        if (!invertRightCheck_.get_sensitive())
            note("'Reverse right channel' is disabled even in stereo");
        onSettingsChanged();
        collectSettings();
        converter_->applySettings(settings_);
        if (!settings_.invertRight) note("'Reverse right channel' did not apply");
        if (!converter_->audioOptions().invertRight)
            note("the converter did not receive the inverted right channel");
        stereoCheck_.set_active(false);
        updateSensitivity();
        if (invertRightCheck_.get_sensitive())
            note("'Reverse right channel' is enabled in mono");
        invertRightCheck_.set_active(false);
        onSettingsChanged();
    }

    // --- the information popovers ----------------------------------------
    // The conversion-mode and technique descriptions moved out of the page and
    // into an (i) button next to each drop down.
    {
        auto textOf = [](const Glib::RefPtr<Gtk::Label>& t) {
            return t ? t->get_text().raw() : std::string();
        };
        if (!modeInfoPopup_ || !methodInfoPopup_) note("an information popover is missing");
        if (textOf(modeInfoText_).empty())
            note("the conversion-mode description popover is empty");
        if (textOf(methodInfoText_).empty())
            note("the technique description popover is empty");
        // The border is CSS, so assert it was actually resolved rather than
        // assuming the stylesheet loaded.
        auto checkFrame = [&](Gtk::Window* popup, const char* what) {
            Gtk::Widget* child = popup ? popup->get_child() : nullptr;
            if (!child) {
                note(std::string("the ") + what + " popup has no content");
                return;
            }
            Gtk::Requisition minimum, natural;
            child->get_preferred_size(minimum, natural);
            if (natural.width < 200 || natural.height < 40)
                note(std::string("the ") + what + " popup frame collapsed to " +
                     std::to_string(natural.width) + "x" + std::to_string(natural.height));
        };
        checkFrame(modeInfoPopup_.get(), "conversion");
        checkFrame(methodInfoPopup_.get(), "technique");
        if (modeInfoPopup_ && modeInfoPopup_->is_visible())
            note("the conversion-mode popup is visible before it is clicked");
        if (methodInfoPopup_ && methodInfoPopup_->is_visible())
            note("the technique popup is visible before it is clicked");
        // Clicking the icon shows it; clicking again puts it away.
        modeInfoButton_.clicked();
        pump();
        if (modeInfoPopup_ && !modeInfoPopup_->is_visible())
            note("clicking the conversion (i) did not show its description");
        modeInfoButton_.clicked();
        pump();
        if (modeInfoPopup_ && modeInfoPopup_->is_visible())
            note("clicking the conversion (i) again did not hide it");
        methodInfoButton_.clicked();
        pump();
        if (methodInfoPopup_ && !methodInfoPopup_->is_visible())
            note("clicking the technique (i) did not show its description");
        methodInfoButton_.clicked();
        pump();
        if (modeInfoPopup_ && modeInfoPopup_->is_visible())
            note("showing the technique popup left the conversion popup open");

        // Switching technique must refresh the text, not leave it stale.
        // Compare two techniques that are guaranteed to differ, whatever the
        // method happens to be when the test starts.
        methodCombo_.set_active(kFsk);
        onQuickMethodChanged();
        const std::string fsk = textOf(methodInfoText_);
        methodCombo_.set_active(kRom);
        onQuickMethodChanged();
        const std::string rom = textOf(methodInfoText_);
        methodCombo_.set_active(kFi);
        onQuickMethodChanged();
        const std::string fi = textOf(methodInfoText_);
        if (fsk == rom || rom == fi)
            note("the technique description popover did not follow the selection");
        // The button must actually open it.
        if (methodInfoPopup_) {
            methodInfoPopup_->show();
            pump();
            if (!methodInfoPopup_->is_visible())
                note("the technique information popover did not open");
            // An "open but empty" popover is the failure that got through
            // once: it must actually have a readable size.
            std::printf("        technique popup : %dx%d, label %dx%d\n",
                        methodInfoPopup_->get_allocated_width(),
                        methodInfoPopup_->get_allocated_height(),
                        methodInfoText_->get_allocated_width(),
                        methodInfoText_->get_allocated_height());
            if (methodInfoPopup_->get_allocated_width() < 200 ||
                methodInfoPopup_->get_allocated_height() < 60)
                note("the technique popover opened but is only " +
                     std::to_string(methodInfoPopup_->get_allocated_width()) + "x" +
                     std::to_string(methodInfoPopup_->get_allocated_height()) +
                     " - the description would be unreadable");
            if (methodInfoText_ && methodInfoText_->get_allocated_width() < 120)
                note("the technique description label collapsed to " +
                     std::to_string(methodInfoText_->get_allocated_width()) + " px wide");
            methodInfoPopup_->hide();
            pump();
        }

        // And switching mode must refresh the other one.  Two modes survive
        // step 15, so the check reads one, switches to the other, and compares.
        modeCombo_.set_active(kConvertNormal);
        onSettingsChanged();
        const std::string normal = textOf(modeInfoText_);
        modeCombo_.set_active(kConvertHiSpeed);
        onSettingsChanged();
        if (textOf(modeInfoText_) == normal)
            note("the conversion-mode popover did not follow the selection");
        modeCombo_.set_active(kConvertHiSpeed);
        onSettingsChanged();
        if (modeInfoPopup_) {
            modeInfoPopup_->show();
            pump();
            if (!modeInfoPopup_->is_visible())
                note("the conversion-mode information popover did not open");
            std::printf("        mode      popup : %dx%d, label %dx%d\n",
                        modeInfoPopup_->get_allocated_width(),
                        modeInfoPopup_->get_allocated_height(),
                        modeInfoText_->get_allocated_width(),
                        modeInfoText_->get_allocated_height());
            if (modeInfoPopup_->get_allocated_width() < 200 ||
                modeInfoPopup_->get_allocated_height() < 60)
                note("the conversion-mode popover opened but is only " +
                     std::to_string(modeInfoPopup_->get_allocated_width()) + "x" +
                     std::to_string(modeInfoPopup_->get_allocated_height()) +
                     " - the description would be unreadable");
            modeInfoPopup_->hide();
            pump();
        }
        modeCombo_.set_active(kConvertHiSpeed);
        onSettingsChanged();
    }

    // --- the layout must not squash anything ------------------------------
    // Captured from a screenshot-driven redesign: a squeezed option group is
    // easy to reintroduce, so check the realised sizes rather than trust it.
    {
        auto wideEnough = [&](Gtk::Widget& w, int minW, int minH, const char* what) {
            if (!w.get_realized()) {
                note(std::string(what) + " was never realised");
                return;
            }
            if (w.get_allocated_width() < minW || w.get_allocated_height() < minH)
                note(std::string(what) + " is squeezed to " +
                     std::to_string(w.get_allocated_width()) + "x" +
                     std::to_string(w.get_allocated_height()) + " (want at least " +
                     std::to_string(minW) + "x" + std::to_string(minH) + ")");
        };
        wideEnough(*dirFrame_, 150, 150, "the Directories pane");
        wideEnough(*fileFrame_, 300, 150, "the Files pane");
        wideEnough(*infoFrame_, 250, 150, "the File info pane");
        wideEnough(fileView_, 300, 80, "the file list");
        wideEnough(dirTree_, 150, 80, "the directory tree");
        wideEnough(blockView_, 200, 60, "the block list");
        wideEnough(playButton_, 30, 20, "the Play button");
        wideEnough(wavButton_, 45, 20, "the -> WAV button");
        wideEnough(mp3Button_, 45, 20, "the -> MP3 button");
        wideEnough(methodCombo_, 100, 20, "the technique combo");
        wideEnough(spbCombo_, 80, 20, "the samples-per-bit combo");
            wideEnough(modeInfoButton_, 16, 16, "the conversion (i) icon");
        wideEnough(methodInfoButton_, 16, 16, "the technique (i) icon");

        // No option group may need a scroll bar: the tallest group sets the
        // height of the row, and the others get whatever is left over.
        auto checkNoScroll = [&note](Gtk::Frame* frame, const char* what) {
            if (!frame) return;
            // frameScroll() puts a ScrolledWindow in the frame; its child is
            // the group whose natural height must fit.
            Gtk::Widget* scroller = frame->get_child();
            if (!scroller) return;
            Gtk::Widget* content = scroller;
            if (auto* sw = dynamic_cast<Gtk::ScrolledWindow*>(scroller)) content = sw->get_child();
            if (!content) return;
            if (!content) return;
            Gtk::Requisition minimum, natural;
              content->get_preferred_size(minimum, natural);
              // Reported either way: this is the diagnostic that shows whether a
              // layout change has squashed any group, so it must not only print
              // when something is already wrong.
              std::printf("        %-22s wants %4d px, has %4d px\n", what, natural.height,
                          content->get_allocated_height());
              if (natural.height > content->get_allocated_height()) {
                  note(std::string(what) + " needs scrolling: wants " +
                       std::to_string(natural.height) + " px, has " +
                       std::to_string(content->get_allocated_height()));
              }
          };
        // The taller row must not push the window past the screen.
        int sw = 0, sh = 0;
        if (Gdk::Display::get_default() && Gdk::Display::get_default()->get_default_screen()) {
            Gdk::Rectangle geo;
            Gdk::Display::get_default()->get_default_screen()->get_monitor_geometry(0, geo);
            sw = geo.get_width();
            sh = geo.get_height();
        }
          std::printf("        window %dx%d on a %dx%d screen (layout asked for %d px)\n",
                    get_allocated_width(), get_allocated_height(), sw, sh, naturalWidth_);
        if (sh > 0 && get_allocated_height() > sh)
            note("the window is " + std::to_string(get_allocated_height() - sh) +
                 " px taller than the screen, so the bottom rows are unreachable");
        // The window must be wide enough for the controls, otherwise the page
        // scrolls sideways and the right-hand groups are cut off.
        if (naturalWidth_ > 0 && get_allocated_width() + 40 < naturalWidth_)
            note("the window is " + std::to_string(naturalWidth_ - get_allocated_width()) +
                 " px too narrow for the layout, so the page scrolls sideways");

        checkNoScroll(methodFrame_, "Loading method");
        checkNoScroll(schemeFrame_, "Loading scheme");
        checkNoScroll(waveFrame_, "Wave properties");
        wideEnough(invertRightCheck_, 80, 20, "the 'Reverse right channel' check box");
    }

    // --- re-apply the settings, as the Options dialog does -----------------
    applySettingsToControls();
    onQuickMethodChanged();
    onSettingsChanged();
    pump();

    std::remove(tempWav_.c_str());
    std::remove(tempTzx_.c_str());
    std::remove(tempMp3_.c_str());
    // And the batch conversions, which went into kSelfTestOutDir.
    if (::mkdir(kSelfTestOutDir, 0700) == 0 || errno == EEXIST) {
        try {
            for (const auto& name : listTapeFiles(kSelfTestOutDir))
                std::remove((std::string(kSelfTestOutDir) + "/" + name).c_str());
        } catch (const Glib::Error&) {
        }
        rmdir(kSelfTestOutDir);
    }
    settings_.outputDirectory = savedOutputDir;
    outputEntry_.set_text(savedOutputDir);
    quiet_ = false;
    hide();
    pump();
    return problems;
}

}  // namespace k7zx
