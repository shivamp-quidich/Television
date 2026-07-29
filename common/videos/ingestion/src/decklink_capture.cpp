#include "decklink_capture.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <GL/glew.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>

// DeckLink SDK headers
#include "DeckLinkAPI.h"
#include "DeckLinkAPIConfiguration.h"

static std::string displayModeToString(BMDDisplayMode mode);
static std::string displayModeFlagsToString(BMDDisplayModeFlags flags);
static std::string detectedInputFlagsToString(BMDDetectedVideoInputFormatFlags flags);

class DeckLinkCaptureCallback : public IDeckLinkInputCallback
{
private:
    DeckLinkCapture *parent;

    // Copy `size` bytes out of the frame's video buffer into `dst` WHILE the
    // buffer access is held. The previous version returned the raw pointer and
    // ended access BEFORE the caller copied — a use-after-EndAccess that lets the
    // DeckLink driver recycle/overwrite the buffer mid-copy, welding the next/
    // previous frame onto the bottom of the current one (a motion tear that is
    // timing/machine-dependent and reproduced on the Ada laptop but not the
    // desktop). Copying inside StartAccess/EndAccess guarantees a coherent frame.
    static bool copyFrameBytes(IDeckLinkVideoFrame* frame, BMDBufferAccessFlags accessFlags,
                               std::vector<uint8_t>& dst, size_t size)
    {
        if (!frame || size == 0)
            return false;

        IDeckLinkVideoBuffer* videoBuffer = nullptr;
        if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) != S_OK || !videoBuffer)
            return false;

        if (videoBuffer->StartAccess(accessFlags) != S_OK)
        {
            videoBuffer->Release();
            return false;
        }

        bool ok = false;
        void* bytes = nullptr;
        if (videoBuffer->GetBytes(&bytes) == S_OK && bytes)
        {
            const uint8_t* src = static_cast<const uint8_t*>(bytes);
            dst.assign(src, src + size);
            ok = true;
        }

        videoBuffer->EndAccess(accessFlags);
        videoBuffer->Release();
        return ok;
    }

    // Helper to access logger through parent
    std::shared_ptr<spdlog::logger> getLogger() const {
        return parent->getLogger();
    }

