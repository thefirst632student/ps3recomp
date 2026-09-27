/*
 * ps3recomp - cellPamf HLE implementation
 *
 * Parses PAMF container headers to extract stream information.
 * The actual PAMF header format is parsed to provide correct stream
 * counts and codec parameters to cellDmux/cellVdec/cellAdec.
 */

#include "cellPamf.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * PAMF file header layout (big-endian on disc)
 *
 * Offset  Size  Description
 * 0x00    4     Magic "PAMF"
 * 0x04    4     Version (typically 0x00040100)
 * 0x08    4     Data offset (header size / start of mux data)
 * 0x0C    4     Data size
 * 0x10    4     Stream descriptor table offset
 * 0x14    2     Number of streams
 * 0x18    4     Presentation start time (90kHz ticks)
 * 0x1C    4     Presentation end time (90kHz ticks)
 * 0x20    4     Mux rate (bytes/sec)
 * 0x24    4     EP table offset
 * 0x28    4     EP count
 *
 * We access these via raw byte offsets rather than a packed struct to
 * avoid alignment/padding issues across compilers.
 * -----------------------------------------------------------------------*/

#define PAMF_OFF_MAGIC            0x00
#define PAMF_OFF_VERSION          0x04
#define PAMF_OFF_DATA_OFFSET      0x08
#define PAMF_OFF_DATA_SIZE        0x0C
#define PAMF_OFF_STREAM_TABLE     0x10
#define PAMF_OFF_NUM_STREAMS      0x14
#define PAMF_OFF_PRESENT_START    0x18
#define PAMF_OFF_PRESENT_END      0x1C
#define PAMF_OFF_MUX_RATE         0x20
#define PAMF_OFF_EP_OFFSET        0x24
#define PAMF_OFF_EP_COUNT         0x28

/* Minimum valid header size (enough to read all fields above) */
#define PAMF_MIN_HEADER_SIZE      0x2C

/* Stream descriptor size (fixed 24 bytes per descriptor) */
#define PAMF_STREAM_DESC_SIZE     24

/* Raw stream type values in the PAMF file (different from SDK constants!) */
#define PAMF_RAW_TYPE_AVC         1
#define PAMF_RAW_TYPE_M2V         2
#define PAMF_RAW_TYPE_ATRAC3PLUS  3
#define PAMF_RAW_TYPE_AC3         4
#define PAMF_RAW_TYPE_LPCM        5
#define PAMF_RAW_TYPE_USERDATA    6

/* ---------------------------------------------------------------------------
 * Endian helpers (PAMF is big-endian)
 * -----------------------------------------------------------------------*/
static u32 be32(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u64 be64(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u64)be32(b) << 32) | be32(b + 4);
}

static u16 be16(const void* p)
{
    const u8* b = (const u8*)p;
    return ((u16)b[0] << 8) | b[1];
}

/* ---------------------------------------------------------------------------
 * Raw-type to SDK-type conversion
 * -----------------------------------------------------------------------*/
static s32 raw_type_to_sdk(u8 rawType)
{
    if (rawType == PAMF_RAW_TYPE_AVC || rawType == CELL_PAMF_CODEC_TYPE_AVC) return CELL_PAMF_STREAM_TYPE_AVC;
    if (rawType == PAMF_RAW_TYPE_M2V || rawType == CELL_PAMF_CODEC_TYPE_M2V) return CELL_PAMF_STREAM_TYPE_M2V;
    if (rawType == PAMF_RAW_TYPE_ATRAC3PLUS || rawType == CELL_PAMF_CODEC_TYPE_ATRAC3PLUS) return CELL_PAMF_STREAM_TYPE_ATRAC3PLUS;
    if (rawType == PAMF_RAW_TYPE_AC3 || rawType == CELL_PAMF_CODEC_TYPE_AC3) return CELL_PAMF_STREAM_TYPE_AC3;
    if (rawType == PAMF_RAW_TYPE_LPCM || rawType == CELL_PAMF_CODEC_TYPE_LPCM) return CELL_PAMF_STREAM_TYPE_PAMF_LPCM;
    if (rawType == PAMF_RAW_TYPE_USERDATA) return CELL_PAMF_STREAM_TYPE_USER_DATA;
    return -1;
}

