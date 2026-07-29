#include "ingest.hpp"

#include <cuda_runtime.h>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>

#include "logger.h"
#include "decklink_capture.hpp"

#ifdef STIQY_ENABLE_DELTACAST
#include "deltacast_capture.cuh"
#endif

namespace {
std::shared_ptr<spdlog::logger> getIngestLogger() {
    static std::shared_ptr<spdlog::logger> logger = getModuleLogger("ingest");
    return logger;
}

struct VideoRateSettings {
    float ideal_fps = 50.0f;
    float fps_threshold = 47.0f;
    bool interlaced = false;
};

// DeckLink's display-mode name contains the signal rate and scan type, for
// example "1080p50" or "1080i50". The latter is delivered as 25 video frames
// per second because one frame contains both 50 Hz fields.
VideoRateSettings getVideoRateSettings(const std::string& display_mode_name) {
    const auto scan_type_pos = display_mode_name.find_first_of("iIpP");
    if (scan_type_pos == std::string::npos || scan_type_pos + 1 >= display_mode_name.size()) {
        return {};
    }

    char* parse_end = nullptr;
    const float signal_rate = std::strtof(display_mode_name.c_str() + scan_type_pos + 1, &parse_end);
    if (parse_end == display_mode_name.c_str() + scan_type_pos + 1 || signal_rate <= 0.0f) {
        return {};
    }

    const bool interlaced = display_mode_name[scan_type_pos] == 'i' ||
                            display_mode_name[scan_type_pos] == 'I';
    const float ideal_fps = interlaced ? signal_rate * 0.5f : signal_rate;

    VideoRateSettings settings;
    settings.ideal_fps = ideal_fps;
    settings.fps_threshold = ideal_fps * 0.94f;
    settings.interlaced = interlaced;
    return settings;
}

bool isDevicePointer(const void* ptr) {
#if CUDART_VERSION >= 10000
    cudaPointerAttributes attr{};
    if (cudaPointerGetAttributes(&attr, ptr) != cudaSuccess) {
        return false;
    }
    return attr.type == cudaMemoryTypeDevice || attr.type == cudaMemoryTypeManaged;
#else
    cudaPointerAttributes attr{};
    if (cudaPointerGetAttributes(&attr, ptr) != cudaSuccess) {
        return false;
    }
    return attr.memoryType == cudaMemoryTypeDevice;
#endif
}

bool copyToDevice(void* dst_device, const void* src, size_t size) {
    if (!dst_device || !src || size == 0) {
        return false;
    }
    cudaError_t err = cudaMemcpy(dst_device, src, size, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        getIngestLogger()->error("cudaMemcpy H2D failed: {}", cudaGetErrorString(err));
        return false;
    }

    // ── Force the H2D DMA to actually COMPLETE before the frame is published ──
    // `src` here is pageable host memory (a std::vector). Per CUDA's documented
    // synchronization behaviour, an H2D cudaMemcpy from PAGEABLE memory returns
    // once the source has been staged for DMA — the DMA into `dst_device` may
    // still be in flight, and it is only ordered against later work on this same
    // (default/null) stream. The AI-keyer / RTM-segmentation consumers read this
    // buffer on their OWN non-blocking streams, so without a host-side barrier
    // they can sample `dst_device` while its bottom rows are still unwritten.
    // With buffer-pool reuse disabled (see DeviceBufferPool) those unwritten rows
    // are freshly-malloc'd zeros → a GREEN band at the bottom of the frame (it was
    // the *previous* frame back when the pool recycled buffers — same root cause).
    // The window is timing-dependent: it opens under GPU-utilisation spikes when
    // the throttled laptop clock (~1920 MHz, can't reach 3000) stretches the DMA.
    // cudaStreamSynchronize(0) blocks the host until the copy DMA is physically
    // done, so any consumer on any stream that starts afterwards sees a complete
    // buffer. Cost is only the tail of an already-issued ~4 MB DMA (sub-millisecond),
    // well within the pipeline's frame budget.
    err = cudaStreamSynchronize(0);
    if (err != cudaSuccess) {
        getIngestLogger()->error("cudaStreamSynchronize after H2D failed: {}", cudaGetErrorString(err));
        return false;
    }
    return true;
}
}

