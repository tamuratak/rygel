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

#include "src/core/libcc/libcc.hh"
#include "types.hh"
#include "src/core/libnet/s3.hh"
#include "src/core/libsqlite/libsqlite.hh"

namespace RG {

enum class rk_DiskMode {
    Secure,
    WriteOnly,
    ReadWrite
};
static const char *const rk_DiskModeNames[] = {
    "Secure",
    "WriteOnly",
    "ReadWrite"
};

enum class rk_ObjectType: int8_t {
    Chunk = 0,
    File = 1,
    Directory1 = 2,
    Directory2 = 5,
    Snapshot1 = 3,
    Snapshot2 = 6,
    Link = 4
};
static const char *const rk_ObjectTypeNames[] = {
    "Chunk",
    "File",
    "Directory1",
    "Snapshot1",
    "Link",
    "Directory2",
    "Snapshot2"
};

class rk_Disk {
protected:
    const char *url = nullptr;

    rk_DiskMode mode = rk_DiskMode::Secure;
    uint8_t pkey[32] = {};
    uint8_t skey[32] = {};

    sq_Database cache_db;

    BlockAllocator str_alloc;

    rk_Disk() = default;

public:
    virtual ~rk_Disk() = default;

    virtual bool Init(const char *full_pwd, const char *write_pwd) = 0;

    bool Open(const char *pwd);
    void Close();

    const char *GetURL() const { return url; }
    Span<const uint8_t> GetSalt() const { return pkey; }
    rk_DiskMode GetMode() const { return mode; }

    sq_Database *GetCache() { return &cache_db; }

    bool ReadObject(const rk_ID &id, rk_ObjectType *out_type, HeapArray<uint8_t> *out_obj);
    Size WriteObject(const rk_ID &id, rk_ObjectType type, Span<const uint8_t> obj);
    bool HasObject(const rk_ID &id);

    Size WriteTag(const rk_ID &id);
    bool ListTags(HeapArray<rk_ID> *out_ids);

protected:
    bool InitKeys(const char *full_pwd, const char *write_pwd);

    virtual bool ReadRaw(const char *path, HeapArray<uint8_t> *out_blob) = 0;
    virtual Size ReadRaw(const char *path, Span<uint8_t> out_buf) = 0;

    virtual Size WriteRaw(const char *path, Size total_len, FunctionRef<bool(FunctionRef<bool(Span<const uint8_t>)>)> func) = 0;
    virtual bool DeleteRaw(const char *path) = 0;

    virtual bool ListRaw(const char *path, Allocator *alloc, HeapArray<const char *> *out_paths) = 0;
    virtual bool TestRaw(const char *path) = 0;

private:
    bool WriteKey(const char *path, const char *pwd, const uint8_t payload[32]);
    bool ReadKey(const char *path, const char *pwd, uint8_t *out_payload, bool *out_error);

    Size WriteDirect(const char *path, Span<const uint8_t> buf);
};

std::unique_ptr<rk_Disk> rk_OpenLocalDisk(const char *path, const char *pwd = nullptr);
std::unique_ptr<rk_Disk> rk_OpenS3Disk(const s3_Config &config, const char *pwd = nullptr);

}