// Minimal dependency-free unit-test harness for native (host) builds.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace gmbtest {

struct Registry {
    struct Case { std::string name; void (*fn)(); };
    std::vector<Case>& cases() {
        static std::vector<Case> c;
        return c;
    }
    int& failures() {
        static int f = 0;
        return f;
    }
    int& checks() {
        static int c = 0;
        return c;
    }
    static Registry& get() {
        static Registry r;
        return r;
    }
};

struct AutoReg {
    AutoReg(const char* name, void (*fn)()) {
        Registry::get().cases().push_back({name, fn});
    }
};

inline int run() {
    auto& r = Registry::get();
    for (auto& c : r.cases()) {
        int before = r.failures();
        c.fn();
        bool ok = r.failures() == before;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name.c_str());
    }
    std::printf("\n%zu tests, %d checks, %d failures\n", r.cases().size(),
                r.checks(), r.failures());
    return r.failures() == 0 ? 0 : 1;
}

}  // namespace gmbtest

#define TEST(name)                                                     \
    static void name();                                                \
    static gmbtest::AutoReg reg_##name(#name, name);                   \
    static void name()

#define CHECK(cond)                                                    \
    do {                                                               \
        gmbtest::Registry::get().checks()++;                           \
        if (!(cond)) {                                                 \
            gmbtest::Registry::get().failures()++;                     \
            std::printf("  CHECK failed: %s (%s:%d)\n", #cond,         \
                        __FILE__, __LINE__);                           \
        }                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                 \
    do {                                                               \
        gmbtest::Registry::get().checks()++;                           \
        if (!((a) == (b))) {                                           \
            gmbtest::Registry::get().failures()++;                     \
            std::printf("  CHECK_EQ failed: %s == %s (%s:%d)\n", #a,   \
                        #b, __FILE__, __LINE__);                       \
        }                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                          \
    do {                                                               \
        gmbtest::Registry::get().checks()++;                           \
        if (std::fabs((double)(a) - (double)(b)) > (eps)) {            \
            gmbtest::Registry::get().failures()++;                     \
            std::printf("  CHECK_NEAR failed: %s ~= %s (%s:%d)\n", #a, \
                        #b, __FILE__, __LINE__);                       \
        }                                                              \
    } while (0)
