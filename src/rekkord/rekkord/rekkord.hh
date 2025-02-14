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
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "src/core/base/base.hh"
#include "src/rekkord/librekkord/librekkord.hh"

namespace RG {

enum class OutputFormat {
    Plain,
    JSON,
    XML
};

static const char *const OutputFormatNames[] = {
    "Plain",
    "JSON",
    "XML"
};

bool FindAndLoadConfig(Span<const char *> arguments, rk_Config *out_config);

}
