#pragma once

#include <string>
#include <opencv2/opencv.hpp>
#include <mutex>
#include <GL/glew.h>
#include <cuda_runtime.h>
#include <vector>
#include <atomic>

#include "logger.h"

// Check if ULONG is already defined (e.g., by DeckLink headers)
#ifndef ULONG
#define ULONG uint32_t
#endif

// Include Deltacast types header instead of forward declarations
#include "VideoMasterHD_Core.h"

// Forward declaration for HANDLE if not already defined
#ifndef HANDLE
typedef void* HANDLE;
#endif

// Forward declaration of the friend function
class DeltacastCapture;
static void ReceptionThreadFunc(DeltacastCapture* parent, HANDLE streamRxHandle, HANDLE* pSlotRxHandle);

class DeltacastCapture
{
public:
    DeltacastCapture();
    ~DeltacastCapture();

    bool initialize(int deviceIndex = 0);
    bool startCapture();
    void stopCapture();
    
    // Get raw GPU buffer pointer and size
    bool getGPUFrame(unsigned char** d_buffer, size_t& bufferSize);
    int getFrameWidth() const { return frameWidth; }
    int getFrameHeight() const { return frameHeight; }
    
    GLuint getLatestTextureId() const { return latestTextureId; }
    std::mutex &getFrameMutex() { return frameMutex; }

    bool hasFrame() const { return frameAvailable; }
    cv::Mat getLatestFrame() const { return latestFrame.clone(); }
    void markFrameConsumed() { frameAvailable = false; }
    
    // Add this method
    uint64_t getCurrentFrameId() const { return currentFrameId.load(); }

    // Output initialization and sending texture
    bool initializeOutput(int deviceIndex = 0);
    // bool sendFrameToDeltacastTx(const cv::cuda::GpuMat& gpu_frame, int width, int height);
    bool sendFrameToDeltacastTx(const void* uyvy_data, size_t data_size, int width, int height); // Add new overload
    int getNumOutputs() const;

    // Public members for thread access
    bool shouldStopThread = false;
    std::mutex frameMutex;
    BYTE* d_currentBuffer = nullptr;
    size_t currentBufferSize = 0;
    bool frameAvailable = false;

protected:
    cv::Mat latestFrame;
    
    // GPU buffer management
    int frameWidth = 0;
    int frameHeight = 0;

private:
    static std::shared_ptr<spdlog::logger> getLogger() {
        static std::shared_ptr<spdlog::logger> logger = getModuleLogger("deltacast");
        return logger;
    }

    // Declare the reception thread function as a friend
    friend void ReceptionThreadFunc(DeltacastCapture* parent, HANDLE streamRxHandle, HANDLE* pSlotRxHandle);
    
    GLuint latestTextureId = 0;
    
    // Deltacast specific handles
    HANDLE boardHandle = nullptr;
    HANDLE streamRxHandle = nullptr;
    HANDLE streamTxHandle = nullptr;
    std::vector<HANDLE> slotRxHandles;
    
    // Application buffers - store as void* to avoid including the header
    std::vector<std::vector<void*>> applicationBuffers;
    
    // TX application buffers
    std::vector<std::vector<void*>> txApplicationBuffers;
    std::vector<HANDLE> slotTxHandles;
    
    bool capturing = false;
    bool outputInitialized = false;
    
    // Thread management
    void* receptionThread = nullptr;
    
    // Video properties
    ULONG videoStandard = 0;
    ULONG interface = 0;
    ULONG width = 0;
    ULONG height = 0;

    // Add this new member
    HANDLE currentSlotHandle = nullptr;

    std::atomic<uint64_t> currentFrameId{0};      // Track which frame we have
    uint64_t lastReturnedFrameId{0};              // Track which frame was last returned
};