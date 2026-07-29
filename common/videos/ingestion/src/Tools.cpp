#if !defined(__linux__) && !defined(__APPLE__)
#include <Windows.h>
#endif

#include "Tools.h"
#include <iostream>
#include <stdio.h>

#if defined(__linux__) || defined(__APPLE__)
#include "Keyboard.h"
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define PAUSE(param) usleep(param * 1000)
#define min std::min
#define max std::max
#else
#define PAUSE(param) Sleep(param)
#include <conio.h>
#define init_keyboard()
#define close_keyboard()
#endif

#define BIT_TO_BYTE(val) ((val) / 8)
#define MS_TO_S(val) ((val) / 1000.)
#define LIMIT_VALUE(val, lowerbound, upperbound)                               \
  (max((lowerbound), min((val), (upperbound))))

void WaitForKey() {
  do {
    _getch();
  } while (_kbhit());
}

void PrintChnType(HANDLE BoardHandle) {
  ULONG ChnType, NbOfChn;

  VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NB_RXCHANNELS, &NbOfChn);
  for (ULONG i = 0; i < NbOfChn; i++) {
    VHD_GetChannelProperty(BoardHandle, VHD_RX_CHANNEL, i, VHD_CORE_CP_TYPE,
                           &ChnType);
    printf("RX%d=%s / ", i,
           VHD_CHANNELTYPE_ToPrettyString(((VHD_CHANNELTYPE)ChnType)));
  }

  VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NB_TXCHANNELS, &NbOfChn);
  for (ULONG i = 0; i < NbOfChn; i++) {
    VHD_GetChannelProperty(BoardHandle, VHD_TX_CHANNEL, i, VHD_CORE_CP_TYPE,
                           &ChnType);
    printf("TX%d=%s / ", i,
           VHD_CHANNELTYPE_ToPrettyString(((VHD_CHANNELTYPE)ChnType)));
  }

  printf("\b\b\b   ");
}

VHD_STREAMTYPE GetRxStreamType(int Idx_i) {
  switch (Idx_i) {
  case 0:
    return VHD_ST_RX0;
  case 1:
    return VHD_ST_RX1;
  case 2:
    return VHD_ST_RX2;
  case 3:
    return VHD_ST_RX3;
  case 4:
    return VHD_ST_RX4;
  case 5:
    return VHD_ST_RX5;
  case 6:
    return VHD_ST_RX6;
  case 7:
    return VHD_ST_RX7;
  case 8:
    return VHD_ST_RX8;
  case 9:
    return VHD_ST_RX9;
  case 10:
    return VHD_ST_RX10;
  case 11:
    return VHD_ST_RX11;
  default:
    return NB_VHD_STREAMTYPES;
  }
}

VHD_STREAMTYPE GetTxStreamType(int Idx_i) {
  switch (Idx_i) {
  case 0:
    return VHD_ST_TX0;
  case 1:
    return VHD_ST_TX1;
  case 2:
    return VHD_ST_TX2;
  case 3:
    return VHD_ST_TX3;
  case 4:
    return VHD_ST_TX4;
  case 5:
    return VHD_ST_TX5;
  case 6:
    return VHD_ST_TX6;
  case 7:
    return VHD_ST_TX7;
  case 8:
    return VHD_ST_TX8;
  case 9:
    return VHD_ST_TX9;
  case 10:
    return VHD_ST_TX10;
  case 11:
    return VHD_ST_TX11;
  default:
    return NB_VHD_STREAMTYPES;
  }
}

VHD_ASI_BITRATESOURCE GetBitRateSrc(int RxIdx_i) {
  switch (RxIdx_i) {
  case 0:
    return VHD_ASI_BR_SRC_RX0;
  case 1:
    return VHD_ASI_BR_SRC_RX1;
  case 2:
    return VHD_ASI_BR_SRC_RX2;
  case 3:
    return VHD_ASI_BR_SRC_RX3;
  case 4:
    return VHD_ASI_BR_SRC_RX4;
  case 5:
    return VHD_ASI_BR_SRC_RX5;
  case 6:
    return VHD_ASI_BR_SRC_RX6;
  case 7:
    return VHD_ASI_BR_SRC_RX7;
  default:
    return NB_VHD_ASI_BITRATESOURCE;
  }
}

