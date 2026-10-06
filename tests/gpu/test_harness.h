#pragma once

#include <cstdio>
#include <vector>

// Harness minimo (sem framework externo). TEST(nome) { CHECK(...); } registra o caso;
// test_main.cpp roda todos e devolve exit code != 0 se algum CHECK falhar.
namespace TestHarness {
    struct TestCase {
        const char* name;
        void (*fn)();
    };

    inline std::vector<TestCase>& Registry() {
        static std::vector<TestCase> registry;
        return registry;
    }

    struct Registrar {
        Registrar(const char* name, void (*fn)()) { Registry().push_back({ name, fn }); }
    };

    extern int g_failures;
}

#define TEST(name)                                                         \
    static void name();                                                    \
    static TestHarness::Registrar registrar_##name(#name, name);           \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("    CHECK falhou: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            ++TestHarness::g_failures;                                                 \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                              \
    do {                                                                                        \
        const unsigned long long a_ = static_cast<unsigned long long>(actual);                  \
        const unsigned long long e_ = static_cast<unsigned long long>(expected);                \
        if (a_ != e_) {                                                                         \
            std::printf("    CHECK_EQ falhou: %s == %s (obtido 0x%llX, esperado 0x%llX) (%s:%d)\n", \
                        #actual, #expected, a_, e_, __FILE__, __LINE__);                        \
            ++TestHarness::g_failures;                                                          \
        }                                                                                       \
    } while (0)
