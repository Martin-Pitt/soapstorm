/**
 * @file ssserial.h
 * @brief Shared byte-level serialisation for the Soapstorm on-disk stores, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_SERIAL_H
#define SS_SERIAL_H

#include "lluuid.h"
#include "llcrc.h"

#include <cstring>
#include <vector>

// Header-only on purpose, and this is the ONLY thing lifted out of ssbc7store.cpp into a lower library. The volume manager was deliberately NOT lifted: doc/strata.md lists mutation support as the first thing the BC7 engine lacks to be general, and the asset tier is the tenant that needs it, so generalising one store into two shapes would have cost the BC7 store's strongest property - that a record is immutable per (uuid, encoder_version), which is what makes plain-UUID dedupe sound there. What genuinely is common between the two stores is the framing: explicit little-endian field writes, one CRC over a fixed-stride record, and a scan that stops at the first record that fails it. That is what lives here.
//
// Nothing in this header touches a filesystem, a setting or a viewer global, so both the viewer build and the offline harness compile it identically.

namespace ssserial
{
    // Every field is written explicitly rather than by struct punning, so packing and endianness are never left to the compiler and the offline harness sees exactly the bytes the viewer wrote. Copied in behaviour from the writer in ssroccache.cpp and ssbc7store.cpp so that a record written by any of the three reads back the same way.
    class Writer
    {
    public:
        explicit Writer(std::vector<U8>& out) : mOut(out) {}

        void bytes(const void* src, size_t n) { const U8* p = (const U8*)src; mOut.insert(mOut.end(), p, p + n); }
        template <typename T> void pod(const T& v) { bytes(&v, sizeof(T)); }
        void uuid(const LLUUID& id) { bytes(id.mData, UUID_BYTES); }
        void zeros(size_t n) { mOut.insert(mOut.end(), n, (U8)0); }
        size_t size() const { return mOut.size(); }

    private:
        std::vector<U8>& mOut;
    };

    // A short read is a hard failure that latches, so a caller may do a whole run of reads and check ok() once at the end rather than after every field - which is what stops a truncated buffer from being read as a record full of stack garbage.
    class Reader
    {
    public:
        Reader(const U8* data, size_t size) : mData(data), mSize(size), mPos(0), mOk(true) {}

        bool ok() const { return mOk; }
        size_t tell() const { return mPos; }
        size_t remaining() const { return mPos <= mSize ? mSize - mPos : 0; }

        bool bytes(void* dst, size_t n)
        {
            if (!mOk || mPos + n > mSize) { mOk = false; return false; }
            memcpy(dst, mData + mPos, n);
            mPos += n;
            return true;
        }
        template <typename T> T pod() { T v = T(); bytes(&v, sizeof(T)); return v; }
        LLUUID uuid() { LLUUID id; bytes(id.mData, UUID_BYTES); return id; }
        void skip(size_t n) { if (!mOk || mPos + n > mSize) { mOk = false; return; } mPos += n; }

    private:
        const U8*   mData;
        size_t      mSize;
        size_t      mPos;
        bool        mOk;
    };

    inline U32 crc32Of(const U8* data, size_t size)
    {
        LLCRC crc;
        if (size) crc.update(data, size);
        return crc.getCRC();
    }

    // The record checksum convention every store here shares: the CRC covers the record's bytes up to but NOT including the four byte checksum that terminates it, so a record can be serialised into its own buffer and checksummed in place without a second copy. `stride` is the full on-disk record size including the checksum and any reserved tail.
    inline U32 recordCRC(const U8* record, size_t stride)
    {
        return stride >= 4 ? crc32Of(record, stride - 4) : 0;
    }

    inline void sealRecord(U8* record, size_t stride)
    {
        const U32 crc = recordCRC(record, stride);
        memcpy(record + stride - 4, &crc, sizeof(U32));
    }

    inline bool checkRecord(const U8* record, size_t stride)
    {
        U32 stored = 0;
        memcpy(&stored, record + stride - 4, sizeof(U32));
        return stored == recordCRC(record, stride);
    }
}

#endif // SS_SERIAL_H
