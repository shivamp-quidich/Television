#include "deltacast_capture.cuh"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>

// Deltacast SDK headers
#include "VideoMasterHD_Core.h"
#include "VideoMasterHD_ApplicationBuffers.h"
#include "Tools.h"

#include "cuda.h"
#include "cuda_runtime_api.h"
#include "cuda_runtime.h"
#include <opencv2/cudaimgproc.hpp>
#include <sstream> // added for logging message construction

#define NBOF_APP_BUFFER 2  // Set 2 for lower latency
#define RDMA_SIZE_ALIGNMENT 64 * 1024

#ifndef ROUND_TO_x
#define ROUND_TO_x(Size, x) (((ULONG)(Size) + x - 1) & ~(x - 1))
#endif

// Forward declare the reception thread function as friend
class DeltacastCapture;
static void ReceptionThreadFunc(DeltacastCapture* parent, HANDLE streamRxHandle, HANDLE* pSlotRxHandle);

// Reception thread function
static void ReceptionThreadFunc(DeltacastCapture* parent, HANDLE streamRxHandle, HANDLE* pSlotRxHandle)
{
    ULONG result = 0;
    ULONG slotsRxCount = 0;
    ULONG slotsRxDropped = 0;
    ULONG bufferSize = 0;
    HANDLE filledSlotHandle;
    uint64_t slotRxCnt = 0;
    BYTE* pBuffer = nullptr;

    using steady_clock = std::chrono::steady_clock;
    auto last_stats_time = steady_clock::now();
    uint64_t window_frames = 0;
    uint64_t window_timeouts = 0;
    ULONG last_slots_dropped = 0;
    
    while (!parent->shouldStopThread)
    {
        result = VHD_WaitSlotFilled(streamRxHandle, &filledSlotHandle, 1);
        
        if (result == VHDERR_NOERROR)
        {
            result = VHD_GetSlotBuffer(filledSlotHandle, VHD_SDI_BT_VIDEO, &pBuffer, &bufferSize);
            
            if (result == VHDERR_NOERROR)
            {
                std::lock_guard<std::mutex> lock(parent->frameMutex);
                
                // If there's a previous slot that hasn't been re-queued, do it now
                if (parent->currentSlotHandle)
                {
                    VHD_QueueInSlot(parent->currentSlotHandle);
                }
                
                parent->d_currentBuffer = pBuffer;
                parent->currentBufferSize = bufferSize;
                parent->frameAvailable = true;
                parent->currentSlotHandle = filledSlotHandle;
                parent->currentFrameId.fetch_add(1);  // <-- INCREMENT FRAME ID
            }
            else
            {
                // std::cerr << "ERROR: Cannot get slot RX buffer. Result = " << std::hex << result << std::endl;
                {
                    std::ostringstream oss;
                    oss << "ERROR: Cannot get slot RX buffer. Result = " << std::hex << result;
                    DeltacastCapture::getLogger()->error(oss.str());
                }
                VHD_QueueInSlot(filledSlotHandle);
            }
            
            slotRxCnt++;
            window_frames++;
        }
        else if (result == VHDERR_TIMEOUT)
        {
            window_timeouts++;
        }
        else if (result != VHDERR_TIMEOUT)
        {
            // std::cerr << "ERROR: Cannot lock slot on RX stream. Result = " << std::hex << result << std::endl;
            {
                std::ostringstream oss;
                oss << "ERROR: Cannot lock slot on RX stream. Result = " << std::hex << result;
                DeltacastCapture::getLogger()->error(oss.str());
            }
            break;
        }

        // Periodic RX stats (1 Hz): confirm whether frames are actually arriving.
        auto now = steady_clock::now();
        const double elapsed_s = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_stats_time).count();
        if (elapsed_s >= 1.0)
        {
            ULONG dropped = 0;
            ULONG count = 0;
            ULONG filling = 0;
            if (VHD_GetStreamProperty(streamRxHandle, VHD_CORE_SP_SLOTS_COUNT, &count) != VHDERR_NOERROR)
            {
                count = 0;
            }
            if (VHD_GetStreamProperty(streamRxHandle, VHD_CORE_SP_SLOTS_DROPPED, &dropped) != VHDERR_NOERROR)
            {
                dropped = 0;
            }
            if (VHD_GetStreamProperty(streamRxHandle, VHD_CORE_SP_BUFFERQUEUE_FILLING, &filling) != VHDERR_NOERROR)
            {
                filling = 0;
            }

            const ULONG dropped_delta = (dropped >= last_slots_dropped) ? (dropped - last_slots_dropped) : 0;
            last_slots_dropped = dropped;
            const double fps = elapsed_s > 0.0 ? (static_cast<double>(window_frames) / elapsed_s) : 0.0;

            slotsRxCount = count;
            slotsRxDropped = dropped;

            DeltacastCapture::getLogger()->info(
                "[Deltacast RX] {:.1f} fps (frames={}, timeouts={}), dropped_total={} (+{}), slots_count={}, filling={}",
                fps,
                window_frames,
                window_timeouts,
                dropped,
                dropped_delta,
                count,
                filling);

            last_stats_time = now;
            window_frames = 0;
            window_timeouts = 0;
        }
    }
    
    // Clean up any remaining slot
    {
        std::lock_guard<std::mutex> lock(parent->frameMutex);
        if (parent->currentSlotHandle)
        {
            VHD_QueueInSlot(parent->currentSlotHandle);
            parent->currentSlotHandle = nullptr;
        }
    }
}

