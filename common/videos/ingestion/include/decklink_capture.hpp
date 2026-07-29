#pragma once

#include <string>
#include <opencv2/opencv.hpp>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <GL/glew.h>
#include "DeckLinkAPI.h"
#include <vector>
#include <atomic>
#include <spdlog/spdlog.h>
#include "logger.h"
#include "shared_state.h"

class DeckLinkCaptureCallback; // Forward declaration
class DeckLinkOutputCallback; // Forward declaration

class DeckLinkCapture
{
public:
    DeckLinkCapture();
    ~DeckLinkCapture();

    bool initialize(int deviceIndex = 0);
    bool startCapture();
    void stopCapture();

    void setSharedState(SharedState* state);

    // Retrieve the latest frame buffer. Returns true if a frame is available.
    bool getFrameBuffer(std::vector<uint8_t> &buffer, int &width, int &height);

    GLuint getLatestTextureId() const { return latestTextureId; }
    std::mutex &getFrameMutex() { return frameMutex; }

    bool hasFrame() const { return frameAvailable; }
    void markFrameConsumed() { frameAvailable = false; }

    // Output initialization and sending texture
    bool initializeOutput(int deviceIndex = 0);
    bool initializeOutput(int deviceIndex, int slotIndex);     // second output slot
    bool disableOutput(int slotIndex);

    bool sendFrameToDecklink(const uint8_t* frame_data, size_t data_size, int width, int height, int pitch);
    bool sendFrameToDecklinkSlot(int slot, const uint8_t* frame_data, size_t data_size, int width, int height, int pitch);
    int getNumOutputs() const;
    int getActiveOutputDeviceIndex() const { return activeOutputDeviceIndex; }

    int getLastFramePitch() const {
        std::lock_guard<std::mutex> lock(frameMutex);
        return latestPitch;
    }

    // New methods for dynamic mode detection
    BMDDisplayMode getCurrentDisplayMode() const { return currentDisplayMode; }
    std::string getDisplayModeName() const;  // Returns INPUT mode name
    std::string getOutputDisplayModeName() const;  // Returns OUTPUT mode name (may differ if deinterlacing)
    bool detectAndSetMode();
    bool setDisplayMode(BMDDisplayMode mode);

    // ✅ ADD: Get current video dimensions
    void getCurrentResolution(int& width, int& height) const {
        std::lock_guard<std::mutex> lock(frameMutex);
        width = latestWidth;
        height = latestHeight;
    }

    void getCurrentDisplayModeResolution(int& width, int& height) const {
        std::lock_guard<std::mutex> lock(frameMutex);
        width = currentModeWidth;
        height = currentModeHeight;
    }

    // Genlock reference status methods
    bool checkGenlockSupport();
    bool getReferenceStatus(BMDReferenceStatus& status);
    std::string getReferenceStatusString(BMDReferenceStatus status) const;
    bool waitForReferenceLock(int timeoutMs = 5000);
    void logGenlockStatus();
    bool isGenlockSupported() const { return genlockSupported; }
    bool checkReferenceSignalCompatibility();
    std::string getReferenceSignalModeName(BMDDisplayMode mode) const;
    bool checkAndConfigureOutputGenlock(IDeckLink* outputDeckLink, IDeckLinkOutput* output);
    void logCompactGenlockStatus(); // DEPRECATED: Use checkReferenceSignalCompatibility()

protected:
    std::vector<uint8_t> latestFrameBuffer;
    int latestWidth = 0;
    int latestHeight = 0;
    int latestPitch = 0;
    int currentModeWidth = 0;
    int currentModeHeight = 0;
    std::atomic<bool> frameAvailable{false};
    BMDDisplayMode currentDisplayMode = bmdModeUnknown;

    // Add the callback class as a friend
    friend class DeckLinkCaptureCallback;

private:

    static std::shared_ptr<spdlog::logger> getLogger() {
        static std::shared_ptr<spdlog::logger> logger = getModuleLogger("decklink");
        return logger;
    }

