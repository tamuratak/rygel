// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "src/core/base/base.hh"

namespace RG {

struct QtInfo {
    const char *qmake = nullptr;
    const char *moc = nullptr;
    const char *rcc = nullptr;
    const char *uic = nullptr;

    const char *binaries = nullptr;
    const char *headers = nullptr;
    const char *libraries = nullptr;
    const char *plugins = nullptr;

    int64_t version = 0;
    int version_major = 0;
    bool shared = false;
};

bool FindQtSdk(const Compiler *compiler, const char *qmake_binary, Allocator *alloc, QtInfo *out_qt);

bool FindArduinoCompiler(const char *name, const char *compiler, Span<char> out_cc);

}