static int is_matching_stream_type(s32 sdkType, u8 requestedType)
{
    if (sdkType < 0)
        return 0;
    if (requestedType == (u8)sdkType)
        return 1;
    if (requestedType == CELL_PAMF_STREAM_TYPE_VIDEO) {
        return (sdkType == CELL_PAMF_STREAM_TYPE_AVC || sdkType == CELL_PAMF_STREAM_TYPE_M2V);
    }
    if (requestedType == CELL_PAMF_STREAM_TYPE_AUDIO) {
        return (sdkType == CELL_PAMF_STREAM_TYPE_ATRAC3PLUS ||
                sdkType == CELL_PAMF_STREAM_TYPE_PAMF_LPCM ||
                sdkType == CELL_PAMF_STREAM_TYPE_AC3);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Reader state lives HOST-side, keyed by the guest EA.
 *
 * The title allocates a CellPamfReader and passes its address in. Our
 * CellPamfReader is a HOST-layout struct -- an 8-byte void* first member, 32
 * bytes total -- while the guest's is 28 bytes with a 4-byte pointer. So we
 * cannot simply translate the pointer and write through it: that stores four
 * bytes past the end of the caller's struct, and the pamfAddr field would hold
 * a 64-bit host pointer the guest could never use. Key off the EA instead and
 * never write the guest's copy; the title treats the reader as opaque.
 *
 * ponytail: linear scan over 8 slots. A title juggling more PAMF readers than
 * that wants a hash; nothing has needed even two.
 * -----------------------------------------------------------------------*/
#define PAMF_MAX_READERS 8
static struct { u32 ea; int in_use; CellPamfReader r; } s_readers[PAMF_MAX_READERS];

static CellPamfReader* pamf_host(CellPamfReader* guest_ea)
{
    u32 ea = (u32)(uintptr_t)guest_ea;
    if (!ea)
        return NULL;
    for (int i = 0; i < PAMF_MAX_READERS; i++)
        if (s_readers[i].in_use && s_readers[i].ea == ea)
            return &s_readers[i].r;
    for (int i = 0; i < PAMF_MAX_READERS; i++)
        if (!s_readers[i].in_use) {
            s_readers[i].in_use = 1;
            s_readers[i].ea = ea;
            memset(&s_readers[i].r, 0, sizeof(s_readers[i].r));
            return &s_readers[i].r;
        }
    return NULL;
}

/* --------------------------------------------------------------------------- 
 * Reader lifecycle
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderInitialize(CellPamfReader* reader, void* pamfAddr,
                              u32 pamfSize, u32 attribute)
{
    reader = pamf_host(reader);
    (void)attribute;

    printf("[cellPamf] ReaderInitialize(addr=%p, size=%u)\n", pamfAddr, pamfSize);

    if (!reader || !pamfAddr || pamfSize < PAMF_MIN_HEADER_SIZE)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    pamfAddr = GUEST_PTR(pamfAddr, void*);
    const u8* base = (const u8*)pamfAddr;

    /* Verify magic */
    u32 magic = be32(base + PAMF_OFF_MAGIC);
    if (magic != CELL_PAMF_MAGIC) {
        printf("[cellPamf] Invalid PAMF magic: 0x%08X\n", magic);
        return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    }

    reader->pamfAddr = pamfAddr;
    reader->pamfSize = pamfSize;

    /* Detect PAMF layout:
     * In PAMF 0041 containers:
     * Offset 0x08: header block count (in 2048-byte blocks)
     * Offset 0x0C: data block count (in 2048-byte blocks)
     * Offset 0x80: 0x00010000, offset 0x84: [0..1]=size, [2..3]=streamCount
     * Stream descriptors start at 0x88, stride 48 bytes.
     */
    if (pamfSize >= 0x90 && be32(base + 0x80) == 0x00010000 && be16(base + 0x86) > 0) {
        reader->dataOffset = be32(base + PAMF_OFF_DATA_OFFSET) * 2048;
        reader->dataSize = be32(base + PAMF_OFF_DATA_SIZE) * 2048;
        reader->streamTableOffset = 0x88;
        reader->numStreams = (u8)be16(base + 0x86);
        reader->streamDescSize = 48;
    } else {
        reader->dataOffset = be32(base + PAMF_OFF_DATA_OFFSET);
        reader->dataSize = be32(base + PAMF_OFF_DATA_SIZE);
        reader->streamTableOffset = be32(base + PAMF_OFF_STREAM_TABLE);
        reader->numStreams = (u8)be16(base + PAMF_OFF_NUM_STREAMS);
        reader->streamDescSize = PAMF_STREAM_DESC_SIZE;
    }
    reader->currentStream = 0;
    reader->currentEP = 0;

    u32 muxRate = (pamfSize >= 0x70) ? be32(base + 0x64) : be32(base + PAMF_OFF_MUX_RATE);

    printf("[cellPamf] PAMF v%08X: %u streams (descSz=%u), dataOffset=0x%X, dataSize=%u, "
           "streamTableOff=0x%X, muxRate=%u\n",
           be32(base + PAMF_OFF_VERSION),
           reader->numStreams, reader->streamDescSize, reader->dataOffset, reader->dataSize,
           reader->streamTableOffset, muxRate);

    /* Log each stream descriptor */
    for (u32 i = 0; i < reader->numStreams; i++) {
        u32 stride = reader->streamDescSize ? reader->streamDescSize : PAMF_STREAM_DESC_SIZE;
        const u8* desc = base + reader->streamTableOffset + i * stride;
        u8 rawType = desc[0];
        u8 channel = (reader->streamDescSize == 48) ? 0 : desc[1];
        s32 sdkType = raw_type_to_sdk(rawType);
        printf("[cellPamf]   stream[%u]: rawType=0x%02X (sdk=%d) channel=%u\n",
               i, rawType, sdkType, channel);
    }

    return CELL_OK;
}

s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, u64* startTime)
{
    reader = pamf_host(reader);
    if (!reader || !startTime)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    u64 pts = (reader->streamDescSize == 48) ? (u64)be32(base + 0x58) : (u64)be32(base + PAMF_OFF_PRESENT_START);
    vm_write64((u32)(uintptr_t)startTime, pts);
    return CELL_OK;
}

