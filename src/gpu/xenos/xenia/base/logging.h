// Shim minimo de xenia/base/logging.h: mensagens vao para stderr.
#pragma once

#include <cstdio>

#define XELOGE(...) std::fputs("[GPU] erro no formato Xenos\n", stderr)
#define XELOGW(...) ((void)0)
#define XELOGI(...) ((void)0)
#define XELOGD(...) ((void)0)
#define XELOGGPU(...) ((void)0)