public:
    DeckLinkCaptureCallback(DeckLinkCapture *_parent) : parent(_parent) {}

    // IUnknown methods
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID *) override
    {
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef(void) override
    {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release(void) override
    {
        return 1;
    }

    // IDeckLinkInputCallback methods
    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoFrame,
                                                     IDeckLinkAudioInputPacket *) override
    {
        if (!videoFrame || !parent)
            return S_OK;

        const int width = videoFrame->GetWidth();
        const int height = videoFrame->GetHeight();
        const long rowBytes = videoFrame->GetRowBytes();
        const size_t frameSize = static_cast<size_t>(rowBytes) * static_cast<size_t>(height);

        // Copy the frame out while the buffer access is held (see copyFrameBytes).
        std::lock_guard<std::mutex> lock(parent->frameMutex);
        if (copyFrameBytes(videoFrame, bmdBufferAccessRead, parent->latestFrameBuffer, frameSize))
        {
            parent->latestWidth = width;
            parent->latestHeight = height;
            parent->latestPitch = rowBytes;
            parent->frameAvailable = true;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                                    IDeckLinkDisplayMode *newDisplayMode,
                                                    BMDDetectedVideoInputFormatFlags detectedSignalFlags) override
    {
        if (!newDisplayMode || !parent)
            return S_OK;

        BMDDisplayMode newMode = newDisplayMode->GetDisplayMode();
        const BMDDisplayModeFlags modeFlags = newDisplayMode->GetFlags();

        getLogger()->info(
            "Video input format changed events=0x{:x}, mode={}, flags={}, detected={} ({:#x})",
            static_cast<uint32_t>(notificationEvents),
            displayModeToString(newMode),
            displayModeFlagsToString(modeFlags),
            detectedInputFlagsToString(detectedSignalFlags),
            static_cast<uint32_t>(detectedSignalFlags));
        
        if (newMode != parent->currentDisplayMode)
        {
            parent->currentDisplayMode = newMode;
            {
                std::lock_guard<std::mutex> lock(parent->frameMutex);
                parent->currentModeWidth = newDisplayMode->GetWidth();
                parent->currentModeHeight = newDisplayMode->GetHeight();
            }
            
            // ✅ Update resolution in shared state
            if (parent->shared_state_) {
                // Preserve the configured input matrix/range when the signal
                // changes mode. Auto selection resolves from the new height.
                VideoInputState::Config video_config = parent->shared_state_
                    ->getData<VideoInputState::Config>()
                    .value_or(VideoInputState::Config{});
                video_config.width = newDisplayMode->GetWidth();
                video_config.height = newDisplayMode->GetHeight();
                video_config.display_mode_name = parent->getDisplayModeName();
                video_config.signal_detected = true;
                parent->shared_state_->setData(video_config);
                
                getLogger()->info("Video format changed: {}", video_config.display_mode_name);
            }
            
            parent->stopCapture();
            parent->startCapture();
        }
        
        return S_OK;
    }
};

class DeckLinkOutputCallback : public IDeckLinkVideoOutputCallback
{
private:
    std::atomic<ULONG> refCount_{1};
    DeckLinkCapture* parent_;

public:
    explicit DeckLinkOutputCallback(DeckLinkCapture* parent) : parent_(parent) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
    {
        if (!ppv)
            return E_POINTER;

        const REFIID iidIUnknown = IID_IUnknown;
        const auto iidEqual = [](const REFIID& a, const REFIID& b) {
            return std::memcmp(&a, &b, sizeof(REFIID)) == 0;
        };

        if (iidEqual(iid, iidIUnknown) || iidEqual(iid, IID_IDeckLinkVideoOutputCallback))
        {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
            AddRef();
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef(void) override
    {
        return ++refCount_;
    }

    ULONG STDMETHODCALLTYPE Release(void) override
    {
        const ULONG newCount = --refCount_;
        if (newCount == 0)
            delete this;
        return newCount;
    }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult) override
    {
        if (parent_)
            parent_->recycleScheduledFrame_(completedFrame);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
    {
        if (parent_)
            parent_->scheduledPlaybackStopped_();
        return S_OK;
    }
};

class DeckLinkOutputCallback1 : public IDeckLinkVideoOutputCallback
{
private:
    std::atomic<ULONG> refCount_{1};
    DeckLinkCapture* parent_;
public:
    explicit DeckLinkOutputCallback1(DeckLinkCapture* p) : parent_(p) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        const auto eq = [](const REFIID& a, const REFIID& b){ return std::memcmp(&a,&b,sizeof(REFIID))==0; };
        if (eq(iid,IID_IUnknown)||eq(iid,IID_IDeckLinkVideoOutputCallback)) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n=--refCount_; if(n==0) delete this; return n; }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* f, BMDOutputFrameCompletionResult) override {
        if (parent_) parent_->recycleSlot1Frame_(f);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override {
        if (parent_) parent_->slot1PlaybackStopped_();
        return S_OK;
    }
};

DeckLinkCapture::DeckLinkCapture()
    : deckLinkInput(nullptr), deckLink(nullptr), capturing(false), latestTextureId(0) {}

void DeckLinkCapture::setSharedState(SharedState* state)
{
    shared_state_ = state;

    if (!shared_state_ || !deckLink)
        return;

    if (deckLinkOutput)
    {
        checkReferenceSignalCompatibility();
        return;
    }

    GenlockState::Status status;
    status.is_locked = false;
    status.ref_format = genlockSupported ? "Output Not Init" : "Not Supported";
    status.last_update = std::chrono::system_clock::now();
    shared_state_->setData(status);
}

DeckLinkCapture::~DeckLinkCapture()
{
    stopCapture();

    teardownScheduledOutput_();
    teardownOutputSlot1_();

    if (callback)
    {
        callback->Release();
        callback = nullptr;
    }
    if (latestTextureId)
    {
        glDeleteTextures(1, &latestTextureId);
        latestTextureId = 0;
    }
    if (deckLinkConfiguration)
    {
        deckLinkConfiguration->Release();
        deckLinkConfiguration = nullptr;
    }
    if (deckLinkAttributes)
    {
        deckLinkAttributes->Release();
        deckLinkAttributes = nullptr;
    }
    if (deckLinkStatus)
    {
        deckLinkStatus->Release();
        deckLinkStatus = nullptr;
    }
    if (deckLinkInput)
    {
        deckLinkInput->Release();
        deckLinkInput = nullptr;
    }
    if (deckLink)
    {
        deckLink->Release();
        deckLink = nullptr;
    }
}

void DeckLinkCapture::teardownScheduledOutput_()
{
    std::unique_lock<std::mutex> lock(outputMutex_);

    if (deckLinkOutput)
    {
        if (scheduledPlaybackStarted_ && outputTimeScale_ != 0)
        {
            BMDTimeValue actualStop = 0;
            deckLinkOutput->StopScheduledPlayback(0, &actualStop, outputTimeScale_);
        }

        if (outputCallback_)
            deckLinkOutput->SetScheduledFrameCompletionCallback(nullptr);
    }

    scheduledPlaybackStarted_ = false;
    nextScheduledTime_ = 0;
    outputFrameDuration_ = 0;
    outputTimeScale_ = 0;
    outputExpectedWidth_ = 0;
    outputExpectedHeight_ = 0;
    outputExpectedRowBytes_ = 0;

    availableOutputFrames_.clear();
    for (auto* f : allOutputFrames_)
    {
        if (f)
            f->Release();
    }
    allOutputFrames_.clear();

    lock.unlock();

    if (outputCallback_)
    {
        outputCallback_->Release();
        outputCallback_ = nullptr;
    }
}

void DeckLinkCapture::recycleScheduledFrame_(IDeckLinkVideoFrame* completedFrame)
{
    if (!completedFrame)
        return;

    std::lock_guard<std::mutex> lock(outputMutex_);
    availableOutputFrames_.push_back(completedFrame);
    outputCv_.notify_one();
}

void DeckLinkCapture::scheduledPlaybackStopped_()
{
    std::lock_guard<std::mutex> lock(outputMutex_);
    scheduledPlaybackStarted_ = false;
    nextScheduledTime_ = 0;
}

bool DeckLinkCapture::ensureScheduledFramePool_(int expectedWidth, int expectedHeight)
{
    if (!deckLinkOutput)
        return false;

    if (outputExpectedWidth_ == expectedWidth && outputExpectedHeight_ == expectedHeight && outputExpectedRowBytes_ == expectedWidth * 2 && !allOutputFrames_.empty())
        return true;

    // (Re)build frame pool.
    availableOutputFrames_.clear();
    for (auto* f : allOutputFrames_)
    {
        if (f)
            f->Release();
    }
    allOutputFrames_.clear();

    outputExpectedWidth_ = expectedWidth;
    outputExpectedHeight_ = expectedHeight;
    outputExpectedRowBytes_ = expectedWidth * 2;

    for (int i = 0; i < kOutputFramePoolSize_; ++i)
    {
        IDeckLinkMutableVideoFrame* frame = nullptr;
        const HRESULT hr = deckLinkOutput->CreateVideoFrame(
            outputExpectedWidth_,
            outputExpectedHeight_,
            outputExpectedRowBytes_,
            bmdFormat8BitYUV,
            bmdFrameFlagDefault,
            &frame);

        if (hr != S_OK || !frame)
        {
            getLogger()->error("Failed to create scheduled output frame (hr=0x{:x})", hr);
            return false;
        }

        allOutputFrames_.push_back(frame);
        availableOutputFrames_.push_back(frame);
    }

    return true;
}

bool DeckLinkCapture::setupScheduledOutput_(int expectedWidth, int expectedHeight)
{
    if (!deckLinkOutput)
        return false;

    if (!outputCallback_)
        outputCallback_ = new DeckLinkOutputCallback(this);

    const HRESULT hrCb = deckLinkOutput->SetScheduledFrameCompletionCallback(outputCallback_);
    if (hrCb != S_OK)
    {
        getLogger()->warn("SetScheduledFrameCompletionCallback failed (hr=0x{:x}); falling back to synchronous output", hrCb);
        return false;
    }

    // Discover output mode timing and dimensions.
    IDeckLinkDisplayModeIterator* modeIter = nullptr;
    if (deckLinkOutput->GetDisplayModeIterator(&modeIter) != S_OK || !modeIter)
    {
        getLogger()->warn("GetDisplayModeIterator failed; falling back to synchronous output");
        return false;
    }

    bool found = false;
    IDeckLinkDisplayMode* mode = nullptr;
    while (modeIter->Next(&mode) == S_OK && mode)
    {
        if (mode->GetDisplayMode() == outputDisplayMode)
        {
            outputExpectedWidth_ = mode->GetWidth();
            outputExpectedHeight_ = mode->GetHeight();
            outputExpectedRowBytes_ = outputExpectedWidth_ * 2;

            if (mode->GetFrameRate(&outputFrameDuration_, &outputTimeScale_) != S_OK || outputFrameDuration_ == 0 || outputTimeScale_ == 0)
            {
                getLogger()->warn("Failed to query output frame rate; falling back to synchronous output");
                mode->Release();
                modeIter->Release();
                return false;
            }

            found = true;
            mode->Release();
            break;
        }

        mode->Release();
        mode = nullptr;
    }
    modeIter->Release();

    if (!found)
    {
        getLogger()->warn("Could not find output display mode in iterator; falling back to synchronous output");
        return false;
    }

    // If caller provided a different buffer size (rare), prefer device mode dims.
    (void)expectedWidth;
    (void)expectedHeight;

    if (!ensureScheduledFramePool_(outputExpectedWidth_, outputExpectedHeight_))
        return false;

    scheduledPlaybackStarted_ = false;
    nextScheduledTime_ = 0;
    return true;
}

bool DeckLinkCapture::initialize(int deviceIndex)
{
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator)
    {
        getLogger()->error("DeckLink drivers not found");
        return false;
    }

    int idx = 0;
    while (deckLinkIterator->Next(&deckLink) == S_OK)
    {
        if (idx == deviceIndex)
            break;
        deckLink->Release();
        deckLink = nullptr;
        ++idx;
    }
    deckLinkIterator->Release();

    if (!deckLink)
    {
        getLogger()->error("DeckLink device not found at index {}", deviceIndex);
        return false;
    }

    if (deckLink->QueryInterface(IID_IDeckLinkInput, (void **)&deckLinkInput) != S_OK)
    {
        getLogger()->error("Failed to get IDeckLinkInput interface");
        deckLink->Release();
        deckLink = nullptr;
        return false;
    }

    // Get configuration interface
    if (deckLink->QueryInterface(IID_IDeckLinkConfiguration, (void **)&deckLinkConfiguration) != S_OK)
    {
        getLogger()->warn("Failed to get IDeckLinkConfiguration interface");
    }
    else
    {
        // ✅ Configure input capture settings for sharp interlaced video
        getLogger()->info("Configuring DeckLink input capture settings...");
        
        // Disable field flicker removal which can soften edges
        // HRESULT flicker_result = deckLinkConfiguration->SetFlag(bmdDeckLinkConfigFieldFlickerRemoval, false);
        // if (flicker_result == S_OK)
        // {
        //     getLogger()->info("✅ Disabled field flicker removal (prevents edge softening)");
        // }
        // else
        // {
        //     getLogger()->warn("Could not disable field flicker removal. Error: 0x{:x}", flicker_result);
        // }
        
        // // For 1080i capture, ensure we're not forcing PsF conversion on input
        // HRESULT psf_result = deckLinkConfiguration->SetFlag(bmdDeckLinkConfigCapture1080pAsPsF, false);
        // if (psf_result == S_OK)
        // {
        //     getLogger()->info("✅ Disabled input PsF conversion (true interlaced capture)");
        // }
        // else
        // {
        //     getLogger()->warn("Could not configure input PsF setting. Error: 0x{:x}", psf_result);
        // }
    }

    // Get attributes interface to check genlock support
    if (deckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&deckLinkAttributes) != S_OK)
    {
        getLogger()->warn("Failed to get IDeckLinkProfileAttributes interface");
    }

    // Get status interface for genlock status checking
    if (deckLink->QueryInterface(IID_IDeckLinkStatus, (void **)&deckLinkStatus) != S_OK)
    {
        getLogger()->warn("Failed to get IDeckLinkStatus interface");
    }

    // Check if device supports genlock
    checkGenlockSupport();

    // Set callback ONCE, after deckLinkInput is valid
    callback = new DeckLinkCaptureCallback(this);
    deckLinkInput->SetCallback(callback);

    return true;
}

