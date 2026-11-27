// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>

int SpirvReferenceImpl(int, char*[]) {
    std::fputs("The independent SPIR-V recompiler is unavailable.\n", stderr);
    return EXIT_FAILURE;
}