void PrintBoardInfo(int BoardIndex) {
  ULONG Result, BoardType, NbOfLane;
  ULONG SerialNumber_UL[4];
  ULONG BusType, FirmwareVersion, Firmware3Version, LowProfile, NbRxChannels,
      NbTxChannels, Firmware4Version, ProductVersion = 0;
  HANDLE BoardHandle = NULL;
  char pIdString_c[64];
  /* Open a handle on first DELTA board */
  Result = VHD_OpenBoardHandle(BoardIndex, &BoardHandle, NULL, 0);
  if (Result == VHDERR_NOERROR) {
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_FIRMWARE_VERSION,
                         &FirmwareVersion);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_BOARD_TYPE, &BoardType);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_SERIALNUMBER_PART1_LSW,
                         &SerialNumber_UL[0]);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_SERIALNUMBER_PART2,
                         &SerialNumber_UL[1]);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_SERIALNUMBER_PART3,
                         &SerialNumber_UL[2]);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_SERIALNUMBER_PART4_MSW,
                         &SerialNumber_UL[3]);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NBOF_LANE, &NbOfLane);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_LOWPROFILE, &LowProfile);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NB_RXCHANNELS, &NbRxChannels);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NB_TXCHANNELS, &NbTxChannels);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_PRODUCT_VERSION,
                         &ProductVersion);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_BUS_TYPE, &BusType);
    VHD_GetPCIeIdentificationString(BoardIndex, pIdString_c);

    printf("  Board %u :  [ %s ]\n", BoardIndex, VHD_GetBoardModel(BoardIndex));
    printf("    - PCIe Id string : %s\n", pIdString_c);
    printf("    - Driver %s\n", VHD_GetDriverStringVersion(BoardHandle));
    printf("    - Board fpga firmware v%02X (%02X-%02X-%02X)\n",
           FirmwareVersion & 0xFF, (FirmwareVersion >> 24) & 0xFF,
           (FirmwareVersion >> 16) & 0xFF, (FirmwareVersion >> 8) & 0xFF);
    if (BoardType == VHD_BOARDTYPE_3G || BoardType == VHD_BOARDTYPE_3GKEY ||
        (BoardType == VHD_BOARDTYPE_HD && NbTxChannels == 4)) {
      VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_FIRMWARE3_VERSION,
                           &Firmware3Version);
      printf("    - Board micro-controller firmware v%02X (%02X-%02X-%02X)\n",
             Firmware3Version & 0xFF, (Firmware3Version >> 24) & 0xFF,
             (Firmware3Version >> 16) & 0xFF, (Firmware3Version >> 8) & 0xFF);
    }
    if (BoardType == VHD_BOARDTYPE_IP) {
      VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_FIRMWARE4_VERSION,
                           &Firmware4Version);
      printf("    - Board microcode firmware v%02X (%02X-%02X-%02X)\n",
             Firmware4Version & 0xFF, (Firmware4Version >> 24) & 0xFF,
             (Firmware4Version >> 16) & 0xFF, (Firmware4Version >> 8) & 0xFF);
    }
    printf("    - Board serial# : 0x%08X%08X%08X%08X\n", SerialNumber_UL[3],
           SerialNumber_UL[2], SerialNumber_UL[1], SerialNumber_UL[0]);

    if (ProductVersion != 0)
      printf("    - Board product v%04X\n", ProductVersion);

    switch (BoardType) {
    case VHD_BOARDTYPE_HD:
      printf("    - %s type", VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_HD));
      break;
    case VHD_BOARDTYPE_MIXEDINTERFACE:
      printf("    - %s type",
             VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_MIXEDINTERFACE));
      break;
    case VHD_BOARDTYPE_3G:
      printf("    - %s type", VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_3G));
      break;
    case VHD_BOARDTYPE_3GKEY:
      printf("    - %s type",
             VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_3GKEY));
      break;
    case VHD_BOARDTYPE_ASI:
      printf("    - %s type", VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_ASI));
      break;
    case VHD_BOARDTYPE_HDMI20:
      printf("    - %s type",
             VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_HDMI20));
      break;
    case VHD_BOARDTYPE_IP:
      printf("    - %s type", VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_IP));
      break;
    case VHD_BOARDTYPE_FLEX_DP:
      printf("    - %s type",
             VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_FLEX_DP));
      break;
    case VHD_BOARDTYPE_FLEX_SDI:
      printf("    - %s type",
             VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_FLEX_SDI));
      break;
    case VHD_BOARDTYPE_FLEX_HMI:
      printf("    - %s type",
             VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_FLEX_HMI));
      break;
    case VHD_BOARDTYPE_12G:
      printf("    - %s type", VHD_BOARDTYPE_ToPrettyString(VHD_BOARDTYPE_12G));
      break;
    default:
      printf("    - Unknown board type");
      break;
    }

    printf(" on ");
    switch (BusType) {
    case VHD_BUSTYPE_PCI:
      printf("%s", VHD_BUSTYPE_ToPrettyString(VHD_BUSTYPE_PCI));
      break;
    case VHD_BUSTYPE_PCIe:
      printf("%s", VHD_BUSTYPE_ToPrettyString(VHD_BUSTYPE_PCIe));
      break;
    case VHD_BUSTYPE_PCIe_gen2:
      printf("%s", VHD_BUSTYPE_ToPrettyString(VHD_BUSTYPE_PCIe_gen2));
      break;
    case VHD_BUSTYPE_PCIe_gen3:
      printf("%s", VHD_BUSTYPE_ToPrettyString(VHD_BUSTYPE_PCIe_gen3));
      break;
    default:
      printf("Unknown bus type");
      break;
    }
    printf(" bus");

    if (NbOfLane)
      printf(" (%d lane%s)\n", NbOfLane, (NbOfLane > 1) ? "s" : "");
    else
      printf("\n");
    printf("    - %s\n", LowProfile ? "Low profile" : "Full height");
    printf("    - %d In / %d Out\n", NbRxChannels, NbTxChannels);

    printf("    - ");
    PrintChnType(BoardHandle);
    printf("\n");

    VHD_CloseBoardHandle(BoardHandle);
  } else
    PRINT_ERROR(Result, "ERROR : Cannot open DELTA board %u handle.",
                BoardIndex)
}

void WaitForChannelLocked(HANDLE BoardHandle, ULONG ChannelIdx_UL) {
  ULONG Status = 0;
  ULONG Result = VHDERR_NOERROR;

  printf("\nWaiting for channel locked, press ESC to abort...\n");

  do {
    if (_kbhit()) {
      _getch();
      break;
    }

    Result = VHD_GetChannelProperty(BoardHandle, VHD_RX_CHANNEL, ChannelIdx_UL,
                                    VHD_CORE_CP_STATUS, &Status);
    PAUSE(100);

    if (Result != VHDERR_NOERROR)
      continue;

  } while (Status & VHD_CORE_RXSTS_UNLOCKED);
}

void WaitForGenlockRef(HANDLE BoardHandle) {
  ULONG Status = 0;
  ULONG Result = VHDERR_NOERROR;

  printf("\nWaiting for genlock reference, press ESC to abort...\n");

  do {
    if (_kbhit()) {
      _getch();
      break;
    }

    Result =
        VHD_GetBoardProperty(BoardHandle, VHD_SDI_BP_GENLOCK_STATUS, &Status);
    PAUSE(100);

    if (Result != VHDERR_NOERROR)
      continue;

  } while ((Status & VHD_SDI_GNLKSTS_NOREF));
}

void WaitForGenlockRefLock(HANDLE BoardHandle) {
  ULONG Status = 0, PrevStatus = Status;
  ULONG Result = VHDERR_NOERROR;
  printf("\nWaiting for genlock reference, press ESC to abort...\n");
  do {
    PrevStatus = Status;
    if (_kbhit()) {
      _getch();
      break;
    }
    Result =
        VHD_GetBoardProperty(BoardHandle, VHD_SDI_BP_GENLOCK_STATUS, &Status);
    PAUSE(100);
    if (Result != VHDERR_NOERROR)
      continue;
    if (PrevStatus != Status && !(Status & VHD_SDI_GNLKSTS_NOREF)) {
      ULONG ClkDiv;
      VHD_GetBoardProperty(BoardHandle, VHD_SDI_BP_GENLOCK_CLOCK_DIV, &ClkDiv);
      ULONG GnlkVid;
      VHD_GetBoardProperty(BoardHandle, VHD_SDI_BP_GENLOCK_VIDEO_STANDARD,
                           &GnlkVid);
      ULONG Interface;
      VHD_GetChannelProperty(BoardHandle, VHD_RX_CHANNEL, 0,
                             VHD_SDI_CP_INTERFACE, &Interface);
      printf("Reference acquired: clock div: %s, video standard: %s\n",
             ClkDiv == VHD_CLOCKDIV_1001 ? "US" : "EU",
             VHD_VIDEOSTANDARD_ToPrettyString(VHD_VIDEOSTANDARD(GnlkVid)));
      VHD_SetBoardProperty(BoardHandle, VHD_SDI_BP_GENLOCK_CLOCK_DIV, ClkDiv);

      /* Adapt genlock video standard for 3GB interfaces */
      if ((Interface == VHD_INTERFACE_3G_B_DL_425_1) ||
          (Interface == VHD_INTERFACE_4X3G_B_DL_425_5)) {
        switch (GnlkVid) {
        case VHD_VIDEOSTD_S2048M_2048p_60Hz:
        case VHD_VIDEOSTD_S274M_1080p_60Hz:
        case VHD_VIDEOSTD_3840x2160p_60Hz:
        case VHD_VIDEOSTD_4096x2160p_60Hz:
          GnlkVid = VHD_VIDEOSTD_S274M_1080i_60Hz;
          break;
        case VHD_VIDEOSTD_S2048M_2048p_50Hz:
        case VHD_VIDEOSTD_S274M_1080p_50Hz:
        case VHD_VIDEOSTD_3840x2160p_50Hz:
        case VHD_VIDEOSTD_4096x2160p_50Hz:
          GnlkVid = VHD_VIDEOSTD_S274M_1080i_50Hz;
          break;
        default:
          break;
        }
      }

      VHD_SetBoardProperty(BoardHandle, VHD_SDI_BP_GENLOCK_VIDEO_STANDARD,
                           GnlkVid);
    }
  } while ((Status & VHD_SDI_GNLKSTS_NOREF) ||
           (Status & VHD_SDI_GNLKSTS_UNLOCKED));
}

