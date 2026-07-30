#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "stype_csv_overlay.hpp"

// Paired video + *_stype.csv from a stype_handover-style recordings folder.
struct RecordingPair {
    std::string name;       // stem, e.g. sdi_20260726_131211
    std::string video_path;
    std::string csv_path;
};

std::vector<RecordingPair> listRecordingPairs(const std::string& recordings_dir);

// OpenCV video player synced to a Stype CSV, with world-origin gizmo overlay
// (same projection approach as stype_handover/scripts/stype_player.py).
class RecordingPlayback {
public:
    RecordingPlayback() = default;
    ~RecordingPlayback();

    RecordingPlayback(const RecordingPlayback&) = delete;
    RecordingPlayback& operator=(const RecordingPlayback&) = delete;

    bool open(const RecordingPair& pair, std::string& error);
    void close();
    bool isOpen() const { return capture_.isOpened(); }

    const std::string& name() const { return name_; }
    int frameCount() const { return frame_count_; }
    int frameIndex() const { return frame_index_; }
    double fps() const { return fps_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const stype::Records& records() const { return records_; }
    bool hasRecord() const { return has_record_; }
    const stype::Record& activeRecord() const { return active_record_; }

    bool playing = false;
    int csv_offset = 0;
    bool show_world_origin = true;
    stype::OverlayOptions overlay_options;

    // Advances one video frame when playing and enough wall time has elapsed.
    // Returns true when display_ was updated.
    bool tick();

    // Seek to an absolute video frame index (clamped).
    bool seek(int frame_index);

    // Re-apply CSV overlay after offset / gizmo options change (same frame).
    void refreshOverlay();

    // BGR frame ready for GL upload / ImGui (includes overlay when enabled).
    const cv::Mat& display() const { return display_; }

private:
    bool readCurrentFrame_(std::string& error);
    void applyOverlay_();

    cv::VideoCapture capture_;
    stype::Records records_;
    std::string name_;
    int frame_count_ = 0;
    int frame_index_ = 0;
    int width_ = 0;
    int height_ = 0;
    double fps_ = 25.0;
    cv::Mat frame_bgr_;
    cv::Mat display_;
    stype::Record active_record_;
    bool has_record_ = false;
    std::int64_t last_advance_ms_ = 0;
};
