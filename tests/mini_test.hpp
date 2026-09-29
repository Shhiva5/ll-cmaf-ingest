// mini_test.hpp
//
// A deliberately tiny test framework -- no GoogleTest/Catch2 dependency, to
// keep the build fully self-contained (just g++ + your standard library).
// For a one-month solo project, the value of a "real" test framework's
// features (fixtures, mocking, parameterized tests) doesn't outweigh the
// extra build complexity. This gives you registration + assertions + a
// pass/fail summary, which is all these tests need.

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <sstream>

namespace mini_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

struct AssertionFailure {
    std::string message;
};

inline int run_all() {
    int passed = 0, failed = 0;
    for (auto& t : registry()) {
        try {
            t.fn();
            std::cout << "[ PASS ] " << t.name << "\n";
            ++passed;
        } catch (const AssertionFailure& e) {
            std::cout << "[ FAIL ] " << t.name << " -- " << e.message << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[ FAIL ] " << t.name << " -- unexpected exception: " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, "
              << registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}

} // namespace mini_test

#define TEST(name) \
    static void test_fn_##name(); \
    static mini_test::Registrar registrar_##name(#name, test_fn_##name); \
    static void test_fn_##name()

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { \
        std::ostringstream oss; oss << "ASSERT_TRUE failed: " #cond " at " __FILE__ ":" << __LINE__; \
        throw mini_test::AssertionFailure{oss.str()}; \
    } } while (0)

#define ASSERT_EQ(a, b) \
    do { auto va = (a); auto vb = (b); if (!(va == vb)) { \
        std::ostringstream oss; oss << "ASSERT_EQ failed: " #a " (" << va << ") != " #b " (" << vb << ") at " __FILE__ ":" << __LINE__; \
        throw mini_test::AssertionFailure{oss.str()}; \
    } } while (0)
