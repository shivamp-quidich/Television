#ifndef _Tools_
#define _Tools_

#if defined(__APPLE__)
#include "VideoMasterHD/VideoMasterHD_Core.h"
#include "VideoMasterHD/VideoMasterHD_Benchmark.h"
#include "VideoMasterHD/VideoMasterHD_Sdi.h"
#include "VideoMasterHD/VideoMasterHD_Ip_ST2110_Board.h"
#include "VideoMasterHD/VideoMasterHD_Ip_ST2110_20.h"
#include "VideoMasterHD/VideoMasterHD_Ip_ST2110_30.h"
#include "VideoMasterHD/VideoMasterHD_Asi.h"
#include "VideoMasterHD/VideoMasterHD_PTP.h"
#include "VideoMasterHD/VideoMasterHD_SDP.h"
#include "VideoMasterHD/VideoMasterHD_String.h"
#include "VideoMasterHD_Audio/VideoMasterHD_Sdi_Audio.h"
#else
#include "VideoMasterHD_Core.h"
#include "VideoMasterHD_Benchmark.h"
#include "VideoMasterHD_Sdi.h"
#include "VideoMasterHD_Ip_ST2110_Board.h"
#include "VideoMasterHD_Ip_ST2110_20.h"
#include "VideoMasterHD_Ip_ST2110_30.h"
#include "VideoMasterHD_Asi.h"
#include "VideoMasterHD_PTP.h"
#include "VideoMasterHD_SDP.h"
#include "VideoMasterHD_String.h"
#include "VideoMasterHD_Sdi_Audio.h"
#endif

#include <utility>

#define CREATE_MASK(len, offset)    (((1 << (len)) - 1) << (offset))
#define GET_BITS(val, start, len)   (((val) & CREATE_MASK(len, start)) >> (start))
#define GET_BYTES(val, start, len)  GET_BITS(val, 8*(start), 8*(len))
#define GET_BYTE(val, start)        GET_BYTES(val, start, 1)

#define PRINT_ERROR(error_code, error_message, ...) \
{ \
    char pLastErrorMessage[VHD_MAX_ERROR_STRING_SIZE] = ""; \
    VHD_GetLastErrorMessage(pLastErrorMessage, VHD_MAX_ERROR_STRING_SIZE); \
    printf(error_message " Result = 0x%08X (%s)\nVHD_GetLastErrorMessage -->\n%s\n", ##__VA_ARGS__, error_code, VHD_ERRORCODE_ToPrettyString(VHD_ERRORCODE(error_code)), pLastErrorMessage); \
}

typedef struct _DV_DETECTED_SIGNAL_INFO
{
    ULONG Width;
    ULONG Height;
    ULONG RefreshRate;
    BOOL32 Interlaced;
    VHD_DV_CS CableColorSpace;
    VHD_DV_SAMPLING CableBitSampling;
} DV_DETECTED_SIGNAL_INFO;

void WaitForKey();
VHD_STREAMTYPE GetRxStreamType( int Idx_i);
VHD_STREAMTYPE GetTxStreamType( int Idx_i);
VHD_ASI_BITRATESOURCE GetBitRateSrc(int RxIdx_i);
void PrintChnType(HANDLE BoardHandle);
void PrintBoardInfo(int BoardIndex);
void WaitForChannelLocked(HANDLE BoardHandle, ULONG ChannelIdx_UL);
void WaitForGenlockRef(HANDLE BoardHandle);
void WaitForGenlockRefLock(HANDLE BoardHandle);
void PrintVideoStandardInfo(ULONG VideoStandard);
BOOL32 GetVideoCharacteristics(ULONG VideoStandard, int * pWidth, int * pHeight, BOOL32 * pInterlaced, BOOL32 * pIsHD);
BOOL32 IsKeyerAvailable(HANDLE BoardHandle);
BOOL32 IsHDMIKeyerAvailable(HANDLE BoardHandle);
void PrintInterfaceInfo(ULONG Interface);
int GetRawFrameHeight(ULONG VideoStandard);
BOOL32 SetNbChannels(ULONG BrdId, ULONG NbRx, ULONG NbTx);
BOOL32 Is4KInterface(ULONG Interface);
BOOL32 Is8KInterface(ULONG Interface);
BOOL32 SingleToQuadLinksInterface( ULONG RXStatus, ULONG *pInterface, ULONG *pVideoStandard);
BOOL32 SingleToQuadLinksVideoStandard( ULONG *pVideoStandard);
BOOL32 GetFrameST2022_6PacketNumber(ULONG VideoStandard, ULONG *pPacketNumber);
BOOL32 GetCarriedVideoStandard(ULONG VideoStandard);
void *PageAlignedAlloc(ULONG Size);
void PageAlignedFree(void *pBuffer);
ULONG GetAsiBufferSize(ULONG BitRate, ULONG BufferSizeInMs, VHD_ASI_TSPACKETTYPE PacketType);
void PrintChannelAvailability(const VHD_PCIE_BENCHMARK_MEASURE& BenchmarkMeasure);
void BenchmarkingCallback(VHD_PCIE_BENCHMARK_MEASURE BenchmarkMeasure);
void PrintBandwidthMeasurements(const VHD_PCIE_BANDWIDTH_MEASURE& BandwidthMeasure);
void BandwidthCallback(VHD_PCIE_BANDWIDTH_MEASURE BandwidthMeasure);
void PrintValidateBoardUsageProgression(ULONG ProgressionPercentage_UL);
int GetST2110RefreshRate(ULONG VideoStandard);
int GetST2110AudioSamplingRate(ULONG AudioSamplingRate);
int GetST2110AudioFormatNbOfBytes(ULONG AudioPayloadFormat);
VHD_CORE_BOARDPROPERTY GetPassiveLoopbackProperty(int ChannelIdx);
VHD_CORE_BOARDPROPERTY GetActiveLoopbackProperty(int ChannelIdx);
VHD_CORE_BOARDPROPERTY GetFirmwareLoopbackProperty(int ChannelIdx);
void SetLoopbackState(HANDLE BoardHandle, int ChannelIndex, BOOL32 State);
BOOL32 IsDvChannelType(VHD_CHANNELTYPE ChannelType_E);
BOOL32 IsHDMIChannelType(VHD_CHANNELTYPE ChannelType_E);

VHD_ERRORCODE DetectDVSignalInformations(HANDLE BoardHandle, ULONG ChanIdx, DV_DETECTED_SIGNAL_INFO* pSignalInfo, BOOL32 IsHDMI, ULONG *pPixelClock);
VHD_AUDIOSAMPLINGRATE GetSdiSamplingRateFromST2110(VHD_ST2110_30_SAMPLING_RATE SamplingRate);
VHD_CLOCKDIVISOR GetSdiClockDivisorFromST2110(VHD_ST2110_20_VIDEO_STANDARD VideoStandard);
VHD_VIDEOSTANDARD GetSdiVideoStandardFromST2110(VHD_ST2110_20_VIDEO_STANDARD VideoStandard);

#define VHD_RX_COUNT 4
#define VHD_TX_COUNT 4

extern const ULONG VHD_CORE_BP_RX_TYPE_MAP[VHD_RX_COUNT];

#endif //_Tools_