class DeckLinkIngest final : public IIngest {
public:
    bool initialize(const IngestConfig& config) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        config_ = config;
        if (!capture_.initialize(config.input_device)) {
            getIngestLogger()->error("DeckLink initialize failed for device {}", config.input_device);
            return false;
        }
        if (shared_state_) {
            capture_.setSharedState(shared_state_);
        }
        return true;
    }

    bool startCapture() override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!capture_.startCapture()) {
            getIngestLogger()->error("DeckLink startCapture failed");
            return false;
        }
        return true;
    }

    void stopCapture() override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        capture_.stopCapture();
    }

    bool reconfigure(const IngestConfig& config) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        getIngestLogger()->info(
            "Runtime ingest reconfigure requested (backend=decklink, input={}, output={}, enable_output={})",
            config.input_device,
            config.output_device,
            config.enable_output);

        capture_.stopCapture();

        if (!capture_.initialize(config.input_device)) {
            getIngestLogger()->error("DeckLink runtime initialize failed for input {}", config.input_device);
            return false;
        }

        if (shared_state_) {
            capture_.setSharedState(shared_state_);
        }

        if (!capture_.startCapture()) {
            getIngestLogger()->error("DeckLink runtime startCapture failed");
            return false;
        }

        if (config.enable_output) {
            if (!capture_.initializeOutput(config.output_device)) {
                getIngestLogger()->error("DeckLink runtime initializeOutput failed for output {}", config.output_device);
                return false;
            }
        }

        config_ = config;
        getIngestLogger()->info(
            "Runtime ingest reconfigure applied (backend=decklink, input={}, output={})",
            config_.input_device,
            config_.output_device);

        return true;
    }

    bool getFrame(std::shared_ptr<FrameData>& out_frame) override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<uint8_t> frameBuffer;
        int width = 0;
        int height = 0;
        if (!capture_.getFrameBuffer(frameBuffer, width, height)) {
            return false;
        }

        const int pitch = capture_.getLastFramePitch();
        const size_t copy_size = static_cast<size_t>(pitch) * static_cast<size_t>(height);
        if (frameBuffer.size() < copy_size || width <= 0 || height <= 0) {
            getIngestLogger()->warn("DeckLink frame invalid (w={}, h={}, pitch={}, size={})",
                                    width, height, pitch, frameBuffer.size());
            return false;
        }

        auto frame = std::make_shared<FrameData>();
        frame->uyvy_frame = std::make_unique<UYVYFrame>();
        if (!frame->uyvy_frame->allocate(width, height, pitch)) {
            getIngestLogger()->error("Failed to allocate UYVYFrame ({}x{}, pitch={})", width, height, pitch);
            return false;
        }

        if (!copyToDevice(frame->uyvy_frame->d_data, frameBuffer.data(), copy_size)) {
            return false;
        }

        out_frame = std::move(frame);
        return true;
    }

    bool initializeOutput(int deviceIndex) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return capture_.initializeOutput(deviceIndex);
    }

    bool initializeOutput(int deviceIndex, int slotIndex) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return capture_.initializeOutput(deviceIndex, slotIndex);
    }

    bool disableOutput(int slotIndex) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return capture_.disableOutput(slotIndex);
    }

    bool sendFrameToOutput(const uint8_t* data,
                           size_t size,
                           int width,
                           int height,
                           int pitch) override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return capture_.sendFrameToDecklink(data, size, width, height, pitch);
    }

    bool sendFrameToOutput(const uint8_t* data,
                           size_t size,
                           int width,
                           int height,
                           int pitch,
                           int slotIndex) override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return capture_.sendFrameToDecklinkSlot(slotIndex, data, size, width, height, pitch);
    }

    int getNumOutputs() const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return capture_.getNumOutputs();
    }

    IngestVideoInfo getVideoInfo() const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        IngestVideoInfo info;
        capture_.getCurrentResolution(info.width, info.height);
        info.pitch = capture_.getLastFramePitch();
        info.display_mode_name = capture_.getDisplayModeName();
        info.signal_detected = capture_.hasFrame();
        const auto rate_settings = getVideoRateSettings(info.display_mode_name);
        info.ideal_fps = rate_settings.ideal_fps;
        info.fps_threshold = rate_settings.fps_threshold;
        info.interlaced = rate_settings.interlaced;
        return info;
    }

    std::string backendName() const override {
        return "decklink";
    }

    void setSharedState(SharedState* state) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        shared_state_ = state;
        capture_.setSharedState(state);
    }