bool DeckLinkCapture::detectAndSetMode()
{
    if (!deckLinkInput)
    {
        getLogger()->error("DeckLinkInput not initialized");
        return false;
    }

    IDeckLinkDisplayModeIterator *displayModeIterator = nullptr;
    IDeckLinkDisplayMode *displayMode = nullptr;
    
    // Get available display modes
    if (deckLinkInput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
    {
        getLogger()->error("Failed to get display mode iterator");
        return false;
    }

    getLogger()->info("Available display modes:");
    int modeIndex = 0;
    while (displayModeIterator->Next(&displayMode) == S_OK)
    {
        const char *modeName = nullptr;
        displayMode->GetName(&modeName);
        getLogger()->info("  {}: {} ({}x{}, flags={})",
                          modeIndex,
                          modeName ? modeName : "Unknown",
                          displayMode->GetWidth(),
                          displayMode->GetHeight(),
                          displayModeFlagsToString(displayMode->GetFlags()));
        long width = displayMode->GetWidth();
        long height = displayMode->GetHeight();
        
        getLogger()->debug("{}: {} ({}x{})", modeIndex++, modeName, width, height);
        
        displayMode->Release();
    }
    displayModeIterator->Release();

    // Enable video input with automatic format detection
    getLogger()->info("Waiting for input signal detection...");
    
    return true;
}

bool DeckLinkCapture::setDisplayMode(BMDDisplayMode mode)
{
    currentDisplayMode = mode;
    
    if (capturing)
    {
        // Restart capture with new mode
        stopCapture();
        return startCapture();
    }
    
    return true;
}

// Helper function to convert BMDDisplayMode to string
static std::string displayModeToString(BMDDisplayMode mode)
{
    switch (mode)
    {
        // HD 1080p modes
        case bmdModeHD1080p2398: return "1080p23.98";
        case bmdModeHD1080p24: return "1080p24";
        case bmdModeHD1080p25: return "1080p25";
        case bmdModeHD1080p2997: return "1080p29.97";
        case bmdModeHD1080p30: return "1080p30";
        case bmdModeHD1080p50: return "1080p50";
        case bmdModeHD1080p5994: return "1080p59.94";
        case bmdModeHD1080p6000: return "1080p60";
        
        // HD 1080i modes
        case bmdModeHD1080i50: return "1080i50";
        case bmdModeHD1080i5994: return "1080i59.94";
        case bmdModeHD1080i6000: return "1080i60";
        
        // 4K UHD modes
        case bmdMode4K2160p2398: return "4K2160p23.98";
        case bmdMode4K2160p24: return "4K2160p24";
        case bmdMode4K2160p25: return "4K2160p25";
        case bmdMode4K2160p2997: return "4K2160p29.97";
        case bmdMode4K2160p30: return "4K2160p30";
        case bmdMode4K2160p50: return "4K2160p50";
        case bmdMode4K2160p5994: return "4K2160p59.94";
        case bmdMode4K2160p60: return "4K2160p60";
        
        default: return "Unknown";
    }
}

static std::string displayModeFlagsToString(BMDDisplayModeFlags flags)
{
    std::vector<std::string> parts;
    if (flags & bmdDisplayModeSupports3D)
        parts.emplace_back("3D");
    if (flags & bmdDisplayModeColorspaceRec601)
        parts.emplace_back("Rec601");
    if (flags & bmdDisplayModeColorspaceRec709)
        parts.emplace_back("Rec709");
    if (flags & bmdDisplayModeColorspaceRec2020)
        parts.emplace_back("Rec2020");

    if (parts.empty())
        return "None";

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
            oss << ", ";
        oss << parts[i];
    }
    return oss.str();
}

static std::string detectedInputFlagsToString(BMDDetectedVideoInputFormatFlags flags)
{
    std::vector<std::string> parts;
    if (flags & bmdDetectedVideoInputYCbCr422)
        parts.emplace_back("YCbCr422");
    if (flags & bmdDetectedVideoInputRGB444)
        parts.emplace_back("RGB444");
    if (flags & bmdDetectedVideoInputDualStream3D)
        parts.emplace_back("DualStream3D");
    if (flags & bmdDetectedVideoInput12BitDepth)
        parts.emplace_back("12Bit");
    if (flags & bmdDetectedVideoInput10BitDepth)
        parts.emplace_back("10Bit");
    if (flags & bmdDetectedVideoInput8BitDepth)
        parts.emplace_back("8Bit");

    if (parts.empty())
        return "None";

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
            oss << ", ";
        oss << parts[i];
    }
    return oss.str();
}

std::string DeckLinkCapture::getDisplayModeName() const
{
    return displayModeToString(currentDisplayMode);
}

std::string DeckLinkCapture::getOutputDisplayModeName() const
{
    return displayModeToString(outputDisplayMode);
}

bool DeckLinkCapture::startCapture()
{
    if (!deckLinkInput)
    {
        getLogger()->error("DeckLinkInput not initialized");
        return false;
    }


    // If mode is unknown, try to detect it first
    if (currentDisplayMode == bmdModeUnknown)
    {
        detectAndSetMode();
        
        // Enable video input with automatic format detection
        BMDPixelFormat pixelFormat = bmdFormat8BitYUV; // UYVY
        
        // Enable with format detection - the callback will update the mode
        HRESULT result = deckLinkInput->EnableVideoInput(bmdModeHD720p50, pixelFormat, bmdVideoInputEnableFormatDetection);
        if (result != S_OK)
        {
            getLogger()->error("Failed to enable video input with format detection. Error code: 0x{:x}", result);
            return false;
        }
        
        getLogger()->info("Video input enabled with automatic format detection");
    }
    else
    {
        // Use the explicitly set mode
        BMDPixelFormat pixelFormat = bmdFormat8BitYUV; // UYVY
        
        HRESULT result = deckLinkInput->EnableVideoInput(currentDisplayMode, pixelFormat, bmdVideoInputEnableFormatDetection);
        if (result != S_OK)
        {
            getLogger()->error("Failed to enable video input. Error code: 0x{:x}", result);
            return false;
        }
        
        getLogger()->info("Video input enabled with mode: {}", getDisplayModeName());
    }

    if (deckLinkInput->StartStreams() != S_OK)
    {
        getLogger()->error("Failed to start streams");
        return false;
    }

    capturing = true;
    getLogger()->info("Capture started (UYVY 4:2:2)");
    return true;
}

void DeckLinkCapture::stopCapture()
{
    if (capturing && deckLinkInput)
    {
        deckLinkInput->StopStreams();
        deckLinkInput->DisableVideoInput();
        capturing = false;
        getLogger()->info("Capture stopped");
    }
}

// New method to get raw UYVY buffer
bool DeckLinkCapture::getFrameBuffer(std::vector<uint8_t> &buffer, int &width, int &height)
{
    std::lock_guard<std::mutex> lock(frameMutex);
    if (!frameAvailable || latestFrameBuffer.empty())
    {
        return false;
    }
    buffer = latestFrameBuffer;
    width = latestWidth;
    height = latestHeight;
    frameAvailable = false;
    return true;
}

bool DeckLinkCapture::disableOutput(int slotIndex)
{
    if (slotIndex == 0)
    {
        if (deckLinkOutput)
        {
            teardownScheduledOutput_();
            deckLinkOutput->DisableVideoOutput();
            deckLinkOutput->Release();
            deckLinkOutput = nullptr;
        }
        outputInitialized = false;
        activeOutputDeviceIndex = -1;
        return true;
    }

    if (slotIndex == 1)
    {
        teardownOutputSlot1_();
        return true;
    }

    getLogger()->error("disableOutput: invalid slotIndex {}", slotIndex);
    return false;
}

