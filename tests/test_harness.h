// Minimal test harness for the k7zx port.
#ifndef K7ZX_TEST_HARNESS_H
#define K7ZX_TEST_HARNESS_H

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace k7zxtest {

struct Case {
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry();

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct Failure {
    std::string message;
};

void fail(const std::string& file, int line, const std::string& what);

#define K7ZX_CONCAT_(a, b) a##b
#define K7ZX_CONCAT(a, b) K7ZX_CONCAT_(a, b)

#define TEST(name)                                                          \
    static void K7ZX_CONCAT(k7zx_test_, __LINE__)();                        \
    static ::k7zxtest::Registrar K7ZX_CONCAT(k7zx_reg_, __LINE__)(         \
        name, &K7ZX_CONCAT(k7zx_test_, __LINE__));                          \
    static void K7ZX_CONCAT(k7zx_test_, __LINE__)()

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) ::k7zxtest::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        const auto _ka = (a);                                               \
        const auto _kb = (b);                                               \
        if (!(_ka == _kb))                                                  \
            ::k7zxtest::fail(__FILE__, __LINE__,                            \
                             std::string(#a " == " #b " (") +               \
                                 ::k7zxtest::show(_ka) + " vs " +           \
                                 ::k7zxtest::show(_kb) + ")");              \
    } while (0)

/// Like CHECK, but appends `detail` to the message -- for the cases where the
/// interesting thing to report is the code under test's own diagnostic.
#define CHECK_MSG(cond, detail)                                              \
    do {                                                                     \
        if (!(cond))                                                         \
            ::k7zxtest::fail(__FILE__, __LINE__,                             \
                             std::string(#cond ": ") + (detail));            \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                               \
    do {                                                                    \
        const double _ka = (a);                                             \
        const double _kb = (b);                                             \
        if (std::fabs(_ka - _kb) > (eps))                                   \
            ::k7zxtest::fail(__FILE__, __LINE__,                            \
                             std::string(#a " ~= " #b " (") +               \
                                 ::k7zxtest::show(_ka) + " vs " +           \
                                 ::k7zxtest::show(_kb) + ")");              \
    } while (0)

std::string show(bool v);
std::string show(int v);
std::string show(unsigned v);
std::string show(long v);
std::string show(unsigned long v);
std::string show(double v);
std::string show(const std::string& v);
std::string show(const char* v);

/// Path to the test data directory (overridable with --data-dir).
const std::string& dataDir();
void setDataDir(const std::string& d);

/// The test currently executing, for CHECK() to report into.
void setCurrent(std::vector<Failure>* f);

}  // namespace k7zxtest

#endif  // K7ZX_TEST_HARNESS_H
