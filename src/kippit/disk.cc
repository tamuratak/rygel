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

#include "src/core/libcc/libcc.hh"
#include "disk.hh"
#include "vendor/libsodium/src/libsodium/include/sodium.h"

namespace RG {

RG_STATIC_ASSERT(crypto_box_PUBLICKEYBYTES == 32);
RG_STATIC_ASSERT(crypto_box_SECRETKEYBYTES == 32);
RG_STATIC_ASSERT(crypto_secretstream_xchacha20poly1305_KEYBYTES == 32);

#pragma pack(push, 1)
struct ObjectIntro {
    int8_t version;
    int8_t type;
    uint8_t ekey[crypto_secretstream_xchacha20poly1305_KEYBYTES + crypto_box_SEALBYTES];
    uint8_t header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
};
#pragma pack(pop)

static const int ObjectVersion = 1;
static const Size ObjectSplit = Kibibytes(32);

bool kt_Disk::ReadObject(const kt_ID &id, kt_ObjectType *out_type, HeapArray<uint8_t> *out_obj)
{
    RG_ASSERT(url);
    RG_ASSERT(mode == kt_DiskMode::ReadWrite);

    Size prev_len = out_obj->len;
    RG_DEFER_N(err_guard) { out_obj->RemoveFrom(prev_len); };

    LocalArray<char, 256> path;
    path.len = Fmt(path.data, "blobs/%1/%2", FmtHex(id.hash[0]).Pad0(-2), id).len;

    // Read the object, we use the same buffer for the cypher and the decrypted data,
    // just 512 bytes apart which is more than enough for ChaCha20 (64-byte blocks).
    Span<const uint8_t> obj;
    {
        out_obj->Grow(512);
        out_obj->len += 512;

        Size offset = out_obj->len;
        if (!ReadRaw(path.data, out_obj))
            return false;
        obj = MakeSpan(out_obj->ptr + offset, out_obj->len - offset);
    }

    // Init object decryption
    crypto_secretstream_xchacha20poly1305_state state;
    kt_ObjectType type;
    {
        ObjectIntro intro;
        if (obj.len < RG_SIZE(intro)) {
            LogError("Truncated object");
            return false;
        }
        memcpy(&intro, obj.ptr, RG_SIZE(intro));

        if (intro.version != ObjectVersion) {
            LogError("Unexpected object version %1 (expected %2)", intro.version, ObjectVersion);
            return false;
        }
        if (intro.type < 0 || intro.type >= RG_LEN(kt_ObjectTypeNames)) {
            LogError("Invalid object type 0x%1", FmtHex(intro.type));
            return false;
        }
        type = (kt_ObjectType)intro.type;

        uint8_t key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
        if (crypto_box_seal_open(key, intro.ekey, RG_SIZE(intro.ekey), pkey, skey) != 0) {
            LogError("Failed to unseal object (wrong key?)");
            return false;
        }

        if (crypto_secretstream_xchacha20poly1305_init_pull(&state, intro.header, key) != 0) {
            LogError("Failed to initialize symmetric decryption (corrupt object?)");
            return false;
        }

        obj.ptr += RG_SIZE(ObjectIntro);
        obj.len -= RG_SIZE(ObjectIntro);
    }

    // Read and decrypt object
    Size new_len = prev_len;
    while (obj.len) {
        Size in_len = std::min(obj.len, ObjectSplit + crypto_secretstream_xchacha20poly1305_ABYTES);
        Size out_len = in_len - crypto_secretstream_xchacha20poly1305_ABYTES;

        Span<const uint8_t> cypher = MakeSpan(obj.ptr, in_len);
        uint8_t *buf = out_obj->ptr + new_len;

        unsigned long long buf_len = 0;
        uint8_t tag;
        if (crypto_secretstream_xchacha20poly1305_pull(&state, buf, &buf_len, &tag,
                                                       cypher.ptr, cypher.len, nullptr, 0) != 0) {
            LogError("Failed during symmetric decryption (corrupt object?)");
            return false;
        }

        obj.ptr += cypher.len;
        obj.len -= cypher.len;
        new_len += out_len;

        if (!obj.len) {
            if (tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
                LogError("Truncated object");
                return false;
            }
            break;
        }
    }
    out_obj->len = new_len;

    *out_type = type;
    err_guard.Disable();
    return true;
}

Size kt_Disk::WriteObject(const kt_ID &id, kt_ObjectType type, Span<const uint8_t> obj)
{
    RG_ASSERT(url);

    LocalArray<char, 256> path;
    path.len = Fmt(path.data, "blobs/%1/%2", FmtHex(id.hash[0]).Pad0(-2), id).len;

    Size written = WriteRaw(path.data, [&](FunctionRef<bool(Span<const uint8_t>)> func) {
        // Write object intro
        crypto_secretstream_xchacha20poly1305_state state;
        {
            ObjectIntro intro = {};

            intro.version = ObjectVersion;
            intro.type = (int8_t)type;

            uint8_t key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
            crypto_secretstream_xchacha20poly1305_keygen(key);
            if (crypto_secretstream_xchacha20poly1305_init_push(&state, intro.header, key) != 0) {
                LogError("Failed to initialize symmetric encryption");
                return false;
            }
            if (crypto_box_seal(intro.ekey, key, RG_SIZE(key), pkey) != 0) {
                LogError("Failed to seal symmetric key");
                return false;
            }

            Span<const uint8_t> buf = MakeSpan((const uint8_t *)&intro, RG_SIZE(intro));
            if (!func(buf))
                return false;
        }

        // Encrypt object data
        {
            bool complete = false;

            do {
                Span<const uint8_t> frag;
                frag.len = std::min(ObjectSplit, obj.len);
                frag.ptr = obj.ptr;

                complete |= (frag.len < ObjectSplit);

                uint8_t cypher[ObjectSplit + crypto_secretstream_xchacha20poly1305_ABYTES];
                unsigned char tag = complete ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;
                unsigned long long cypher_len;
                crypto_secretstream_xchacha20poly1305_push(&state, cypher, &cypher_len, frag.ptr, frag.len, nullptr, 0, tag);

                if (!func(MakeSpan(cypher, (Size)cypher_len)))
                    return false;

                obj.ptr += frag.len;
                obj.len -= frag.len;
            } while (!complete);
        }

        return true;
    });

    return written;
}

Size kt_Disk::WriteTag(const kt_ID &id)
{
    // Prepare sealed ID
    uint8_t cypher[crypto_box_SEALBYTES + 32];
    if (crypto_box_seal(cypher, id.hash, RG_SIZE(id.hash), pkey) != 0) {
        LogError("Failed to seal ID");
        return -1;
    }

    // Write tag file with random name, retry if name is already used
    for (int i = 0; i < 1000; i++) {
        LocalArray<char, 256> path;
        path.len = Fmt(path.data, "tags/%1", FmtRandom(8)).len;

        Size written = WriteRaw(path.data, [&](FunctionRef<bool(Span<const uint8_t>)> func) { return func(cypher); });

        if (written > 0)
            return written;
        if (written < 0)
            return -1;
    }

    // We really really should never reach this...
    LogError("Failed to create tag for '%1'", id);
    return -1;
}

}