DeltacastCapture::DeltacastCapture() 
{
    applicationBuffers.resize(NBOF_APP_BUFFER);
    for (auto& slotBuffers : applicationBuffers)
    {
        slotBuffers.resize(NB_VHD_SDI_BUFFERTYPE, nullptr);
    }
    slotRxHandles.resize(NBOF_APP_BUFFER);
    
    // Initialize CUDA but use the primary context
    cuInit(0);
    
    // Get the primary context for device 0
    CUdevice device;
    CUcontext context;
    cuDeviceGet(&device, 0);
    cuDevicePrimaryCtxRetain(&context, device);
    cuCtxSetCurrent(context);
}

DeltacastCapture::~DeltacastCapture()
{
    stopCapture();
    
    if (latestTextureId)
    {
        glDeleteTextures(1, &latestTextureId);
        latestTextureId = 0;
    }
    
    // Stop TX stream if running
    if (streamTxHandle && outputInitialized)
    {
        VHD_StopStream(streamTxHandle);
    }
    
    // Free RX application buffers
    for (auto& bufferSet : applicationBuffers)
    {
        for (auto& bufferPtr : bufferSet)
        {
            auto buffer = static_cast<VHD_APPLICATION_BUFFER_DESCRIPTOR*>(bufferPtr);
            if (buffer && buffer->pBuffer)
            {
                if (buffer->RDMAEnabled)
                {
                    cudaFree(buffer->pBuffer);
                }
                else
                {
                    PageAlignedFree(buffer->pBuffer);
                }
                delete buffer;
            }
        }
    }
    
    // Free TX application buffers
    for (auto& bufferSet : txApplicationBuffers)
    {
        for (auto& bufferPtr : bufferSet)
        {
            auto buffer = static_cast<VHD_APPLICATION_BUFFER_DESCRIPTOR*>(bufferPtr);
            if (buffer && buffer->pBuffer)
            {
                PageAlignedFree(buffer->pBuffer);
                delete buffer;
            }
        }
    }
    
    if (streamRxHandle)
    {
        VHD_CloseStreamHandle(streamRxHandle);
        streamRxHandle = nullptr;
    }
    
    if (streamTxHandle)
    {
        VHD_CloseStreamHandle(streamTxHandle);
        streamTxHandle = nullptr;
    }
    
    if (boardHandle)
    {
        VHD_CloseBoardHandle(boardHandle);
        boardHandle = nullptr;
    }
    
    // Release the primary context
    CUdevice device;
    cuDeviceGet(&device, 0);
    cuDevicePrimaryCtxRelease(device);
}