void PrintVideoStandardInfo(ULONG VideoStandard) {
  printf("\nIncoming video standard : %s\n",
         VHD_VIDEOSTANDARD_ToPrettyString(VHD_VIDEOSTANDARD(VideoStandard)));
}

BOOL32 GetVideoCharacteristics(ULONG VideoStandard, int *pWidth, int *pHeight,
                               BOOL32 *pInterlaced, BOOL32 *pIsHD) {
  int Width = 0, Height = 0;
  BOOL32 Interlaced = FALSE, IsHD = FALSE;

  switch (VideoStandard) {
  case VHD_VIDEOSTD_S259M_NTSC:
    Width = 720;
    Height = 487;
    Interlaced = TRUE;
    IsHD = FALSE;
    break;
  case VHD_VIDEOSTD_S259M_NTSC_480:
    Width = 720;
    Height = 480;
    Interlaced = TRUE;
    IsHD = FALSE;
    break;
  case VHD_VIDEOSTD_S259M_PAL:
    Width = 720;
    Height = 576;
    Interlaced = TRUE;
    IsHD = FALSE;
    break;
  case VHD_VIDEOSTD_S296M_720p_24Hz:
  case VHD_VIDEOSTD_S296M_720p_25Hz:
  case VHD_VIDEOSTD_S296M_720p_30Hz:
  case VHD_VIDEOSTD_S296M_720p_50Hz:
  case VHD_VIDEOSTD_S296M_720p_60Hz:
    Width = 1280;
    Height = 720;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_S274M_1080i_50Hz:
  case VHD_VIDEOSTD_S274M_1080i_60Hz:
  case VHD_VIDEOSTD_S274M_1080psf_24Hz:
  case VHD_VIDEOSTD_S274M_1080psf_25Hz:
  case VHD_VIDEOSTD_S274M_1080psf_30Hz:
    Width = 1920;
    Height = 1080;
    Interlaced = TRUE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_S274M_1080p_25Hz:
  case VHD_VIDEOSTD_S274M_1080p_24Hz:
  case VHD_VIDEOSTD_S274M_1080p_30Hz:
  case VHD_VIDEOSTD_S274M_1080p_60Hz:
  case VHD_VIDEOSTD_S274M_1080p_50Hz:
    Width = 1920;
    Height = 1080;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_S2048M_2048psf_24Hz:
  case VHD_VIDEOSTD_S2048M_2048psf_25Hz:
  case VHD_VIDEOSTD_S2048M_2048psf_30Hz:
    Width = 2048;
    Height = 1080;
    Interlaced = TRUE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_S2048M_2048p_24Hz:
  case VHD_VIDEOSTD_S2048M_2048p_25Hz:
  case VHD_VIDEOSTD_S2048M_2048p_30Hz:
  case VHD_VIDEOSTD_S2048M_2048p_60Hz:
  case VHD_VIDEOSTD_S2048M_2048p_50Hz:
  case VHD_VIDEOSTD_S2048M_2048p_48Hz:
    Width = 2048;
    Height = 1080;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_4096x2160psf_24Hz:
  case VHD_VIDEOSTD_4096x2160psf_25Hz:
  case VHD_VIDEOSTD_4096x2160psf_30Hz:
    Width = 4096;
    Height = 2160;
    Interlaced = TRUE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_4096x2160p_24Hz:
  case VHD_VIDEOSTD_4096x2160p_25Hz:
  case VHD_VIDEOSTD_4096x2160p_30Hz:
  case VHD_VIDEOSTD_4096x2160p_60Hz:
  case VHD_VIDEOSTD_4096x2160p_50Hz:
  case VHD_VIDEOSTD_4096x2160p_48Hz:
    Width = 4096;
    Height = 2160;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_3840x2160psf_24Hz:
  case VHD_VIDEOSTD_3840x2160psf_25Hz:
  case VHD_VIDEOSTD_3840x2160psf_30Hz:
    Width = 3840;
    Height = 2160;
    Interlaced = TRUE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_3840x2160p_24Hz:
  case VHD_VIDEOSTD_3840x2160p_25Hz:
  case VHD_VIDEOSTD_3840x2160p_30Hz:
  case VHD_VIDEOSTD_3840x2160p_60Hz:
  case VHD_VIDEOSTD_3840x2160p_50Hz:
    Width = 3840;
    Height = 2160;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_7680x4320p_24Hz:
  case VHD_VIDEOSTD_7680x4320p_25Hz:
  case VHD_VIDEOSTD_7680x4320p_30Hz:
  case VHD_VIDEOSTD_7680x4320p_60Hz:
  case VHD_VIDEOSTD_7680x4320p_50Hz:
    Width = 7680;
    Height = 4320;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;
  case VHD_VIDEOSTD_8192x4320p_24Hz:
  case VHD_VIDEOSTD_8192x4320p_25Hz:
  case VHD_VIDEOSTD_8192x4320p_30Hz:
  case VHD_VIDEOSTD_8192x4320p_48Hz:
  case VHD_VIDEOSTD_8192x4320p_50Hz:
  case VHD_VIDEOSTD_8192x4320p_60Hz:
    Width = 8192;
    Height = 4320;
    Interlaced = FALSE;
    IsHD = TRUE;
    break;

  default:
    return FALSE;
  }

  if (pWidth)
    *pWidth = Width;
  if (pHeight)
    *pHeight = Height;
  if (pIsHD)
    *pIsHD = IsHD;
  if (pInterlaced)
    *pInterlaced = Interlaced;

  return TRUE;
}

