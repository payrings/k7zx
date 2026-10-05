// k7zx GUI - the main window, laid out to match the original MainForm.
#ifndef K7ZX_MAINWINDOW_H
#define K7ZX_MAINWINDOW_H

#include <gtkmm.h>

#include <cstdint>
#include <memory>

#include <string>
#include <vector>

#include "core/convert.h"
#include "core/settings.h"
#include "player.h"

namespace k7zx {

class MainWindow : public Gtk::Window {
public:
    /// `configPath` defaults to the user's k7zx.ini.  Pass a different one to
    /// keep a run away from the real file -- --self-test does, because the
    /// window persists its geometry on close and the self-test drives the
    /// window.
    explicit MainWindow(const std::string& configPath = std::string());
    ~MainWindow() override;

    /// Open a file passed on the command line.
    void openPath(const std::string& path);

    /// Convert and play without showing the window.  Used by `k7zx --play`.
    static int convertAndPlayHeadless(const std::string& path, const std::string& method,
                                      int samplesPerBit, const std::string& scheme,
                                      int sampleRate = 0);

    /// Drive every user-facing action for `path` without showing a window or
    /// blocking on a dialog.  Used by `k7zx --self-test`.
    /// Returns the number of problems found (0 = clean).
    int runSelfTest(const std::string& path);

    /// Point the window at a different settings file and reload from it.  Used
    /// by --self-test so a test run neither reads nor writes the user's real
    /// k7zx.ini.
    void setConfigPath(const std::string& path);

protected:
    bool on_delete_event(GdkEventAny*) override;

private:
    // --- construction ------------------------------------------------------
    void buildNotebook();
    void buildBrowserRow();      ///< directories | files | file info
    void buildDirectoryTree();
    void buildFileList();
    void buildFileInfo();
    void buildOptionRow();       ///< technique / speed / wave / scheme
    void installInfoIconCss();
    Gtk::Button* makeInfoButton(Glib::RefPtr<Gtk::Window>& popup, Gtk::Button& button,
                                Glib::RefPtr<Gtk::Label>& text, const char* tip);
    /// Any click on the main window that is not on an (i) icon closes the popups.
    bool onWindowButtonPress(GdkEventButton* event);
    void buildBottomRow();       ///< checkboxes, play controls, output, mp3
    void buildAboutPage();

    // --- actions -----------------------------------------------------------
    void onConvertWav();
    void onConvertMp3();
    void onEmulate();
    void onPlay();
    void onStop();
    void onPlayerFinished();
    void onConvertAll();

    // --- callbacks ---------------------------------------------------------
    void onDirectoryActivated(const Gtk::TreeModel::Path& path);
    void onDirectorySelected();
    void onFileActivated(const Gtk::TreeModel::Path& path);
    void onFileSelected();
    void onBlockToggled(const Gtk::TreeModel::Path& path);
    void onClearChanged();
    void onUsrChanged();
    void onNameChanged();
    void onPokeToggled();
    void onSettingsChanged();
    void onQuickMethodChanged();
    void updateSensitivity();

    // --- helpers -----------------------------------------------------------
    void reloadDirectory();
    void reloadFiles();
    void refreshBlockList();
    void refreshFileInfo();
    void collectSettings();
    void applySettingsToControls();
    void setStatus(const std::string& text);
    void showReport(const std::string& title, const std::string& body);
    std::string outputPathFor(const std::string& input, const char* extension) const;
    /// Run `lame` (or ffmpeg) over `wav` to produce `mp3`.
    bool encodeMp3(const std::string& wav, const std::string& mp3, std::string& error) const;

    // --- state -------------------------------------------------------------
    std::unique_ptr<Converter> converter_;
    Settings settings_;
    std::string configPath_;
    std::string tempWav_;
    std::string tempTzx_;
    std::string tempMp3_;
    std::string currentDir_;
    std::string currentFile_;
    Player player_;

    // notebook
    Gtk::Box* settingsPage_ = nullptr;
    Gtk::Box* aboutPage_ = nullptr;

    // browser row
    Gtk::Entry dirPathEntry_;
    Gtk::TreeView dirTree_;
    Glib::RefPtr<Gtk::TreeStore> dirModel_;
    Gtk::TreeView fileView_;
    std::vector<std::string> fileList_;
    Glib::RefPtr<Gtk::ListStore> fileStore_;
    Gtk::Label programLabel_;
    Gtk::Label clearLabel_;
    Gtk::Label usrLabel_;
    Gtk::TreeView blockView_;
    Glib::RefPtr<Gtk::ListStore> blockStore_;
    Gtk::Label blockWarning_;
    Gtk::Frame* dirFrame_ = nullptr;
    Gtk::Frame* fileFrame_ = nullptr;
    Gtk::Frame* infoFrame_ = nullptr;

    // options
    Gtk::ComboBoxText modeCombo_;
    Gtk::ComboBoxText methodCombo_;
    Gtk::Frame* methodFrame_ = nullptr;
    Gtk::Frame* schemeFrame_ = nullptr;
    Gtk::Frame* waveFrame_ = nullptr;
    Gtk::ComboBoxText spbCombo_;
    Gtk::ComboBoxText rateCombo_;
    Gtk::ComboBoxText waveformCombo_;
    std::vector<int> spbValues_;
    Gtk::CheckButton invertCheck_;
    Gtk::CheckButton invertRightCheck_;
    Gtk::CheckButton stereoCheck_;
    Gtk::CheckButton finalToneCheck_;
    Gtk::CheckButton accelerateCheck_;
    Gtk::CheckButton loaderCheck_;
    Gtk::CheckButton checksumCheck_;
    Gtk::CheckButton kolmogorovCheck_;
    Gtk::CheckButton compressCheck_;
    Gtk::RadioButton::Group schemeGroup_;
    Gtk::RadioButton oneBlockRadio_;
    Gtk::RadioButton manyBlocksRadio_;
    Gtk::RadioButton originalRadio_;
    Gtk::Entry clearEntry_;
    Gtk::Entry usrEntry_;
    Gtk::Entry nameEntry_;
    Gtk::Button modeInfoButton_;
    Glib::RefPtr<Gtk::Window> modeInfoPopup_;
    Glib::RefPtr<Gtk::Label> modeInfoText_;
    Gtk::Label bpsLabel_;
    Gtk::Button methodInfoButton_;
    Glib::RefPtr<Gtk::Window> methodInfoPopup_;
    Glib::RefPtr<Gtk::Label> methodInfoText_;

    // bottom row
    Gtk::CheckButton pokeCheck_;
    Gtk::CheckButton infoNameCheck_;
    Gtk::Entry outputEntry_;
    Gtk::CheckButton lameCheck_;
    Gtk::Entry lameEntry_;
    Gtk::Button playButton_;
    Gtk::Button stopButton_;
    Gtk::Button emuButton_;
    Gtk::Button wavButton_;
    Gtk::Button mp3Button_;
    Gtk::Button allButton_;
    Gtk::Statusbar status_;

    /// Guards against the combo handlers re-entering each other.
    bool updating_ = false;
    /// When set, showReport() logs instead of opening a modal dialog.
    bool quiet_ = false;
    /// Set by runSelfTest() so the size diagnostics are printed.
    /// Window width the layout asked for, kept so the self-test can compare it
    /// with what the window actually got.
    int naturalWidth_ = 0;
};

}  // namespace k7zx

#endif  // K7ZX_MAINWINDOW_H