bool DeltacastCapture::initialize(int deviceIndex)
{
    ULONG result, dllVersion, nbBoards;
    
    // Query VideoMasterHD information
    result = VHD_GetApiInfo(&dllVersion, &nbBoards);
    if (result != VHDERR_NOERROR || nbBoards == 0)
    {
        // std::cerr << "No Deltacast board detected" << std::endl;
        getLogger()->error(std::string("No Deltacast board detected"));
        return false;
    }
    
    // Open board handle
    result = VHD_OpenBoardHandle(deviceIndex, &boardHandle, nullptr, 0);
    if (result != VHDERR_NOERROR)
    {
        // std::cerr << "Failed to open board handle" << std::endl;
        getLogger()->error(std::string("Failed to open board handle"));
        return false;
    }
    
    // Check channel type
    ULONG chnTypeRx;
    VHD_GetBoardProperty(boardHandle, VHD_CORE_BP_RX_TYPE_MAP[0], &chnTypeRx);
    if (chnTypeRx != VHD_CHNTYPE_12GSDI)
    {
        // std::cerr << "Channel type is not 12G-SDI" << std::endl;
        getLogger()->error(std::string("Channel type is not 12G-SDI"));
        VHD_CloseBoardHandle(boardHandle);
        boardHandle = nullptr;
        return false;
    }

    // Try to use BlackBurst as genlock source
    bool blackburstAvailable = false;
    
    // Enable blackburst detection
    result = VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_BLACKBURST0_DETECTION_ENABLE, TRUE);
    if (result == VHDERR_NOERROR)
    {
        // std::cout << "BlackBurst detection enabled" << std::endl;
        getLogger()->info(std::string("BlackBurst detection enabled"));
        
        // Wait longer for detection to stabilize
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        
        // Auto-detect the video standard from BlackBurst
        ULONG detectedStandard = 0;
        ULONG detectedClockDiv = 0;
        
        result = VHD_GetBoardProperty(boardHandle, VHD_SDI_BP_BB0_STANDARD, &detectedStandard);
        if (result == VHDERR_NOERROR && detectedStandard != 0)
        {
            // std::cout << "BlackBurst auto-detected standard: 0x" << std::hex << detectedStandard << std::dec << std::endl;
            {
                std::ostringstream oss;
                oss << "BlackBurst auto-detected standard: 0x" << std::hex << detectedStandard << std::dec;
                getLogger()->info(oss.str());
            }
            
            result = VHD_GetBoardProperty(boardHandle, VHD_SDI_BP_BB0_CLOCK_DIV, &detectedClockDiv);
            if (result == VHDERR_NOERROR)
            {
                // std::cout << "BlackBurst auto-detected clock divisor: " << detectedClockDiv << std::endl;
                {
                    std::ostringstream oss;
                    oss << "BlackBurst auto-detected clock divisor: " << detectedClockDiv;
                    getLogger()->info(oss.str());
                }
            }
            
            // Use detected values instead of hardcoded ones
            result = VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_SOURCE, VHD_GENLOCK_BB0);
            if (result == VHDERR_NOERROR)
            {
                // Use auto-detected values
                VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_VIDEO_STANDARD, detectedStandard);
                VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_CLOCK_DIV, detectedClockDiv);
                
                // Wait longer for genlock to stabilize (2 seconds)
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                
                // Check genlock status
                ULONG genlockStatus;
                VHD_GetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_STATUS, &genlockStatus);
                
                // std::cout << "Genlock status after 2s: 0x" << std::hex << genlockStatus << std::dec << std::endl;
                {
                    std::ostringstream oss;
                    oss << "Genlock status after 2s: 0x" << std::hex << genlockStatus << std::dec;
                    getLogger()->info(oss.str());
                }
                
                if (!(genlockStatus & VHD_SDI_GNLKSTS_UNLOCKED))
                {
                    // std::cout << "Successfully locked to BlackBurst reference signal" << std::endl;
                    getLogger()->info(std::string("Successfully locked to BlackBurst reference signal"));
                    blackburstAvailable = true;
                }
                else
                {
                    // std::cout << "BlackBurst genlock still unlocked after 2s. Status flags:" << std::endl;
                    getLogger()->warn(std::string("BlackBurst genlock still unlocked after 2s. Status flags:"));
                    if (genlockStatus & 0x1) getLogger()->warn(std::string("  - Genlock missing"));
                    if (genlockStatus & 0x2) getLogger()->warn(std::string("  - Genlock unlocked"));
                    if (genlockStatus & 0x4) getLogger()->warn(std::string("  - Genlock locked"));
                }
            }
        }
        else
        {
            // std::cout << "BlackBurst signal not auto-detected. Manual configuration may be needed." << std::endl;
            getLogger()->info(std::string("BlackBurst signal not auto-detected. Manual configuration may be needed."));
        }
    }
    else
    {
        // std::cerr << "Failed to enable BlackBurst detection. Error: 0x" 
        //           << std::hex << result << std::dec << std::endl;
        {
            std::ostringstream oss;
            oss << "Failed to enable BlackBurst detection. Error: 0x" << std::hex << result << std::dec;
            getLogger()->error(oss.str());
        }
    }
    
    // Fallback to RX0 if BlackBurst not available
    if (!blackburstAvailable)
    {
        // std::cout << "External BlackBurst reference signal not available, falling back to RX0 genlock" << std::endl;
        getLogger()->info(std::string("External BlackBurst reference signal not available, falling back to RX0 genlock"));
        
        // Disable BlackBurst detection to avoid conflicts with LTC
        VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_BLACKBURST0_DETECTION_ENABLE, FALSE);
        
        // Set genlock to RX0
        result = VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_SOURCE, VHD_GENLOCK_RX0);
        if (result != VHDERR_NOERROR)
        {
            // std::cerr << "Failed to set genlock source to RX0. Error: 0x" 
            //           << std::hex << result << std::dec << std::endl;
            {
                std::ostringstream oss;
                oss << "Failed to set genlock source to RX0. Error: 0x" << std::hex << result << std::dec;
                getLogger()->error(oss.str());
            }
        }
        else
        {
            VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_VIDEO_STANDARD, VHD_VIDEOSTD_S274M_1080p_50Hz);
            VHD_SetBoardProperty(boardHandle, VHD_SDI_BP_GENLOCK_CLOCK_DIV, VHD_CLOCKDIV_1);
            // std::cout << "Genlock set to RX0 input" << std::endl;
            getLogger()->info(std::string("Genlock set to RX0 input"));
        }
    }
    
    // Wait for channel locked
    ULONG status;
    int timeout = 50; // 5 seconds timeout
    do {
        VHD_GetBoardProperty(boardHandle, VHD_CORE_BP_RX0_STATUS, &status);
        if (!(status & VHD_CORE_RXSTS_UNLOCKED))  // Check if NOT unlocked (i.e., locked)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (--timeout > 0);
    
    if (status & VHD_CORE_RXSTS_UNLOCKED)  // Check if still unlocked after timeout
    {
        // std::cerr << "No signal detected on RX0" << std::endl;
        getLogger()->error(std::string("No signal detected on RX0"));
        VHD_CloseBoardHandle(boardHandle);
        boardHandle = nullptr;
        return false;
    }
    
    // std::cout << "RX0 channel locked" << std::endl;
    getLogger()->info(std::string("RX0 channel locked"));
    
    // Create stream handle
    result = VHD_OpenStreamHandle(boardHandle, VHD_ST_RX0, VHD_SDI_STPROC_DISJOINED_VIDEO,
                                  nullptr, &streamRxHandle, nullptr);
    if (result != VHDERR_NOERROR)
    {
        // std::cerr << "Failed to open stream handle" << std::endl;
        getLogger()->error(std::string("Failed to open stream handle"));
        VHD_CloseBoardHandle(boardHandle);
        boardHandle = nullptr;
        return false;
    }
    
    // Get video properties - convert to ULONG
    ULONG videoStd, iface;
    VHD_GetStreamProperty(streamRxHandle, VHD_SDI_SP_VIDEO_STANDARD, &videoStd);
    VHD_GetStreamProperty(streamRxHandle, VHD_SDI_SP_INTERFACE, &iface);
    videoStandard = videoStd;
    interface = iface;
    
    // Get video dimensions based on video standard
    switch (videoStandard)
    {
        case VHD_VIDEOSTD_S274M_1080p_25Hz:
        case VHD_VIDEOSTD_S274M_1080p_24Hz:
        case VHD_VIDEOSTD_S274M_1080p_30Hz:
        case VHD_VIDEOSTD_S274M_1080p_60Hz:
        case VHD_VIDEOSTD_S274M_1080p_50Hz:
            width = 1920;
            height = 1080;
            break;
        case VHD_VIDEOSTD_3840x2160p_24Hz:
        case VHD_VIDEOSTD_3840x2160p_25Hz:
        case VHD_VIDEOSTD_3840x2160p_30Hz:
        case VHD_VIDEOSTD_3840x2160p_60Hz:
        case VHD_VIDEOSTD_3840x2160p_50Hz:
            width = 3840;
            height = 2160;
            break;
        default:
            width = 1920;
            height = 1080;
            break;
    }

    frameWidth = width;
    frameHeight = height;
    
    // std::cout << "Detected video: " << width << "x" << height 
    //           << " (standard: 0x" << std::hex << videoStandard << std::dec 
    //           << ", interface: 0x" << std::hex << interface << std::dec << ")" << std::endl;
    {
        std::ostringstream oss;
        oss << "Detected video: " << width << "x" << height
            << " (standard: 0x" << std::hex << videoStandard << std::dec
            << ", interface: 0x" << std::hex << interface << std::dec << ")";
        getLogger()->info(oss.str());
    }
    
    // Configure stream
    VHD_SetStreamProperty(streamRxHandle, VHD_SDI_SP_VIDEO_STANDARD, videoStandard);
    VHD_SetStreamProperty(streamRxHandle, VHD_SDI_SP_INTERFACE, interface);
    // Match Deltacast samples: no internal preload when using application buffers.
    VHD_SetStreamProperty(streamRxHandle, VHD_CORE_SP_BUFFERQUEUE_PRELOAD, 0);
    
    // Initialize application buffers
    result = VHD_InitApplicationBuffers(streamRxHandle);
    if (result != VHDERR_NOERROR)
    {
        // std::cerr << "Failed to initialize application buffers" << std::endl;
        getLogger()->error(std::string("Failed to initialize application buffers"));
        return false;
    }
    
    // Ensure storage is indexed by BufferTypeIdx.
    applicationBuffers.resize(NBOF_APP_BUFFER);
    for (auto& slotBuffers : applicationBuffers)
    {
        slotBuffers.assign(NB_VHD_SDI_BUFFERTYPE, nullptr);
    }

    // Allocate buffers
    for (int bufferTypeIdx = 0; bufferTypeIdx < NB_VHD_SDI_BUFFERTYPE; bufferTypeIdx++)
    {
        ULONG bufferSize;
        VHD_GetApplicationBuffersSize(streamRxHandle, bufferTypeIdx, &bufferSize);
        
        if (bufferSize > 0)
        {
            for (int slotIdx = 0; slotIdx < NBOF_APP_BUFFER; slotIdx++)
            {
                auto buffer = new VHD_APPLICATION_BUFFER_DESCRIPTOR;
                buffer->Size = sizeof(VHD_APPLICATION_BUFFER_DESCRIPTOR);
                buffer->pBuffer = nullptr;
                buffer->RDMAEnabled = false;
                
                if (bufferTypeIdx == VHD_SDI_BT_VIDEO)
                {
                    // Allocate GPU buffer for video
                    const ULONG alignedSize = ROUND_TO_x(bufferSize, RDMA_SIZE_ALIGNMENT);
                    cudaError_t err = cudaMalloc((void**)&buffer->pBuffer, alignedSize);
                    if (err != cudaSuccess)
                    {
                        // std::cerr << "Failed to allocate GPU buffer" << std::endl;
                        getLogger()->error(std::string("Failed to allocate GPU buffer"));
                        delete buffer;
                        return false;
                    }
                    
                    // Set sync memops attribute
                    unsigned int flag = 1;
                    CUresult result = cuPointerSetAttribute(&flag, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                                           (CUdeviceptr)buffer->pBuffer);
                    if (result != CUDA_SUCCESS)
                    {
                        // std::cerr << "Failed to set CUDA pointer attribute" << std::endl;
                        getLogger()->error(std::string("Failed to set CUDA pointer attribute"));
                        cudaFree(buffer->pBuffer);
                        delete buffer;
                        return false;
                    }
                    
                    buffer->RDMAEnabled = true;
                }
                else
                {
                    // Allocate CPU buffer for non-video
                    buffer->pBuffer = static_cast<BYTE*>(PageAlignedAlloc(bufferSize));
                    buffer->RDMAEnabled = false;
                }

                applicationBuffers[slotIdx][bufferTypeIdx] = buffer;
            }
        }
    }
    
    // Create slots
    for (int slotIdx = 0; slotIdx < NBOF_APP_BUFFER; slotIdx++)
    {
        // IMPORTANT: VHD_CreateSlotEx expects an array indexed by BufferTypeIdx
        // (size NB_VHD_SDI_BUFFERTYPE), not a compacted list.
        std::vector<VHD_APPLICATION_BUFFER_DESCRIPTOR> descriptors(NB_VHD_SDI_BUFFERTYPE);
        for (int bufferTypeIdx = 0; bufferTypeIdx < NB_VHD_SDI_BUFFERTYPE; ++bufferTypeIdx)
        {
            auto* bufferDescPtr = static_cast<VHD_APPLICATION_BUFFER_DESCRIPTOR*>(applicationBuffers[slotIdx][bufferTypeIdx]);
            if (bufferDescPtr)
            {
                descriptors[bufferTypeIdx] = *bufferDescPtr;
            }
        }

        ULONG create_res = VHD_CreateSlotEx(streamRxHandle, descriptors.data(), &slotRxHandles[slotIdx]);
        if (create_res != VHDERR_NOERROR)
        {
            std::ostringstream oss;
            oss << "Failed to create RX slot " << slotIdx << ". Error: 0x" << std::hex << create_res << std::dec;
            getLogger()->error(oss.str());
            return false;
        }

        ULONG q_res = VHD_QueueInSlot(slotRxHandles[slotIdx]);
        if (q_res != VHDERR_NOERROR)
        {
            std::ostringstream oss;
            oss << "Failed to queue RX slot " << slotIdx << ". Error: 0x" << std::hex << q_res << std::dec;
            getLogger()->error(oss.str());
            return false;
        }
    }
    
    return true;
}