BOOL32 IsKeyerAvailable(HANDLE BoardHandle) {
  BOOL32 KeyerSupported = FALSE;
  ULONG Result = VHD_GetBoardCapability(BoardHandle, VHD_KEYER_BOARD_CAP_KEYER,
                                        (ULONG *)&KeyerSupported);
  if (Result == VHDERR_NOERROR && KeyerSupported) {
    return TRUE;
  }

  return FALSE;
}

BOOL32 IsHDMIKeyerAvailable(HANDLE BoardHandle) {
  ULONG KeyerSupported = 0;
  ULONG Result = VHD_GetBoardCapability(BoardHandle, VHD_KEYER_BOARD_CAP_KEYER,
                                        (ULONG *)&KeyerSupported);
  if (Result == VHDERR_NOERROR) {
    return KeyerSupported == 2;
  }

  return FALSE;
}

void PrintInterfaceInfo(ULONG Interface) {
  printf("\nIncoming interface : %s\n",
         VHD_INTERFACE_ToString(VHD_INTERFACE(Interface)));
}

int GetRawFrameHeight(ULONG VideoStandard) {
  int RawFrameHeight = 0;

  switch (VideoStandard) {
  case VHD_VIDEOSTD_S259M_NTSC:
  case VHD_VIDEOSTD_S259M_NTSC_480:
    RawFrameHeight = 525;
    break;
  case VHD_VIDEOSTD_S259M_PAL:
    RawFrameHeight = 625;
    break;
  case VHD_VIDEOSTD_S296M_720p_50Hz:
  case VHD_VIDEOSTD_S296M_720p_60Hz:
  case VHD_VIDEOSTD_S296M_720p_24Hz:
  case VHD_VIDEOSTD_S296M_720p_25Hz:
  case VHD_VIDEOSTD_S296M_720p_30Hz:
    RawFrameHeight = 750;
    break;
  case VHD_VIDEOSTD_S274M_1080i_50Hz:
  case VHD_VIDEOSTD_S274M_1080i_60Hz:
  case VHD_VIDEOSTD_S274M_1080p_25Hz:
  case VHD_VIDEOSTD_S274M_1080p_30Hz:
  case VHD_VIDEOSTD_S274M_1080p_24Hz:
  case VHD_VIDEOSTD_S274M_1080p_60Hz:
  case VHD_VIDEOSTD_S274M_1080p_50Hz:
  case VHD_VIDEOSTD_S274M_1080psf_24Hz:
  case VHD_VIDEOSTD_S274M_1080psf_25Hz:
  case VHD_VIDEOSTD_S274M_1080psf_30Hz:
  case VHD_VIDEOSTD_S2048M_2048p_24Hz:
  case VHD_VIDEOSTD_S2048M_2048p_25Hz:
  case VHD_VIDEOSTD_S2048M_2048p_30Hz:
  case VHD_VIDEOSTD_S2048M_2048psf_24Hz:
  case VHD_VIDEOSTD_S2048M_2048psf_25Hz:
  case VHD_VIDEOSTD_S2048M_2048psf_30Hz:
  case VHD_VIDEOSTD_S2048M_2048p_60Hz:
  case VHD_VIDEOSTD_S2048M_2048p_50Hz:
  case VHD_VIDEOSTD_S2048M_2048p_48Hz:
    RawFrameHeight = 1125;
    break;
  default:
    RawFrameHeight = 0;
    break;
  }

  return RawFrameHeight;
}

BOOL32 SetNbChannels(ULONG BrdId, ULONG NbRx, ULONG NbTx) {
  BOOL32 Result = FALSE;
  ULONG VhdResult = VHDERR_NOERROR;
  ULONG NbRxOnBoard = 0, NbTxOnBoard = 0, NbChanOnBoard = 0;
  HANDLE BoardHandle = NULL;
  BOOL32 IsBiDir = FALSE;

  VhdResult = VHD_OpenBoardHandle(BrdId, &BoardHandle, NULL, 0);
  if (VhdResult == VHDERR_NOERROR) {
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NB_RXCHANNELS, &NbRxOnBoard);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_NB_TXCHANNELS, &NbTxOnBoard);
    VHD_GetBoardProperty(BoardHandle, VHD_CORE_BP_IS_BIDIR, (ULONG *)&IsBiDir);

    VHD_CloseBoardHandle(BoardHandle);

    NbChanOnBoard = NbRxOnBoard + NbTxOnBoard;

    if ((NbRxOnBoard >= NbRx) && (NbTxOnBoard >= NbTx))
      Result = TRUE;

    if (!Result) {
      if (IsBiDir) {
        if ((NbRx + NbTx) <= NbChanOnBoard) {
          printf("\nChanging board configuration... ");

          if (NbChanOnBoard == 4) {
            switch (NbRx) {
            case 0:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_04);
              break;
            case 1:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_13);
              break;
            case 2:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_22);
              break;
            case 3:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_31);
              break;
            case 4:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_40);
              break;
            }
          } else if (NbChanOnBoard == 8) {
            switch (NbRx) {
            case 0:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_08);
              break;
            case 1:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_17);
              break;
            case 2:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_26);
              break;
            case 3:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_35);
              break;
            case 4:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_44);
              break;
            case 5:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_53);
              break;
            case 6:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_62);
              break;
            case 7:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_71);
              break;
            case 8:
              VHD_SetBiDirCfg(BrdId, VHD_BIDIR_80);
              break;
            }
          }

          Result = TRUE;

          printf("Set configuration %d In / %d Out\n", NbRx,
                 NbChanOnBoard - NbRx);
        }
      }
    }
  }

  return Result;
}

BOOL32 Is4KInterface(ULONG Interface) {
  BOOL32 Result = FALSE;
  switch (Interface) {
  case VHD_INTERFACE_4XHD_QUADRANT:
  case VHD_INTERFACE_4X3G_A_QUADRANT:
  case VHD_INTERFACE_4X3G_B_DL_QUADRANT:
  case VHD_INTERFACE_2X3G_B_DS_425_3:
  case VHD_INTERFACE_4X3G_A_425_5:
  case VHD_INTERFACE_4X3G_B_DL_425_5:
  case VHD_INTERFACE_2X3G_B_DS_425_3_DUAL:
  case VHD_INTERFACE_4XHD_QUADRANT_DUAL:
  case VHD_INTERFACE_4X3G_A_QUADRANT_DUAL:
  case VHD_INTERFACE_4X3G_A_425_5_DUAL:
  case VHD_INTERFACE_4X3G_B_DL_QUADRANT_DUAL:
  case VHD_INTERFACE_4X3G_B_DL_425_5_DUAL:
    Result = TRUE;
    break;
  default:
    Result = FALSE;
    break;
  }

  return Result;
}

