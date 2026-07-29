#pragma once

// (Includes are the same as before)
#include <opencv2/opencv.hpp>
#include <chrono>
#include <optional>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <string>
#include <map>
#include <any>
#include <typeindex>
#include <array>
#include <opencv2/cudaimgproc.hpp>
#include <GL/glew.h>  // Add this for GLuint
#include <cuda_runtime.h>  // Add for CUDA memory management

// UI state shared by all sports for the complete Tracking Controls panel.
// Keeping this outside any sport-specific configuration prevents individual
// Tracking Controls sections from acquiring separate lock states.
namespace TrackingControlsState {
    struct Config {
        bool locked = false;
    };
}



// (Module data structs like PlayerTrackerData, PerspectiveData, etc., are unchanged)
struct PlayerTrackerData
{
    int players_found;
};
struct PointTrackerData
{
    int points_tracked;
};

struct ChromaData
{
    // Texture ID for the final R8 alpha mask used by the compositor (when NOT in preview mode)
    GLuint alpha_mask_texture_id = 0;

    // When preview mode is active, the shader writes FragColor -> preview_texture (RGBA)
    // and FragMask -> preview_texture_mask (R8). These IDs are provided to the UI/renderer.
    GLuint preview_texture_id = 0;

    // AI segmentation mask (R8, player=1/background=0) for AI-only and Hybrid keying modes.
    GLuint ai_mask_texture_id = 0;

    // Raw UYVY texture uploaded by the chroma keyer (shared with UI to avoid re-uploads)
    GLuint uyvy_texture_id = 0;

    // GL fence signalled when the chroma keyer has finished rendering the textures
    // above. Consumers in a DIFFERENT shared GL context (the compositor thread)
    // must glWaitSync() on this before sampling them, otherwise on drivers that
    // don't implicitly serialize cross-context access (RTX Ada) they can latch a
    // half-rendered texture — top rows frame N, bottom rows frame N-1 (motion tear).
    // Owned by the ChromaKey pool (one per slot); consumers wait but never delete.
    GLsync render_fence = nullptr;

    // True when preview_texture_* are valid and should be used by the UI/preview pipeline.
    bool preview_enabled = false;
};

struct CompositorData {
    GLuint output_texture_id;
    bool is_composition_active = false;
    bool show_compositor_output = false;
    uint64_t timestamp_ms = 0;
};

// Lightweight per-frame segmentation matte quality diagnostics.
struct KeyingMetricsData {
    uint64_t frame_id = 0;
    bool has_mask = false;
    int mask_non_zero = 0;
    int edge_pixels = 0;
    int changed_pixels = 0;
    int width = 0;
    int height = 0;
    float mask_coverage = 0.0f;   // non-zero / total pixels
    float edge_density = 0.0f;    // edge pixels / mask_non_zero pixels
    float temporal_delta = 0.0f;  // changed pixels / total pixels vs previous mask
};


struct PerspectiveData
{
    int perspective_changes;
    bool transform_applied = false;
    cv::Mat perspective_matrix;  // Add transformation matrix
    std::vector<cv::Point2f> tracked_boundary; // NEW: For visualization
    bool is_editing = false; // NEW: Indicates if in editing mode
    bool has_valid_boundary = false;
    bool homography_valid = false;
    cv::Mat homography;
    bool pose_valid = false;
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float pan_deg = 0.0f;
    float tilt_deg = 0.0f;
    float twist_deg = 0.0f;
    float view_angle_deg = 0.0f;
    float aspect_ratio = 16.0f / 9.0f;
    float lens_k1 = 0.0f;
    float lens_k2 = 0.0f;
    float lens_center_x = 0.0f;
    float lens_center_y = 0.0f;
};

// Sponsor graphic data for non-calibrated camera tracking (NVIDIA OFA grid tracker)
struct SponsorGraphicData {
    GLuint texture_id = 0;                          // OpenGL texture ID
    std::array<cv::Point2f, 4> quad;                // Warped quad corners (video coordinates)
    bool is_valid = false;                          // Whether to render this graphic
    uint64_t frame_id = 0;                          // Associated frame ID
    cv::Mat image_data;                             // Original image data for compositor
};

// Second sponsor graphic (slot 2). Distinct type so it is keyed separately in
// the per-frame module-data map. Tracked by warping graphic 1's
// reference->current homography onto graphic 2's placement quad (shared tracker).
struct SponsorGraphic2Data : SponsorGraphicData {};


// Add a new struct to hold raw UYVY data
struct UYVYFrame {
    void* d_data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    int pitch = 0;  // Use actual DeckLink rowBytes
    int actual_width_bytes = 0;  // Add this for actual data width

private:
    struct DeviceBuffer {
        void* ptr = nullptr;
        size_t bytes = 0;
    };