bool DeltacastCapture::startCapture()
{
    if (!streamRxHandle)
    {
        // std::cerr << "Stream not initialized" << std::endl;
        getLogger()->error(std::string("Stream not initialized"));
        return false;
    }
    
    ULONG result = VHD_StartStream(streamRxHandle);
    if (result != VHDERR_NOERROR)
    {
        // std::cerr << "Failed to start stream" << std::endl;
        getLogger()->error(std::string("Failed to start stream"));
        return false;
    }
    
    capturing = true;
    shouldStopThread = false;
    
    // Start reception thread
    receptionThread = new std::thread(ReceptionThreadFunc, this, streamRxHandle, slotRxHandles.data());
    
    // std::cout << "Capture started" << std::endl;
    getLogger()->info(std::string("Capture started"));
    return true;
}

void DeltacastCapture::stopCapture()
{
    if (capturing && streamRxHandle)
    {
        shouldStopThread = true;
        
        if (receptionThread)
        {
            static_cast<std::thread*>(receptionThread)->join();
            delete static_cast<std::thread*>(receptionThread);
            receptionThread = nullptr;
        }
        
        VHD_StopStream(streamRxHandle);
        capturing = false;
        
        // std::cout << "Capture stopped" << std::endl;
        getLogger()->info(std::string("Capture stopped"));
    }
}