s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, u64* endTime)
{
    reader = pamf_host(reader);
    if (!reader || !endTime)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    u64 pts;
    if (reader->streamDescSize == 48) {
        pts = ((u64)be32(base + 0x5C) << 32) | be32(base + 0x60);
        if (pts == 0) pts = be32(base + 0x60);
    } else {
        pts = (u64)be32(base + PAMF_OFF_PRESENT_END);
    }
    vm_write64((u32)(uintptr_t)endTime, pts);
    return CELL_OK;
}

u32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader)
{
    reader = pamf_host(reader);
    if (!reader)
        return 0;

    const u8* base = (const u8*)reader->pamfAddr;
    return (reader->streamDescSize == 48) ? be32(base + 0x64) : be32(base + PAMF_OFF_MUX_RATE);
}

/* ---------------------------------------------------------------------------
 * Stream queries
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    return reader->numStreams;
}

/* Get pointer to raw stream descriptor bytes by absolute index */
static const u8* get_stream_desc_raw(CellPamfReader* reader, u32 index)
{
    if (!reader || index >= reader->numStreams)
        return NULL;

    const u8* base = (const u8*)reader->pamfAddr;
    u32 stride = reader->streamDescSize ? reader->streamDescSize : PAMF_STREAM_DESC_SIZE;
    return base + reader->streamTableOffset + index * stride;
}

/* Get SDK stream type for a given absolute stream index */
static s32 get_stream_sdk_type(CellPamfReader* reader, u32 index)
{
    const u8* desc = get_stream_desc_raw(reader, index);
    if (!desc) return -1;
    return raw_type_to_sdk(desc[0]);
}

s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    s32 count = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        s32 sdkType = get_stream_sdk_type(reader, i);
        if (is_matching_stream_type(sdkType, streamType))
            count++;
    }
    return count;
}

s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u32 streamIndex)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    u32 found = 0;
    for (u32 i = 0; i < reader->numStreams; i++) {
        s32 sdkType = get_stream_sdk_type(reader, i);
        if (is_matching_stream_type(sdkType, streamType)) {
            if (found == streamIndex) {
                reader->currentStream = (u8)i;
                return CELL_OK;
            }
            found++;
        }
    }
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}

s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u32 channel)
{
    /* Forward the GUEST pointer untouched: cellPamfReaderSetStreamWithType
     * resolves it itself, and pamf_host keys on the guest EA -- handing it an
     * already-resolved host pointer would key a second, empty slot. */
    return cellPamfReaderSetStreamWithType(reader, streamType, channel);
}

s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u32 streamIndex)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    if (streamIndex >= reader->numStreams)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    reader->currentStream = (u8)streamIndex;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Sample rate / channel lookup helpers for audio descriptors
 * -----------------------------------------------------------------------*/
static u32 audio_sample_rate(u8 code)
{
    switch (code) {
    case 1: return 44100;
    case 2: return 48000;
    default: return 48000;
    }
}