private:
    DeckLinkCapture capture_{};
    IngestConfig config_{};
    SharedState* shared_state_ = nullptr;
    mutable std::shared_mutex mutex_;
};

#ifdef STIQY_ENABLE_DELTACAST
class DeltacastIngest final : public IIngest {
public:
    bool initialize(const IngestConfig& config) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        config_ = config;
        if (!capture_.initialize(config.input_device)) {
            getIngestLogger()->error("Deltacast initialize failed for device {}", config.input_device);
            return false;
        }
        return true;
    }

    bool startCapture() override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!capture_.startCapture()) {
            getIngestLogger()->error("Deltacast startCapture failed");
            return false;
        }
        return true;
    }

    void stopCapture() override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        capture_.stopCapture();
    }

    bool reconfigure(const IngestConfig& config) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        capture_.stopCapture();
        if (!capture_.initialize(config.input_device)) {
            getIngestLogger()->error("Deltacast runtime initialize failed for input {}", config.input_device);
            return false;
        }
        if (!capture_.startCapture()) {
            getIngestLogger()->error("Deltacast runtime startCapture failed");
            return false;
        }
        if (config.enable_output && !capture_.initializeOutput(config.output_device)) {
            getIngestLogger()->error("Deltacast runtime initializeOutput failed for output {}", config.output_device);
            return false;
        }

        config_ = config;
        return true;
    }

    bool getFrame(std::shared_ptr<FrameData>& out_frame) override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        unsigned char* d_or_h_buffer = nullptr;
        size_t bufferSize = 0;
        if (!capture_.getGPUFrame(&d_or_h_buffer, bufferSize)) {
            return false;
        }

        const int width = capture_.getFrameWidth();
        const int height = capture_.getFrameHeight();
        if (width <= 0 || height <= 0 || bufferSize == 0) {
            capture_.markFrameConsumed();
            return false;
        }

        const int pitch = static_cast<int>(bufferSize / static_cast<size_t>(height));
        auto frame = std::make_shared<FrameData>();
        frame->uyvy_frame = std::make_unique<UYVYFrame>();
        if (!frame->uyvy_frame->allocate(width, height, pitch)) {
            capture_.markFrameConsumed();
            getIngestLogger()->error("Failed to allocate UYVYFrame for Deltacast ({}x{}, pitch={})", width, height, pitch);
            return false;
        }

        const bool device_ptr = isDevicePointer(d_or_h_buffer);
        cudaError_t err = cudaMemcpy(frame->uyvy_frame->d_data,
                                     d_or_h_buffer,
                                     static_cast<size_t>(pitch) * static_cast<size_t>(height),
                                     device_ptr ? cudaMemcpyDeviceToDevice : cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            capture_.markFrameConsumed();
            getIngestLogger()->error("Deltacast copy failed: {}", cudaGetErrorString(err));
            return false;
        }

        // Ensure the copy DMA has completed before publishing the frame, so
        // consumers on non-blocking streams never sample a half-written buffer
        // (green-band tear under GPU-util spikes — see copyToDevice for the full
        // rationale). Covers both the pageable-H2D and the D2D case above.
        err = cudaStreamSynchronize(0);
        if (err != cudaSuccess) {
            capture_.markFrameConsumed();
            getIngestLogger()->error("Deltacast copy sync failed: {}", cudaGetErrorString(err));
            return false;
        }

        capture_.markFrameConsumed();
        out_frame = std::move(frame);
        return true;
    }

    bool initializeOutput(int deviceIndex) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return capture_.initializeOutput(deviceIndex);
    }

    bool initializeOutput(int deviceIndex, int /*slotIndex*/) override {
        // Deltacast does not support dual independent outputs; fall through to primary.
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return capture_.initializeOutput(deviceIndex);
    }

    bool disableOutput(int /*slotIndex*/) override {
        return true;
    }

    bool sendFrameToOutput(const uint8_t* data,
                           size_t size,
                           int width,
                           int height,
                           int pitch) override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (!data || size == 0) {
            return false;
        }

        const bool device_ptr = isDevicePointer(data);
        if (device_ptr) {
            return capture_.sendFrameToDeltacastTx(data, size, width, height);
        }

        if (!ensureOutputStaging(size)) {
            return false;
        }

        cudaError_t err = cudaMemcpy(output_staging_.get(), data, size, cudaMemcpyHostToDevice);
        if (err != cudaSuccess) {
            getIngestLogger()->error("Deltacast output staging copy failed: {}", cudaGetErrorString(err));
            return false;
        }

        return capture_.sendFrameToDeltacastTx(output_staging_.get(), size, width, height);
    }

    bool sendFrameToOutput(const uint8_t* data, size_t size, int width, int height,
                           int pitch, int /*slotIndex*/) override {
        // Deltacast single output — ignore slotIndex
        return sendFrameToOutput(data, size, width, height, pitch);
    }

    int getNumOutputs() const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return capture_.getNumOutputs();
    }

    IngestVideoInfo getVideoInfo() const override {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        IngestVideoInfo info;
        info.width = capture_.getFrameWidth();
        info.height = capture_.getFrameHeight();
        if (info.width > 0 && info.height > 0) {
            info.pitch = info.width * 2;
        }
        info.display_mode_name = "Deltacast";
        info.signal_detected = capture_.hasFrame();
        return info;
    }

    std::string backendName() const override {
        return "deltacast";
    }

    void setSharedState(SharedState* state) override {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        shared_state_ = state;
    }