BOOL32 Is8KInterface(ULONG Interface) {
  BOOL32 Result = FALSE;
  switch (Interface) {
  case VHD_INTERFACE_4X12G_2082_10_QUADRANT:
  case VHD_INTERFACE_4X6G_2081_12:
  case VHD_INTERFACE_4X12G_2082_12:
    Result = TRUE;
    break;
  default:
    Result = FALSE;
    break;
  }

  return Result;
}

BOOL32 SingleToQuadLinksInterface(ULONG RXStatus, ULONG *pInterface,
                                  ULONG *pVideoStandard) {
  BOOL32 Result = TRUE;

  if (pInterface && pVideoStandard) {
    switch (*pVideoStandard) {
    case VHD_VIDEOSTD_S274M_1080p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_24Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_25Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_30Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080psf_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160psf_24Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080psf_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160psf_25Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080psf_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160psf_30Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_50Hz;
      if (RXStatus & VHD_SDI_RXSTS_LEVELB_3G)
        *pInterface = VHD_INTERFACE_4X3G_B_DL_QUADRANT;
      else
        *pInterface = VHD_INTERFACE_4X3G_A_QUADRANT;
      break;
    case VHD_VIDEOSTD_S274M_1080p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_60Hz;
      if (RXStatus & VHD_SDI_RXSTS_LEVELB_3G)
        *pInterface = VHD_INTERFACE_4X3G_B_DL_QUADRANT;
      else
        *pInterface = VHD_INTERFACE_4X3G_A_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_24Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_25Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_30Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048psf_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160psf_24Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048psf_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160psf_25Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048psf_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160psf_30Hz;
      *pInterface = VHD_INTERFACE_4XHD_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_48Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_48Hz;
      if (RXStatus & VHD_SDI_RXSTS_LEVELB_3G)
        *pInterface = VHD_INTERFACE_4X3G_B_DL_QUADRANT;
      else
        *pInterface = VHD_INTERFACE_4X3G_A_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_50Hz;
      if (RXStatus & VHD_SDI_RXSTS_LEVELB_3G)
        *pInterface = VHD_INTERFACE_4X3G_B_DL_QUADRANT;
      else
        *pInterface = VHD_INTERFACE_4X3G_A_QUADRANT;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_60Hz;
      if (RXStatus & VHD_SDI_RXSTS_LEVELB_3G)
        *pInterface = VHD_INTERFACE_4X3G_B_DL_QUADRANT;
      else
        *pInterface = VHD_INTERFACE_4X3G_A_QUADRANT;
      break;
    case VHD_VIDEOSTD_3840x2160p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_24Hz;
      *pInterface = VHD_INTERFACE_4X6G_2081_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_3840x2160p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_25Hz;
      *pInterface = VHD_INTERFACE_4X6G_2081_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_3840x2160p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_30Hz;
      *pInterface = VHD_INTERFACE_4X6G_2081_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_3840x2160p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_50Hz;
      *pInterface = VHD_INTERFACE_4X12G_2082_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_3840x2160p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_60Hz;
      *pInterface = VHD_INTERFACE_4X12G_2082_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_4096x2160p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_24Hz;
      *pInterface = VHD_INTERFACE_4X6G_2081_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_4096x2160p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_25Hz;
      *pInterface = VHD_INTERFACE_4X6G_2081_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_4096x2160p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_30Hz;
      *pInterface = VHD_INTERFACE_4X6G_2081_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_4096x2160p_48Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_48Hz;
      *pInterface = VHD_INTERFACE_4X12G_2082_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_4096x2160p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_50Hz;
      *pInterface = VHD_INTERFACE_4X12G_2082_10_QUADRANT;
      break;
    case VHD_VIDEOSTD_4096x2160p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_60Hz;
      *pInterface = VHD_INTERFACE_4X12G_2082_10_QUADRANT;
      break;
    default:
      Result = FALSE;
      break;
    }
  } else
    Result = FALSE;

  return Result;
}

BOOL32 SingleToQuadLinksVideoStandard(ULONG *pVideoStandard) {
  BOOL32 Result = TRUE;

  if (pVideoStandard) {
    switch (*pVideoStandard) {
    case VHD_VIDEOSTD_S274M_1080p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_24Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_25Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_30Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080psf_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160psf_24Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080psf_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160psf_25Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080psf_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160psf_30Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_50Hz;
      break;
    case VHD_VIDEOSTD_S274M_1080p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_3840x2160p_60Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_24Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_25Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_30Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048psf_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160psf_24Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048psf_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160psf_25Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048psf_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160psf_30Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_48Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_48Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_50Hz;
      break;
    case VHD_VIDEOSTD_S2048M_2048p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_4096x2160p_60Hz;
      break;
    case VHD_VIDEOSTD_3840x2160p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_24Hz;
      break;
    case VHD_VIDEOSTD_3840x2160p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_25Hz;
      break;
    case VHD_VIDEOSTD_3840x2160p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_30Hz;
      break;
    case VHD_VIDEOSTD_3840x2160p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_50Hz;
      break;
    case VHD_VIDEOSTD_3840x2160p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_7680x4320p_60Hz;
      break;
    case VHD_VIDEOSTD_4096x2160p_24Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_24Hz;
      break;
    case VHD_VIDEOSTD_4096x2160p_25Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_25Hz;
      break;
    case VHD_VIDEOSTD_4096x2160p_30Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_30Hz;
      break;
    case VHD_VIDEOSTD_4096x2160p_48Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_48Hz;
      break;
    case VHD_VIDEOSTD_4096x2160p_50Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_50Hz;
      break;
    case VHD_VIDEOSTD_4096x2160p_60Hz:
      *pVideoStandard = VHD_VIDEOSTD_8192x4320p_60Hz;
      break;
    default:
      Result = FALSE;
      break;
    }
  } else
    Result = FALSE;

  return Result;
}

BOOL32 GetFrameST2022_6PacketNumber(ULONG VideoStandard, ULONG *pPacketNumber) {
  ULONG PacketNumber = 0;

  switch (VideoStandard) {
  case VHD_VIDEOSTD_S259M_NTSC:
    PacketNumber = 819;
    break;
  case VHD_VIDEOSTD_S259M_PAL:
    PacketNumber = 982;
    break;
  case VHD_VIDEOSTD_S296M_720p_60Hz:
    PacketNumber = 2249;
    break;
  case VHD_VIDEOSTD_S296M_720p_50Hz:
    PacketNumber = 2699;
    break;
  case VHD_VIDEOSTD_S274M_1080i_50Hz:
    PacketNumber = 5397;
    break;
  case VHD_VIDEOSTD_S274M_1080i_60Hz:
    PacketNumber = 4497;
    break;
  case VHD_VIDEOSTD_S274M_1080psf_25Hz:
    PacketNumber = 5397;
    break;
  case VHD_VIDEOSTD_S274M_1080psf_30Hz:
    PacketNumber = 4497;
    break;
  case VHD_VIDEOSTD_S274M_1080p_25Hz:
    PacketNumber = 5397;
    break;
  case VHD_VIDEOSTD_S274M_1080p_30Hz:
    PacketNumber = 4497;
    break;
  case VHD_VIDEOSTD_S274M_1080p_50Hz:
    PacketNumber = 5397;
    break;
  case VHD_VIDEOSTD_S274M_1080p_60Hz:
    PacketNumber = 4497;
    break;
  case VHD_VIDEOSTD_S274M_1080p_24Hz:
    PacketNumber = 5621;
    break;
  case VHD_VIDEOSTD_S274M_1080psf_24Hz:
    PacketNumber = 5621;
    break;
  default:
    return FALSE;
  }

  if (pPacketNumber)
    *pPacketNumber = PacketNumber;

  return TRUE;
}