static u8 audio_channels(u8 code)
{
    switch (code) {
    case 1: return 1;
    case 2: return 2;
    case 6: return 6;
    case 8: return 8;
    default: return 2;
    }
}

static u8 lpcm_bits(u8 code)
{
    switch (code) {
    case 1: return 16;
    case 2: return 24;
    default: return 16;
    }
}

/* ---------------------------------------------------------------------------
 * Stream info extraction — reads real codec parameters from descriptors
 *
 * Stream descriptor layout (24 bytes, big-endian):
 *   Byte  0:    stream type (raw: 1=AVC, 2=M2V, 3=ATRAC3+, 4=AC3, 5=LPCM)
 *   Byte  1:    channel index
 *   For AVC (type 1):
 *     Byte  2:  profileIdc (66=baseline, 77=main, 100=high)
 *     Byte  3:  levelIdc
 *     Byte  4:  frameMbsOnlyFlag (bit 7), videoSignalInfoFlag (bit 6)
 *     Byte  5:  frameRateCode (upper nibble), aspectRatioIdc (lower nibble)
 *     Bytes 6-7:   sarWidth  (be16)
 *     Bytes 8-9:   sarHeight (be16)
 *     Bytes 14-15:  horizontalSize (be16)
 *     Bytes 16-17:  verticalSize   (be16)
 *     Bytes 18-19:  frameCropLeft/Right
 *     Bytes 20-21:  frameCropTop/Bottom
 *   For ATRAC3+ (type 3) / AC3 (type 4):
 *     Byte 10:  channel configuration
 *     Byte 12:  sample rate code
 *   For LPCM (type 5):
 *     Byte 10:  channel configuration
 *     Byte 11:  bits-per-sample code
 *     Byte 12:  sample rate code
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize)
{
    reader = pamf_host(reader);
    if (!reader || !info)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* desc = get_stream_desc_raw(reader, reader->currentStream);
    if (!desc)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    s32 sdkType = raw_type_to_sdk(desc[0]);
    u32 ea = (u32)(uintptr_t)info;
    if (!ea)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    void* hostDst = GUEST_PTR(info, void*);
    if (hostDst && infoSize > 0) {
        memset(hostDst, 0, infoSize);
    }

    switch (sdkType) {
    case CELL_PAMF_STREAM_TYPE_AVC: {
        u8 profileIdc, levelIdc, frameMbsOnlyFlag, videoSignalInfoFlag, frameRateInfo;
        u16 sarWidth = 1, sarHeight = 1, horizontalSize = 1280, verticalSize = 720;
        u16 cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;

        if (reader->streamDescSize == 48) {
            profileIdc = desc[0x10];
            levelIdc = desc[0x11];
            frameMbsOnlyFlag = (desc[0x12] >> 7) & 1;
            videoSignalInfoFlag = (desc[0x12] >> 6) & 1;
            frameRateInfo = desc[0x13];
            horizontalSize = be16(desc + 0x18);
            if (horizontalSize <= 128 && horizontalSize > 0) horizontalSize *= 16;
            verticalSize = be16(desc + 0x1A);
            if (verticalSize <= 128 && verticalSize > 0) verticalSize *= 16;
        } else {
            profileIdc = desc[2];
            levelIdc = desc[3];
            frameMbsOnlyFlag = (desc[4] >> 7) & 1;
            videoSignalInfoFlag = (desc[4] >> 6) & 1;
            frameRateInfo = (desc[5] >> 4) & 0x0F;
            sarWidth = be16(desc + 6);
            sarHeight = be16(desc + 8);
            horizontalSize = be16(desc + 14);
            verticalSize = be16(desc + 16);
            cropLeft = desc[18];
            cropRight = desc[19];
            cropTop = desc[20];
            cropBottom = desc[21];
        }

        /* Write big-endian guest struct fields matching official CellPamfAvcInfo:
         * uint8_t  profileIdc;               // +0
         * uint8_t  levelIdc;                 // +1
         * uint8_t  frameMbsOnlyFlag;         // +2
         * uint8_t  videoSignalInfoFlag;      // +3
         * uint8_t  frameRateInfo;            // +4
         * uint8_t  aspectRatioIdc;           // +5
         * uint16_t sarWidth;                 // +6
         * uint16_t sarHeight;                // +8
         * uint16_t horizontalSize;           // +10
         * uint16_t verticalSize;             // +12
         * uint16_t frameCropLeftOffset;      // +14
         * uint16_t frameCropRightOffset;     // +16
         * uint16_t frameCropTopOffset;       // +18
         * uint16_t frameCropBottomOffset;    // +20
         */
        vm_write8(ea + 0, profileIdc);
        vm_write8(ea + 1, levelIdc);
        vm_write8(ea + 2, frameMbsOnlyFlag);
        vm_write8(ea + 3, videoSignalInfoFlag);
        vm_write8(ea + 4, frameRateInfo);
        vm_write8(ea + 5, 1); /* aspectRatioIdc */
        vm_write16(ea + 6, sarWidth);
        vm_write16(ea + 8, sarHeight);
        vm_write16(ea + 10, horizontalSize);
        vm_write16(ea + 12, verticalSize);
        vm_write16(ea + 14, cropLeft);
        vm_write16(ea + 16, cropRight);
        vm_write16(ea + 18, cropTop);
        vm_write16(ea + 20, cropBottom);

        printf("[cellPamf] AVC info: profile=%u level=%u %ux%u "
               "mbsOnly=%u cropBot=%u\n",
               profileIdc, levelIdc, horizontalSize, verticalSize,
               frameMbsOnlyFlag, cropBottom);
        break;
    }

    case CELL_PAMF_STREAM_TYPE_M2V: {
        u8 profileAndLevel = desc[2];
        u8 progressive = 1;
        u16 horizontalSize = be16(desc + 14);
        u16 verticalSize = be16(desc + 16);
        if (reader->streamDescSize == 48) {
            profileAndLevel = desc[0x10];
            horizontalSize = be16(desc + 0x18);
            if (horizontalSize <= 128 && horizontalSize > 0) horizontalSize *= 16;
            verticalSize = be16(desc + 0x1A);
            if (verticalSize <= 128 && verticalSize > 0) verticalSize *= 16;
        }
        vm_write8(ea + 0, profileAndLevel);
        vm_write8(ea + 1, progressive);
        vm_write8(ea + 2, 0); /* videoSignalInfoFlag */
        vm_write8(ea + 3, desc[5] >> 4); /* frameRateInfo */
        vm_write8(ea + 4, 1); /* aspectRatioIdc */
        vm_write16(ea + 6, 1); /* sarWidth */
        vm_write16(ea + 8, 1); /* sarHeight */
        vm_write16(ea + 10, horizontalSize);
        vm_write16(ea + 12, verticalSize);
        vm_write16(ea + 14, horizontalSize);
        vm_write16(ea + 16, verticalSize);
        printf("[cellPamf] M2V info: %ux%u\n", horizontalSize, verticalSize);
        break;
    }

    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS: {
        u32 srate = 48000;
        u8 ch = 2;
        if (reader->streamDescSize == 48) {
            if (desc[0x12] > 0) ch = desc[0x12];
        } else {
            ch = audio_channels(desc[10]);
            srate = audio_sample_rate(desc[12]);
        }
        vm_write32(ea + 0, srate);
        vm_write8(ea + 4, ch);
        printf("[cellPamf] ATRAC3+ info: %u ch, %u Hz\n", ch, srate);
        break;
    }

    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM: {
        u32 srate = 48000;
        u8 ch = 2;
        u8 bits = 16;
        if (reader->streamDescSize != 48) {
            ch = audio_channels(desc[10]);
            bits = lpcm_bits(desc[11]);
            srate = audio_sample_rate(desc[12]);
        }
        vm_write32(ea + 0, srate);
        vm_write8(ea + 4, ch);
        vm_write16(ea + 6, bits);
        printf("[cellPamf] LPCM info: %u ch, %u Hz, %u bits\n", ch, srate, bits);
        break;
    }

    case CELL_PAMF_STREAM_TYPE_AC3: {
        u32 srate = 48000;
        u8 ch = 2;
        if (reader->streamDescSize != 48) {
            ch = audio_channels(desc[10]);
            srate = audio_sample_rate(desc[12]);
        }
        vm_write32(ea + 0, srate);
        vm_write8(ea + 4, ch);
        printf("[cellPamf] AC3 info: %u ch, %u Hz\n", ch, srate);
        break;
    }

    default:
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    }

    return CELL_OK;
}