    struct DeviceBufferPool {
        std::mutex mutex;
        std::vector<DeviceBuffer> free_list;
        size_t max_buffers = 8;

        // ── Pool reuse DISABLED — fixes the frame tear ────────────────────────
        // This pool handed the same device buffer to two in-flight frames: a new
        // capture's H2D copyToDevice overwrote a buffer while a previous frame's
        // consumer was still reading it (async reads on non-blocking streams that
        // outlive the FrameData). Result: the previous frame welded onto the bottom
        // of the current one — a motion-only, timing/GPU-dependent tear that hit
        // EVERY consumer of uyvy_frame->d_data (preview, compositor, SDI, recorder)
        // and reproduced on the RTX Ada laptop but not the desktop. Confirmed by
        // disabling reuse: the tear vanished.
        //
        // Fix: never reuse a buffer — each frame gets a fresh cudaMalloc, freed on
        // destruction. cudaFree's implicit device sync also guarantees no in-flight
        // read is overwritten. The per-frame malloc/free cost is negligible for a
        // handful of same-size ~4 MB buffers at field rate, and the pipeline already
        // synchronizes per frame elsewhere. If pooling is ever reinstated for perf,
        // it MUST synchronize (event/device sync) before re-issuing copyToDevice
        // into a recycled buffer. Set to false to restore the (unsafe) reuse.
        //
        // NOTE (green-band tear, AI-keyer/RTM era): disabling reuse only changed the
        // SYMPTOM, not the root cause. The real defect is that the H2D upload into
        // d_data (pageable cudaMemcpy) can return before its DMA completes, so
        // consumers on non-blocking streams sample a half-written buffer. With reuse
        // ON the unwritten rows showed the PREVIOUS frame; with reuse OFF they show
        // freshly-malloc'd zeros = a GREEN band. The actual cure is the H2D
        // completion barrier (cudaStreamSynchronize) added at every ingest upload
        // site (see copyToDevice in common/videos/ingestion/src/ingest.cpp). Keep
        // reuse disabled regardless.
        static constexpr bool kPoolReuseDisabled = true;

        DeviceBuffer acquire(size_t min_bytes)
        {
            if (kPoolReuseDisabled)
                return {};
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = free_list.begin(); it != free_list.end(); ++it)
            {
                if (it->ptr && it->bytes >= min_bytes)
                {
                    DeviceBuffer buf = *it;
                    free_list.erase(it);
                    return buf;
                }
            }
            return {};
        }

        void release(DeviceBuffer buf)
        {
            if (!buf.ptr || buf.bytes == 0)
            {
                return;
            }

            if (kPoolReuseDisabled)
            {
                cudaFree(buf.ptr);
                return;
            }

            std::lock_guard<std::mutex> lock(mutex);
            if (free_list.size() >= max_buffers)
            {
                cudaFree(buf.ptr);
                return;
            }
            free_list.push_back(buf);
        }
    };

    static DeviceBufferPool& pool()
    {
        static DeviceBufferPool p;
        return p;
    }

    void releaseDeviceBuffer()
    {
        if (!d_data || size == 0)
        {
            d_data = nullptr;
            size = 0;
            return;
        }

        pool().release(DeviceBuffer{d_data, size});
        d_data = nullptr;
        size = 0;
    }

public:
    
    // Add default constructor
    UYVYFrame() = default;
    
    // Add destructor to free CUDA memory
    ~UYVYFrame() {
        releaseDeviceBuffer();
    }
    
    // Delete copy constructor and assignment to prevent double-free
    UYVYFrame(const UYVYFrame&) = delete;
    UYVYFrame& operator=(const UYVYFrame&) = delete;
    
    // Allow move operations
    UYVYFrame(UYVYFrame&& other) noexcept {
        d_data = other.d_data;
        size = other.size;
        width = other.width;
        height = other.height;
        pitch = other.pitch;
        actual_width_bytes = other.actual_width_bytes;
        
        other.d_data = nullptr;
        other.size = 0;
        other.width = 0;
        other.height = 0;
        other.pitch = 0;
        other.actual_width_bytes = 0;
    }
    
    UYVYFrame& operator=(UYVYFrame&& other) noexcept {
        if (this != &other) {
            releaseDeviceBuffer();
            d_data = other.d_data;
            size = other.size;
            width = other.width;
            height = other.height;
            pitch = other.pitch;
            actual_width_bytes = other.actual_width_bytes;
            
            other.d_data = nullptr;
            other.size = 0;
            other.width = 0;
            other.height = 0;
            other.pitch = 0;
            other.actual_width_bytes = 0;
        }
        return *this;
    }
    
    // Update allocate method to accept pitch
    bool allocate(int w, int h, int row_pitch = 0) {
        width = w;
        height = h;
        actual_width_bytes = w * 2;  // Actual UYVY data width
        pitch = row_pitch > 0 ? row_pitch : w * 2;  // Use provided pitch or calculate
        const size_t required_bytes = static_cast<size_t>(pitch) * static_cast<size_t>(height);

        // Reuse existing allocation if it's large enough.
        if (d_data && size >= required_bytes)
        {
            size = required_bytes;
            return true;
        }

        // Return any too-small allocation to the pool, then acquire one.
        releaseDeviceBuffer();

        DeviceBuffer buf = pool().acquire(required_bytes);
        if (buf.ptr)
        {
            d_data = buf.ptr;
            size = required_bytes;
            return true;
        }

        size = required_bytes;
        cudaError_t error = cudaMalloc(&d_data, size);
        return error == cudaSuccess;
    }
};

