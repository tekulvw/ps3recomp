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
#define CELL_PAMF_ERROR_STREAM_NOT_FOUND     0x80610501
#define CELL_PAMF_ERROR_INVALID_HEADER       0x80610502
#define CELL_PAMF_ERROR_INVALID_ARG          0x80610503
#define CELL_PAMF_ERROR_UNKNOWN_TYPE         0x80610504
#define CELL_PAMF_ERROR_UNSUPPORTED_VERSION  0x80610505
#define CELL_PAMF_ERROR_EP_NOT_FOUND         0x80610507
#define CELL_PAMF_ERROR_NOT_AVAILABLE        0x80610508
#define CELL_PAMF_ATTRIBUTE_VERIFY_ON        1
#define CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER   2
#define CELL_PAMF_STREAM_TYPE_VIDEO          20
#define CELL_PAMF_STREAM_TYPE_AUDIO          21

/* ---------------------------------------------------------------------------
 * Stream types
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_STREAM_TYPE_AVC            0   /* H.264/AVC video */
#define CELL_PAMF_STREAM_TYPE_M2V            1   /* MPEG-2 video */
#define CELL_PAMF_STREAM_TYPE_ATRAC3PLUS     2   /* ATRAC3plus audio */
#define CELL_PAMF_STREAM_TYPE_PAMF_LPCM      3   /* LPCM audio */
#define CELL_PAMF_STREAM_TYPE_AC3            4   /* AC3 audio */
#define CELL_PAMF_STREAM_TYPE_USER_DATA      5   /* User data */

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
#define CELL_PAMF_VERSION       0x30303431   /* ASCII "0041" */

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

/* Guest ABI bytes, not host-native pointers or integers. Copyable as 128 bytes.
 * The backing PAMF header must remain resident for the reader's lifetime. */
typedef struct CellPamfReader { u8 guest[128]; } CellPamfReader;

typedef struct CellPamfAvcInfo {
    u8 profileIdc, levelIdc, frameMbsOnlyFlag, videoSignalInfoFlag;
    u8 frameRateInfo, aspectRatioIdc;
    u16 sarWidth, sarHeight, horizontalSize, verticalSize;
    u16 frameCropLeftOffset, frameCropRightOffset, frameCropTopOffset, frameCropBottomOffset;
    u8 videoFormat, videoFullRangeFlag, colourPrimaries, transferCharacteristics;
    u8 matrixCoefficients, entropyCodingModeFlag, deblockingFilterFlag;
    u8 minNumSlicePerPictureIdc, nfwIdc, maxMeanBitrate;
} CellPamfAvcInfo;

typedef struct CellPamfM2vInfo {
    u8 profileAndLevelIndication, progressiveSequence, videoSignalInfoFlag, frameRateInfo;
    u8 aspectRatioIdc;
    u16 sarWidth, sarHeight, horizontalSize, verticalSize;
    u16 horizontalSizeValue, verticalSizeValue;
    u8 videoFormat, videoFullRangeFlag, colourPrimaries, transferCharacteristics, matrixCoefficients;
} CellPamfM2vInfo;

typedef struct CellPamfAtrac3plusInfo {
    u32 samplingFrequency;
    u8  numberOfChannels;
} CellPamfAtrac3plusInfo;

typedef struct CellPamfLpcmInfo {
    u32 samplingFrequency;
    u8  numberOfChannels;
    u16 bitsPerSample;
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
                              u64 pamfSize, u32 attribute);
s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, u64* startTime);
s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, u64* endTime);
u32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader);

/* Stream queries */
s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader);
s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType);
s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u32 streamIndex);
s32 cellPamfReaderSetStreamWithTypeAndIndex(CellPamfReader* reader, u8 streamType, u8 streamIndex);
s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u32 channel);
s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u32 streamIndex);
s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize);
s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader);

/* Stream type queries */
s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 streamIndex,
                                     void* esFilterId);

/* Entry point navigation */
u32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader);
s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PAMF_H */