private:
    struct CudaDeleter {
        void operator()(void* ptr) const {
            if (ptr) {
                cudaFree(ptr);
            }
        }
    };

    bool ensureOutputStaging(size_t size) {
        if (output_staging_ && output_staging_size_ >= size) {
            return true;
        }
        output_staging_.reset();
        void* ptr = nullptr;
        cudaError_t err = cudaMalloc(&ptr, size);
        if (err != cudaSuccess) {
            getIngestLogger()->error("Failed to allocate Deltacast output staging: {}", cudaGetErrorString(err));
            return false;
        }
        output_staging_.reset(ptr);
        output_staging_size_ = size;
        return true;
    }

    DeltacastCapture capture_{};
    IngestConfig config_{};
    SharedState* shared_state_ = nullptr;
    std::unique_ptr<void, CudaDeleter> output_staging_{nullptr};
    size_t output_staging_size_ = 0;
    mutable std::shared_mutex mutex_;
};
#endif

std::unique_ptr<IIngest> CreateIngest(const IngestConfig& config) {
    if (config.backend == "decklink") {
        return std::make_unique<DeckLinkIngest>();
    }

#ifdef STIQY_ENABLE_DELTACAST
    if (config.backend == "deltacast") {
        return std::make_unique<DeltacastIngest>();
    }
#else
    if (config.backend == "deltacast") {
        getIngestLogger()->error("Deltacast backend requested but not compiled in (enable STIQY_ENABLE_DELTACAST)");
        return nullptr;
    }
#endif

    getIngestLogger()->error("Unknown ingest backend: {}", config.backend);
    return nullptr;
}
