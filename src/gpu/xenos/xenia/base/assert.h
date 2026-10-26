// Shim minimo de xenia/base/assert.h para os headers do Xenia portados em src/gpu/xenos.
#pragma once

#include <cassert>
#include <cstddef>

#define static_assert_size(type, size) \
    static_assert(sizeof(type) == (size), "bad definition for " #type ": must be " #size " bytes")

#define assert_always(...) assert(false)
#define assert_true(expr, ...) assert(expr)
#define assert_false(expr, ...) assert(!(expr))
#define assert_zero(expr, ...) assert((expr) == 0)
#define assert_not_zero(expr, ...) assert((expr) != 0)
#define assert_null(expr, ...) assert((expr) == nullptr)
#define assert_not_null(expr, ...) assert((expr) != nullptr)
#define assert_unhandled_case(variable) assert(false && "unhandled case")