bool DeckLinkCapture::initializeOutput(int deviceIndex)
{
    // Re-init safety: clean up any existing output.
    disableOutput(0);

    auto computeDesiredOutputMode = [&]() {
        if (currentDisplayMode == bmdModeHD1080i50)
        {
            outputDisplayMode = bmdModeHD1080i50;
            getLogger()->info("Output 1080i50 (matching input, no decimator needed)");
        }
        else if (currentDisplayMode == bmdModeHD1080i5994)
        {
            outputDisplayMode = bmdModeHD1080i5994;
            getLogger()->info("Output 1080i59.94 (matching input, no decimator needed)");
        }
        else if (currentDisplayMode == bmdModeHD1080i6000)
        {
            outputDisplayMode = bmdModeHD1080i6000;
            getLogger()->info("Output 1080i60 (matching input, no decimator needed)");
        }
        else if (currentDisplayMode != bmdModeUnknown)
        {
            outputDisplayMode = currentDisplayMode;
            getLogger()->info("Using detected input mode for output: {}", getDisplayModeName());
        }
        else
        {
            outputDisplayMode = bmdModeHD1080p25;
            getLogger()->info("Using default output mode: 1080p25");
        }
    };

    computeDesiredOutputMode();

    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator)
    {
        getLogger()->error("DeckLink drivers not found for output");
        return false;
    }

    // Enumerate output-capable devices.
    std::vector<IDeckLink*> outputDevices;
    {
        IDeckLink* dev = nullptr;
        while (deckLinkIterator->Next(&dev) == S_OK)
        {
            IDeckLinkOutput* tmp = nullptr;
            if (dev->QueryInterface(IID_IDeckLinkOutput, (void**)&tmp) == S_OK && tmp)
            {
                tmp->Release();
                outputDevices.push_back(dev); // keep ref
            }
            else
            {
                dev->Release();
            }
        }
        deckLinkIterator->Release();
    }

    if (outputDevices.empty())
    {
        getLogger()->error("No output-capable DeckLink devices found");
        return false;
    }

    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(outputDevices.size()))
    {
        getLogger()->error("DeckLink output index {} out of range (0-{})", deviceIndex, static_cast<int>(outputDevices.size()) - 1);
        for (auto* d : outputDevices) d->Release();
        return false;
    }

    auto attrString = [&](IDeckLinkProfileAttributes* attrs, BMDDeckLinkAttributeID id) -> std::string {
        if (!attrs) return {};
        const char* value = nullptr;
        if (attrs->GetString(id, &value) == S_OK && value)
            return std::string(value);
        return {};
    };

    auto attrInt = [&](IDeckLinkProfileAttributes* attrs, BMDDeckLinkAttributeID id, int64_t& out) -> bool {
        out = 0;
        if (!attrs) return false;
        return attrs->GetInt(id, &out) == S_OK;
    };

    auto statusInt = [&](IDeckLink* dev, BMDDeckLinkStatusID id, int64_t& out) -> bool {
        out = 0;
        IDeckLinkStatus* st = nullptr;
        if (dev->QueryInterface(IID_IDeckLinkStatus, (void**)&st) != S_OK || !st)
            return false;
        const bool ok = (st->GetInt(id, &out) == S_OK);
        st->Release();
        return ok;
    };

    auto logOutputDevice = [&](IDeckLink* dev, int outIdx) {
        IDeckLinkProfileAttributes* attrs = nullptr;
        if (dev->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs) != S_OK)
            attrs = nullptr;

        int64_t ioSupport = 0, duplex = 0, profileId = 0, subIdx = -1, outConns = 0;
        attrInt(attrs, BMDDeckLinkVideoIOSupport, ioSupport);
        attrInt(attrs, BMDDeckLinkDuplex, duplex);
        attrInt(attrs, BMDDeckLinkProfileID, profileId);
        attrInt(attrs, BMDDeckLinkSubDeviceIndex, subIdx);
        attrInt(attrs, BMDDeckLinkVideoOutputConnections, outConns);

        int64_t busy = 0;
        statusInt(dev, bmdDeckLinkStatusBusy, busy);

        getLogger()->info(
            "[Output {}] {} / {} (subDevice={}, profile=0x{:x}, duplex=0x{:x}, ioSupport=0x{:x}, outConns=0x{:x}, busy=0x{:x})",
            outIdx,
            attrString(attrs, BMDDeckLinkDisplayName).empty() ? "<unknown>" : attrString(attrs, BMDDeckLinkDisplayName),
            attrString(attrs, BMDDeckLinkModelName).empty() ? "<unknown>" : attrString(attrs, BMDDeckLinkModelName),
            subIdx,
            static_cast<uint32_t>(profileId),
            static_cast<uint32_t>(duplex),
            static_cast<uint32_t>(ioSupport),
            static_cast<uint32_t>(outConns),
            static_cast<uint32_t>(busy));

        if (attrs) attrs->Release();
    };

    for (int i = 0; i < static_cast<int>(outputDevices.size()); ++i)
        logOutputDevice(outputDevices[i], i);

    auto tryEnableOnDevice = [&](IDeckLink* outputDeckLink, int outIdx) -> bool {
        IDeckLinkOutput* out = nullptr;
        if (outputDeckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&out) != S_OK || !out)
        {
            getLogger()->error("[Output {}] Failed to get IDeckLinkOutput", outIdx);
            return false;
        }

        IDeckLinkProfileAttributes* attrs = nullptr;
        if (outputDeckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void**)&attrs) != S_OK)
            attrs = nullptr;

        int64_t ioSupport = 0;
        attrInt(attrs, BMDDeckLinkVideoIOSupport, ioSupport);
        if ((static_cast<uint32_t>(ioSupport) & bmdDeviceSupportsPlayback) == 0)
        {
            getLogger()->error("[Output {}] Device active profile does not support PLAYBACK (ioSupport=0x{:x})", outIdx, static_cast<uint32_t>(ioSupport));
            if (attrs) attrs->Release();
            out->Release();
            return false;
        }

        int64_t busy = 0;
        statusInt(outputDeckLink, bmdDeckLinkStatusBusy, busy);
        if ((static_cast<uint32_t>(busy) & bmdDeviceCaptureBusy) != 0)
        {
            getLogger()->warn("[Output {}] Device reports CAPTURE busy (busy=0x{:x}). If this is the same connector as input, output may fail in half-duplex mode.", outIdx, static_cast<uint32_t>(busy));
        }

        int64_t outConnsBits = 0;
        attrInt(attrs, BMDDeckLinkVideoOutputConnections, outConnsBits);
        const uint32_t outConns = static_cast<uint32_t>(outConnsBits);

        IDeckLinkConfiguration* outputConfig = nullptr;
        if (outputDeckLink->QueryInterface(IID_IDeckLinkConfiguration, (void**)&outputConfig) != S_OK)
            outputConfig = nullptr;

        auto trySetOutputConnection = [&](BMDVideoConnection connection) -> bool {
            if (connection != bmdVideoConnectionUnspecified)
            {
                if ((outConns & static_cast<uint32_t>(connection)) == 0)
                    return false;
            }
            if (!outputConfig)
                return (connection == bmdVideoConnectionUnspecified);

            const HRESULT hr = outputConfig->SetInt(bmdDeckLinkConfigVideoOutputConnection, static_cast<int64_t>(connection));
            if (hr != S_OK)
            {
                getLogger()->warn("[Output {}] Failed to set output connection {} (hr=0x{:x})",
                                  outIdx, static_cast<uint32_t>(connection), hr);
                return false;
            }
            return true;
        };

        auto isModeSupported = [&](BMDVideoConnection connection, BMDDisplayMode requested, BMDDisplayMode& actual) -> bool {
            actual = requested;
            bool supportedCurrent = false;

            const HRESULT hrCurrent = out->DoesSupportVideoMode(
                connection,
                requested,
                bmdFormat8BitYUV,
                bmdNoVideoOutputConversion,
                bmdSupportedVideoModeDefault,
                &actual,
                &supportedCurrent);

            if (hrCurrent != S_OK)
            {
                getLogger()->warn("[Output {}] DoesSupportVideoMode failed for {} (conn={}, hr=0x{:x})",
                                  outIdx, displayModeToString(requested), static_cast<uint32_t>(connection), hrCurrent);
                return false;
            }

            if (!supportedCurrent)
            {
                bool supportedAny = false;
                BMDDisplayMode ignored = requested;
                const HRESULT hrAny = out->DoesSupportVideoMode(
                    connection,
                    requested,
                    bmdFormat8BitYUV,
                    bmdNoVideoOutputConversion,
                    static_cast<BMDSupportedVideoModeFlags>(bmdSupportedVideoModeDefault | bmdSupportedVideoModeInAnyProfile),
                    &ignored,
                    &supportedAny);

                if (hrAny == S_OK && supportedAny)
                {
                    getLogger()->warn(
                        "[Output {}] Mode {} supported in another profile, not active profile (conn={}). Check Desktop Video Setup connector direction/profile.",
                        outIdx, displayModeToString(requested), static_cast<uint32_t>(connection));
                }
                return false;
            }

            return true;
        };

        auto chooseSupportedModeForConnection = [&](BMDVideoConnection connection) -> bool {
            const std::array<BMDDisplayMode, 8> candidates = {
                outputDisplayMode,
                currentDisplayMode,
                bmdModeHD1080p25,
                bmdModeHD1080p50,
                bmdModeHD1080i50,
                bmdModeHD720p50,
                bmdModeHD1080p2997,
                bmdModeHD1080i5994,
            };

            for (const auto mode : candidates)
            {
                if (mode == bmdModeUnknown)
                    continue;
                BMDDisplayMode actual = mode;
                if (isModeSupported(connection, mode, actual))
                {
                    outputDisplayMode = actual;
                    return true;
                }
            }
            return false;
        };

        struct ConnCandidate { BMDVideoConnection conn; const char* name; };
        const std::array<ConnCandidate, 3> connCandidates = {
            ConnCandidate{ bmdVideoConnectionSDI, "SDI" },
            ConnCandidate{ bmdVideoConnectionHDMI, "HDMI" },
            ConnCandidate{ bmdVideoConnectionUnspecified, "Unspecified" },
        };

        BMDVideoConnection selectedConnection = bmdVideoConnectionUnspecified;
        bool found = false;
        for (const auto& c : connCandidates)
        {
            if (!trySetOutputConnection(c.conn))
                continue;

            if (outputConfig)
            {
                outputConfig->SetInt(bmdDeckLinkConfigVideoOutputConversionMode, static_cast<int64_t>(bmdNoVideoOutputConversion));
                outputConfig->SetInt(bmdDeckLinkConfigVideoOutputConversionColorspaceSource, static_cast<int64_t>(bmdColorspaceRec709));
                outputConfig->SetInt(bmdDeckLinkConfigVideoOutputConversionColorspaceDestination, static_cast<int64_t>(bmdColorspaceRec709));
                outputConfig->SetFlag(bmdDeckLinkConfigRec2020Output, false);
            }

            if (chooseSupportedModeForConnection(c.conn))
            {
                selectedConnection = c.conn;
                getLogger()->info("[Output {}] Selected output connection: {}", outIdx, c.name);
                found = true;
                break;
            }
        }

        if (outputConfig) outputConfig->Release();
        if (attrs) attrs->Release();

        if (!found)
        {
            getLogger()->error("[Output {}] No supported output mode in ACTIVE profile", outIdx);
            out->Release();
            return false;
        }

        const HRESULT hrEnable = out->EnableVideoOutput(outputDisplayMode, bmdVideoOutputFlagDefault);
        if (hrEnable != S_OK)
        {
            getLogger()->error("[Output {}] EnableVideoOutput failed (mode={}, conn={}, hr=0x{:x})",
                               outIdx, displayModeToString(outputDisplayMode), static_cast<uint32_t>(selectedConnection), hrEnable);
            out->Release();
            return false;
        }

        deckLinkOutput = out;
        outputInitialized = true;
        activeOutputDeviceIndex = outIdx;
        getLogger()->info("Successfully initialized DeckLink output on output index {} with mode: {}", outIdx, getOutputDisplayModeName());
        checkAndConfigureOutputGenlock(outputDeckLink, deckLinkOutput);
        checkReferenceSignalCompatibility();

        // Prefer scheduled playback to avoid synchronous output stalls.
        {
            std::lock_guard<std::mutex> lock(outputMutex_);
            if (!setupScheduledOutput_(0, 0))
            {
                // Keep output initialized; sendFrameToDecklink will fall back to synchronous mode.
                getLogger()->warn("Scheduled playback not available; using DisplayVideoFrameSync output path");
            }
            else
            {
                getLogger()->info("DeckLink scheduled playback enabled ({}x{}, duration={}, scale={})",
                                  outputExpectedWidth_, outputExpectedHeight_, outputFrameDuration_, outputTimeScale_);
            }
        }
        return true;
    };

    bool ok = false;
    for (int attempt = 0; attempt < static_cast<int>(outputDevices.size()); ++attempt)
    {
        const int idx = (deviceIndex + attempt) % static_cast<int>(outputDevices.size());
        if (attempt > 0)
            getLogger()->warn("Requested output index {} failed; trying fallback output index {}", deviceIndex, idx);
        if (tryEnableOnDevice(outputDevices[idx], idx))
        {
            ok = true;
            break;
        }
    }

    for (auto* d : outputDevices) d->Release();
    return ok;

}

