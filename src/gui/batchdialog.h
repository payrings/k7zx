// k7zx GUI - batch conversion dialog (port of ProressForm + PlayerForm).
#ifndef K7ZX_BATCHDIALOG_H
#define K7ZX_BATCHDIALOG_H

#include <gtkmm.h>

#include <memory>
#include <string>
#include <vector>

#include "core/convert.h"
#include "core/settings.h"

namespace k7zx {

/// Column record for the batch results table.
struct BatchColumns : public Gtk::TreeModel::ColumnRecord {
    BatchColumns() {
        add(file);
        add(program);
        add(usr);
        add(clear);
        add(seconds);
        add(status);
    }
    Gtk::TreeModelColumn<Glib::ustring> file;
    Gtk::TreeModelColumn<Glib::ustring> program;
    Gtk::TreeModelColumn<Glib::ustring> usr;
    Gtk::TreeModelColumn<Glib::ustring> clear;
    Gtk::TreeModelColumn<Glib::ustring> seconds;
    Gtk::TreeModelColumn<Glib::ustring> status;
};

class BatchDialog : public Gtk::Dialog {
public:
    BatchDialog(Gtk::Window& parent, const std::vector<std::string>& files,
                const std::string& outputDir, const Settings& settings);
    ~BatchDialog() override;

    /// Begin the batch.  Exposed so the self-test can exercise the pending
    /// timeout path without going through run().
    void startBatch() { start(); }

    /// Cancel a running batch and drop any queued step.
    void stopNow() {
        running_ = false;
        if (tick_.connected()) tick_.disconnect();
    }

protected:
    bool on_delete_event(GdkEventAny* e) override;

private:
    void start();
    void convertOne();
    void scheduleNext();
    void onRowActivated(const Gtk::TreeModel::Path& path);
    void onResponse(int response);
    void finish();

    std::vector<std::string> files_;
    Settings settings_;
    std::size_t current_ = 0;
    bool running_ = false;
    /// Drives the conversion chain.  Held as a member so it can be cancelled
    /// in the destructor: a raw connect_once() lambda capturing `this` would
    /// fire on a destroyed dialog if the user closed it mid-batch.
    sigc::connection tick_;

    Gtk::TreeView view_;
    Gtk::Button closeButton_;
    Glib::RefPtr<Gtk::ListStore> store_;
    std::unique_ptr<struct BatchColumns> columns_;
    Gtk::ProgressBar progress_;
    Gtk::Button runButton_;
    Gtk::Button stopButton_;
    Gtk::Entry outputEntry_;
    Gtk::Statusbar status_;
};

}  // namespace k7zx

#endif  // K7ZX_BATCHDIALOG_H
