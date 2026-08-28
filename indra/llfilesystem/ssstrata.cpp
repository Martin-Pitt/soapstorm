/**
 * @file ssstrata.cpp
 * @brief Strata asset volume store, format half - every byte the store puts on disk, and nothing that touches a disk, see doc/strata.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "ssstrata.h"
#include "ssserial.h"

#include <cstring>
#include <ctime>

// Split from ssstrataio.cpp deliberately, exactly as ssbc7store.cpp is split from ssbc7storeio.cpp: this half reaches for no directory, no handle and no setting, so the exact bytes the store writes can be built, parsed and corrupted in an offline test that links this file alone.

SSStrataRecord::SSStrataRecord()
:   mOffset(0),
    mSize(0),
    mDataCRC(0),
    mVolume(0),
    mLastUseDay(0),
    mFlags(0),
    mAssetType(0xFF),
    mReserved(0),
    mRecordCRC(0),
    mTouchSecond(0)
{
}

SSStrataBlobHeader::SSStrataBlobHeader()
:   mVersion(0),
    mSize(0),
    mDataCRC(0),
    mFlags(0),
    mAssetType(0xFF),
    mReserved(0)
{
}

const char* ssStrataTenantName(ESSStrataTenant tenant)
{
    switch (tenant)
    {
    case SSSTRATA_TENANT_ASSETS:    return "assets";
    case SSSTRATA_TENANT_TEXTURES:  return "textures";
    default:                        return "?";
    }
}

const char* ssStrataPackVerdictName(ESSStrataPackVerdict verdict)
{
    switch (verdict)
    {
    case SSSTRATA_PACK_RAN:                 return "RAN";
    case SSSTRATA_PACK_NOTHING_READY:       return "NOTHING_READY";
    case SSSTRATA_PACK_NO_CANDIDATES:       return "NO_CANDIDATES";
    case SSSTRATA_PACK_DISABLED:            return "DISABLED";
    case SSSTRATA_PACK_READ_ONLY:           return "READ_ONLY";
    case SSSTRATA_PACK_NOT_READY:           return "NOT_READY";
    case SSSTRATA_PACK_STALE:               return "STALE";
    case SSSTRATA_PACK_ALREADY_RUNNING:     return "ALREADY_RUNNING";
    case SSSTRATA_PACK_OVER_BUDGET:         return "OVER_BUDGET";
    case SSSTRATA_PACK_NO_VOLUME:           return "NO_VOLUME";
    case SSSTRATA_PACK_VOLUME_WRITE_FAILED: return "VOLUME_WRITE_FAILED";
    case SSSTRATA_PACK_INDEX_WRITE_FAILED:  return "INDEX_WRITE_FAILED";
    default:                                return "?";
    }
}

const char* ssStrataReclaimVerdictName(ESSStrataReclaimVerdict verdict)
{
    switch (verdict)
    {
    case SSSTRATA_RECLAIM_RAN:                  return "RAN";
    case SSSTRATA_RECLAIM_NOT_NEEDED:           return "NOT_NEEDED";
    case SSSTRATA_RECLAIM_DISABLED:             return "DISABLED";
    case SSSTRATA_RECLAIM_READ_ONLY:            return "READ_ONLY";
    case SSSTRATA_RECLAIM_NOT_READY:            return "NOT_READY";
    case SSSTRATA_RECLAIM_STALE:                return "STALE";
    case SSSTRATA_RECLAIM_ALREADY_RUNNING:      return "ALREADY_RUNNING";
    case SSSTRATA_RECLAIM_COOLDOWN:             return "COOLDOWN";
    case SSSTRATA_RECLAIM_NO_CANDIDATE:         return "NO_CANDIDATE";
    case SSSTRATA_RECLAIM_TOO_YOUNG:            return "TOO_YOUNG";
    case SSSTRATA_RECLAIM_ALL_HOT:              return "ALL_HOT";
    case SSSTRATA_RECLAIM_INDEX_WRITE_FAILED:   return "INDEX_WRITE_FAILED";
    case SSSTRATA_RECLAIM_UNLINK_FAILED:        return "UNLINK_FAILED";
    default:                                    return "?";
    }
}

const char* ssStrataUnpackName(ESSStrataUnpack verdict)
{
    switch (verdict)
    {
    case SSSTRATA_UNPACK_NOT_PACKED:        return "NOT_PACKED";
    case SSSTRATA_UNPACK_DONE:              return "DONE";
    case SSSTRATA_UNPACK_DROPPED:           return "DROPPED";
    case SSSTRATA_UNPACK_DISABLED:          return "DISABLED";
    case SSSTRATA_UNPACK_READ_ONLY_FORGOT:  return "READ_ONLY_FORGOT";
    case SSSTRATA_UNPACK_READ_FAILED:       return "READ_FAILED";
    case SSSTRATA_UNPACK_WRITE_FAILED:      return "WRITE_FAILED";
    default:                                return "?";
    }
}

// Days since the unix epoch. A drop-first priority signal only - it is never read as an expiry, so a clock that jumps backwards costs reclaim ordering and nothing else.
U16 ssStrataToday()
{
    return (U16)llmin((U64)(time(NULL) / 86400), (U64)0xFFFFu);
}

U32 ssStrataNowSeconds()
{
    return (U32)((U64)time(NULL) & 0xFFFFFFFFull);
}

// ---------------------------------------------------------------------------
// Index records
// ---------------------------------------------------------------------------

void ssStrataSerializeRecord(const SSStrataRecord& rec, U8* out)
{
    std::vector<U8> buf;
    buf.reserve(SSSTRATA_RECORD_SIZE);
    ssserial::Writer w(buf);

    w.uuid(rec.mUUID);
    w.pod<U64>(rec.mOffset);
    w.pod<U32>(rec.mSize);
    w.pod<U32>(rec.mDataCRC);
    w.pod<U16>(rec.mVolume);
    w.pod<U16>(rec.mLastUseDay);
    w.pod<U16>(rec.mFlags);
    w.pod<U8>(rec.mAssetType);
    w.pod<U8>(rec.mReserved);
    w.zeros(SSSTRATA_RECORD_SIZE - 4 - buf.size());   // reserved tail, zeroed so the checksum is defined and a future field costs no version bump
    w.zeros(4);

    memcpy(out, buf.data(), SSSTRATA_RECORD_SIZE);
    ssserial::sealRecord(out, SSSTRATA_RECORD_SIZE);
}

bool ssStrataDeserializeRecord(const U8* in, SSStrataRecord& out)
{
    if (!ssserial::checkRecord(in, SSSTRATA_RECORD_SIZE)) return false;

    ssserial::Reader r(in, SSSTRATA_RECORD_SIZE);
    out = SSStrataRecord();
    out.mUUID       = r.uuid();
    out.mOffset     = r.pod<U64>();
    out.mSize       = r.pod<U32>();
    out.mDataCRC    = r.pod<U32>();
    out.mVolume     = r.pod<U16>();
    out.mLastUseDay = r.pod<U16>();
    out.mFlags      = r.pod<U16>();
    out.mAssetType  = r.pod<U8>();
    out.mReserved   = r.pod<U8>();
    if (!r.ok()) return false;

    memcpy(&out.mRecordCRC, in + SSSTRATA_RECORD_SIZE - 4, sizeof(U32));

    // A tombstone carries no geometry, so only its uuid is meaningful and the checks below would reject every one of them.
    if (out.isTombstone()) return !out.mUUID.isNull();

    if (out.mUUID.isNull()) return false;
    if (out.mVolume >= SSSTRATA_MAX_VOLUMES) return false;
    if (out.mSize == 0 || out.mSize > SSSTRATA_MAX_OBJECT_CEILING) return false;
    if (out.mOffset < SSSTRATA_VOL_HEADER_SIZE) return false;
    if ((out.mOffset % SSSTRATA_ALIGN) != 0) return false;

    // Against the FORMAT ceiling and never against the runtime cap: the runtime cap moves with the user's budget, so validating against it would silently discard the tail of an index written while the slider was higher.
    if (out.blobEnd() > SSSTRATA_VOLUME_CAP) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Container headers
// ---------------------------------------------------------------------------

void ssStrataBuildIndexHeader(U32 disk_cache_version, std::vector<U8>& out)
{
    out.clear();
    out.reserve(SSSTRATA_HEADER_SIZE);
    ssserial::Writer w(out);

    w.pod<U32>(SSSTRATA_IDX_MAGIC);
    w.pod<U32>(SSSTRATA_FORMAT_VERSION);
    w.pod<U32>(disk_cache_version);
    w.zeros(SSSTRATA_HEADER_SIZE - 4 - out.size());
    w.zeros(4);

    ssserial::sealRecord(out.data(), SSSTRATA_HEADER_SIZE);
}

bool ssStrataParseIndexHeader(const U8* data, U32 disk_cache_version)
{
    if (!ssserial::checkRecord(data, SSSTRATA_HEADER_SIZE)) return false;

    ssserial::Reader r(data, SSSTRATA_HEADER_SIZE);
    if (r.pod<U32>() != SSSTRATA_IDX_MAGIC) return false;
    if (r.pod<U32>() != SSSTRATA_FORMAT_VERSION) return false;

    // Wipe, never migrate. A DiskCacheVersion bump already means the viewer decided the whole cache is stale, and this is what makes the volumes go with it rather than surviving as an unreachable four gigabytes.
    if (r.pod<U32>() != disk_cache_version) return false;

    return r.ok();
}

void ssStrataBuildVolumeHeader(U32 volume, std::vector<U8>& out)
{
    out.clear();
    out.reserve(SSSTRATA_VOL_HEADER_SIZE);
    ssserial::Writer w(out);

    w.pod<U32>(SSSTRATA_VOL_MAGIC);
    w.pod<U32>(SSSTRATA_FORMAT_VERSION);
    w.pod<U32>(volume);
    w.zeros(SSSTRATA_VOL_HEADER_SIZE - 4 - out.size());
    w.zeros(4);

    ssserial::sealRecord(out.data(), SSSTRATA_VOL_HEADER_SIZE);
}

bool ssStrataParseVolumeHeader(const U8* data, U32 expect_volume)
{
    if (!ssserial::checkRecord(data, SSSTRATA_VOL_HEADER_SIZE)) return false;

    ssserial::Reader r(data, SSSTRATA_VOL_HEADER_SIZE);
    if (r.pod<U32>() != SSSTRATA_VOL_MAGIC) return false;
    if (r.pod<U32>() != SSSTRATA_FORMAT_VERSION) return false;
    if (r.pod<U32>() != expect_volume) return false;

    return r.ok();
}

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------

bool ssStrataBuildBlob(const LLUUID& id, const U8* payload, U32 size, U8 asset_type, std::vector<U8>& out_blob, SSStrataRecord& out_record)
{
    out_blob.clear();
    if (id.isNull()) return false;
    if (size == 0 || size > SSSTRATA_MAX_OBJECT_CEILING) return false;
    if (!payload) return false;

    const U32 data_crc = ssserial::crc32Of(payload, size);

    out_blob.reserve((size_t)SSSTRATA_BLOB_HEADER_SIZE + size);
    ssserial::Writer w(out_blob);
    w.pod<U32>(SSSTRATA_BLOB_MAGIC);
    w.pod<U32>(SSSTRATA_FORMAT_VERSION);
    w.uuid(id);
    w.pod<U32>(size);
    w.pod<U32>(data_crc);
    w.pod<U16>((U16)0);
    w.pod<U8>(asset_type);
    w.pod<U8>((U8)0);
    w.zeros(SSSTRATA_BLOB_HEADER_SIZE - 4 - out_blob.size());
    w.zeros(4);
    ssserial::sealRecord(out_blob.data(), SSSTRATA_BLOB_HEADER_SIZE);

    w.bytes(payload, size);

    out_record = SSStrataRecord();
    out_record.mUUID       = id;
    out_record.mSize       = size;
    out_record.mDataCRC    = data_crc;
    out_record.mAssetType  = asset_type;
    out_record.mLastUseDay = ssStrataToday();
    out_record.mTouchSecond = ssStrataNowSeconds();
    return true;
}

bool ssStrataParseBlobHeader(const U8* data, size_t size, SSStrataBlobHeader& out)
{
    if (!data || size < SSSTRATA_BLOB_HEADER_SIZE) return false;
    if (!ssserial::checkRecord(data, SSSTRATA_BLOB_HEADER_SIZE)) return false;

    ssserial::Reader r(data, SSSTRATA_BLOB_HEADER_SIZE);
    if (r.pod<U32>() != SSSTRATA_BLOB_MAGIC) return false;

    out = SSStrataBlobHeader();
    out.mVersion    = r.pod<U32>();
    out.mUUID       = r.uuid();
    out.mSize       = r.pod<U32>();
    out.mDataCRC    = r.pod<U32>();
    out.mFlags      = r.pod<U16>();
    out.mAssetType  = r.pod<U8>();
    out.mReserved   = r.pod<U8>();
    if (!r.ok()) return false;

    if (out.mVersion != SSSTRATA_FORMAT_VERSION) return false;
    if (out.mSize == 0 || out.mSize > SSSTRATA_MAX_OBJECT_CEILING) return false;
    return true;
}

bool ssStrataVerifyBlobHeader(const U8* data, size_t size, const LLUUID& expect_uuid, U32 expect_size)
{
    SSStrataBlobHeader hdr;
    if (!ssStrataParseBlobHeader(data, size, hdr)) return false;

    // FIRST and UNCONDITIONALLY. A reclaimed volume id can be handed out again, so a stale offset lands on a blob that passes every checksum it carries and the only thing separating it from the blob the caller asked for is this comparison.
    if (hdr.mUUID != expect_uuid) return false;
    if (expect_size != 0 && hdr.mSize != expect_size) return false;
    return true;
}

bool ssStrataVerifyBlob(const U8* data, size_t size, const LLUUID& expect_uuid)
{
    SSStrataBlobHeader hdr;
    if (!ssStrataParseBlobHeader(data, size, hdr)) return false;
    if (hdr.mUUID != expect_uuid) return false;
    if (size < (size_t)SSSTRATA_BLOB_HEADER_SIZE + (size_t)hdr.mSize) return false;

    return ssserial::crc32Of(data + SSSTRATA_BLOB_HEADER_SIZE, hdr.mSize) == hdr.mDataCRC;
}