int DeckLinkCapture::getNumOutputs() const
{
    int count = 0;
    IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
    if (!deckLinkIterator)
        return 0;

    IDeckLink *deckLink = nullptr;
    while (deckLinkIterator->Next(&deckLink) == S_OK)
    {
        IDeckLinkOutput *deckLinkOutput = nullptr;
        if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void **)&deckLinkOutput) == S_OK)
        {
            ++count;
            deckLinkOutput->Release();
        }
        deckLink->Release();
    }
    deckLinkIterator->Release();
    return count;
}

bool DeckLinkCapture::sendFrameToDecklink(const uint8_t* frame_data, size_t data_size, int width, int height, int pitch)
{
    if (!outputInitialized)
    {
        getLogger()->error("Output not initialized");
        return false;
    }
    if (!deckLinkOutput)
    {
        getLogger()->error("DeckLinkOutput pointer is null");
        return false;
    }

    // Fast path: scheduled playback (non-blocking on DeckLink I/O).
    {
        std::unique_lock<std::mutex> lock(outputMutex_);
        if (outputCallback_ && outputFrameDuration_ != 0 && outputTimeScale_ != 0)
        {
            // If upstream dimensions don't match device mode, refuse rather than corrupt output.
            if (outputExpectedWidth_ != 0 && outputExpectedHeight_ != 0)
            {
                if (width != outputExpectedWidth_ || height != outputExpectedHeight_)
                {
                    getLogger()->error("Output frame size {}x{} does not match enabled DeckLink mode {}x{}",
                                       width, height, outputExpectedWidth_, outputExpectedHeight_);
                    return false;
                }
            }

            if (!ensureScheduledFramePool_(width, height))
            {
                getLogger()->warn("Failed to ensure scheduled frame pool; falling back to synchronous output");
            }
            else
            {
                // Wait for an available reusable output frame.
                outputCv_.wait(lock, [&]{ return !availableOutputFrames_.empty(); });
                IDeckLinkVideoFrame* outFrame = availableOutputFrames_.front();
                availableOutputFrames_.pop_front();
                lock.unlock();

                // Copy into the frame buffer.
                IDeckLinkVideoBuffer* videoBuffer = nullptr;
                void* dstBytes = nullptr;
                if (outFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) != S_OK || !videoBuffer)
                {
                    getLogger()->error("Failed to query IDeckLinkVideoBuffer from scheduled output frame");
                    std::lock_guard<std::mutex> lk(outputMutex_);
                    availableOutputFrames_.push_back(outFrame);
                    outputCv_.notify_one();
                    return false;
                }

                bool copied = false;
                if (videoBuffer->StartAccess(bmdBufferAccessWrite) == S_OK)
                {
                    if (videoBuffer->GetBytes(&dstBytes) == S_OK && dstBytes)
                    {
                        const uint8_t* src = frame_data;
                        uint8_t* dst = static_cast<uint8_t*>(dstBytes);

                        const int dstRowBytes = outputExpectedRowBytes_;
                        const int srcRowBytes = pitch;
                        const int rows = height;
                        const int copyBytes = std::min(dstRowBytes, srcRowBytes);

                        for (int y = 0; y < rows; ++y)
                        {
                            memcpy(dst + (size_t)y * (size_t)dstRowBytes,
                                   src + (size_t)y * (size_t)srcRowBytes,
                                   (size_t)copyBytes);
                        }
                        (void)data_size;
                        copied = true;
                    }
                    videoBuffer->EndAccess(bmdBufferAccessWrite);
                }
                videoBuffer->Release();

                if (!copied)
                {
                    getLogger()->error("Failed to access scheduled output frame bytes");
                    std::lock_guard<std::mutex> lk(outputMutex_);
                    availableOutputFrames_.push_back(outFrame);
                    outputCv_.notify_one();
                    return false;
                }

                // Schedule the frame.
                lock.lock();
                // Keep a small scheduling lead to avoid "late" frames when the sender thread jitters.
                // If we schedule too close to the current stream time, DeckLink will report late/dropped frames
                // and you will observe < 50fps E2E even though average work fits.
                BMDTimeValue streamTime = 0;
                double playbackSpeed = 0.0;
                const HRESULT hrStream = deckLinkOutput->GetScheduledStreamTime(outputTimeScale_, &streamTime, &playbackSpeed);
                if (hrStream != S_OK)
                {
                    // If we can't query stream time, fall back to monotonically increasing scheduling.
                    streamTime = 0;
                }

                const BMDTimeValue minLead = 2 * outputFrameDuration_;
                const BMDTimeValue desiredTime = (streamTime > 0) ? (streamTime + minLead) : nextScheduledTime_;
                if (nextScheduledTime_ < desiredTime)
                    nextScheduledTime_ = desiredTime;

                // Cap the scheduling lead. `nextScheduledTime_` only ever ratchets
                // upward (floor clamp above + monotonic += below), so a momentary
                // producer burst permanently inflates the lead and parks extra
                // frames in the DeckLink driver queue — pure output latency. If the
                // lead has drifted beyond maxLead, drop THIS frame instead of
                // scheduling it: streamTime then advances while nextScheduledTime_
                // holds, draining the lead back toward minLead. No reordering (we
                // never schedule earlier than a pending frame); one dropped frame is
                // imperceptible at 25/50 fps.
                // maxLead is 1 frame above minLead: the lead parks near 2 (from the
                // floor/preroll) and we only drop when it genuinely drifts past 3, so
                // ordinary producer/clock jitter does NOT cause continuous drops.
                const BMDTimeValue maxLead = 3 * outputFrameDuration_;
                if (streamTime > 0 && (nextScheduledTime_ - streamTime) > maxLead)
                {
                    availableOutputFrames_.push_back(outFrame);
                    outputCv_.notify_one();
                    ++outputLeadDrops_;
                    return true;
                }

                const BMDTimeValue displayTime = nextScheduledTime_;
                const BMDTimeValue displayDuration = outputFrameDuration_;
                const BMDTimeScale timeScale = outputTimeScale_;

                const HRESULT hrSchedule = deckLinkOutput->ScheduleVideoFrame(outFrame, displayTime, displayDuration, timeScale);
                if (hrSchedule != S_OK)
                {
                    getLogger()->error("ScheduleVideoFrame failed (hr=0x{:x})", hrSchedule);
                    availableOutputFrames_.push_back(outFrame);
                    outputCv_.notify_one();
                    return false;
                }

                nextScheduledTime_ += outputFrameDuration_;

                if (!scheduledPlaybackStarted_)
                {
                    // Initial preroll: schedule the very first frame a couple of frame
                    // durations into the timeline (matches minLead). Lower preroll =
                    // lower steady-state output latency; the lead cap above keeps it here.
                    if (displayTime == 0)
                    {
                        nextScheduledTime_ = 2 * outputFrameDuration_;
                    }
                    const HRESULT hrStart = deckLinkOutput->StartScheduledPlayback(0, timeScale, 1.0);
                    if (hrStart != S_OK)
                    {
                        getLogger()->error("StartScheduledPlayback failed (hr=0x{:x})", hrStart);
                        // Playback did not start; next calls will retry.
                        scheduledPlaybackStarted_ = false;
                        nextScheduledTime_ = 0;
                        return false;
                    }
                    scheduledPlaybackStarted_ = true;
                }

                return true;
            }
        }
    }

    // Create DeckLink video frame directly from raw bytes (UYVY format)
    IDeckLinkMutableVideoFrame *videoFrame = nullptr;
    HRESULT hr = deckLinkOutput->CreateVideoFrame(
        width, height, pitch, bmdFormat8BitYUV, bmdFrameFlagDefault, &videoFrame);

    if (hr != S_OK || !videoFrame)
    {
        getLogger()->error("Failed to create DeckLink video frame. Error: 0x{:x}", hr);
        return false;
    }

    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    void *frameBytes = nullptr;
    if (videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer) != S_OK || !videoBuffer)
    {
        getLogger()->error("Failed to query IDeckLinkVideoBuffer from output frame");
        videoFrame->Release();
        return false;
    }

    bool copied = false;
    if (videoBuffer->StartAccess(bmdBufferAccessWrite) == S_OK)
    {
        if (videoBuffer->GetBytes(&frameBytes) == S_OK && frameBytes)
        {
            const size_t maxSize = static_cast<size_t>(pitch) * static_cast<size_t>(height);
            memcpy(frameBytes, frame_data, std::min(data_size, maxSize));
            copied = true;
        }
        videoBuffer->EndAccess(bmdBufferAccessWrite);
    }

    videoBuffer->Release();
    if (!copied)
    {
        getLogger()->error("Failed to access output frame bytes");
        videoFrame->Release();
        return false;
    }

    // Display frame synchronously
    hr = deckLinkOutput->DisplayVideoFrameSync(videoFrame);
    videoFrame->Release();

    if (hr != S_OK)
    {
        getLogger()->error("Failed to display video frame synchronously. Error: 0x{:x}", hr);
        return false;
    }

    return true;
}