bool DeltacastCapture::getGPUFrame(unsigned char** d_buffer, size_t& bufferSize)
{
    std::lock_guard<std::mutex> lock(frameMutex);
    
    if (!frameAvailable || !d_currentBuffer)
    {
        return false;
    }
    
    *d_buffer = d_currentBuffer;
    bufferSize = currentBufferSize;
    
    return true;
}

int DeltacastCapture::getNumOutputs() const
{
    // Current implementation uses a single TX stream (see initializeOutput()).
    // If/when multiple TX channels are supported, this should be updated to query
    // the board capabilities and honor the requested output index.
    return 1;
}

bool DeltacastCapture::sendFrameToDeltacastTx(const void* uyvy_data, size_t data_size, int width, int height)
{
    if (!outputInitialized || !streamTxHandle)
    {
        // std::cerr << "TX output not initialized" << std::endl;
        getLogger()->error(std::string("TX output not initialized"));
        return false;
    }

    // Enhanced static state
    static int nextSlotIdx = 0;
    static std::array<HANDLE, NBOF_APP_BUFFER> activeSlots = {nullptr};
    static std::array<cudaEvent_t, NBOF_APP_BUFFER> slotEvents = {nullptr};
    static cudaStream_t txStream = nullptr;
    static bool cudaInit = false;
    static int frameCount = 0;
    static int dropCount = 0;
    static auto lastStatsTime = std::chrono::high_resolution_clock::now();
    static ULONG expectedBufferSize = 0;

    if (!cudaInit)
    {
        cudaStreamCreateWithFlags(&txStream, cudaStreamNonBlocking);
        for (int i = 0; i < NBOF_APP_BUFFER; ++i) {
            cudaEventCreateWithFlags(&slotEvents[i], cudaEventDisableTiming);
        }
        
        // Get the actual buffer size required by hardware
        ULONG bufferSize = 0;
        VHD_GetApplicationBuffersSize(streamTxHandle, VHD_SDI_BT_VIDEO, &bufferSize);
        expectedBufferSize = bufferSize;
        // std::cout << "[TX Init] Hardware expects buffer size: " << expectedBufferSize << " bytes (UYVY)" << std::endl;
        {
            std::ostringstream oss;
            oss << "[TX Init] Hardware expects buffer size: " << expectedBufferSize << " bytes (UYVY)";
            getLogger()->info(oss.str());
        }
        
        cudaInit = true;
    }

    frameCount++;

    // Process ALL completed slots immediately (change from while to aggressive polling)
    HANDLE sentSlotHandle = nullptr;
    while (VHD_WaitSlotSent(streamTxHandle, &sentSlotHandle, 0) == VHDERR_NOERROR) {
        for (int i = 0; i < NBOF_APP_BUFFER; ++i) {
            if (activeSlots[i] == sentSlotHandle) {
                activeSlots[i] = nullptr;
                break;
            }
        }
    }

    // Find available slot - if none available, DROP this frame
    int slotIdx = -1;
    for (int i = 0; i < NBOF_APP_BUFFER; ++i) {
        if (activeSlots[i] == nullptr) {
            slotIdx = i;
            break;
        }
    }

    if (slotIdx == -1) {
        dropCount++;
        // Drop frame instead of waiting - critical for low latency
        return false;
    }

    // Get buffer for the slot
    BYTE* pBuffer = nullptr;
    ULONG bufferSize = 0;
    ULONG result = VHD_GetSlotBuffer(slotTxHandles[slotIdx], VHD_SDI_BT_VIDEO, &pBuffer, &bufferSize);
    if (result != VHDERR_NOERROR || !pBuffer) {
        // std::cerr << "Failed to get TX slot buffer" << std::endl;
        getLogger()->error(std::string("Failed to get TX slot buffer"));
        return false;
    }

    // Calculate expected UYVY size (2 bytes per pixel)
    size_t expectedDataSize = width * height * 2;
    
    if (data_size != expectedDataSize) {
        // std::cerr << "UYVY data size mismatch. Expected: " << expectedDataSize 
        //           << ", Got: " << data_size << std::endl;
        {
            std::ostringstream oss;
            oss << "UYVY data size mismatch. Expected: " << expectedDataSize << ", Got: " << data_size;
            getLogger()->error(oss.str());
        }
        return false;
    }

    // Check if buffer size matches
    if (expectedDataSize > bufferSize) {
        // std::cerr << "[TX] Buffer too small - Data: " << expectedDataSize 
        //           << ", Hardware buffer: " << bufferSize << std::endl;
        {
            std::ostringstream oss;
            oss << "[TX] Buffer too small - Data: " << expectedDataSize << ", Hardware buffer: " << bufferSize;
            getLogger()->error(oss.str());
        }
        return false;
    }

    // Copy UYVY data directly to the buffer (no conversion needed)
    cudaError_t err = cudaMemcpyAsync(pBuffer, uyvy_data, expectedDataSize, cudaMemcpyDeviceToDevice, txStream);
    if (err != cudaSuccess) {
        // std::cerr << "Failed to copy UYVY data to TX buffer: " << cudaGetErrorString(err) << std::endl;
        {
            std::ostringstream oss;
            oss << "Failed to copy UYVY data to TX buffer: " << cudaGetErrorString(err);
            getLogger()->error(oss.str());
        }
        return false;
    }

    // Record completion event
    cudaEventRecord(slotEvents[slotIdx], txStream);
    
    
    // Queue the slot - hardware will output UYVY directly
    result = VHD_QueueOutSlot(slotTxHandles[slotIdx]);
    if (result != VHDERR_NOERROR) {
        // std::cerr << "Failed to queue TX slot. Error: 0x" << std::hex << result << std::dec << std::endl;
        {
            std::ostringstream oss;
            oss << "Failed to queue TX slot. Error: 0x" << std::hex << result << std::dec;
            getLogger()->error(oss.str());
        }
        return false;
    }

    activeSlots[slotIdx] = slotTxHandles[slotIdx];

    // CRITICAL: Wait for CUDA copy to complete before queueing
    cudaEventSynchronize(slotEvents[slotIdx]);  // Add this line

    // Print statistics every second
    auto now = std::chrono::high_resolution_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStatsTime).count() >= 1) {
        // std::cout << "[TX Stats] Frames: " << frameCount << ", Drops: " << dropCount 
        //           << " (" << (dropCount * 100.0 / frameCount) << "%)" << std::endl;
        {
            std::ostringstream oss;
            oss << "[TX Stats] Frames: " << frameCount << ", Drops: " << dropCount
                << " (" << (dropCount * 100.0 / frameCount) << "%)";
            getLogger()->info(oss.str());
        }
        frameCount = 0;
        dropCount = 0;
        lastStatsTime = now;
    }

    return true;
}

