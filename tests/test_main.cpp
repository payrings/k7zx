#include "test_harness.h"

#include <cstdlib>
#include <cstring>
#include <exception>

namespace k7zxtest {
namespace {
std::string g_dataDir = "tests/data";
std::vector<Failure>* g_current = nullptr;
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

const std::string& dataDir() { return g_dataDir; }

void setDataDir(const std::string& d) { g_dataDir = d; }

void beginCase() { g_current = nullptr; }
void setCurrent(std::vector<Failure>* f) { g_current = f; }

void fail(const std::string& file, int line, const std::string& what) {
    const std::string msg = file + ":" + std::to_string(line) + ": " + what;
    if (g_current) {
        g_current->push_back({msg});
    } else {
        std::fprintf(stderr, "FAIL %s\n", msg.c_str());
    }
}

std::string show(bool v) { return v ? "true" : "false"; }
std::string show(int v) { return std::to_string(v); }
std::string show(unsigned v) { return std::to_string(v); }
std::string show(long v) { return std::to_string(v); }
std::string show(unsigned long v) { return std::to_string(v); }
std::string show(double v) { return std::to_string(v); }
std::string show(const std::string& v) { return v; }
std::string show(const char* v) { return v ? v : "(null)"; }

}  // namespace k7zxtest

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            k7zxtest::setDataDir(argv[++i]);
        }
    }

    int passed = 0, failed = 0;
    for (auto& c : k7zxtest::registry()) {
        std::vector<k7zxtest::Failure> failures;
        k7zxtest::setCurrent(&failures);
        try {
            c.fn();
        } catch (const std::exception& e) {
            failures.push_back({std::string("unexpected exception: ") + e.what()});
        } catch (...) {
            failures.push_back({"unexpected non-standard exception"});
        }
        k7zxtest::setCurrent(nullptr);

        if (failures.empty()) {
            ++passed;
            std::printf("  ok    %s\n", c.name.c_str());
        } else {
            ++failed;
            std::printf("  FAIL  %s\n", c.name.c_str());
            for (const auto& f : failures) std::printf("          %s\n", f.message.c_str());
        }
    }

    std::printf("\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
    return failed == 0 ? 0 : 1;
}
