/*
 * ps3recomp - cellPamf HLE
 *
 * PAMF (PlayStation AV Media File) container parser. PAMF files contain
 * multiplexed H.264/MPEG2 video and AAC/ATRAC3+ audio streams used
 * for PS3 game cutscenes and FMVs.
 */

#ifndef PS3RECOMP_CELL_PAMF_H
#define PS3RECOMP_CELL_PAMF_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_ERROR_INVALID_ARG          0x80610301
#define CELL_PAMF_ERROR_INVALID_HEADER       0x80610302
#define CELL_PAMF_ERROR_STREAM_NOT_FOUND     0x80610303
#define CELL_PAMF_ERROR_EP_NOT_FOUND         0x80610304
#define CELL_PAMF_ERROR_NOT_AVAILABLE        0x80610305

/* ---------------------------------------------------------------------------
 * Stream types
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_STREAM_TYPE_AVC            0   /* H.264/AVC video */
#define CELL_PAMF_STREAM_TYPE_M2V            1   /* MPEG-2 video */
#define CELL_PAMF_STREAM_TYPE_ATRAC3PLUS     2   /* ATRAC3plus audio */
#define CELL_PAMF_STREAM_TYPE_PAMF_LPCM      3   /* LPCM audio */
#define CELL_PAMF_STREAM_TYPE_AC3            4   /* AC3 audio */
#define CELL_PAMF_STREAM_TYPE_USER_DATA      5   /* User data */
#define CELL_PAMF_STREAM_TYPE_VIDEO          20  /* Any video stream (AVC/M2V) */
#define CELL_PAMF_STREAM_TYPE_AUDIO          21  /* Any audio stream (ATRAC3+/LPCM/AC3) */

/* Codec type constants */
#define CELL_PAMF_CODEC_TYPE_AVC             0x1B
#define CELL_PAMF_CODEC_TYPE_M2V             0x02
#define CELL_PAMF_CODEC_TYPE_ATRAC3PLUS      0xDC
#define CELL_PAMF_CODEC_TYPE_LPCM            0x80
#define CELL_PAMF_CODEC_TYPE_AC3             0x81

/* ---------------------------------------------------------------------------
 * PAMF header magic
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_MAGIC         0x50414D46   /* "PAMF" */
#define CELL_PAMF_VERSION       0x00040100   /* v4.1.0 */

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

typedef struct CellPamfReader {
    void* pamfAddr;       /* base address of PAMF data */
    u32   pamfSize;       /* total size */
    u32   dataOffset;     /* offset to mux data */
    u32   dataSize;       /* mux data size */
    u32   streamTableOffset; /* offset to stream descriptor table */
    u8    numStreams;      /* number of streams */
    u8    currentStream;  /* current stream index */
    u8    streamDescSize; /* stream descriptor stride (24 or 48) */
    u8    reserved;
    u32   currentEP;      /* current entry point */
} CellPamfReader;

typedef struct CellPamfAvcInfo {
    u8   profileIdc;
    u8   levelIdc;
    u8   frameMbsOnlyFlag;
    u8   videoSignalInfoFlag;
    u8   frameRateInfo;
    u8   aspectRatioIdc;
    u16  sarWidth;
    u16  sarHeight;
    u16  horizontalSize;
    u16  verticalSize;
    u16  frameCropLeftOffset;
    u16  frameCropRightOffset;
    u16  frameCropTopOffset;
    u16  frameCropBottomOffset;
    u8   videoFormat;
    u8   videoFullRangeFlag;
    u8   colourPrimaries;
    u8   transferCharacteristics;
    u8   matrixCoefficients;
    u8   entropyCodingModeFlag;
    u8   deblockingFilterFlag;
    u8   minNumSlicePerPictureIdc;
    u8   nfwIdc;
    u8   maxMeanBitrate;
} CellPamfAvcInfo;

typedef struct CellPamfM2vInfo {
    u8   profileAndLevelIndication;
    u8   progressiveSequence;
    u8   videoSignalInfoFlag;
    u8   frameRateInfo;
    u8   aspectRatioIdc;
    u8   reserved;
    u16  sarWidth;
    u16  sarHeight;
    u16  horizontalSize;
    u16  verticalSize;
    u16  horizontalSizeValue;
    u16  verticalSizeValue;
    u8   videoFormat;
    u8   videoFullRangeFlag;
    u8   colourPrimaries;
    u8   transferCharacteristics;
    u8   matrixCoefficients;
    u8   pad;
} CellPamfM2vInfo;

typedef struct CellPamfAtrac3plusInfo {
    u32 samplingFrequency;
    u8  numberOfChannels;
} CellPamfAtrac3plusInfo;

typedef struct CellPamfLpcmInfo {
    u32 samplingFrequency;
    u8  numberOfChannels;
    u8  bitsPerSample;
} CellPamfLpcmInfo;

typedef struct CellPamfAc3Info {
    u32 samplingFrequency;
    u8  numberOfChannels;
} CellPamfAc3Info;

typedef struct CellPamfEp {
    u32 pts;              /* presentation timestamp (90kHz) */
    u32 rpnOffset;        /* byte offset from data start */
} CellPamfEp;

typedef struct CellPamfStreamInfo {
    u8  streamType;
    u8  streamIndex;
    u8  streamId;
    u8  reserved;
} CellPamfStreamInfo;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

/* Reader lifecycle */
s32 cellPamfReaderInitialize(CellPamfReader* reader, void* pamfAddr,
                              u32 pamfSize, u32 attribute);
s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, u64* startTime);
s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, u64* endTime);
u32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader);

/* Stream queries */
s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader);
s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType);
s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u32 streamIndex);
s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u32 channel);
s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u32 streamIndex);
s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize);
s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader);

/* Stream type queries */
s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 streamIndex,
                                     void* esFilterId);

/* Entry point navigation */
s32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader, s32* numEp);
s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep);

/* Header and stream queries */
s32 cellPamfGetHeaderSize(const void* pamfAddr, u64 fileSize, u64* headerSize);
s32 cellPamfGetStreamOffsetAndSize(const void* pamfAddr, u64 fileSize, u64* streamOffset, u64* streamSize);
s32 cellPamfVerify(const void* pamfAddr, u64 fileSize);
s32 cellPamfReaderSetStreamWithTypeAndIndex(CellPamfReader* reader, u8 streamType, u32 streamIndex);
s32 cellPamfReaderGetStreamTypeAndChannel(CellPamfReader* reader, u8* pStreamType, u8* pCh);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PAMF_H */
