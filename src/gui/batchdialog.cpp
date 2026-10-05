// k7zx GUI - batch conversion dialog.
#include "batchdialog.h"

#include <algorithm>
#include <cstdio>

#include "core/texts.h"

namespace k7zx {
BatchDialog::BatchDialog(Gtk::Window& parent, const std::vector<std::string>& files,
                         const std::string& outputDir, const Settings& settings)
    : Gtk::Dialog("k7zx - Convert all", parent, true),
      files_(files),
      settings_(settings) {
    set_default_size(940, 480);
    get_content_area()->set_border_width(10);
    get_content_area()->set_spacing(8);

    columns_ = std::make_unique<BatchColumns>();
    store_ = Gtk::ListStore::create(*columns_);
    view_.set_model(store_);
    view_.append_column("File", columns_->file);
    view_.append_column("Program", columns_->program);
    view_.append_column("USR", columns_->usr);
    view_.append_column("CLEAR", columns_->clear);
    view_.append_column("Seconds", columns_->seconds);
    view_.append_column_editable("Status", columns_->status);
    for (const auto& f : files_) {
        Gtk::TreeModel::Row row = *(store_->append());
        row[columns_->file] = f;
        row[columns_->status] = "pending";
    }

    auto* scroll = Gtk::manage(new Gtk::ScrolledWindow());
    scroll->add(view_);
    get_content_area()->pack_start(*scroll, Gtk::PACK_EXPAND_WIDGET);

    auto* outBox = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    outBox->pack_start(*Gtk::manage(new Gtk::Label("Output folder", Gtk::ALIGN_START)),
                       Gtk::PACK_SHRINK);
    outputEntry_.set_text(outputDir.empty() ? "." : outputDir);
    outputEntry_.set_hexpand(true);
    outBox->pack_start(outputEntry_, Gtk::PACK_EXPAND_WIDGET);
    auto* browse = Gtk::manage(new Gtk::Button("Browse..."));
    browse->signal_clicked().connect([this] {
        Gtk::FileChooserDialog dlg(*this, "Choose the output folder",
                                   Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        dlg.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
        dlg.add_button("_Select", Gtk::RESPONSE_OK);
        if (dlg.run() == Gtk::RESPONSE_OK) outputEntry_.set_text(dlg.get_filename());
    });
    outBox->pack_start(*browse, Gtk::PACK_SHRINK);
    get_content_area()->pack_start(*outBox, Gtk::PACK_SHRINK);

    progress_.set_show_text(true);
    progress_.set_text("idle");
    get_content_area()->pack_start(progress_, Gtk::PACK_SHRINK);

    status_.push("ready");
    get_content_area()->pack_start(status_, Gtk::PACK_SHRINK);

    runButton_.set_label("Convert all");
    stopButton_.set_label("Stop");
    closeButton_.set_label("Close");
    closeButton_.signal_clicked().connect(
        sigc::bind(sigc::mem_fun(*this, &Gtk::Dialog::response), Gtk::RESPONSE_CLOSE));

    get_content_area()->pack_start(runButton_, Gtk::PACK_SHRINK);
    get_content_area()->pack_start(stopButton_, Gtk::PACK_SHRINK);
    get_content_area()->pack_start(closeButton_, Gtk::PACK_SHRINK);

    runButton_.signal_clicked().connect(sigc::mem_fun(*this, &BatchDialog::start));
    stopButton_.signal_clicked().connect([this] {
        running_ = false;
        if (tick_.connected()) tick_.disconnect();
        runButton_.set_sensitive(true);
        stopButton_.set_sensitive(false);
        progress_.set_text("stopped");
    });
    view_.signal_row_activated().connect([this](const Gtk::TreeModel::Path& p,
                                                Gtk::TreeViewColumn*) { onRowActivated(p); });

    signal_response().connect(sigc::mem_fun(*this, &BatchDialog::onResponse));
    get_content_area()->show_all();
    runButton_.set_sensitive(true);
    stopButton_.set_sensitive(false);
}

BatchDialog::~BatchDialog() {
    running_ = false;
    if (tick_.connected()) tick_.disconnect();
}

void BatchDialog::scheduleNext() {
    if (tick_.connected()) tick_.disconnect();
    // Returning false detaches this firing; scheduleNext() re-arms the chain.
    // tick_ is disconnected in the destructor, so the captured `this` can never
    // outlive the dialog.
    tick_ = Glib::signal_timeout().connect(
        [this]() -> bool {
            convertOne();
            return false;
        },
        1);
}

void BatchDialog::start() {
    current_ = 0;
    running_ = true;
    runButton_.set_sensitive(false);
    stopButton_.set_sensitive(true);
    scheduleNext();
}

void BatchDialog::onRowActivated(const Gtk::TreeModel::Path& path) {
    Gtk::TreeModel::iterator it = store_->get_iter(path);
    if (!it) return;
    const Glib::ustring file = (*it)[columns_->file];
    current_ = 0;
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i] == file) {
            current_ = i;
            break;
        }
    }
    running_ = true;
    runButton_.set_sensitive(false);
    stopButton_.set_sensitive(true);
    // Held as a member, like the rest of the chain.  Glib's connect_once()
    // returns void -- the source belongs to the default main context and there
    // is no handle to cancel it -- so an uncancellable timeout could fire
    // convertOne() on a BatchDialog that had already been destroyed, and then
    // re-arm a second one.  See the comment on tick_ in the header.
    if (tick_.connected()) tick_.disconnect();
    tick_ = Glib::signal_timeout().connect([this] {
        if (tick_.connected()) tick_.disconnect();
        tick_ = sigc::connection();
        convertOne();
        return false;
    }, 10);
}