BOOL32 GetCarriedVideoStandard(ULONG VideoStandard) {
  ULONG ExpectedVideoStandard = NB_VHD_VIDEOSTANDARDS;

  switch (VideoStandard) {
  case VHD_VIDEOSTD_S274M_1080p_50Hz:
    ExpectedVideoStandard = VHD_VIDEOSTD_S274M_1080i_50Hz;
    break;
  case VHD_VIDEOSTD_S274M_1080p_60Hz:
    ExpectedVideoStandard = VHD_VIDEOSTD_S274M_1080i_60Hz;
    break;
  case VHD_VIDEOSTD_S2048M_2048p_48Hz:
    ExpectedVideoStandard = VHD_VIDEOSTD_S2048M_2048psf_24Hz;
    break;
  case VHD_VIDEOSTD_S2048M_2048p_50Hz:
    ExpectedVideoStandard = VHD_VIDEOSTD_S2048M_2048psf_25Hz;
    break;
  case VHD_VIDEOSTD_S2048M_2048p_60Hz:
    ExpectedVideoStandard = VHD_VIDEOSTD_S2048M_2048psf_30Hz;
    break;
  default:
    ExpectedVideoStandard = VideoStandard;
    break;
  }

  return ExpectedVideoStandard;
}

void *PageAlignedAlloc(ULONG Size) {
  void *pBuffer = NULL;
  int Error_i = 0;

#ifdef WIN32
  pBuffer = VirtualAlloc(NULL, Size, MEM_COMMIT | MEM_TOP_DOWN,
                         PAGE_EXECUTE_READWRITE);
#else
  Error_i = posix_memalign(&pBuffer, 4096, Size);
  if (Error_i != 0) {
    printf("posix_memalign failed to alloc %d bytes (error = %d)\n", Size,
           Error_i);
  }
#endif
  return pBuffer;
}

void PageAlignedFree(void *pBuffer) {
#ifdef WIN32
  /* If the dwFreeType parameter is MEM_RELEASE, dwSize parameter must be 0
   * (zero). The function frees the entire region that is reserved in the
   * initial allocation call to VirtualAlloc. */
  VirtualFree(pBuffer, 0, MEM_RELEASE);
#else
  free(pBuffer);
#endif
}

ULONG GetAsiBufferSize(ULONG BitRate, ULONG BufferSizeInMs,
                       VHD_ASI_TSPACKETTYPE PacketType) {
  ULONG BufferSize = 0;
  ULONG BufferSizeConstraint = ((PacketType == VHD_ASI_PKT_188) ? 188 : 204);

  BufferSizeConstraint *= 16;

  BufferSize =
      (ULONG)(BIT_TO_BYTE((LONGLONG)BitRate) * (MS_TO_S(BufferSizeInMs)));
  BufferSize =
      LIMIT_VALUE(BufferSize, (ULONG)0x800,
                  (ULONG)0x800000); // Buffer size must be between 2KB and 8MB
  BufferSize -= BufferSize % BufferSizeConstraint;

  return BufferSize;
}

void PrintChannelAvailability(
    const VHD_PCIE_BENCHMARK_MEASURE &BenchmarkMeasure) {
  BOOL32 IsRx =
      ((BenchmarkMeasure.Direction_E == VHD_UNIDIRECTIONAL_BENCHMARK_RX) ||
       (BenchmarkMeasure.Direction_E == VHD_FULLDUPLEX_BENCHMARK_RX));
  BOOL32 IsFullDuplex =
      ((BenchmarkMeasure.Direction_E == VHD_FULLDUPLEX_BENCHMARK_RX) ||
       (BenchmarkMeasure.Direction_E == VHD_FULLDUPLEX_BENCHMARK_TX));

  printf("\t%s%s\t%u%%\t\t%u%%\t\t%u%%\n", IsRx ? "RX" : "TX",
         (IsFullDuplex ? " Full Duplex" : "\t"),
         BenchmarkMeasure.MinimumPercentage_UL,
         BenchmarkMeasure.MaximumPercentage_UL,
         BenchmarkMeasure.AveragePercentage_UL);
}

void BenchmarkingCallback(VHD_PCIE_BENCHMARK_MEASURE BenchmarkMeasure) {
  PrintChannelAvailability(BenchmarkMeasure);
}

void PrintBandwidthMeasurements(
    const VHD_PCIE_BANDWIDTH_MEASURE &BandwidthMeasure) {
  BOOL32 IsRx =
      ((BandwidthMeasure.Direction_E == VHD_UNIDIRECTIONAL_BENCHMARK_RX) ||
       (BandwidthMeasure.Direction_E == VHD_FULLDUPLEX_BENCHMARK_RX));
  BOOL32 IsFullDuplex =
      ((BandwidthMeasure.Direction_E == VHD_FULLDUPLEX_BENCHMARK_RX) ||
       (BandwidthMeasure.Direction_E == VHD_FULLDUPLEX_BENCHMARK_TX));

  printf("\t%s%s\t%.2fMiB/s\t%.2fMiB/s\t%.2fMiB/s\n", IsRx ? "RX" : "TX",
         (IsFullDuplex ? " Full Duplex" : "\t"),
         BandwidthMeasure.MinimumBandwidth_ULL / 1024. / 1024.,
         BandwidthMeasure.MaximumBandwidth_ULL / 1024. / 1024.,
         BandwidthMeasure.AverageBandwidth_ULL / 1024. / 1024.);
}

void BandwidthCallback(VHD_PCIE_BANDWIDTH_MEASURE BandwidthMeasure) {
  PrintBandwidthMeasurements(BandwidthMeasure);
}

void PrintValidateBoardUsageProgression(ULONG ProgressionPercentage_UL) {
  printf("Validate progression: %d%%\r", ProgressionPercentage_UL);
}