s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader)
{
    reader = pamf_host(reader);
    if (!reader)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    return reader->currentStream;
}

s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 streamIndex,
                                     void* esFilterId)
{
    (void)streamType; (void)streamIndex;
    if (!esFilterId)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    u32 ea = (u32)(uintptr_t)esFilterId;
    vm_write8(ea + 0, streamType);
    vm_write8(ea + 1, streamIndex);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Entry point navigation
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader, s32* numEp)
{
    reader = pamf_host(reader);
    if (!reader || !numEp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    vm_write32((u32)(uintptr_t)numEp, be32(base + PAMF_OFF_EP_COUNT));
    return CELL_OK;
}

s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep)
{
    reader = pamf_host(reader);
    if (!reader || !ep)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = (const u8*)reader->pamfAddr;
    u32 epCount = be32(base + PAMF_OFF_EP_COUNT);
    if (epIndex >= epCount)
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;

    /* EP table entries are at epOffset, each 8 bytes (pts + offset) */
    u32 epTableOff = be32(base + PAMF_OFF_EP_OFFSET);
    const u8* epData = base + epTableOff + epIndex * 8;
    vm_write32((u32)(uintptr_t)ep + 0, be32(epData));
    vm_write32((u32)(uintptr_t)ep + 4, be32(epData + 4));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Header and stream queries
 * -----------------------------------------------------------------------*/

s32 cellPamfGetHeaderSize(const void* pamfAddr, u64 fileSize, u64* headerSize)
{
    if (!pamfAddr || !headerSize || fileSize < 16)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = GUEST_PTR(pamfAddr, const u8*);
    u32 magic = be32(base + PAMF_OFF_MAGIC);
    if (magic != CELL_PAMF_MAGIC) {
        printf("[cellPamf] GetHeaderSize: invalid magic 0x%08X\n", magic);
        return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    }

    u32 hdrBlocks = be32(base + PAMF_OFF_DATA_OFFSET);
    u64 hSize = (u64)hdrBlocks * 2048ULL;
    if (hSize == 0) hSize = 2048ULL;

    vm_write64((u32)(uintptr_t)headerSize, hSize);
    printf("[cellPamf] GetHeaderSize(fileSize=%llu) -> headerSize=%llu\n",
           (unsigned long long)fileSize, (unsigned long long)hSize);
    return CELL_OK;
}

s32 cellPamfGetStreamOffsetAndSize(const void* pamfAddr, u64 fileSize, u64* streamOffset, u64* streamSize)
{
    if (!pamfAddr || !streamOffset || !streamSize || fileSize < 16)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = GUEST_PTR(pamfAddr, const u8*);
    u32 magic = be32(base + PAMF_OFF_MAGIC);
    if (magic != CELL_PAMF_MAGIC) {
        printf("[cellPamf] GetStreamOffsetAndSize: invalid magic 0x%08X\n", magic);
        return (s32)CELL_PAMF_ERROR_INVALID_HEADER;
    }

    u32 hdrBlocks = be32(base + PAMF_OFF_DATA_OFFSET);
    u32 dataBlocks = be32(base + PAMF_OFF_DATA_SIZE);
    u64 offset = (u64)hdrBlocks * 2048ULL;
    u64 size = (u64)dataBlocks * 2048ULL;

    vm_write64((u32)(uintptr_t)streamOffset, offset);
    vm_write64((u32)(uintptr_t)streamSize, size);
    printf("[cellPamf] GetStreamOffsetAndSize -> offset=%llu, size=%llu\n",
           (unsigned long long)offset, (unsigned long long)size);
    return CELL_OK;
}

s32 cellPamfVerify(const void* pamfAddr, u64 fileSize)
{
    if (!pamfAddr || fileSize < 16)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* base = GUEST_PTR(pamfAddr, const u8*);
    u32 magic = be32(base + PAMF_OFF_MAGIC);
    if (magic != CELL_PAMF_MAGIC)
        return (s32)CELL_PAMF_ERROR_INVALID_HEADER;

    return CELL_OK;
}

s32 cellPamfReaderSetStreamWithTypeAndIndex(CellPamfReader* reader, u8 streamType, u32 streamIndex)
{
    return cellPamfReaderSetStreamWithType(reader, streamType, streamIndex);
}

s32 cellPamfReaderGetStreamTypeAndChannel(CellPamfReader* reader, u8* pStreamType, u8* pCh)
{
    reader = pamf_host(reader);
    if (!reader || !pStreamType || !pCh)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u8* desc = get_stream_desc_raw(reader, reader->currentStream);
    if (!desc)
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;

    s32 sdkType = raw_type_to_sdk(desc[0]);
    if (sdkType < 0)
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;

    u8 channel = (reader->streamDescSize == 48) ? 0 : desc[1];
    vm_write8((u32)(uintptr_t)pStreamType, (u8)sdkType);
    vm_write8((u32)(uintptr_t)pCh, channel);
    return CELL_OK;
}