// Also update the initialization to better handle 50Hz
bool DeltacastCapture::initializeOutput(int deviceIndex)
{
    ULONG result = VHD_OpenStreamHandle(boardHandle, VHD_ST_TX1, VHD_SDI_STPROC_DISJOINED_VIDEO,
                                       nullptr, &streamTxHandle, nullptr);
    if (result != VHDERR_NOERROR)
    {
        // std::cerr << "Failed to open TX stream" << std::endl;
        getLogger()->error(std::string("Failed to open TX stream"));
        return false;
    }

    // Configure TX stream with same properties as RX
    VHD_SetStreamProperty(streamTxHandle, VHD_SDI_SP_VIDEO_STANDARD, videoStandard);
    VHD_SetStreamProperty(streamTxHandle, VHD_SDI_SP_INTERFACE, interface);
    
    // Enable TX genlock
    VHD_SetStreamProperty(streamTxHandle, VHD_SDI_SP_TX_GENLOCK, TRUE);

    // Set buffer packing to UYVY (YUV 4:2:2 8-bit)
    // std::cout << "Setting TX buffer format to UYVY (YUV 4:2:2)" << std::endl;
    getLogger()->info(std::string("Setting TX buffer format to UYVY (YUV 4:2:2)"));
    result = VHD_SetStreamProperty(streamTxHandle, VHD_CORE_SP_BUFFER_PACKING, VHD_BUFPACK_VIDEO_YUV422_8);
    if (result != VHDERR_NOERROR) {
        // std::cerr << "Failed to set UYVY buffer packing. Error: 0x" << std::hex << result << std::dec << std::endl;
        {
            std::ostringstream oss;
            oss << "Failed to set UYVY buffer packing. Error: 0x" << std::hex << result << std::dec;
            getLogger()->error(oss.str());
        }
        return false;
    }

    // Set buffer queue properties
    VHD_SetStreamProperty(streamTxHandle, VHD_CORE_SP_BUFFERQUEUE_PRELOAD, 0);
    ULONG queueDepth = NBOF_APP_BUFFER;
    VHD_SetStreamProperty(streamTxHandle, VHD_CORE_SP_BUFFERQUEUE_DEPTH, queueDepth);
    
    // Initialize application buffers AFTER setting all stream properties
    result = VHD_InitApplicationBuffers(streamTxHandle);
    if (result != VHDERR_NOERROR)
    {
        // std::cerr << "Failed to initialize TX application buffers. Error code: 0x" 
        //           << std::hex << result << std::dec << std::endl;
        {
            std::ostringstream oss;
            oss << "Failed to initialize TX application buffers. Error code: 0x" << std::hex << result << std::dec;
            getLogger()->error(oss.str());
        }
        return false;
    }
    
    // std::cout << "TX application buffers initialized successfully" << std::endl;
    getLogger()->info(std::string("TX application buffers initialized successfully"));
    
    // Allocate TX buffers
    txApplicationBuffers.resize(NB_VHD_SDI_BUFFERTYPE);
    slotTxHandles.resize(NBOF_APP_BUFFER);
    
    // Track which buffer types have been allocated
    bool hasVideoBuffer = false;
    
    for (int bufferTypeIdx = 0; bufferTypeIdx < NB_VHD_SDI_BUFFERTYPE; bufferTypeIdx++)
    {
        ULONG bufferSize;
        result = VHD_GetApplicationBuffersSize(streamTxHandle, bufferTypeIdx, &bufferSize);
        if (result != VHDERR_NOERROR || bufferSize == 0)
            continue;

        {
            std::ostringstream oss;
            oss << "Buffer type " << bufferTypeIdx << " size: " << bufferSize << " bytes";
            getLogger()->info(oss.str());
        }
        
        // Align buffer size
        ULONG alignedSize = ROUND_TO_x(bufferSize, RDMA_SIZE_ALIGNMENT);
        
        txApplicationBuffers[bufferTypeIdx].resize(NBOF_APP_BUFFER);
        
        for (int bufIdx = 0; bufIdx < NBOF_APP_BUFFER; bufIdx++)
        {
            auto buffer = new VHD_APPLICATION_BUFFER_DESCRIPTOR;
            buffer->Size = sizeof(VHD_APPLICATION_BUFFER_DESCRIPTOR);
            
            if (bufferTypeIdx == VHD_SDI_BT_VIDEO)
            {
                hasVideoBuffer = true;
                
                // Allocate GPU buffer for video (UYVY)
                cudaError_t err = cudaMalloc((void**)&buffer->pBuffer, alignedSize);
                if (err != cudaSuccess) {
                    // std::cerr << "Failed to allocate TX CUDA buffer: " << cudaGetErrorString(err) << std::endl;
                    {
                        std::ostringstream oss;
                        oss << "Failed to allocate TX CUDA buffer: " << cudaGetErrorString(err);
                        getLogger()->error(oss.str());
                    }
                    delete buffer;
                    return false;
                }
                
                // Initialize buffer to black (UYVY black = 0x80, 0x10, 0x80, 0x10, ...)
                cudaMemset(buffer->pBuffer, 0x10, alignedSize);
                
                // Set sync memops attribute
                unsigned int flag = 1;
                CUresult cudaResult = cuPointerSetAttribute(&flag, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                                                               (CUdeviceptr)buffer->pBuffer);
                if (cudaResult != CUDA_SUCCESS) {
                    // std::cerr << "Failed to set CUDA pointer attribute for TX" << std::endl;
                    getLogger()->error(std::string("Failed to set CUDA pointer attribute for TX"));
                    cudaFree(buffer->pBuffer);
                    delete buffer;
                    return false;
                }
                
                buffer->RDMAEnabled = true;
                {
                    std::ostringstream oss;
                    oss << "Allocated GPU buffer " << bufIdx << " of size " << alignedSize << " bytes (UYVY format)";
                    getLogger()->info(oss.str());
                }
            }
            else
            {
                // Allocate CPU buffer for non-video
                buffer->pBuffer = static_cast<BYTE*>(PageAlignedAlloc(alignedSize));
                if (!buffer->pBuffer) {
                    // std::cerr << "Failed to allocate CPU buffer for buffer type " << bufferTypeIdx << std::endl;
                    {
                        std::ostringstream oss;
                        oss << "Failed to allocate CPU buffer for buffer type " << bufferTypeIdx;
                        getLogger()->error(oss.str());
                    }
                    delete buffer;
                    return false;
                }
                memset(buffer->pBuffer, 0, alignedSize);
                buffer->RDMAEnabled = false;
            }
            
            txApplicationBuffers[bufferTypeIdx][bufIdx] = buffer;
        }
    }
    
    if (!hasVideoBuffer) {
        // std::cerr << "ERROR: No video buffer was allocated!" << std::endl;
        getLogger()->error(std::string("ERROR: No video buffer was allocated!"));
        return false;
    }
    
    // Create TX slots
    for (int slotIdx = 0; slotIdx < NBOF_APP_BUFFER; slotIdx++)
    {
        std::vector<VHD_APPLICATION_BUFFER_DESCRIPTOR> descriptors;
        
        // Collect all allocated buffers for this slot
        for (int bufferTypeIdx = 0; bufferTypeIdx < NB_VHD_SDI_BUFFERTYPE; bufferTypeIdx++)
        {
            if (bufferTypeIdx < txApplicationBuffers.size() && 
                slotIdx < txApplicationBuffers[bufferTypeIdx].size() &&
                txApplicationBuffers[bufferTypeIdx][slotIdx] != nullptr)
            {
                auto bufferDesc = static_cast<VHD_APPLICATION_BUFFER_DESCRIPTOR*>(
                    txApplicationBuffers[bufferTypeIdx][slotIdx]);
                descriptors.push_back(*bufferDesc);
            }
        }
        
        if (descriptors.empty()) {
            // std::cerr << "No buffers available for slot " << slotIdx << std::endl;
            {
                std::ostringstream oss;
                oss << "No buffers available for slot " << slotIdx;
                getLogger()->error(oss.str());
            }
            return false;
        }
        
        result = VHD_CreateSlotEx(streamTxHandle, descriptors.data(), &slotTxHandles[slotIdx]);
        if (result != VHDERR_NOERROR) {
            // std::cerr << "Failed to create TX slot " << slotIdx << ". Error: 0x" 
            //           << std::hex << result << std::dec << std::endl;
            {
                std::ostringstream oss;
                oss << "Failed to create TX slot " << slotIdx << ". Error: 0x" << std::hex << result << std::dec;
                getLogger()->error(oss.str());
            }
            return false;
        }
        
        // std::cout << "Created TX slot " << slotIdx << " with " << descriptors.size() << " buffers" << std::endl;
        {
            std::ostringstream oss;
            oss << "Created TX slot " << slotIdx << " with " << descriptors.size() << " buffers";
            getLogger()->info(oss.str());
        }
    }
    
    // Start TX stream
    result = VHD_StartStream(streamTxHandle);
    if (result != VHDERR_NOERROR) {
        // std::cerr << "Failed to start TX stream. Error: 0x" << std::hex << result << std::dec << std::endl;
        {
            std::ostringstream oss;
            oss << "Failed to start TX stream. Error: 0x" << std::hex << result << std::dec;
            getLogger()->error(oss.str());
        }
        return false;
    }

    outputInitialized = true;
    // std::cout << "TX output initialized successfully with UYVY format" << std::endl;
    getLogger()->info(std::string("TX output initialized successfully with UYVY format"));
    return true;
}