int GetST2110RefreshRate(ULONG VideoStandard) {
  switch (VideoStandard) {
  case VHD_ST2110_20_VIDEOSTD_720x480i59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_720x487i59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_720x576i50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_1280x720p50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_1280x720p59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_1280x720p60:
    return 60;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080i50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080i59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080i60:
    return 60;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p23:
    return 23;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p24:
    return 24;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p25:
    return 25;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p29:
    return 29;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p30:
    return 30;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p60:
    return 60;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p23:
    return 23;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p24:
    return 24;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p25:
    return 25;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p29:
    return 29;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p30:
    return 30;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p47:
    return 47;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p48:
    return 48;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p60:
    return 60;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p23:
    return 23;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p24:
    return 24;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p25:
    return 25;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p29:
    return 29;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p30:
    return 30;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p60:
    return 60;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p25:
    return 25;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p23:
    return 23;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p24:
    return 24;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p29:
    return 29;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p30:
    return 30;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p47:
    return 47;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p48:
    return 48;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p50:
    return 50;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p59:
    return 59;
    break;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p60:
    return 60;
    break;
  default:
    return 30;
  }
}

int GetST2110AudioSamplingRate(ULONG AudioSamplingRate) {
  switch (AudioSamplingRate) {
  case VHD_ST2110_30_SAMPLINGRATE_48KHZ:
    return 48000;
    break;
  case VHD_ST2110_30_SAMPLINGRATE_96KHZ:
    return 96000;
    break;
  default:
    return 48000;
  }
}

int GetST2110AudioFormatNbOfBytes(ULONG AudioPayloadFormat) {
  switch (AudioPayloadFormat) {
  case VHD_ST2110_30_FORMAT_L16:
    return 2;
    break;
  case VHD_ST2110_30_FORMAT_L24:
    return 3;
    break;
  default:
    return 2;
  }
}

VHD_CORE_BOARDPROPERTY GetPassiveLoopbackProperty(int ChannelIdx) {
  switch (ChannelIdx) {
  case 0:
    return VHD_CORE_BP_BYPASS_RELAY_0;
  case 1:
    return VHD_CORE_BP_BYPASS_RELAY_1;
  case 2:
    return VHD_CORE_BP_BYPASS_RELAY_2;
  case 3:
    return VHD_CORE_BP_BYPASS_RELAY_3;
  default:
    return NB_VHD_CORE_BOARDPROPERTIES;
  }
}

VHD_CORE_BOARDPROPERTY GetActiveLoopbackProperty(int ChannelIdx) {
  switch (ChannelIdx) {
  case 0:
    return VHD_CORE_BP_ACTIVE_LOOPBACK_0;
  default:
    return NB_VHD_CORE_BOARDPROPERTIES;
  }
}

VHD_CORE_BOARDPROPERTY GetFirmwareLoopbackProperty(int ChannelIdx) {
  switch (ChannelIdx) {
  case 0:
    return VHD_CORE_BP_FIRMWARE_LOOPBACK_0;
  case 1:
    return VHD_CORE_BP_FIRMWARE_LOOPBACK_1;
  default:
    return NB_VHD_CORE_BOARDPROPERTIES;
  }
}

VHD_VIDEOSTANDARD
GetSdiVideoStandardFromST2110(VHD_ST2110_20_VIDEO_STANDARD VideoStandard) {
  switch (VideoStandard) {
  case VHD_ST2110_20_VIDEOSTD_720x480i59:
    return VHD_VIDEOSTD_S259M_NTSC_480;
  case VHD_ST2110_20_VIDEOSTD_720x487i59:
    return VHD_VIDEOSTD_S259M_NTSC_487;
  case VHD_ST2110_20_VIDEOSTD_720x576i50:
    return VHD_VIDEOSTD_S259M_PAL;
  case VHD_ST2110_20_VIDEOSTD_1280x720p50:
    return VHD_VIDEOSTD_S296M_720p_50Hz;
  case VHD_ST2110_20_VIDEOSTD_1280x720p59:
    return VHD_VIDEOSTD_S296M_720p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_1280x720p60:
    return VHD_VIDEOSTD_S296M_720p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080i50:
    return VHD_VIDEOSTD_S274M_1080i_50Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080i59:
    return VHD_VIDEOSTD_S274M_1080i_60Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080i60:
    return VHD_VIDEOSTD_S274M_1080i_60Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p23:
    return VHD_VIDEOSTD_S274M_1080p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p24:
    return VHD_VIDEOSTD_S274M_1080p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p25:
    return VHD_VIDEOSTD_S274M_1080p_25Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p29:
    return VHD_VIDEOSTD_S274M_1080p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p30:
    return VHD_VIDEOSTD_S274M_1080p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p50:
    return VHD_VIDEOSTD_S274M_1080p_50Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p59:
    return VHD_VIDEOSTD_S274M_1080p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_1920x1080p60:
    return VHD_VIDEOSTD_S274M_1080p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p23:
    return VHD_VIDEOSTD_S2048M_2048p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p24:
    return VHD_VIDEOSTD_S2048M_2048p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p25:
    return VHD_VIDEOSTD_S2048M_2048p_25Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p29:
    return VHD_VIDEOSTD_S2048M_2048p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p30:
    return VHD_VIDEOSTD_S2048M_2048p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p47:
    return VHD_VIDEOSTD_S2048M_2048p_48Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p48:
    return VHD_VIDEOSTD_S2048M_2048p_48Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p50:
    return VHD_VIDEOSTD_S2048M_2048p_50Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p59:
    return VHD_VIDEOSTD_S2048M_2048p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_2048x1080p60:
    return VHD_VIDEOSTD_S2048M_2048p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p23:
    return VHD_VIDEOSTD_3840x2160p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p24:
    return VHD_VIDEOSTD_3840x2160p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p25:
    return VHD_VIDEOSTD_3840x2160p_25Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p29:
    return VHD_VIDEOSTD_3840x2160p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p30:
    return VHD_VIDEOSTD_3840x2160p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p50:
    return VHD_VIDEOSTD_3840x2160p_50Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p59:
    return VHD_VIDEOSTD_3840x2160p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_3840x2160p60:
    return VHD_VIDEOSTD_3840x2160p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p25:
    return VHD_VIDEOSTD_4096x2160p_25Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p23:
    return VHD_VIDEOSTD_4096x2160p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p24:
    return VHD_VIDEOSTD_4096x2160p_24Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p29:
    return VHD_VIDEOSTD_4096x2160p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p30:
    return VHD_VIDEOSTD_4096x2160p_30Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p47:
    return VHD_VIDEOSTD_4096x2160p_48Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p48:
    return VHD_VIDEOSTD_4096x2160p_48Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p50:
    return VHD_VIDEOSTD_4096x2160p_50Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p59:
    return VHD_VIDEOSTD_4096x2160p_60Hz;
  case VHD_ST2110_20_VIDEOSTD_4096x2160p60:
    return VHD_VIDEOSTD_4096x2160p_60Hz;
  default:
    return NB_VHD_VIDEOSTANDARDS;
  }
}

