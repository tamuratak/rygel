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
#include "src/core/network/network.hh"

namespace RG {

int RunInit(Span<const char *> arguments);
int RunMigrate(Span<const char *> arguments);
int RunKeys(Span<const char *> arguments);

int RunUnseal(Span<const char *> arguments);

void HandleDemo(const http_RequestInfo &request, http_IO *io);

void HandleInstanceCreate(const http_RequestInfo &request, http_IO *io);
void HandleInstanceDelete(const http_RequestInfo &request, http_IO *io);
void HandleInstanceConfigure(const http_RequestInfo &request, http_IO *io);
void HandleInstanceList(const http_RequestInfo &request, http_IO *io);
void HandleInstanceAssign(const http_RequestInfo &request, http_IO *io);
void HandleInstancePermissions(const http_RequestInfo &request, http_IO *io);

bool ArchiveDomain();

void HandleArchiveCreate(const http_RequestInfo &request, http_IO *io);
void HandleArchiveDelete(const http_RequestInfo &request, http_IO *io);
void HandleArchiveList(const http_RequestInfo &request, http_IO *io);
void HandleArchiveDownload(const http_RequestInfo &request, http_IO *io);
void HandleArchiveUpload(const http_RequestInfo &request, http_IO *io);
void HandleArchiveRestore(const http_RequestInfo &request, http_IO *io);

void HandleUserCreate(const http_RequestInfo &request, http_IO *io);
void HandleUserEdit(const http_RequestInfo &request, http_IO *io);
void HandleUserDelete(const http_RequestInfo &request, http_IO *io);
void HandleUserList(const http_RequestInfo &request, http_IO *io);

}
