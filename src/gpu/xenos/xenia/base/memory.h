// Shim minimo de xenia/base/memory.h.
#pragma once

#include <cstring>

#include "xenia/base/math.h"

namespace xe::memory {
    template <typename T, typename F>
    T Reinterpret(F from) {
        static_assert(sizeof(T) == sizeof(F));
        T to;
        std::memcpy(&to, &from, sizeof(T));
        return to;
    }
}
