#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "shared_data.h"
#include "shared_state.h"

struct IngestConfig {
    std::string backend = "decklink"; // decklink | deltacast
    int input_device = 0;
    int output_device = 0;
    bool enable_output = true;
};

struct IngestVideoInfo {
    int width = 0;
    int height = 0;
    int pitch = 0;
    std::string display_mode_name = "Unknown";
    bool signal_detected = false;
    bool interlaced = false;
    float ideal_fps = 50.0f;
    float fps_threshold = 47.0f;
};

class IIngest {
public:
    virtual ~IIngest() = default;

    virtual bool initialize(const IngestConfig& config) = 0;
    virtual bool startCapture() = 0;
    virtual void stopCapture() = 0;
    virtual bool reconfigure(const IngestConfig& config) = 0;

    // Returns a fully populated FrameData with GPU UYVY data
    virtual bool getFrame(std::shared_ptr<FrameData>& out_frame) = 0;

    // Output path
    virtual bool initializeOutput(int deviceIndex) = 0;
    virtual bool initializeOutput(int deviceIndex, int slotIndex) = 0;  // second SDI output
    virtual bool disableOutput(int slotIndex) = 0;
    virtual bool sendFrameToOutput(const uint8_t* data,
                                   size_t size,
                                   int width,
                                   int height,
                                   int pitch) = 0;
    // Send to a specific output slot (0 = primary, 1 = secondary SDI).
    virtual bool sendFrameToOutput(const uint8_t* data,
                                   size_t size,
                                   int width,
                                   int height,
                                   int pitch,
                                   int slotIndex) = 0;
    virtual int getNumOutputs() const = 0;

    // Info + state
    virtual IngestVideoInfo getVideoInfo() const = 0;
    virtual std::string backendName() const = 0;
    virtual void setSharedState(SharedState* state) = 0;
};

std::unique_ptr<IIngest> CreateIngest(const IngestConfig& config);