VHD_CLOCKDIVISOR
GetSdiClockDivisorFromST2110(VHD_ST2110_20_VIDEO_STANDARD VideoStandard) {
  switch (VideoStandard) {
  case VHD_ST2110_20_VIDEOSTD_720x576i50:
  case VHD_ST2110_20_VIDEOSTD_1280x720p50:
  case VHD_ST2110_20_VIDEOSTD_1280x720p60:
  case VHD_ST2110_20_VIDEOSTD_1920x1080i50:
  case VHD_ST2110_20_VIDEOSTD_1920x1080i60:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p24:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p25:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p30:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p50:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p60:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p24:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p25:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p48:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p50:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p24:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p25:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p60:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p30:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p50:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p24:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p60:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p25:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p30:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p48:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p50:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p60:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p30:
    return VHD_CLOCKDIV_1;
  case VHD_ST2110_20_VIDEOSTD_720x480i59:
  case VHD_ST2110_20_VIDEOSTD_720x487i59:
  case VHD_ST2110_20_VIDEOSTD_1280x720p59:
  case VHD_ST2110_20_VIDEOSTD_1920x1080i59:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p23:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p29:
  case VHD_ST2110_20_VIDEOSTD_1920x1080p59:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p23:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p29:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p47:
  case VHD_ST2110_20_VIDEOSTD_2048x1080p59:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p23:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p29:
  case VHD_ST2110_20_VIDEOSTD_3840x2160p59:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p23:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p29:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p47:
  case VHD_ST2110_20_VIDEOSTD_4096x2160p59:
    return VHD_CLOCKDIV_1001;
  default:
    return NB_VHD_CLOCKDIVISORS;
  }
}

VHD_AUDIOSAMPLINGRATE
GetSdiSamplingRateFromST2110(VHD_ST2110_30_SAMPLING_RATE SamplingRate) {
  switch (SamplingRate) {
  case VHD_ST2110_30_SAMPLINGRATE_48KHZ:
    return VHD_ASR_48000;
  default:
    return NB_VHD_AUDIOSAMPLINGRATE;
  }
}

void SetLoopbackState(HANDLE BoardHandle, int ChannelIndex, BOOL32 State) {
  ULONG HasPassiveLoopback = FALSE;
  ULONG HasActiveLoopback = FALSE;
  ULONG HasFirmwareLoopback = FALSE;

  VHD_GetBoardCapability(BoardHandle, VHD_CORE_BOARD_CAP_PASSIVE_LOOPBACK,
                         &HasPassiveLoopback);
  VHD_GetBoardCapability(BoardHandle, VHD_CORE_BOARD_CAP_ACTIVE_LOOPBACK,
                         &HasActiveLoopback);
  VHD_GetBoardCapability(BoardHandle, VHD_CORE_BOARD_CAP_FIRMWARE_LOOPBACK,
                         &HasFirmwareLoopback);

  if (HasFirmwareLoopback && GetFirmwareLoopbackProperty(ChannelIndex) !=
                                 NB_VHD_CORE_BOARDPROPERTIES) {
    VHD_SetBoardProperty(BoardHandle, GetFirmwareLoopbackProperty(ChannelIndex),
                         State);
    return;
  }

  if (HasActiveLoopback &&
      GetActiveLoopbackProperty(ChannelIndex) != NB_VHD_CORE_BOARDPROPERTIES) {
    VHD_SetBoardProperty(BoardHandle, GetActiveLoopbackProperty(ChannelIndex),
                         State);
    return;
  }

  if (HasPassiveLoopback &&
      GetPassiveLoopbackProperty(ChannelIndex) != NB_VHD_CORE_BOARDPROPERTIES)
    VHD_SetBoardProperty(BoardHandle, GetPassiveLoopbackProperty(ChannelIndex),
                         State);
}

BOOL32 IsDvChannelType(VHD_CHANNELTYPE ChannelType_E) {
  if (IsHDMIChannelType(ChannelType_E))
    return TRUE;

  switch (ChannelType_E) {
  case VHD_CHNTYPE_DVI:
  case VHD_CHNTYPE_DISPLAYPORT:
    return TRUE;
  default:
    return FALSE;
  }
}

BOOL32 IsHDMIChannelType(VHD_CHANNELTYPE ChannelType_E) {
  switch (ChannelType_E) {
  case VHD_CHNTYPE_HDMI_TMDS:
  case VHD_CHNTYPE_HDMI_FRL3:
  case VHD_CHNTYPE_HDMI_FRL4:
  case VHD_CHNTYPE_HDMI_FRL5:
  case VHD_CHNTYPE_HDMI_FRL6:
    return TRUE;
  default:
    return FALSE;
  }
}

VHD_ERRORCODE DetectDVSignalInformations(HANDLE BoardHandle, ULONG ChanIdx,
                                         DV_DETECTED_SIGNAL_INFO *pSignalInfo,
                                         BOOL32 IsHDMI, ULONG *pPixelClock) {
  VHD_ERRORCODE Result = VHDERR_NOERROR;
  if (pSignalInfo == NULL)
    return VHDERR_INVALIDPOINTER;

  Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
      BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_ACTIVE_WIDTH,
      &pSignalInfo->Width);
  if (Result != VHDERR_NOERROR)
    return Result;
  Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
      BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_ACTIVE_HEIGHT,
      &pSignalInfo->Height);
  if (Result != VHDERR_NOERROR)
    return Result;
  Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
      BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_REFRESH_RATE,
      &pSignalInfo->RefreshRate);
  if (Result != VHDERR_NOERROR)
    return Result;
  Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
      BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_INTERLACED,
      (ULONG *)&pSignalInfo->Interlaced);
  if (Result != VHDERR_NOERROR)
    return Result;
  Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
      BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_CABLE_COLOR_SPACE,
      (ULONG *)&pSignalInfo->CableColorSpace);
  if (Result != VHDERR_NOERROR)
    return Result;
  Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
      BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_CABLE_BIT_SAMPLING,
      (ULONG *)&pSignalInfo->CableBitSampling);
  if (Result != VHDERR_NOERROR)
    return Result;
  if (IsHDMI && pPixelClock) {
    Result = (VHD_ERRORCODE)VHD_GetChannelProperty(
        BoardHandle, VHD_RX_CHANNEL, ChanIdx, VHD_DV_CP_PIXEL_CLOCK,
        pPixelClock);
    if (Result != VHDERR_NOERROR)
      return Result;
  }

  return Result;
}

const ULONG VHD_CORE_BP_RX_TYPE_MAP[VHD_RX_COUNT] = {
    VHD_CORE_BP_RX0_TYPE, VHD_CORE_BP_RX1_TYPE, VHD_CORE_BP_RX2_TYPE,
    VHD_CORE_BP_RX3_TYPE};