    mutable std::mutex frameMutex;
    GLuint latestTextureId = 0;
    struct IDeckLinkInput *deckLinkInput;
    struct IDeckLink *deckLink;
    bool capturing;
    IDeckLinkInputCallback *callback = nullptr;
    struct IDeckLinkOutput *deckLinkOutput = nullptr;
    bool outputInitialized = false;
    int activeOutputDeviceIndex = -1;
    BMDDisplayMode outputDisplayMode = bmdModeUnknown;
    SharedState* shared_state_ = nullptr;

    // Scheduled playback output (non-blocking sendFrameToDecklink) — slot 0.
    friend class DeckLinkOutputCallback;
    void recycleScheduledFrame_(IDeckLinkVideoFrame* completedFrame);
    void scheduledPlaybackStopped_();
    void teardownScheduledOutput_();
    bool setupScheduledOutput_(int expectedWidth, int expectedHeight);
    bool ensureScheduledFramePool_(int expectedWidth, int expectedHeight);

    std::mutex outputMutex_;
    std::condition_variable outputCv_;
    IDeckLinkVideoOutputCallback* outputCallback_ = nullptr;
    std::deque<IDeckLinkVideoFrame*> availableOutputFrames_;
    std::vector<IDeckLinkMutableVideoFrame*> allOutputFrames_;

    int outputExpectedWidth_ = 0;
    int outputExpectedHeight_ = 0;
    int outputExpectedRowBytes_ = 0;
    BMDTimeValue outputFrameDuration_ = 0;
    BMDTimeScale outputTimeScale_ = 0;
    BMDTimeValue nextScheduledTime_ = 0;
    bool scheduledPlaybackStarted_ = false;
    // Bounds how many frames the producer can queue ahead in the DeckLink
    // scheduler before back-pressure (outputCv_.wait) stalls it. Each queued
    // frame is ~one frame-duration of SDI-out latency, so keep this small:
    // minLead(2) + 1 in flight. Larger pools trade latency for jitter tolerance.
    static constexpr int kOutputFramePoolSize_ = 8;
    uint64_t outputLeadDrops_ = 0;  // frames dropped to drain an over-deep scheduling lead

    // ── Second output slot (slot 1 / SDI B) ─────────────────────────────
    struct OutputSlot1 {
        IDeckLinkOutput*               output      = nullptr;
        bool                           initialized = false;
        BMDDisplayMode                 displayMode = bmdModeUnknown;
        IDeckLinkVideoOutputCallback*  callback    = nullptr;
        std::mutex                     mutex;
        std::condition_variable        cv;
        std::deque<IDeckLinkVideoFrame*>          available;
        std::vector<IDeckLinkMutableVideoFrame*>  all;
        int  w = 0, h = 0, rowbytes = 0;
        BMDTimeValue frameDuration = 0;
        BMDTimeScale timeScale     = 0;
        BMDTimeValue nextTime      = 0;
        bool         playing       = false;
        uint64_t     leadDrops     = 0;  // frames dropped to drain an over-deep scheduling lead
        static constexpr int kPoolSize = 8;  // see kOutputFramePoolSize_: caps SDI-B scheduler latency
    };
    OutputSlot1 out1_;

    friend class DeckLinkOutputCallback1;
    void recycleSlot1Frame_(IDeckLinkVideoFrame* completedFrame);
    void slot1PlaybackStopped_();
    void teardownOutputSlot1_();
    bool setupOutputSlot1_(int expectedWidth, int expectedHeight);
    bool ensureOutputSlot1Pool_(int expectedWidth, int expectedHeight);
    
    // Genlock support
    struct IDeckLinkConfiguration *deckLinkConfiguration = nullptr;
    struct IDeckLinkProfileAttributes *deckLinkAttributes = nullptr;
    struct IDeckLinkStatus *deckLinkStatus = nullptr;
    bool genlockSupported = false;
};