// Update FrameData struct
struct FrameData
{
    int id;
    std::unique_ptr<UYVYFrame> uyvy_frame;  // Raw UYVY data
    std::chrono::steady_clock::time_point timestamp;
    uint64_t sync_timestamp_ms = 0; // Wall-clock timestamp for metadata/NDI sync
    std::map<std::type_index, std::any> module_data;
    std::mutex data_mutex;
    std::atomic<int> modules_to_process{0};

    // Ensure GPU mats and frames freed on destruction
    ~FrameData() {
        // unique_ptr will call destructors to free d_data
        uyvy_frame.reset();
    }

    // Explicitly free only the raw UYVY GPU buffer (safe to call from any thread)
    void freeUYVYData()
    {
        std::lock_guard<std::mutex> lock(data_mutex);
        uyvy_frame.reset();
    }
    
};

// --- Thread-Safe Ring Buffer with Graceful Shutdown ---
template <typename T>
class SharedRingBuffer
{
public:
    explicit SharedRingBuffer(size_t capacity) : buffer_(capacity), capacity_(capacity), head_(0), tail_(0), count_(0), is_shutdown_(false) {}

    // Writes an item to the buffer. Returns false if the buffer is shut down.
    bool write(T item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Wait until there's space OR the buffer is shut down
        not_full_.wait(lock, [this]
                       { return count_ < capacity_ || is_shutdown_; });

        if (is_shutdown_)
        {
            return false; // Do not write if shutdown is initiated
        }

        buffer_[head_] = std::move(item);
        head_ = (head_ + 1) % capacity_;
        count_++;
        not_empty_.notify_one();
        return true;
    }

    // Writes an item without blocking.
    // If the buffer is full, overwrites the oldest item so producers never stall.
    // Returns false only if shutdown is initiated.
    bool writeLatest(T item)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (is_shutdown_ || capacity_ == 0)
        {
            return false;
        }

        const bool was_full = (count_ >= capacity_);

        buffer_[head_] = std::move(item);
        head_ = (head_ + 1) % capacity_;

        if (was_full)
        {
            // Drop/overwrite the oldest element.
            tail_ = (tail_ + 1) % capacity_;
            count_ = capacity_;
        }
        else
        {
            count_++;
        }

        not_empty_.notify_one();
        return true;
    }

    // Reads an item from the buffer. Returns std::nullopt if shut down or timed out.
    std::optional<T> read(std::chrono::milliseconds timeout = std::chrono::milliseconds(0))
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait until there's an item OR the buffer is shut down
        auto wait_predicate = [this]
        { return count_ > 0 || is_shutdown_; };

        // IMPORTANT: timeout == 0 is a non-blocking poll.
        if (timeout.count() == 0)
        {
            if (count_ == 0)
            {
                return std::nullopt;
            }
        }
        else
        {
            if (!not_empty_.wait_for(lock, timeout, wait_predicate))
            {
                return std::nullopt; // Timeout occurred
            }
        }

        if (is_shutdown_ && count_ == 0)
        {
            return std::nullopt; // Shutdown and empty, so exit
        }

        T item = std::move(buffer_[tail_]);
        tail_ = (tail_ + 1) % capacity_;
        count_--;
        not_full_.notify_one();
        return item;
    }

    // Initiates shutdown, waking up all waiting threads.
    void shutdown()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        is_shutdown_ = true;
        // Notify all threads waiting on 'not_full_' or 'not_empty_'
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    size_t capacity() const
    {
        return capacity_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::vector<T> buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t count_;
    std::atomic<bool> is_shutdown_; // Shutdown flag
};