void BatchDialog::finish() {
    running_ = false;
    runButton_.set_sensitive(true);
    stopButton_.set_sensitive(false);
    progress_.set_fraction(1.0);
    progress_.set_text("done");
    status_.pop();
    status_.push("finished");
}

void BatchDialog::convertOne() {
    if (!running_ || current_ >= files_.size()) {
        finish();
        return;
    }

    const std::string& in = files_[current_];
    progress_.set_fraction(static_cast<double>(current_) / files_.size());
    progress_.set_text(in);
    status_.pop();
    status_.push(in);

    Converter conv;
    if (!conv.load(in)) {
        Gtk::TreeModel::iterator it = store_->get_iter(Gtk::TreePath(static_cast<int>(current_)));
        if (it) (*it)[columns_->status] = conv.errorMessage();
    } else {
        const TapeImage& img = conv.image();
        conv.applySettings(settings_);

        const std::string outDir = outputEntry_.get_text();
        const std::string out =
            (outDir.empty() ? std::string(".") : outDir) + "/" + conv.suggestedOutputName(in);
        const bool normalMode = settings_.conversionMode == kConvertNormal;
        const ConversionResult r = conv.convert(out, normalMode);

        Gtk::TreeModel::iterator it = store_->get_iter(Gtk::TreePath(static_cast<int>(current_)));
        if (it) {
            (*it)[columns_->program] = img.prgName();
            (*it)[columns_->usr] = std::to_string(img.usrN());
            (*it)[columns_->clear] = std::to_string(img.clearN());
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", r.duration);
            (*it)[columns_->seconds] = r.ok ? buf : "";
            (*it)[columns_->status] = r.ok ? (r.warnings.empty() ? "OK" : "OK (warnings)")
                                            : (r.errors.empty() ? "conversion failed" : r.errors);
        }
    }

    ++current_;
    // Yield to the main loop so the tree view updates between files.
    scheduleNext();
}

void BatchDialog::onResponse(int) {
    running_ = false;
    if (tick_.connected()) tick_.disconnect();
}

bool BatchDialog::on_delete_event(GdkEventAny* e) {
    running_ = false;
    if (tick_.connected()) tick_.disconnect();
    return Gtk::Dialog::on_delete_event(e);
}

}  // namespace k7zx