bool DeckLinkCapture::checkGenlockSupport()
{
    if (!deckLinkAttributes)
    {
        getLogger()->warn("Cannot check genlock support - attributes interface not available");
        genlockSupported = false;
        return false;
    }

    bool hasReferenceInput = false;
    HRESULT result = deckLinkAttributes->GetFlag(BMDDeckLinkHasReferenceInput, &hasReferenceInput);
    
    if (result == S_OK && hasReferenceInput)
    {
        genlockSupported = true;
        getLogger()->info("Device supports genlock reference input");
        return true;
    }
    else
    {
        genlockSupported = false;
        getLogger()->info("Device does not support genlock reference input");
        return false;
    }
}

bool DeckLinkCapture::getReferenceStatus(BMDReferenceStatus& status)
{
    if (!deckLinkStatus)
    {
        getLogger()->error("Cannot get reference status - status interface not available");
        return false;
    }

    if (!genlockSupported)
    {
        getLogger()->warn("Genlock not supported by this device");
        status = bmdReferenceNotSupportedByHardware;
        return false;
    }

    // Check if reference signal is locked using IDeckLinkStatus
    bool isLocked = false;
    HRESULT result = deckLinkStatus->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &isLocked);
    
    if (result != S_OK)
    {
        getLogger()->error("Failed to get reference signal lock status. Error code: 0x{:x}", result);
        return false;
    }

    // Set status based on lock flag
    if (isLocked)
    {
        status = bmdReferenceLocked;
    }
    else
    {
        status = bmdReferenceUnlocked;
    }

    return true;
}

std::string DeckLinkCapture::getReferenceStatusString(BMDReferenceStatus status) const
{
    switch (status)
    {
        case bmdReferenceUnlocked:
            return "Unlocked - Genlock reference lock has not been achieved";
        case bmdReferenceNotSupportedByHardware:
            return "Not Supported - Device does not have a genlock input connector";
        case bmdReferenceLocked:
            return "Locked - Genlock reference lock achieved";
        default:
            return "Unknown status";
    }
}

