// k7zx GUI - application entry point.
#include <gtkmm.h>

#include <cstdlib>
#include <iostream>

#include "mainwindow.h"
#include "core/settings.h"

int main(int argc, char** argv) {
    Gtk::Main kit(argc, argv);

    std::string openPath;
    bool convertAndPlay = false;
    bool selfTest = false;
    std::string method, scheme;
    int samplesPerBit = -1;
    int sampleRate = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "k7zx: " << what << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            std::cout
                << "k7zx 5.0 (modern C++ port)\n\n"
                   "Usage: k7zx [options] [file.tap|file.tzx|file.sna|file.z80|file.sbb|file.hex]\n\n"
                   "  -p, --play          convert the file and play it, without showing the window\n"
                   "      --self-test     run every GUI action once and report problems\n"
                   "  -t, --method NAME   rom milks fsk slow delta raudo ultra npu fi fiq\n"
                   "                      manchester manchester-dif escurrido rayo\n"
                   "  -s, --spb N         samples per bit, e.g. 2.75\n"
                   "  -c, --scheme NAME   one | many | original\n"
                   "  -r, --rate HZ       44100 or 48000\n"
                   "      --version       print the version\n";
            return 0;
        }
        if (a == "--version") {
            std::cout << "k7zx 5.0 (modern C++ port)\n";
            return 0;
        }
        if (a == "-p" || a == "--play") convertAndPlay = true;
        else if (a == "--self-test") selfTest = true;
        else if (a == "-t" || a == "--method") method = next("--method");
        else if (a == "-s" || a == "--spb") {
            // The speeds are 2.75, 5.00, 1.33 and so on, not integers, so this
            // has to go through the shared parser: atoi("2.75") is 2.
            const std::string text = next("--spb");
            int v = 0;
            if (!k7zx::Settings::parseSamplesPerBit(text, v)) {
                std::cerr << "k7zx: '" << text << "' is not a speed; try 2.75 or 275\n";
                return 2;
            }
            samplesPerBit = v;
        }
        else if (a == "-c" || a == "--scheme") scheme = next("--scheme");
        // The value used to be parsed and then thrown away.
        else if (a == "-r" || a == "--rate") sampleRate = std::atoi(next("--rate").c_str());
        else if (!a.empty() && a[0] != '-') openPath = a;
    }

    if (selfTest) {
        if (openPath.empty()) {
            std::cerr << "k7zx: --self-test needs a file\n";
            return 2;
        }
        // A self-test must not read or write the user's real k7zx.ini: it drives
        // every control, so it would pick up their settings and -- if the window
        // ever emitted a delete event -- save over them.  The path has to be
        // given to the constructor: setting it afterwards left the real
        // settings loaded and applied, because loadSettings() does not touch
        // `settings` when the file is missing.
        k7zx::MainWindow w("/tmp/k7zx-selftest.ini");
        const int problems = w.runSelfTest(openPath);
        w.hide();
        if (problems == 0) {
            std::cout << "self-test passed: every GUI action ran cleanly\n";
            return 0;
        }
        std::cout << "self-test found " << problems << " problem(s)\n";
        return 1;
    }

    if (convertAndPlay) {
        if (openPath.empty()) {
            std::cerr << "k7zx: --play needs a file\n";
            return 2;
        }
        return k7zx::MainWindow::convertAndPlayHeadless(openPath, method, samplesPerBit, scheme,
                                                        sampleRate);
    }

    k7zx::MainWindow window;
    window.show();
    if (!openPath.empty()) window.openPath(openPath);

    kit.run();
    return 0;
}