bool DeckLinkCapture::waitForReferenceLock(int timeoutMs)
{
    if (!genlockSupported)
    {
        getLogger()->warn("Cannot wait for reference lock - genlock not supported");
        return false;
    }

    getLogger()->info("Waiting for genlock reference lock (timeout: {}ms)...", timeoutMs);
    
    auto startTime = std::chrono::steady_clock::now();
    BMDReferenceStatus status;
    
    while (true)
    {
        if (!getReferenceStatus(status))
        {
            return false;
        }

        if (status == bmdReferenceLocked)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            getLogger()->info("Genlock reference locked after {}ms", elapsed);
            return true;
        }
        else if (status == bmdReferenceNotSupportedByHardware)
        {
            getLogger()->error("Genlock not supported by hardware");
            return false;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        
        if (elapsed >= timeoutMs)
        {
            getLogger()->error("Timeout waiting for genlock reference lock. Status: {}", 
                             getReferenceStatusString(status));
            return false;
        }

        // Check every 100ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void DeckLinkCapture::logGenlockStatus()
{
    if (!genlockSupported)
    {
        getLogger()->debug("Genlock not supported by device");
        return;
    }

    BMDReferenceStatus status;
    if (getReferenceStatus(status))
    {
        getLogger()->info("Current genlock status: {}", getReferenceStatusString(status));
    }
}

bool DeckLinkCapture::checkAndConfigureOutputGenlock(IDeckLink* outputDeckLink, IDeckLinkOutput* output)
{
    if (!outputDeckLink || !output)
    {
        getLogger()->error("Invalid output device or output interface");
        return false;
    }

    getLogger()->info("");
    getLogger()->info("═══════════════════════════════════════════════════════════");
    getLogger()->info("  CHECKING OUTPUT GENLOCK STATUS");
    getLogger()->info("═══════════════════════════════════════════════════════════");

    // Check if device supports reference input
    IDeckLinkProfileAttributes *outputAttrs = nullptr;
    if (outputDeckLink->QueryInterface(IID_IDeckLinkProfileAttributes, (void **)&outputAttrs) != S_OK)
    {
        getLogger()->info("Could not query output device attributes");
        return false;
    }

    bool hasReferenceInput = false;
    bool genlockConfigured = false;
    
    if (outputAttrs->GetFlag(BMDDeckLinkHasReferenceInput, &hasReferenceInput) == S_OK && hasReferenceInput)
    {
        getLogger()->info("Output device supports genlock - will sync to reference input");
        
        // Use IDeckLinkOutput::GetReferenceStatus() instead of IDeckLinkStatus
        BMDReferenceStatus refStatus;
        if (output->GetReferenceStatus(&refStatus) == S_OK)
        {
            getLogger()->info("Output genlock status: {}", getReferenceStatusString(refStatus));
            getLogger()->info("");
            
            if (!(refStatus & bmdReferenceLocked))
            {
                getLogger()->warn("Output is not locked to genlock reference. Waiting for lock...");
                
                // Wait up to 5 seconds for genlock lock
                auto startTime = std::chrono::steady_clock::now();
                bool locked = false;
                
                while (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime).count() < 5000)
                {
                    if (output->GetReferenceStatus(&refStatus) == S_OK && 
                        (refStatus & bmdReferenceLocked))
                    {
                        locked = true;
                        getLogger()->info("Output genlock reference locked!");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                
                if (!locked)
                {
                    getLogger()->warn("Output genlock reference lock timeout. Output may not be synchronized.");
                }
                else
                {
                    genlockConfigured = true;
                }
            }
            else
            {
                getLogger()->info("Output is already locked to genlock reference.");
                genlockConfigured = true;
            }
        }
    }
    else
    {
        getLogger()->warn("Output device does not support genlock reference input");
    }
    
    outputAttrs->Release();
    return genlockConfigured;
}

std::string DeckLinkCapture::getReferenceSignalModeName(BMDDisplayMode mode) const
{
    switch (mode)
    {
        // SD modes
        case bmdModePAL: return "PAL (576i50)";
        case bmdModeNTSC: return "NTSC (480i59.94)";
        case bmdModeNTSC2398: return "NTSC 23.98";
        
        // HD 720p modes
        case bmdModeHD720p50: return "720p50";
        case bmdModeHD720p5994: return "720p59.94";
        case bmdModeHD720p60: return "720p60";
        
        // HD 1080p modes
        case bmdModeHD1080p2398: return "1080p23.98";
        case bmdModeHD1080p24: return "1080p24";
        case bmdModeHD1080p25: return "1080p25";
        case bmdModeHD1080p2997: return "1080p29.97";
        case bmdModeHD1080p30: return "1080p30";
        case bmdModeHD1080p50: return "1080p50";
        case bmdModeHD1080p5994: return "1080p59.94";
        case bmdModeHD1080p6000: return "1080p60";
        
        // HD 1080i modes
        case bmdModeHD1080i50: return "1080i50";
        case bmdModeHD1080i5994: return "1080i59.94";
        case bmdModeHD1080i6000: return "1080i60";
        
        // 4K UHD modes
        case bmdMode4K2160p2398: return "4K2160p23.98";
        case bmdMode4K2160p24: return "4K2160p24";
        case bmdMode4K2160p25: return "4K2160p25";
        case bmdMode4K2160p2997: return "4K2160p29.97";
        case bmdMode4K2160p30: return "4K2160p30";
        case bmdMode4K2160p50: return "4K2160p50";
        case bmdMode4K2160p5994: return "4K2160p59.94";
        case bmdMode4K2160p60: return "4K2160p60";
        
        default: return "Unknown";
    }
}

bool DeckLinkCapture::checkReferenceSignalCompatibility()
{
    if (!genlockSupported)
    {
        getLogger()->debug("Genlock: Not Supported");
        // Update SharedState
        if (shared_state_) {
            GenlockState::Status status;
            status.is_locked = false;
            status.ref_format = "Not Supported";
            status.last_update = std::chrono::system_clock::now();
            shared_state_->setData(status);
        }
        return false;
    }

    if (!deckLinkOutput)
    {
        getLogger()->debug("Genlock: Output device not initialized");
        // Update SharedState
        if (shared_state_) {
            GenlockState::Status status;
            status.is_locked = false;
            status.ref_format = "Output Not Init";
            status.last_update = std::chrono::system_clock::now();
            shared_state_->setData(status);
        }
        return false;
    }

    // Get the reference/genlock status using DeckLink API
    BMDReferenceStatus referenceStatus;
    HRESULT result = deckLinkOutput->GetReferenceStatus(&referenceStatus);
    
    if (result != S_OK)
    {
        getLogger()->warn("Genlock: Failed to get reference status");
        // Update SharedState
        if (shared_state_) {
            GenlockState::Status status;
            status.is_locked = false;
            status.ref_format = "Check Failed";
            status.last_update = std::chrono::system_clock::now();
            shared_state_->setData(status);
        }
        return false;
    }

    // Check the status bits
    if (referenceStatus & bmdReferenceNotSupportedByHardware)
    {
        getLogger()->debug("Genlock: Not supported by hardware");
        // Update SharedState
        if (shared_state_) {
            GenlockState::Status status;
            status.is_locked = false;
            status.ref_format = "Not Supported";
            status.last_update = std::chrono::system_clock::now();
            shared_state_->setData(status);
        }
        return false;
    }
    
    // Try to get reference signal format (works even if not locked yet)
    std::string refFormat = "No Signal";
    
    if (deckLinkStatus)
    {
        int64_t refSignalMode = 0;
        HRESULT modeResult = deckLinkStatus->GetInt(bmdDeckLinkStatusReferenceSignalMode, &refSignalMode);
        if (modeResult == S_OK)
        {
            BMDDisplayMode referenceMode = (BMDDisplayMode)refSignalMode;
            refFormat = getReferenceSignalModeName(referenceMode);
        }
    }
    
    bool is_locked = (referenceStatus & bmdReferenceLocked);
    
    // Update SharedState
    if (shared_state_) {
        GenlockState::Status status;
        status.is_locked = is_locked;
        status.ref_format = refFormat;
        status.last_update = std::chrono::system_clock::now();
        shared_state_->setData(status);
    }
    
    if (is_locked)
    {
        getLogger()->info("Genlock: LOCKED | {}", refFormat);
        return true;
    }
    else
    {
        getLogger()->warn("Genlock: UNLOCKED | CHECK BMD APP");
        return false;
    }
}

// =============================================================================
//  Second output slot (slot 1 / SDI B) — mirrors slot-0 logic.
// =============================================================================

void DeckLinkCapture::recycleSlot1Frame_(IDeckLinkVideoFrame* f)
{
    std::lock_guard<std::mutex> lock(out1_.mutex);
    out1_.available.push_back(f);
    out1_.cv.notify_one();
}

void DeckLinkCapture::slot1PlaybackStopped_()
{
    std::lock_guard<std::mutex> lock(out1_.mutex);
    out1_.playing = false;
    out1_.nextTime = 0;
}

void DeckLinkCapture::teardownOutputSlot1_()
{
    std::unique_lock<std::mutex> lock(out1_.mutex);
    if (out1_.output) {
        if (out1_.playing && out1_.timeScale != 0) {
            BMDTimeValue act = 0;
            out1_.output->StopScheduledPlayback(0, &act, out1_.timeScale);
        }
        if (out1_.callback)
            out1_.output->SetScheduledFrameCompletionCallback(nullptr);
    }
    out1_.playing = false; out1_.nextTime = 0;
    out1_.frameDuration = 0; out1_.timeScale = 0;
    out1_.w = 0; out1_.h = 0; out1_.rowbytes = 0;
    out1_.available.clear();
    for (auto* f : out1_.all) if (f) f->Release();
    out1_.all.clear();
    lock.unlock();
    if (out1_.callback) { out1_.callback->Release(); out1_.callback = nullptr; }
    if (out1_.output) {
        out1_.output->DisableVideoOutput();
        out1_.output->Release();
        out1_.output = nullptr;
    }
    out1_.initialized = false;
}

bool DeckLinkCapture::ensureOutputSlot1Pool_(int w, int h)
{
    if (!out1_.output) return false;
    if (out1_.w == w && out1_.h == h && out1_.rowbytes == w*2 && !out1_.all.empty()) return true;
    out1_.available.clear();
    for (auto* f : out1_.all) if (f) f->Release();
    out1_.all.clear();
    out1_.w = w; out1_.h = h; out1_.rowbytes = w*2;
    for (int i = 0; i < OutputSlot1::kPoolSize; ++i) {
        IDeckLinkMutableVideoFrame* frame = nullptr;
        if (out1_.output->CreateVideoFrame(w, h, w*2, bmdFormat8BitYUV,
                                           bmdFrameFlagDefault, &frame) != S_OK || !frame)
        { getLogger()->error("Slot1: failed to create output frame"); return false; }
        out1_.all.push_back(frame);
        out1_.available.push_back(frame);
    }
    return true;
}

bool DeckLinkCapture::setupOutputSlot1_(int w, int h)
{
    if (!out1_.output) return false;
    if (!out1_.callback)
        out1_.callback = new DeckLinkOutputCallback1(this);
    if (out1_.output->SetScheduledFrameCompletionCallback(out1_.callback) != S_OK) return false;

    IDeckLinkDisplayModeIterator* it = nullptr;
    if (out1_.output->GetDisplayModeIterator(&it) != S_OK || !it) return false;
    bool found = false;
    IDeckLinkDisplayMode* m = nullptr;
    while (it->Next(&m) == S_OK && m) {
        if (m->GetDisplayMode() == out1_.displayMode) {
            out1_.w  = m->GetWidth(); out1_.h = m->GetHeight(); out1_.rowbytes = out1_.w*2;
            m->GetFrameRate(&out1_.frameDuration, &out1_.timeScale);
            found = true; m->Release(); break;
        }
        m->Release(); m = nullptr;
    }
    it->Release();
    if (!found || out1_.frameDuration == 0 || out1_.timeScale == 0) return false;
    return ensureOutputSlot1Pool_(out1_.w, out1_.h);
}

bool DeckLinkCapture::initializeOutput(int deviceIndex, int slotIndex)
{
    if (slotIndex == 0) return initializeOutput(deviceIndex);
    if (slotIndex != 1) { getLogger()->error("initializeOutput: invalid slotIndex {}", slotIndex); return false; }

    disableOutput(1);

    // Determine output mode (same logic as slot 0)
    if (currentDisplayMode == bmdModeHD1080i50)        out1_.displayMode = bmdModeHD1080i50;
    else if (currentDisplayMode == bmdModeHD1080i5994) out1_.displayMode = bmdModeHD1080i5994;
    else if (currentDisplayMode == bmdModeHD1080i6000) out1_.displayMode = bmdModeHD1080i6000;
    else if (currentDisplayMode != bmdModeUnknown)     out1_.displayMode = currentDisplayMode;
    else                                                out1_.displayMode = bmdModeHD1080p25;

    IDeckLinkIterator* iter = CreateDeckLinkIteratorInstance();
    if (!iter) return false;
    std::vector<IDeckLink*> devs;
    IDeckLink* dev = nullptr;
    while (iter->Next(&dev) == S_OK) {
        IDeckLinkOutput* tmp = nullptr;
        if (dev->QueryInterface(IID_IDeckLinkOutput,(void**)&tmp)==S_OK && tmp) { tmp->Release(); devs.push_back(dev); }
        else dev->Release();
    }
    iter->Release();
    if (devs.empty() || deviceIndex < 0 || deviceIndex >= (int)devs.size()) {
        for (auto* d:devs) d->Release();
        getLogger()->error("Slot1 output device {} not found", deviceIndex);
        return false;
    }
    devs[deviceIndex]->QueryInterface(IID_IDeckLinkOutput,(void**)&out1_.output);
    for (auto* d:devs) d->Release();
    if (!out1_.output) { getLogger()->error("Slot1: QueryInterface IDeckLinkOutput failed"); return false; }

    HRESULT hr = out1_.output->EnableVideoOutput(out1_.displayMode, bmdVideoOutputFlagDefault);
    if (hr != S_OK) { getLogger()->error("Slot1 EnableVideoOutput failed (hr=0x{:x})", hr); out1_.output->Release(); out1_.output=nullptr; return false; }

    setupOutputSlot1_(0, 0); // non-fatal if scheduled setup fails
    out1_.initialized = true;
    getLogger()->info("DeckLink slot 1 output initialized (device {})", deviceIndex);
    return true;
}

bool DeckLinkCapture::sendFrameToDecklinkSlot(int slot, const uint8_t* data,
                                               size_t size, int w, int h, int pitch)
{
    if (slot == 0) return sendFrameToDecklink(data, size, w, h, pitch);
    if (slot != 1) return false;
    if (!out1_.initialized || !out1_.output) return false;

    // Scheduled path
    {
        std::unique_lock<std::mutex> lock(out1_.mutex);
        if (out1_.callback && out1_.frameDuration != 0 && out1_.timeScale != 0) {
            if (out1_.w != 0 && out1_.h != 0 && (w != out1_.w || h != out1_.h)) {
                getLogger()->error("Slot1 frame size mismatch"); return false;
            }
            if (!ensureOutputSlot1Pool_(w, h)) goto sync_path;
            out1_.cv.wait(lock, [&]{ return !out1_.available.empty(); });
            IDeckLinkVideoFrame* frame = out1_.available.front();
            out1_.available.pop_front();
            lock.unlock();

            IDeckLinkVideoBuffer* vb = nullptr;
            void* dst = nullptr;
            if (frame->QueryInterface(IID_IDeckLinkVideoBuffer,(void**)&vb)!=S_OK||!vb) {
                std::lock_guard<std::mutex> lk(out1_.mutex);
                out1_.available.push_back(frame); out1_.cv.notify_one(); return false;
            }
            bool copied = false;
            if (vb->StartAccess(bmdBufferAccessWrite)==S_OK) {
                if (vb->GetBytes(&dst)==S_OK && dst) {
                    for (int y=0;y<h;++y)
                        memcpy((uint8_t*)dst+y*(size_t)out1_.rowbytes, data+y*(size_t)pitch, std::min(out1_.rowbytes,pitch));
                    copied = true;
                }
                vb->EndAccess(bmdBufferAccessWrite);
            }
            vb->Release();
            if (!copied) { std::lock_guard<std::mutex> lk(out1_.mutex); out1_.available.push_back(frame); out1_.cv.notify_one(); return false; }

            lock.lock();
            BMDTimeValue streamTime=0; double speed=0;
            if (out1_.output->GetScheduledStreamTime(out1_.timeScale,&streamTime,&speed)!=S_OK) streamTime=0;
            BMDTimeValue minLead = 2*out1_.frameDuration;
            BMDTimeValue desired = streamTime>0 ? streamTime+minLead : out1_.nextTime;
            if (out1_.nextTime < desired) out1_.nextTime = desired;
            // Cap the scheduling lead (see slot0 for rationale): drop this frame to
            // drain an over-deep lead rather than parking latency in the driver queue.
            const BMDTimeValue maxLead = 3*out1_.frameDuration;
            if (streamTime > 0 && (out1_.nextTime - streamTime) > maxLead)
            {
                out1_.available.push_back(frame); out1_.cv.notify_one();
                ++out1_.leadDrops;
                return true;
            }
            const HRESULT hs = out1_.output->ScheduleVideoFrame(frame, out1_.nextTime, out1_.frameDuration, out1_.timeScale);
            if (hs != S_OK) { getLogger()->error("Slot1 ScheduleVideoFrame failed"); out1_.available.push_back(frame); out1_.cv.notify_one(); return false; }
            out1_.nextTime += out1_.frameDuration;
            if (!out1_.playing) {
                if (out1_.nextTime == out1_.frameDuration) out1_.nextTime = 2*out1_.frameDuration;
                if (out1_.output->StartScheduledPlayback(0, out1_.timeScale, 1.0) != S_OK) { out1_.playing=false; out1_.nextTime=0; return false; }
                out1_.playing = true;
            }
            return true;
        }
    }

sync_path:
    // Synchronous fallback
    IDeckLinkMutableVideoFrame* vf = nullptr;
    if (out1_.output->CreateVideoFrame(w,h,pitch,bmdFormat8BitYUV,bmdFrameFlagDefault,&vf)!=S_OK||!vf) return false;
    IDeckLinkVideoBuffer* vb2=nullptr;
    void* dst2=nullptr;
    bool ok=false;
    if (vf->QueryInterface(IID_IDeckLinkVideoBuffer,(void**)&vb2)==S_OK && vb2) {
        if (vb2->StartAccess(bmdBufferAccessWrite)==S_OK) {
            if (vb2->GetBytes(&dst2)==S_OK && dst2) { memcpy(dst2,data,std::min(size,(size_t)pitch*h)); ok=true; }
            vb2->EndAccess(bmdBufferAccessWrite);
        }
        vb2->Release();
    }
    if (!ok) { vf->Release(); return false; }
    out1_.output->DisplayVideoFrameSync(vf);
    vf->Release();
    return true;
}

// ⚠️ DEPRECATED: Use checkReferenceSignalCompatibility() instead for more reliable genlock checking
void DeckLinkCapture::logCompactGenlockStatus()
{
    // This function has been deprecated in favor of checkReferenceSignalCompatibility()
    // which provides more reliable and detailed genlock status checking
    getLogger()->warn("logCompactGenlockStatus() is deprecated. Use checkReferenceSignalCompatibility() instead.");
    checkReferenceSignalCompatibility();
}
