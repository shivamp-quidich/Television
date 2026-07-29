#pragma once

#include <vector>
#include <mutex>
#include <any>       // For std::any
#include <typeindex> // For std::type_index
#include <map>       // For std::map
#include <optional>  // For safe data retrieval
#include <opencv2/core/types.hpp>
#include <opencv2/core.hpp> // For cv::Mat (sponsor grid state)
#include <atomic>    // For std::atomic
#include <memory>    // For std::shared_ptr
#include <string>
#include <chrono>
#include <array>     // For std::array (sponsor grid state)
#include <cstdint>   // For fixed-width integer types
#include <cstddef>   // For std::size_t
#include <utility>   // For std::pair
#include <algorithm> // For std::clamp
#include <cmath>     // For std::isfinite

#include "perspective_template.h"
#include "ycbcr_colorimetry.h"

struct FrameData;

// --- Point Tracker Module Data Structures ---
namespace PointTrackerState {
    // Configuration sent from UI to the point tracker module
    struct Config {
        std::vector<cv::Point2f> initial_points;
        int bbox_size = 50;
    };

    // Command to add a single new tracker without reinitializing existing ones
    struct AddTrackerCommand {
        cv::Point2f point;       // Location to add the new tracker
        bool execute = false;    // Set to true to execute the addition
    };

    // Command to remove a specific tracker by ID
    struct RemovalCommand {
        int tracker_id = -1;  // ID of the tracker to remove, -1 means no removal pending
        bool execute = false; // Set to true to execute the removal
    };

    // Command to clear all trackers
    struct ClearAllCommand {
        bool execute = false;    // Set to true to clear all trackers
    };

    // Individual tracked object information
    struct TrackedObject {
        int id;
        cv::Point2f centroid;
        cv::Rect bounding_box;
    };

    // Results sent from point tracker module back to UI
    struct Results {
        std::vector<TrackedObject> tracked_objects;
        uint64_t frame_id = 0;
    };
}

enum class BBoxColorScheme {
    RED = 0,
    GREEN = 1,
    CYAN = 2,
    ORANGE = 3
};

// --- Player Tracker Module Data Structures ---
namespace PlayerTrackerState {
    enum class ActiveModelType {
        Detection = 0,
        Segmentation = 1
    };

    struct TrackedPlayer {
        int id;
        cv::Rect bounding_box;    // integer, used for display/collision
        cv::Rect2f bounding_box_f; // float, preserves sub-pixel Kalman output for filtering
        float confidence;
        bool has_head = false;
        cv::Rect head_bounding_box;
        float head_confidence = 0.0f;

        // Segmentation mask fields (only populated when a seg model is active)
        std::vector<cv::Point> mask_contour;  // silhouette contour in video pixel coordinates
        bool has_mask = false;
    };

    struct Results {
        std::vector<TrackedPlayer> players;
        uint64_t frame_id = 0;
        cv::Mat segmentation_mask;   // CV_8UC1, video resolution; player=255, background=0
        bool has_segmentation_mask = false;

        // Add method to find player by ID for head association
        TrackedPlayer* findPlayerById(int id) {
            for (auto& player : players) {
                if (player.id == id) {
                    return &player;
                }
            }
            return nullptr;
        }
    };

    // - AIOnly:  use AI mask alone (no chroma key required)
    // - Hybrid:  multiply AI mask × chroma alpha (both must agree)
    // - Chroma:  chroma key only (no AI involvement)
    enum class KeyingMode { AIOnly, Hybrid, Chroma };

    // AI matte post-processing refinement mode.
    enum class AIRefinementMode { FixedErosion, BoundaryAware };

    struct Config {
        std::string new_model_path;
        std::string model_name;
        bool switch_requested = false;

        // AI segmentation keying controls
        bool segmentation_keyout_enabled = true;
        bool segmentation_keyout_invert  = false;
        int  ai_mask_offset_x_px = 0;
        KeyingMode keying_mode = KeyingMode::AIOnly;
        ActiveModelType active_model_type = ActiveModelType::Detection;

        // Key only selected players (AI Only mode)
        bool key_selected_only = true;

        // AI matte refinement
        AIRefinementMode ai_refinement_mode = AIRefinementMode::BoundaryAware;
        int erosion_iterations = 2;
        int boundary_band_px   = 6;

        // Segmentation inference decimation (1=every frame, 2=every 2nd frame, etc.)
        int ai_inference_stride = 2;

        // Cap on tracked players. A cricket frame has ~15-16 people incl. umpires, so
        // anything beyond this is crowd / spurious tracks — and BYTETracker's cost grows
        // ~O(N^2), so letting the count run to 30+ blows the frame budget. Detections fed
        // to the tracker AND the tracked-player output are both capped to the highest-
        // confidence N. 0 = unlimited.
        int max_tracked_players = 20;

        // Adaptive refinement based on detection confidence
        bool  adaptive_refinement_enabled    = false;
        float adaptive_confidence_low        = 0.35f;
        float adaptive_confidence_high       = 0.75f;
        float adaptive_edge_uncertainty_high = 0.5f;
        int   adaptive_erosion_min           = 1;
        int   adaptive_erosion_max           = 4;

        // Bounding box color configuration
        BBoxColorScheme bbox_color = BBoxColorScheme::ORANGE;

        // RTM two-stage segmentation (uses YOLO for detection, RTMDet-Ins for masks).
        // When enabled, the YOLO seg mask is replaced by RTM per-player instance masks.
        bool        rtm_segmentation_enabled = false;
        std::string rtm_engine_path = "models/rtmdet-ins/epoch_293_ktk10_448.engine";

        // Extra fractional bbox expansion applied to detection-model bboxes before RTM.
        // Detection bboxes are tight; RTM needs surrounding context for accurate segmentation.
        // 0.10 = expand each side by 10% of the bbox dimension (on top of RTM's own 5% pad).
        // Only applied when the YOLO base model is detection-only (no segmentation head).
        float rtm_det_bbox_extra_pad = 0.10f;

        // Restrict RTM segmentation to players overlapping a placed sponsor graphic.
        // Players away from every graphic quad don't occlude any graphic, so skipping
        // them cuts RTM crop count (the main latency driver) with no visible effect.
        // When no graphic is placed (or the toggle is off) all tracked players are segmented.
        bool  rtm_sponsor_roi_only   = true;   // toggle, default ON
        float rtm_sponsor_roi_margin = 0.15f;  // outward pad of the quad, fraction of its size

        // Hybrid masking: the YOLO seg proto mask is the always-on base for every
        // player (cheap, never flickers); RTM refines only the few players actually
        // on the mat. RTM is compute-bound (~8 ms/crop @640²), so cap the crop count
        // to keep the per-frame budget — the extras keep their YOLO mask.
        int rtm_max_crops = 2;                 // 0 = unlimited

        // Min RTM detection score for a crop's mask to be used. Below this the player
        // keeps its YOLO mask — the RTM<->YOLO toggle is what reads as flicker, so a
        // low bar is desirable: the crop is already centred on a tracked player.
        float rtm_score_threshold = 0.25f;

        // Debug: stand-in sponsor ROIs in video pixels. When enabled these REPLACE the
        // sponsor-grid quads for RTM gating, so mask quality can be evaluated without
        // running camera tracking / the sponsor grid tracker. Players overlapping one
        // of these rects get the RTM matte; everyone else keeps the YOLO mask.
        bool                  rtm_debug_rois_enabled = false;
        std::vector<cv::Rect> rtm_debug_rois;

        // Alternative RTM gating mode. OFF (default): sponsor-quad ROI based gating
        // (rtm_sponsor_roi_only / rtm_debug_rois above) — RTM only refines players
        // on/near a placed graphic. ON: ignore the ROI entirely and instead RTM-refine
        // the top rtm_max_crops players by YOLO detection confidence, gated at a
        // minimum percentage (rtm_confidence_threshold). In both modes players not
        // chosen for RTM keep their YOLO segmentation mask (the fallback is always YOLO).
        bool  rtm_confidence_gating_enabled = false;
        float rtm_confidence_threshold      = 0.60f;   // 0..1 (shown as % in the UI)
    };
}

// --- Player Selection State ---
namespace PlayerSelectionState {
    struct Config {
        std::vector<int> selected_player_ids;
    };
}

// --- Compositor Module Data Structures ---
namespace CompositorState {
    struct Config {
        bool is_enabled = true;
        // Add other parameters here later, like opacity sliders.
    };
}

// --- Chroma Key Module Data Structures ---
namespace ChromaKeyState
{
    // Keep this in sync with the GLSL uniform array in ycbcr_keyer.frag.
    inline constexpr std::size_t kMaxKeyColors = 40;
    inline constexpr float kMinTolerance = 0.001f;

    inline float sanitizeTolerance(float value, float fallback)
    {
        return std::isfinite(value) ? std::max(value, kMinTolerance) : fallback;
    }

    // A colour is sampled from one frame at the time its point is selected.
    // Keeping the point and its sampled value together prevents a later click
    // from re-sampling earlier selections against a newer frame.
    struct ColorSample
    {
        cv::Point2f point;
        cv::Vec3f ycbcr;

        bool operator==(const ColorSample& other) const
        {
            return point.x == other.point.x && point.y == other.point.y &&
                   ycbcr[0] == other.ycbcr[0] && ycbcr[1] == other.ycbcr[1] &&
                   ycbcr[2] == other.ycbcr[2];
        }

        bool operator!=(const ColorSample& other) const
        {
            return !(*this == other);
        }
    };

    struct Config
    {
        float cr_tolerance = 10.0f;
        float cb_tolerance = 8.0f;
        float luma_tolerance = 25.0f;
        float softness = 0.2f;
        float gradient_strength = 0.5f;
        float edge_sharpness = 0.55f;
        bool  preserve_detail = true;
        std::vector<ColorSample> color_samples;
        bool preview_enabled = false;

        bool operator==(const Config& other) const
        {
            if (cr_tolerance != other.cr_tolerance ||
                cb_tolerance != other.cb_tolerance ||
                luma_tolerance != other.luma_tolerance ||
                softness != other.softness ||
                gradient_strength != other.gradient_strength ||
                edge_sharpness != other.edge_sharpness ||
                preserve_detail != other.preserve_detail ||
                preview_enabled != other.preview_enabled ||
                color_samples.size() != other.color_samples.size())
            {
                return false;
            }

            for (size_t index = 0; index < color_samples.size(); ++index)
            {
                if (color_samples[index] != other.color_samples[index])
                {
                    return false;
                }
            }

            return true;
        }

        bool operator!=(const Config& other) const
        {
            return !(*this == other);
        }
    };
}

// --- Keying Diagnostics ---
namespace KeyingDiagnosticsState
{
    struct Config
    {
        // Enables player_tracker matte metrics emission
        bool metrics_enabled = false;
        int  metrics_interval = 30;

        // Optional CSV export of per-frame metrics
        bool        metrics_csv_export_enabled = false;
        std::string metrics_csv_export_path    = "logs/keying_metrics.csv";
        int         metrics_csv_flush_interval = 30;

        // Compositor debug view: off | ai | chroma | hybrid
        std::string debug_view = "off";
    };
}

// --- Perspective Tracker Module Data Structures ---
namespace PerspectiveTrackerState
{
    struct Config
    {
        std::array<cv::Point2f, perspective_template::kTemplatePointCount> image_points{};
        bool is_configured = false;
        bool is_frozen = false;
        bool request_reinit = false;
        int output_width = 1920;
        int output_height = 1080;
        
        // Editing support
        bool editing_enabled = false;
        std::vector<cv::Point2f> dragged_boundary;
        bool confirm_edit = false;
    };
    
    // Tracker state enum (matches PerspectiveTracker internal state)
    enum class TrackerState {
        TRIVIA,              // Normal tracking
        DISENGAGE,           // Tracking lost
        ATTEMPTING_REINIT    // Attempting to re-initialize
    };
    
    struct Results
    {
        cv::Mat perspective_matrix;
        bool transform_available = false;
        int perspective_changes = 0;
        std::vector<cv::Point2f> tracked_boundary;
        cv::Mat camera_rotation;
        cv::Mat camera_translation;
        bool rotation_valid = false;
        float rotation_pan_deg = 0.0f;
        float rotation_tilt_deg = 0.0f;
        float rotation_twist_deg = 0.0f;
        float rotation_view_angle_deg = 0.0f;
        TrackerState tracker_state = TrackerState::DISENGAGE;
    };
}

// --- Overlay Controls Shared State ---
namespace OverlayState
{
    struct Config
    {
        float offset_x_m = 0.0f; // Positive moves overlay to camera-right
        float offset_y_m = 0.0f; // Positive moves overlay down the pitch
        bool visible = true;
    };
}

// --- Rotation Offset Controls Shared State ---
namespace RotationOffsetState
{
    struct Config
    {
        float offset_x_deg = 0.0f;
        float offset_y_deg = 0.0f;
        float offset_z_deg = 0.0f;
    };
}

// --- UDP Configuration Data Structures ---
namespace UDPState {
    struct Config {
        std::string ip_address;
        int port;
        bool config_changed = false;  // Flag to detect changes
        
        bool operator!=(const Config& other) const {
            return ip_address != other.ip_address || port != other.port;
        }
    };
    struct Status {
        bool connected = false;
        std::string last_error;
        std::chrono::system_clock::time_point last_send_time;
    };
}

// NDI State namespace
namespace NDIState {
    struct Config {
        std::string everest_source;
        std::string timer_source;
        bool source_changed = false;
        bool is_running = true;
    };
}

// --- WSS JSON Client State ---
namespace WSSState {
    struct Config {
        std::string host;
        int port = 443;
        std::string path = "/";
        bool use_tls = true;
        bool enabled = true;
    };

    struct Status {
        bool connected = false;
        std::string last_message;
        std::string last_error;
        std::chrono::system_clock::time_point last_update;
    };
}

// --- JSON UDP Ingest State ---
namespace JsonIngestState {
    enum class ShapeType {
        Line,
        Arrow,
        DoubleArrow,
        Freehand
    };

    struct OverlayShape {
        ShapeType type = ShapeType::Line;
        std::string color = "green";
        cv::Point2f start{0.0f, 0.0f};
        cv::Point2f end{0.0f, 0.0f};
        std::vector<cv::Point2f> points;
        bool is_complete = false;
    };

    struct OverlayData {
        std::map<int, OverlayShape> shapes;
        std::chrono::system_clock::time_point last_update;
    };

    struct ZoomData {
        bool active = false;
        float x_norm = 0.0f;
        float y_norm = 0.0f;
        float zoom = 1.0f;
        float size = 150.0f;
        float radius = 75.0f;
        std::chrono::system_clock::time_point last_update;
    };

    struct TelestratorStatus {
        bool mode_enabled = false;
        bool client_connected = false;
        std::string client_status = "disconnected";
        std::chrono::system_clock::time_point last_update;
    };

    struct Config {
        std::string ws_host;
        int ws_port = 9011;
        std::string ws_path = "/";
        bool enabled = true;
    };

    struct Status {
        std::string last_message;
        std::string last_sender;
        std::string last_error;
        std::vector<std::string> pending_messages;
        std::chrono::system_clock::time_point last_update;
    };

    struct OutgoingCommand {
        std::string message;
        bool pending = false;
        std::chrono::system_clock::time_point last_update;
    };
}

// --- Video Input Module Data Structures ---
namespace VideoInputState {
    struct Config {
        int width = 3840;
        int height = 2160;
        std::string display_mode_name = "Unknown";
        bool signal_detected = false;
        // True only when the detected SDI signal carries two time-separated
        // fields per frame (for example 1080i50). The compositor uses this to
        // decide whether it may weave two Stype HF poses; doing so on 1080p
        // creates alternating-line combing on rectangular virtual graphics.
        bool interlaced = false;

        // Matrix/range are properties of the current input signal. The
        // selection is configured once at ingest; Auto derives a conventional
        // SD/HD matrix from the active DeckLink display mode metadata.
        YCbCrMatrixSelection ycbcr_matrix_selection = YCbCrMatrixSelection::Rec709;
        YCbCrRange ycbcr_range = YCbCrRange::Legal;

        YCbCrColorimetry inputColorimetry() const
        {
            return resolveYCbCrColorimetry(
                ycbcr_matrix_selection, ycbcr_range, height);
        }

        // The frame rate expected by the pipeline for the detected input.
        // Interlaced SDI modes report their field rate in the mode name (for
        // example, 1080i50), but each captured frame contains two fields and
        // therefore has a 25 fps frame rate.
        float ideal_fps = 50.0f;

        // Minimum FPS considered healthy by the UI. Keep a small tolerance
        // below the ideal rate so normal measurement jitter does not turn the
        // metric red (47 fps for a 50p input, 23.5 fps for a 50i input).
        float fps_threshold = 47.0f;
    };
}

// --- Runtime Ingest Reconfiguration State ---
namespace IngestRuntimeState {
    struct Command {
        uint64_t request_id = 0;
        int input_device = 0;
        int output_device = 0;
        int output_device_b = -1;
        bool enable_output = true;
        bool enable_output_b = false;
        bool dual_output_mode = false;
    };

    struct Status {
        uint64_t applied_request_id = 0;
        bool success = false;
        int input_device = 0;
        int output_device = 0;
        int output_device_b = -1;
        bool output_b_active = false;
        bool dual_output_mode = false;
        std::string backend;
        std::string message;
    };
}

// --- Playback Control State ---
namespace PlaybackState {
    struct Control {
        bool paused = false;
        std::chrono::system_clock::time_point last_update;
    };

    struct PausedFrame {
        std::shared_ptr<FrameData> frame;
        std::chrono::system_clock::time_point last_update;
    };

    // ── Recorded-file playback (UI → capture thread) ──────────────────────────
    // Drives the capture thread to ingest a recorded mp4 + sidecar Stype CSV
    // instead of the live SDI source, so tracking can be tested against recorded
    // data. The decoded video flows through the normal compositor pipeline and
    // the per-frame Stype pose is published to STypeState::CameraData, so the AR
    // overlay is composited over the recording exactly as it would be live.
    struct FilePlayback {
        bool        active     = false;   // true = play recording, false = live source
        bool        loop       = true;    // restart at end of file
        std::string video_path;           // .mp4 / .mkv recording
        std::string csv_path;             // matching *_stype.csv sidecar
        uint64_t    request_id = 0;       // bump to (re)start playback
    };

    // Capture thread → UI progress/health for the active playback.
    struct FilePlaybackStatus {
        bool        active             = false;
        uint64_t    applied_request_id = 0;
        uint64_t    frame_index        = 0;   // current frame within the file
        uint64_t    total_frames       = 0;   // CSV row count (≈ video frames)
        std::string current_file;
        std::string error;
    };
}

// --- Header Panel Module Data Structures ---
namespace HeaderPanelState {
    struct TeamSelection {
        std::string team_a_name = "Team A (unselected)";
        std::string team_b_name = "Team B (unselected)";
        bool team_a_selected = false;
        bool team_b_selected = false;
    };
}

// --- Sports Configuration ---
namespace SportsState {
    enum class SportType {
        VIRTUAL_AD,
        CRICKET,
        CYCLING,
        UNKNOWN
    };
    
    struct Config {
        SportType sport = SportType::CRICKET;  // Default to cricket
        
        // Helper to convert string to enum
        static SportType fromString(const std::string& str) {
            if (str == "virtual_ad") return SportType::VIRTUAL_AD;
            if (str == "cricket") return SportType::CRICKET;
            if (str == "cycling") return SportType::CYCLING;
            return SportType::UNKNOWN;
        }
        
        // Helper to convert enum to string
        static std::string toString(SportType type) {
            switch(type) {
                case SportType::VIRTUAL_AD: return "virtual_ad";
                case SportType::CRICKET: return "cricket";
                case SportType::CYCLING: return "cycling";
                default: return "unknown";
            }
        }
    };

    inline bool isVirtualAd(SportType type) noexcept {
        return type == SportType::VIRTUAL_AD;
    }
}

// --- Genlock Status ---
namespace GenlockState {
    struct Status {
        bool is_locked = false;
        std::string ref_format = "No Signal";
        std::chrono::system_clock::time_point last_update;
    };
}

// --- Cursor Tracker Module Data Structures ---
namespace CursorTrackerState {
    // Configuration for cursor tracking mode
    struct Config {
        bool enabled = false;           // Is cursor tracking mode active
        bool is_mouse_held = false;     // Is left mouse button currently held
        cv::Point2f last_coord{0.0f, 0.0f};  // Last cursor coordinate in video space
        cv::Point2f current_coord{0.0f, 0.0f}; // Current cursor coordinate when mouse is held
        
        // Helper to check if we have a valid coordinate to send
        bool hasValidCoord() const {
            return enabled && (last_coord.x != 0.0f || last_coord.y != 0.0f);
        }
    };
}

// --- Stype FreeD D1 / Stype HF Camera Tracking State ---
namespace STypeState {
    constexpr int kMaxVirtualAdGridAxis = 6;
    constexpr int kVirtualAdOutputCount = 2;
    constexpr int kMaxVirtualAdSlots = kMaxVirtualAdGridAxis * kMaxVirtualAdGridAxis;
    constexpr float kMaxFovExpandPct = 50.0f;
    // Track-delay slider range/step (Stype/FreeD alignment offset, milliseconds).
    constexpr int kMaxTrackDelayMs = 600;
    constexpr int kTrackDelayStepMs = 5;
    // Master-grid spacing is stored in millimetres for the projection and
    // tracker math. The operator-facing UI exposes the same value in metres
    // and allows up to 100 m (100,000 mm).
    constexpr float kMaxMasterGridSpacingMm = 100000.0f;
    // Large enough for a full sports field while keeping bad UI/config input
    // from producing unusable world coordinates.
    constexpr float kMaxVerificationOffsetMm = 100000.0f;
    constexpr float kMinCalibrationConeScale = 0.01f;
    constexpr float kMaxCalibrationConeScale = 100.0f;

    // Shared palettes for the two calibration cones. Renderers intentionally
    // consume these values rather than inventing their own similar colours.
    struct CalibrationConeColor {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };
    inline constexpr CalibrationConeColor kReferenceConeFillColor{255, 200, 50, 80};
    inline constexpr CalibrationConeColor kReferenceConeEdgeColor{255, 220, 80, 200};
    inline constexpr CalibrationConeColor kVerificationConeFillColor{111, 211, 255, 80};
    inline constexpr CalibrationConeColor kVerificationConeEdgeColor{111, 211, 255, 200};

    // Protocol selection for the receiver.
    // FreeDd1 = classic Stype FreeD D1 (29-byte, 0xD1 header)
    // StypeHF = Stype HF high-frequency protocol (67-byte, 0x0F header)
    enum class Protocol { FreeDd1 = 0, StypeHF = 1 };

    // Configured Stype camera endpoints for the Virtual Ad workflow.  These
    // are operator-facing settings, separate from Config, which remains the
    // active receiver's single bind configuration.
    struct CameraEndpoint {
        std::string camera_name;
        std::string ip_address;
        int         port = 6301;
    };

    struct CameraEndpointConfig {
        std::vector<CameraEndpoint> cameras;
    };

    struct Config {
        bool enabled = false;
        std::string bind_ip = "0.0.0.0";
        int port = 6305;
        std::string camera_id = "CAM_01";
        bool config_changed = false;
        Protocol protocol = Protocol::FreeDd1;
        float vfov_deg = 25.0f; // Vertical field-of-view estimate (degrees) for projection
        bool show_debug_overlay = false; // Show projected pixel coords on video
        // Tracking-to-video alignment offset in milliseconds (0 = no delay).
        // Applied by interpolating the buffered pose stream, so any multiple
        // of kTrackDelayStepMs is honored, not just whole packet periods.
        int frame_delay_ms = 0;

        // Zoom encoder calibration.
        // Stype Kit sends zoom/focus as 24-bit unsigned values.
        // The high byte is a fixed protocol constant (0x08), so the actual
        // encoder varies in the lower 16 bits: range 0x080000..0x08FFFF.
        // zoom_min_raw  = encoder value at the WIDEST  position (maps to LUT row 0)
        // zoom_max_raw  = encoder value at the TELE    position (maps to LUT row N-1)
        // Swap min/max to invert the encoder direction if needed.
        int   zoom_min_raw  = 0x080000;   // 524288
        int   zoom_max_raw  = 0x08FFFF;   // 589823
        // Focus encoder calibration.
        // focus_min_raw = encoder value at 0%  focus stroke (near focus)
        // focus_max_raw = encoder value at 100% focus stroke (infinity / far focus)
        int   focus_min_raw = 0x080000;   // 524288
        int   focus_max_raw = 0x08FFFF;   // 589823

        // Stype-calibrated world origin (mm). Virtual-ad placements, the master
        // grid, and preview gizmos all use this common origin. The default
        // calibrated origin is (0, 0, 0).
        float world_origin_x_mm = 0.0f;
        float world_origin_y_mm = 0.0f;
        float world_origin_z_mm = 0.0f;

        // Orientation of the virtual-ad ground plane about the calibrated world
        // origin (degrees). The rotations are applied yaw → pitch → roll:
        // yaw around the configured up axis, pitch around the yawed right axis,
        // and roll around the yawed/pitched forward axis.
        float world_yaw_deg   = 0.0f;
        float world_pitch_deg = 0.0f;
        float world_roll_deg  = 0.0f;

        // ── Axis remapping ────────────────────────────────────────────────
        // The FreeD D1 packet contains 3 angle slots (A=bytes2-4, B=5-7, C=8-10)
        // and 3 position slots (A=bytes11-13, B=14-16, C=17-19).
        // Each field below selects which slot (0=A, 1=B, 2=C) feeds that semantic
        // axis, and whether to negate it.
        // Defaults reproduce the original Stype D1 hardcoded layout.
        int   angle_pan_src   = 0;  bool  angle_pan_neg   = false; // A → pan
        int   angle_tilt_src  = 1;  bool  angle_tilt_neg  = false; // B → tilt
        int   angle_roll_src  = 2;  bool  angle_roll_neg  = false; // C → roll
        int   pos_x_src       = 0;  bool  pos_x_neg       = false; // A → cam X
        int   pos_z_src       = 1;  bool  pos_z_neg       = false; // B → cam Z
        int   pos_y_src       = 2;  bool  pos_y_neg       = false; // C → cam Y

        // World "up" axis: which axis is vertical in the Stype world coordinate system.
        // 0 = X-up, 1 = Y-up (default, standard FreeD), 2 = Z-up
        // Determines the ground plane of the AR circle:
        //   Y-up  → circle drawn in XZ plane (Y = world_origin_y_mm)
        //   Z-up  → circle drawn in XY plane (Z = world_origin_y_mm)
        //   X-up  → circle drawn in YZ plane (X = world_origin_y_mm)
        int   world_up_axis   = 1;

        // Origin gizmo style: false = XYZ axis arrows, true = cone
        bool  gizmo_show_cone  = false;
        float cone_scale       = 1.0f;  // multiplier for cone H (500 mm) and R (300 mm)

        // Verification marker in the calibrated local Stype X/Y/Z frame. A
        // zero offset deliberately coincides with the reference world origin.
        // All distances remain millimetres in state; the UI presents metres.
        bool  show_verification_cone = false;
        float verification_offset_x_mm = 0.0f;
        float verification_offset_y_mm = 0.0f;
        float verification_offset_z_mm = 0.0f;
        float verification_cone_scale  = 1.0f;

        // Individual overlay/gizmo visibility (independent of show_debug_overlay,
        // which now only controls the debug text/coords readout).
        bool  show_world_origin     = false;            // world-origin ring + axes (preview)
        bool  show_origin_gizmo     = false;            // cone/axes gizmo at the world origin
        bool  show_graphic_gizmo[2] = { false, false }; // per virtual-ad anchor gizmo

        // HF packet options (ignored for FreeD D1)
        bool  hf_apply_distortion   = true;  // apply k1/k2 radial distortion from packet

        // Extra FOV expansion applied to the GL projection matrix only (not to
        // the CPU project() used for visibility checks). Widens the frustum so
        // graphics near the frame edges don't get near-clipped.  0 = no change;
        // 10 = 10% wider frustum.  Typical fix range: 1–10%.
        float fov_expand_pct        = 0.0f;

        // Virtual-ad slot grid dimensions published by the left-panel
        // "MASTER GRID SETUP" controls. The right panel uses this to build the
        // slot-selection grid and to clamp SDI output slot assignments.
        int master_grid_rows        = 2;
        int master_grid_cols        = 3;
        // Uniform scale for the generated master-grid footprint. 1.0 keeps
        // the reference cell dimensions; values below/above it shrink/enlarge
        // every cell and its offset from the grid centre together. Individual
        // slot width/height values do not redefine this global grid geometry.
        float master_grid_scale     = 1.0f;
        // Additional gap between generated grid cells, measured along the
        // column (X) and row (Y) axes of the grid plane. Zero preserves the
        // original edge-to-edge master-grid layout.
        float master_grid_spacing_x_mm = 0.0f;
        float master_grid_spacing_y_mm = 0.0f;
    };

    // ── Virtual-ad graphic placement ──────────────────────────────────────
    // A resolved flat textured quad for a specific SDI output. Shared slot
    // transform state and the output-specific image/opacity are combined into
    // this shape before the compositor or preview consumes it.
    struct GraphicPlacement {
        bool        enabled   = true;
        // Shadow settings are deliberately shared by slot (not SDI appearance)
        // so the same placed ad casts the same shadow on every output.
        bool        shadow_enabled = false;
        // The shadow is derived from this already-resolved ad placement.  Zero
        // degrees extends along the ad's local forward axis; positive angles
        // rotate the fall direction towards the ad's local right axis.
        float shadow_direction_deg = 0.0f;
        float shadow_length_mm = 700.0f;
        float shadow_lateral_offset_mm = 0.0f;
        float shadow_base_offset_mm = 25.0f;
        float shadow_width_scale = 1.0f;
        float shadow_opacity = 0.40f;
        // Radius, in source-texture pixels, used to feather the alpha mask.
        float shadow_softness_px = 0.0f;
        // Moves the shadow a tiny amount below the ad plane to avoid coplanar
        // depth artefacts in render paths that use a depth buffer.
        float shadow_layer_depth_mm = 1.0f;
        float shadow_color_r = 0.08f;
        float shadow_color_g = 0.08f;
        float shadow_color_b = 0.08f;
        std::string image_path;               // e.g. "data/graphics/ad0.jpg" ("" = none)

        // Anchor offset from world origin, in calibrated world axes (mm).
        float pos_x_mm = 0.0f;
        float pos_y_mm = 0.0f;
        float pos_z_mm = 0.0f;

        // Orientation of the quad about its anchor (degrees).
        // yaw   = rotation about the world up axis (spin the ad on the ground)
        // pitch = tilt about the quad's right axis
        // roll  = tilt about the quad's forward axis
        float yaw_deg   = 0.0f;
        float pitch_deg = 0.0f;
        float roll_deg  = 0.0f;

        // Uniform final scale of the quad. The base quad is 4 m wide and its
        // height is derived from image_aspect_ratio, so scaling cannot stretch
        // the source artwork.
        float scale = 1.0f;
        float image_aspect_ratio = 2.0f; // width / height

        // Pivot offset of the quad relative to its anchor, expressed in the
        // quad's own (already-rotated) local axes (mm). The pivot point is what
        // sits at the anchor (pos_*) and what the quad rotates about.
        //   pivot_x_mm → along local right (width) axis
        //   pivot_y_mm → along local forward (height) axis
        //   pivot_z_mm → along local normal (up) axis
        // (0,0,0) keeps the anchor at the quad centre (default).
        float pivot_x_mm = 0.0f;
        float pivot_y_mm = 0.0f;
        float pivot_z_mm = 0.0f;

        float opacity   = 1.0f;               // 0..1 global alpha
    };

    // Shared local slot transform state. These values are common to SDI-1 and
    // SDI-2 for a given slot number, and are composed on top of that slot's
    // generated master-grid anchor only.
    struct GraphicPlacementShared {
        // Per-slot visibility shared by both SDI outputs.
        bool  enabled   = true;
        bool  shadow_enabled = false;
        float shadow_direction_deg = 0.0f;
        float shadow_length_mm = 700.0f;
        float shadow_lateral_offset_mm = 0.0f;
        float shadow_base_offset_mm = 25.0f;
        float shadow_width_scale = 1.0f;
        float shadow_opacity = 0.40f;
        float shadow_softness_px = 0.0f;
        float shadow_layer_depth_mm = 1.0f;
        float shadow_color_r = 0.08f;
        float shadow_color_g = 0.08f;
        float shadow_color_b = 0.08f;
        float pos_x_mm  = 0.0f;
        float pos_y_mm  = 0.0f;
        float pos_z_mm  = 0.0f;
        float yaw_deg   = 0.0f;
        float pitch_deg = 0.0f;
        float roll_deg  = 0.0f;
        // Local, uniform scale applied after the master-grid transform.
        float scale = 1.0f;
        float pivot_x_mm = 0.0f;
        float pivot_y_mm = 0.0f;
        float pivot_z_mm = 0.0f;
    };

    // SDI-specific appearance state for a slot. Image and opacity are unique
    // per slot/SDI output combination.
    struct GraphicAppearance {
        std::string image_path;
        float opacity = 1.0f;
        // Stored with the image path so every consumer projects the source at
        // its native aspect ratio without reading image files on every frame.
        float image_aspect_ratio = 2.0f; // width / height
    };

    struct GraphicSlotConfig {
        GraphicPlacementShared placement;
        std::array<GraphicAppearance, kVirtualAdOutputCount> appearance{};
    };

    // Slot bank. Every active master-grid slot is rendered for each SDI output
    // using that output's own image and opacity. output_slot_indices is retained
    // solely as the Right Panel's per-output editing selection; it does not
    // control SDI compositor routing.
    struct GraphicConfig {
        std::array<GraphicSlotConfig, kMaxVirtualAdSlots> slots{};
        std::array<int, kVirtualAdOutputCount> output_slot_indices{{0, 1}};
        // Bumped by the UI whenever an image_path changes so the compositor
        // knows to (re)load the corresponding texture.
        uint64_t revision = 0;
        // When true, the compositor thread renders each physical SDI output
        // separately before UYVY conversion. Each render still includes the
        // complete active master grid for its SDI appearance.
        bool dual_output_mode = false;
    };

    // UI-only preview routing for the virtual-ad video panel. This is separate
    // from GraphicConfig so PREVIEW never alters SDI compositor routing.
    struct PreviewState {
        bool enabled = false;
        int output_index = 0;  // SDI-1 = 0, SDI-2 = 1
        // Incremented whenever the operator presses PREVIEW. Consumers use it
        // to reload an image even when its path did not change on disk.
        uint64_t asset_revision = 0;
    };

    // One-shot request from the video-panel placement grid to the Right Panel.
    // Keeping this separate from GraphicConfig means a preview click cannot
    // accidentally change compositor routing or mutate a slot configuration.
    struct SlotPickRequest {
        int slot_index = -1;
        int output_index = 0;
        uint64_t request_id = 0;
    };

    // Virtual Ad output visibility controlled by the footer ANIMATE IN button.
    // The compositor reads this state for both SDI outputs: false means the
    // underlying frame is sent with no virtual-ad slot graphics rendered.
    struct AnimateInState {
        bool enabled = false;
    };

    // Shared latch for invalid Stype HF projection data.  The first bad packet
    // snapshots both visibility controls and forces them off; a later valid
    // packet restores exactly that snapshot instead of unconditionally turning
    // graphics back on.
    struct HfFovSafetyState {
        bool tripped = false;
        bool animate_in_was_enabled = false;
        bool preview_was_enabled = false;
    };

    inline int clampMasterGridAxisValue(int value)
    {
        return std::clamp(value, 1, kMaxVirtualAdGridAxis);
    }

    inline float clampFovExpandPct(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.0f, kMaxFovExpandPct) : 0.0f;
    }

    inline float clampVerificationOffsetMm(float value)
    {
        return std::isfinite(value)
            ? std::clamp(value, -kMaxVerificationOffsetMm, kMaxVerificationOffsetMm)
            : 0.0f;
    }

    inline float clampCalibrationConeScale(float value)
    {
        return std::isfinite(value)
            ? std::clamp(value, kMinCalibrationConeScale, kMaxCalibrationConeScale)
            : 1.0f;
    }

    // Range-only clamp. Deliberately does not snap to kTrackDelayStepMs: the
    // UI arrows/wheel move in 5ms steps, but an operator can type or drag in
    // a value that isn't a multiple of 5 for finer control.
    inline int clampTrackDelayMs(int value_ms)
    {
        return std::clamp(value_ms, 0, kMaxTrackDelayMs);
    }

    inline int getMasterGridSlotCount(const Config& cfg)
    {
        return clampMasterGridAxisValue(cfg.master_grid_rows) *
               clampMasterGridAxisValue(cfg.master_grid_cols);
    }

    inline float clampMasterGridScale(float value)
    {
        return std::clamp(value, 0.10f, 4.0f);
    }

    inline float clampGraphicScale(float value)
    {
        return std::clamp(value, 0.10f, 4.0f);
    }

    inline float clampShadowDirectionDeg(float value)
    {
        return std::isfinite(value) ? std::clamp(value, -180.0f, 180.0f) : 0.0f;
    }

    inline float clampShadowLengthMm(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 100000.0f) : 0.0f;
    }

    inline float clampShadowOffsetMm(float value)
    {
        return std::isfinite(value) ? std::clamp(value, -100000.0f, 100000.0f) : 0.0f;
    }

    inline float clampShadowWidthScale(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.05f, 4.0f) : 1.0f;
    }

    inline float clampShadowOpacity(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.40f;
    }

    inline float clampShadowSoftnessPx(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 32.0f) : 0.0f;
    }

    inline float clampShadowLayerDepthMm(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1000.0f) : 1.0f;
    }

    inline float clampShadowColor(float value)
    {
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.08f;
    }

    inline float sanitizeGraphicImageAspectRatio(float value)
    {
        return std::clamp(value, 0.01f, 100.0f);
    }

    inline float clampMasterGridSpacingMm(float value)
    {
        return std::clamp(value, 0.0f, kMaxMasterGridSpacingMm);
    }

    inline int clampVirtualAdSlotIndex(int slot_index, int total_slots)
    {
        const int safe_total = std::clamp(total_slots, 1, kMaxVirtualAdSlots);
        return std::clamp(slot_index, 0, safe_total - 1);
    }

    inline int clampVirtualAdStorageSlotIndex(int slot_index)
    {
        return std::clamp(slot_index, 0, kMaxVirtualAdSlots - 1);
    }

    inline bool isVirtualAdSlotActive(const Config& cfg, int slot_index)
    {
        return slot_index >= 0 && slot_index < getMasterGridSlotCount(cfg);
    }

    inline GraphicPlacement resolveGraphicPlacementForSlot(const GraphicConfig& cfg,
                                                           int slot_index,
                                                           int output_index = 0)
    {
        const int safe_slot = clampVirtualAdStorageSlotIndex(slot_index);
        const int safe_output = std::clamp(output_index, 0, kVirtualAdOutputCount - 1);
        const GraphicSlotConfig& slot = cfg.slots[safe_slot];

        GraphicPlacement resolved;
        resolved.enabled    = slot.placement.enabled;
        resolved.shadow_enabled = slot.placement.shadow_enabled;
        resolved.shadow_direction_deg = clampShadowDirectionDeg(slot.placement.shadow_direction_deg);
        resolved.shadow_length_mm = clampShadowLengthMm(slot.placement.shadow_length_mm);
        resolved.shadow_lateral_offset_mm =
            clampShadowOffsetMm(slot.placement.shadow_lateral_offset_mm);
        resolved.shadow_base_offset_mm = clampShadowOffsetMm(slot.placement.shadow_base_offset_mm);
        resolved.shadow_width_scale = clampShadowWidthScale(slot.placement.shadow_width_scale);
        resolved.shadow_opacity = clampShadowOpacity(slot.placement.shadow_opacity);
        resolved.shadow_softness_px = clampShadowSoftnessPx(slot.placement.shadow_softness_px);
        resolved.shadow_layer_depth_mm =
            clampShadowLayerDepthMm(slot.placement.shadow_layer_depth_mm);
        resolved.shadow_color_r = clampShadowColor(slot.placement.shadow_color_r);
        resolved.shadow_color_g = clampShadowColor(slot.placement.shadow_color_g);
        resolved.shadow_color_b = clampShadowColor(slot.placement.shadow_color_b);
        resolved.image_path = slot.appearance[safe_output].image_path;
        resolved.pos_x_mm   = slot.placement.pos_x_mm;
        resolved.pos_y_mm   = slot.placement.pos_y_mm;
        resolved.pos_z_mm   = slot.placement.pos_z_mm;
        resolved.yaw_deg    = slot.placement.yaw_deg;
        resolved.pitch_deg  = slot.placement.pitch_deg;
        resolved.roll_deg   = slot.placement.roll_deg;
        resolved.scale      = clampGraphicScale(slot.placement.scale);
        resolved.image_aspect_ratio =
            sanitizeGraphicImageAspectRatio(slot.appearance[safe_output].image_aspect_ratio);
        resolved.pivot_x_mm = slot.placement.pivot_x_mm;
        resolved.pivot_y_mm = slot.placement.pivot_y_mm;
        resolved.pivot_z_mm = slot.placement.pivot_z_mm;
        resolved.opacity    = slot.appearance[safe_output].opacity;
        return resolved;
    }

    inline int selectedVirtualAdSlotForOutput(const GraphicConfig& cfg,
                                              const Config& stype_cfg,
                                              int output_index)
    {
        const int safe_output = std::clamp(output_index, 0, kVirtualAdOutputCount - 1);
        return clampVirtualAdSlotIndex(cfg.output_slot_indices[safe_output],
                                       getMasterGridSlotCount(stype_cfg));
    }

    inline GraphicPlacement resolveGraphicPlacementForOutput(const GraphicConfig& cfg,
                                                             const Config& stype_cfg,
                                                             int output_index)
    {
        const int safe_output = std::clamp(output_index, 0, kVirtualAdOutputCount - 1);
        const int slot_index = selectedVirtualAdSlotForOutput(cfg, stype_cfg, safe_output);
        return resolveGraphicPlacementForSlot(cfg, slot_index, safe_output);
    }

    // Latest decoded camera data (updated at ~50 Hz when running).
    // Populated by both FreeD D1 and Stype HF receivers.
    struct CameraData {
        int  camera_id  = 0;
        float pan_deg   = 0.0f;  // Yaw  (broadcast pan)
        float tilt_deg  = 0.0f;  // Pitch (broadcast tilt, negative = looking down)
        float roll_deg  = 0.0f;  // Dutch / roll
        float x_mm      = 0.0f;  // Camera world position, mm
        float y_mm      = 0.0f;
        float z_mm      = 0.0f;
        int   zoom_raw  = 0;
        int   focus_raw = 0;
        // Horizontal FOV from Stype HF (degrees); 0 means not available (use lens table).
        float hfov_deg  = 0.0f;
        // Center shift, distortion, and projection area from Stype HF packet.
        // Used by StypeProjector in HF mode.
        float hf_cx_mm       = 0.0f;   // horizontal center shift (mm)
        float hf_cy_mm       = 0.0f;   // vertical center shift (mm)
        float hf_pa_width_mm = 0.0f;   // projection area width (mm)
        float hf_ar          = 0.0f;   // aspect ratio
        float hf_k1          = 0.0f;   // radial distortion first harmonic (mm⁻²)
        float hf_k2          = 0.0f;   // radial distortion second harmonic (mm⁻⁴)
        bool  cs_ok     = false;
        bool  is_valid  = false;
        std::chrono::steady_clock::time_point last_update{};
        // Rolling packet counter from the Stype HF packet (byte 5); -1 if N/A
        // (e.g. FreeD D1). Used to align the two interlaced fields to a single
        // camera frame so field rendering pairs are coherent (no straddling).
        int   packet_no = -1;
        // Raw packet slots before axis remapping (for debug HUD)
        float raw_slot_angle[3] = {};  // slots A/B/C in degrees
        float raw_slot_pos[3]   = {};  // slots A/B/C in mm
    };

    // Stype HF projection requires all three optical dimensions. Keep this
    // check beside CameraData so receiver, compositor, and UI use the same
    // acceptance rule before enabling virtual-ad graphics.
    inline bool hasValidHfFov(const CameraData& camera)
    {
        return camera.is_valid &&
               std::isfinite(camera.hfov_deg) &&
               camera.hfov_deg > 0.5f && camera.hfov_deg < 180.0f &&
               std::isfinite(camera.hf_ar) && camera.hf_ar > 0.0f &&
               std::isfinite(camera.hf_pa_width_mm) &&
               camera.hf_pa_width_mm > 0.0f;
    }

    // Two most-recent 50 Hz pose samples, published by the receiver alongside
    // CameraData. Used by the compositor to field-render the interlaced SDI
    // output: the upper field (earlier in time) is drawn with `older`, the
    // lower field with `newer`, giving 50 Hz graphics motion on 1080i50.
    // This is NOT interpolation — each field uses its own real pose sample.
    struct CameraFieldPoses {
        CameraData older;          // earlier sample → upper field
        CameraData newer;          // later sample   → lower field
        bool       valid = false;  // true once two consecutive samples exist
    };

    // Snapshot of the delayed tracking state captured with one video frame.
    // The compositor must consume this instead of sampling the receiver's
    // latest shared state after variable-latency processing (for example RTM)
    // has completed.  `camera` is used for progressive rendering; `fields`
    // retains the matching 50 Hz samples for interlaced output.
    struct FrameCameraTracking {
        CameraData       camera;
        CameraFieldPoses fields;
        bool             valid = false;
    };

    struct Status {
        bool is_running       = false;
        int  packets_received = 0;
        int  packet_errors    = 0;
        std::chrono::steady_clock::time_point last_update{};
    };

    // Raw Stype UDP datagram, stored as hex string for CSV logging so a
    // recording can be replayed byte-for-byte. Holds either a 29-byte FreeD D1
    // or a 67-byte Stype HF packet (the longest supported); `len` is the actual
    // datagram size. Updated at the same rate as CameraData (~50 Hz).
    struct RawPacket {
        static constexpr int kMaxLen = 67;   // Stype HF packet length
        static constexpr int kLen    = 29;   // legacy FreeD D1 length (kept for callers)
        uint8_t bytes[kMaxLen]{};   // raw datagram bytes
        int     len      = 0;       // actual datagram length in bytes
        bool    is_valid = false;   // false until first packet received
        std::chrono::steady_clock::time_point last_update{};
    };

    // Parsed Stype lens calibration file — zoom/focus → horizontal FOV lookup table.
    // Zoom encoder (0‑65535) maps linearly across num_zoom calibration lines (Z 0..N-1).
    // Focus encoder (0‑65535) maps via focus_pct percentages to F 0..M-1.
    struct LensTable {
        bool loaded       = false;
        std::string lens_name;
        int  num_zoom     = 0;
        int  num_focus    = 0;
        std::vector<float>              focus_pct;  // focus percentages, size=num_focus
        std::vector<std::vector<float>> hfov;       // hfov_deg[zoom_idx][focus_idx]
        // Principal point offsets from lens calibration file.
        // Fixed component (constant across zoom):
        float center_shift_fix_x = 0.0f;   // fraction of image half-width
        float center_shift_fix_y = 0.0f;   // fraction of image half-height
        // Zoom-dependent floating component (one entry per zoom line):
        std::vector<float> center_shift_float_x;
        std::vector<float> center_shift_float_y;
    };
}

// --- Video Input Delay State ---
namespace VideoDelayState {
    struct Config {
        // Delay in field units (1 field = 20 ms at 50 Hz / 1080i50).
        // Valid range 0–15 (0–300 ms). Buffer depth = fields / 2 video frames.
        // Note: STypeState::Config::frame_delay_ms is a separate, millisecond-
        // resolution delay — the two are no longer expressed in the same unit.
        int frames = 0;
    };
}

// --- End-to-end Video Pipeline Stats (SDI in -> compositor -> SDI out) ---
namespace VideoPipelineState {
    struct Stats {
        // SDI input rate (frames captured)
        float sdi_in_fps = 0.0f;
        uint64_t sdi_in_frames_total = 0;

        // Overall SDI output rate (all frames sent)
        float sdi_out_fps = 0.0f;

        // Mean capture->SDI-out latency (ms) for all frames sent
        float sdi_avg_latency_ms = 0.0f;

        // SDI output rate when raw (non-composited) frames are being sent
        float raw_out_fps = 0.0f;

        // Mean capture->SDI-out latency (ms) for raw frames
        float raw_avg_latency_ms = 0.0f;

        // SDI output rate when compositor output is being sent
        float compositor_out_fps = 0.0f;

        // Mean capture->SDI-out latency (ms) for compositor frames
        float compositor_avg_latency_ms = 0.0f;

        // Diagnostics based on FrameData::id continuity (compositor frames)
        uint64_t compositor_frames_sent_total = 0;
        uint64_t compositor_duplicates_total = 0;
        uint64_t compositor_drops_total = 0;

        int last_compositor_frame_id = -1;
        std::chrono::steady_clock::time_point last_update_steady;
    };
}

// --- Video Recorder State ---
namespace RecorderState {
    // Command sent from the UI to the VideoRecorder module
    struct Command {
        bool start_requested = false;   // Set true to begin recording
        bool stop_requested  = false;   // Set true to end recording
        std::string output_dir = "./recordings"; // Where to save files
    };

    // Status reported back to the UI by the VideoRecorder module
    struct Status {
        bool is_recording = false;
        uint64_t frame_count = 0;
        std::string current_video_file;
        std::string current_csv_file;
        std::chrono::steady_clock::time_point rec_start{};
        std::string last_error;
    };
}

// --- Legacy Data Structures (for backward compatibility) ---
// These can be removed once all code is updated to use the namespaced versions

// A struct to hold the final, aggregated information for a single tracked object.


// Add new struct for real-time player tracker parameters
struct PlayerTrackerRuntimeParams
{
    float confidence_threshold = 0.25f;
    float nms_threshold = 0.7f;
    bool filter_enabled = true;
};


// --- Sponsor Export State (recording of tracked sponsor placements) ---
namespace SponsorExportState {
    struct Command {
        bool start = false;   // one-shot: begin recording
        bool stop  = false;   // one-shot: stop recording
        std::string output_dir = "exports";  // directory for video + csv
        uint64_t request_id = 0;  // monotonic, to detect new commands
    };

    struct Status {
        bool is_recording = false;
        uint64_t frames_written = 0;
        std::string video_path;
        std::string csv_path;
        std::string last_error;
    };
}

// --- Sponsor Grid Tracker State (non-calibrated camera tracking, NVIDIA OFA) ---
namespace SponsorGridState {
    // Tracking status enumeration
    enum class TrackingStatus {
        IDLE,               // Not tracking
        ALIGNING,           // User is aligning the grid
        TRACKING,           // Actively tracking grid points
        LOST,               // Tracking lost (>20% points lost)
        RECOVERING          // Attempting to recover tracking
    };

    // Grid transformation parameters
    struct Transform {
        float offset_x = 0.0f;          // X offset in pixels
        float offset_y = 0.0f;          // Y offset in pixels
        float depth_z = 1.0f;           // Simulated Z depth (scale factor: 0.1 - 2.0)
        float rotation_deg = 0.0f;      // Rotation around Z-axis in degrees
        float pitch_deg = 0.0f;         // Optional: Rotation around X-axis
        float roll_deg = 0.0f;          // Optional: Rotation around Y-axis
        float grid_width = 1920.0f;     // Grid width in pixels (full frame)
        float grid_height = 1080.0f;    // Grid height in pixels (full frame)
        int grid_cols = 20;             // Number of columns in the grid
        int grid_rows = 15;             // Number of rows in the grid
    };

    // Configuration sent from UI to the module
    struct Config {
        // Sponsor-grid tracking is a Virtual Ad-only backend. Keep this flag
        // explicit so a sport change can disable an already-created module
        // without relying on the UI tab state or removing shared data.
        bool enabled = true;
        Transform transform;
        bool transform_changed = false;
        
        // Alignment control
        bool start_alignment = false;
        bool apply_alignment = false;
        bool cancel_alignment = false;
        
        // Graphic selection
        std::string graphic_path;
        bool graphic_changed = false;
        
        // Graphic transform (scale and orientation)
        float graphic_scale = 1.0f;         // Scale factor (0.1 - 5.0)
        float graphic_rotation = 0.0f;      // Rotation in degrees (-180 to 180)
        float graphic_yaw_deg = 0.0f;       // Yaw (spin around Z axis)
        float graphic_pitch_deg = 0.0f;     // Pitch (rotate around X axis)
        float graphic_roll_deg = 0.0f;      // Roll (rotate around Y axis)
        float graphic_opacity = 1.0f;       // Opacity (0 = hidden, 1 = opaque)
        bool graphic_transform_changed = false;
        
        // Graphic placement
        // One-shot command: make the tracked ad-grid cells render using the
        // current virtual-ad slot configuration. This is separate from the
        // legacy single-graphic right-click placement command below.
        bool place_grid = false;
        // Persistent operator intent for the unified ad grid. Once enabled by
        // Place Grid, a restored profile automatically renders its configured
        // cells after the operator completes the required fresh alignment.
        bool grid_graphics_enabled = false;
        bool place_graphic = false;
        cv::Point2f placement_point{-1, -1};
        uint64_t placement_request_id = 0;  // Monotonic ID for one-shot placement detection
        // Optional UI placement source. When set, the normal right-click
        // placement lifecycle is used but the selected ad-grid cell supplies
        // the perspective/reference quad and saved tracking points.
        bool place_graphic_from_grid = false;
        int placement_grid_slot = -1;
        std::array<cv::Point2f, 4> placement_reference_quad{};
        float graphic_width = 100.0f;
        float graphic_height = 100.0f;

        // =====================================================================
        // SECOND GRAPHIC (slot 2) — up to two graphics can be displayed.
        // Tracked by the same camera tracker as slot 1 (shared tracker): slot 2
        // is warped each frame by slot 1's reference->current homography.
        // active_graphic_slot routes load/place requests (0 = slot 1, 1 = slot 2).
        // =====================================================================
        int  active_graphic_slot = 0;
        std::string graphic2_path;
        bool graphic2_changed = false;
        float graphic2_opacity = 1.0f;
        bool remove_graphic2 = false;
        float graphic2_scale = 1.0f;
        float graphic2_yaw_deg = 0.0f;
        float graphic2_pitch_deg = 0.0f;
        float graphic2_roll_deg = 0.0f;
        bool graphic2_transform_changed = false;
        
        // UI toggles for visualization
        bool show_tracking_mask = false;    // Overlay chroma/tracking mask on video
        bool show_alignment_grid = false;   // Show alignment grid (default: hidden after alignment)
        bool show_dense_flow_debug = false; // Dense optical flow debug visualization
        
        // Tracking control
        bool freeze_tracking = false;       // Pause/freeze tracking (hold last position)
        bool request_reinit = false;        // Manual reinit request (one-shot)

        // Runtime tuning / diagnostics
        bool enable_async_loftr = false;    // Legacy: async Python LoFTR reinit
        bool save_debug_frames = false;     // Dump placement debug frames to logs/sponsor_tracker
        int reinit_retry_cooldown_ms = 300; // Throttle dense reinit retries while in REINIT

        // Reinit sidecar settings (ALIKED + LightGlue)
        bool enable_reinit_sidecar = false;
        std::string reinit_sidecar_host = "127.0.0.1";
        int reinit_sidecar_port = 5557;
        int reinit_sidecar_slots = 2;
        int reinit_sidecar_timeout_ms = 200;
        bool reinit_sidecar_only = false;
        
        // PTZ-adaptive smoothing and motion handling (runtime tunables)
        bool enable_ptz_adaptive_smoothing = false; // Enable PTZ adaptive smoothing
        float one_euro_min_cutoff = 0.5f;   // One-Euro min cutoff (Hz)
        float one_euro_beta = 0.007f;       // One-Euro beta (speed coefficient)
        float one_euro_d_cutoff = 1.0f;     // One-Euro derivative cutoff (Hz)
        float deadzone_ptz = 1.5f;          // Dead-zone (pixels) to use during PTZ
        float ptz_motion_threshold = 50.0f; // Motion magnitude above which PTZ mode activates
        bool enable_global_motion_subtraction = true; // Subtract global camera motion for dense tracking
        
        // Camera identifier for scoping reinit references to the active camera
        std::string active_camera_id = "";
        bool camera_id_changed = false;

        // One-shot override: which camera ID to use for the next manual reinit.
        // Empty = use active_camera_id. Cleared by the tracker after processing.
        std::string reinit_from_camera_id = "";

        // Grass color mask for optical flow filtering (HSV, OpenCV ranges: H 0-180, S/V 0-255)
        bool enable_grass_mask = false;
        int grass_hue_min = 30;
        int grass_hue_max = 80;
        int grass_sat_min = 40;
        int grass_sat_max = 255;
        int grass_val_min = 40;
        int grass_val_max = 255;
        bool show_grass_mask_preview = false;       // Show grass/chroma mask as green overlay on video
        bool use_chroma_mask_for_tracking = false;  // Use inverted chroma keyer mask instead of HSV grass mask
        bool invert_chroma_mask = true;             // Invert chroma mask (255=player → we want pitch)

        // NVIDIA OFA (dense optical flow) presets for the placed-graphic tracking path.
        // These are the biggest per-frame lever: ofa_perf_level 0=SLOW(best/slowest),
        // 1=MEDIUM, 2=FAST; ofa_grid_size 1/2/4 = output vector density (coarser=faster).
        // Defaults mirror the historical hard-coded values (SLOW, gs=4) so behaviour is
        // unchanged unless the operator tunes them. Applied live into g_tracking_params.
        int ofa_perf_level = 1;   // 0=SLOW, 1=MEDIUM, 2=FAST
        int ofa_grid_size = 4;    // 1, 2, or 4

        // Zoom-adaptive focal length calibration
        bool request_focal_capture = false;  // One-shot: estimate & store focal at current zoom level
        bool focal_calib_reset = false;       // One-shot: clear the calibration table

        // Debug overlays
        bool show_3d_pose_debug = false;          // Draw pose axes, drift, world origin on video
        bool auto_reinit_on_camera_return = false; // Auto-sidecar-reinit when camera goes still after PTZ

        // =====================================================================
        // STABILIZATION EXPERIMENTS (R&D toggles for uncalibrated AR ad placement)
        // Switchable in realtime from RightPanel to A/B test tracking strategies.
        // =====================================================================
        // Tracking mode for the legacy single-graphic path. The unified
        // sponsor grid always uses the immutable Apply Alignment anchor,
        // regardless of this legacy experiment setting.
        //   0 = Frame-chained homography (legacy)
        //   1 = Reference-anchored homography (legacy)
        int  exp_tracking_mode = 0;
        // Temporal averaging of projected quad corners over a sliding window —
        // trades a little lag for reduced jitter / sub-pixel OFA noise.
        bool exp_homography_averaging = false;
        int  exp_homography_avg_window = 5;       // frames (1..30)
        // Use the legacy solvePnP/projectPoints 3D-pose path. Needs REAL camera
        // intrinsics; on uncalibrated feeds it produces garbage — default OFF.
        bool exp_pnp_pose = false;
        // Force the grass/chroma mask into OFA flow sampling so player/foreground
        // motion is rejected from the camera-pose estimate.
        bool exp_chroma_stabilize = false;
        // Scale the graphic quad by the OFA-estimated zoom factor (anchor→current scale).
        bool exp_zoom_adaptive = false;
        // Only re-anchor to frames with sufficient sharpness/texture (Exp A).
        // Prevents locking a motion-blurred frame as the OFA reference during fast PTZ.
        bool exp_anchor_quality_gate = false;
        // Scale the RANSAC reprojection threshold with estimated flow magnitude (Exp C).
        // Loosens the threshold during fast camera motion (less precise OFA vectors),
        // tightens it during stationary phases (expect sub-pixel accuracy).
        bool exp_adaptive_ransac = false;
        // Replace the fixed 60-frame re-anchor counter with a per-anchor displacement
        // budget (Exp F). Re-anchor when accumulated drift exceeds exp_drift_budget_px,
        // regardless of frame count — avoids late forced-reanchor during fast motion.
        bool exp_drift_budget_reanchor = false;
        float exp_drift_budget_px = 200.0f;
        // Use motion jerk (velocity change) instead of a binary fast/slow flag to
        // trigger Kalman velocity snap after PTZ deceleration (Exp G).
        bool exp_jerk_snap = false;

        // Reset flags after processing
        void clearFlags() {
            transform_changed = false;
            start_alignment = false;
            apply_alignment = false;
            cancel_alignment = false;
            graphic_changed = false;
            place_grid = false;
            place_graphic = false;
            place_graphic_from_grid = false;
            graphic_transform_changed = false;
            request_reinit = false;
            camera_id_changed = false;
            graphic2_changed = false;
            remove_graphic2 = false;
            graphic2_transform_changed = false;
        }
    };

    // Results sent from module back to UI
    struct Results {
        TrackingStatus status = TrackingStatus::IDLE;
        float tracking_confidence = 0.0f;
        int active_point_count = 0;
        int total_point_count = 0;
        
        // Grid point positions for visualization
        std::vector<cv::Point2f> grid_points;
        std::vector<float> grid_confidence;              // Confidence per point
        std::vector<uint8_t> grid_allowed;              // Per-grid-point allowed mask (1 = allowed, 0 = disallowed)
        
        // Placed graphic data
        bool has_placed_graphic = false;
        std::array<cv::Point2f, 4> graphic_quad;        // Warped quad corners (slot 1)
        // Slot-2 graphic quad, published on the same stable shared-state channel so
        // consumers that run before the tracker in the frame (e.g. player_tracking)
        // can read a reliable last-known quad instead of same-frame per-frame data.
        bool has_placed_graphic2 = false;
        std::array<cv::Point2f, 4> graphic_quad2;       // Warped quad corners (slot 2)

        // Sponsor-grid placement cells. These are published independently of
        // the legacy single-graphic fields above so the old sponsor path stays
        // dormant until a legacy graphic is explicitly placed.
        std::vector<std::array<cv::Point2f, 4>> slot_quads;
        std::vector<uint8_t> slot_valid;
        // The aligned footprint can be tracked before an operator commits a
        // graphic reference.  Keep the cells available to the control panel,
        // but do not render them to video/SDI until Place Graphic is pressed.
        bool grid_graphics_placed = false;
        // When true, BOTH sponsor graphics must be hidden in the video panel and
        // compositor (set during reinit / disengage / feed-change / freeze).
        bool sponsor_graphics_hidden = false;
        
        // Homography matrix for external use
        cv::Mat homography;
        
        // Dense optical flow debug visualization
        bool show_dense_flow_debug = false;
        std::vector<cv::Point2f> dense_flow_old_pts;    // Previous frame points
        std::vector<cv::Point2f> dense_flow_new_pts;    // Current frame tracked points
        std::vector<cv::Point2f> dense_boundary;        // Current sponsor boundary (4 points)
        std::string dense_tracker_state;                // "TRIVIA", "DISENGAGE", "REINIT", etc.
        std::string exp_status;                         // active experiment summary (UI readout)

        // Camera debug info
        std::string active_camera_id;       // camera ID currently set
        std::string last_reinit_camera_id;  // camera whose refs produced the last successful reinit
        std::string last_placed_camera_id;  // camera ID active when graphic was last placed

        // 3D pose debug overlay data (populated when show_3d_pose_debug is true)
        bool show_3d_pose_debug = false;
        // Projected screen positions: [graphic_origin, X_tip, Y_tip, Z_tip,
        //                              world_origin, world_X, world_Y, world_Z]
        // In video pixel coords (before scale_x/scale_y correction).
        // Empty when pose is unavailable or flag off.
        std::vector<cv::Point2f> debug_pose_axes_pts;
        // Per-corner drift from initial placement quad (indices 0-3 match graphic_quad corners)
        std::array<cv::Point2f, 4> debug_initial_quad = {};
        std::array<float, 4>       debug_corner_drift_px = {};
        float debug_cumulative_drift_px = 0.0f;   // Average corner drift
        // Auto-reinit status: -1 = disabled, 0 = triggered this frame, >0 = frames since last
        int debug_auto_reinit_frames_stationary = 0;
        bool debug_auto_reinit_active = false;
        std::string debug_pose_info;              // e.g. "dist:12.3m tilt:28° drift:4.1px"

        // Reinit timing: duration of the most recent reinit cycle (0 = never reinit'd)
        float last_reinit_duration_ms = 0.0f;

        // Optical flow engine status
        bool ofa_active = false;                         // true when NvidiaOpticalFlow is driving tracking
        int  ofa_tracked_count = 0;                      // points tracked via OFA in last frame
        // OFA flow debug visualization (sampled motion vectors from last OFA frame)
        bool show_ofa_flow_debug = false;
        std::vector<cv::Point2f> ofa_flow_src_pts;       // Frame N positions
        std::vector<cv::Point2f> ofa_flow_dst_pts;       // Frame N+1 positions (after flow)
        cv::Mat ofa_affine_2x3;                          // Last successful affine pose (2x3 double)
        cv::Mat grass_mask_preview;                      // CV_8UC1, 255=grass; populated when show_grass_mask_preview=true

        // Per-experiment drift sparkline (last kDriftHistorySize frames, newest at end).
        // Always populated when tracking+placed. Empty otherwise. Used for in-video drift overlay.
        std::vector<float> drift_sparkline;
        float drift_current_px = 0.0f;  // Most recent avg corner drift (px)

        // Zoom-adaptive focal calibration readouts
        std::vector<std::pair<float,float>> focal_calib_entries;  // (zoom_scale, focal_px) sorted by zoom
        float current_focal_px = 0.0f;        // Currently active focal length (from table or heuristic)
        float current_ofa_zoom_scale = 1.0f;  // Current OFA anchor→current scale factor

        // Helper methods
        bool isTracking() const {
            return status == TrackingStatus::TRACKING;
        }
        
        bool isUnstable() const {
            return status == TrackingStatus::LOST || status == TrackingStatus::RECOVERING;
        }
    };
}

namespace TrackingPlaybackState {
    struct Config {
        std::string csv_path;
        int frame_offset = 0;
        bool overlay_enabled = true;
    };
}

// This class provides a generic, thread-safe container for sharing data from
// the UI thread to any of the processing module threads.
// This class provides a generic, thread-safe container for sharing data from
// the UI thread to any of the processing module threads.
class SharedState
{
public:
    SharedState() = default;

    // --- Generic Data Access Methods ---

    /**
     * @brief Sets a data object of a specific type in the shared state.
     * This is called by the UI thread to provide new parameters.
     * @tparam T The type of the data object.
     * @param data The data object to store.
     */
    template <typename T>
    void setData(const T &data)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        shared_data_[std::type_index(typeid(T))] = data;
    }

    /**
     * @brief Gets a data object of a specific type from the shared state.
     * This is called by module threads to retrieve the latest parameters.
     * @tparam T The type of the data object to retrieve.
     * @return An std::optional<T> containing the data if it exists, otherwise std::nullopt.
     */
    template <typename T>
    std::optional<T> getData()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = shared_data_.find(std::type_index(typeid(T)));
        if (it != shared_data_.end())
        {
            try
            {
                return std::any_cast<T>(it->second);
            }
            catch (const std::bad_any_cast &e)
            {
                // This case is rare but indicates a logic error
                return std::nullopt;
            }
        }
        return std::nullopt; // Data of this type has not been set yet
    }

    /**
     * @brief Removes a data object of a specific type from the shared state.
     * @tparam T The type of the data object to remove.
     * @return true if the data was found and removed, false otherwise.
     */
    template <typename T>
    bool removeData()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = shared_data_.find(std::type_index(typeid(T)));
        if (it != shared_data_.end())
        {
            shared_data_.erase(it);
            return true;
        }
        return false;
    }

    // Atomically disarm virtual-ad visibility for a bad HF projection packet.
    // Keeping the snapshot in SharedState prevents the receiver, compositor,
    // and UI threads from each overwriting it with an already-disabled state.
    bool tripHfFovSafety()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto safety_key = std::type_index(typeid(STypeState::HfFovSafetyState));
        auto safety_it = shared_data_.find(safety_key);
        STypeState::HfFovSafetyState safety;
        if (safety_it != shared_data_.end()) {
            try {
                safety = std::any_cast<STypeState::HfFovSafetyState>(safety_it->second);
            } catch (const std::bad_any_cast&) {
                safety = {};
            }
        }
        const auto animate_key = std::type_index(typeid(STypeState::AnimateInState));
        const auto preview_key = std::type_index(typeid(STypeState::PreviewState));
        if (safety.tripped) {
            // Keep the safety interlock asserted even if another UI/module
            // writes one of the visibility states while the packet is still
            // invalid.  Do not overwrite the original snapshot.
            auto animate = STypeState::AnimateInState{};
            auto animate_state_it = shared_data_.find(animate_key);
            if (animate_state_it != shared_data_.end()) {
                try {
                    animate = std::any_cast<STypeState::AnimateInState>(animate_state_it->second);
                } catch (const std::bad_any_cast&) {
                    animate = {};
                }
            }
            animate.enabled = false;
            shared_data_[animate_key] = animate;

            auto preview = STypeState::PreviewState{};
            auto preview_state_it = shared_data_.find(preview_key);
            if (preview_state_it != shared_data_.end()) {
                try {
                    preview = std::any_cast<STypeState::PreviewState>(preview_state_it->second);
                } catch (const std::bad_any_cast&) {
                    preview = {};
                }
            }
            preview.enabled = false;
            shared_data_[preview_key] = preview;
            return false;
        }

        auto animate_it = shared_data_.find(animate_key);
        if (animate_it != shared_data_.end()) {
            try {
                safety.animate_in_was_enabled =
                    std::any_cast<STypeState::AnimateInState>(animate_it->second).enabled;
            } catch (const std::bad_any_cast&) {
                safety.animate_in_was_enabled = false;
            }
        }
        auto preview_it = shared_data_.find(preview_key);
        if (preview_it != shared_data_.end()) {
            try {
                safety.preview_was_enabled =
                    std::any_cast<STypeState::PreviewState>(preview_it->second).enabled;
            } catch (const std::bad_any_cast&) {
                safety.preview_was_enabled = false;
            }
        }

        safety.tripped = true;
        shared_data_[safety_key] = safety;
        auto animate = STypeState::AnimateInState{};
        animate.enabled = false;
        shared_data_[animate_key] = animate;
        auto preview = STypeState::PreviewState{};
        preview.enabled = false;
        // Preserve the selected output/revision while only disarming preview.
        if (preview_it != shared_data_.end()) {
            try {
                preview = std::any_cast<STypeState::PreviewState>(preview_it->second);
            } catch (const std::bad_any_cast&) {
                preview = {};
            }
            preview.enabled = false;
        }
        shared_data_[preview_key] = preview;
        return true;
    }

    // Atomically restore the visibility controls captured by the first bad HF
    // packet.  Returns true only for the bad -> good transition.
    bool restoreHfFovSafety()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto safety_key = std::type_index(typeid(STypeState::HfFovSafetyState));
        auto safety_it = shared_data_.find(safety_key);
        if (safety_it == shared_data_.end())
            return false;

        STypeState::HfFovSafetyState safety;
        try {
            safety = std::any_cast<STypeState::HfFovSafetyState>(safety_it->second);
        } catch (const std::bad_any_cast&) {
            return false;
        }
        if (!safety.tripped)
            return false;

        STypeState::AnimateInState animate;
        const auto animate_key = std::type_index(typeid(STypeState::AnimateInState));
        auto animate_state_it = shared_data_.find(animate_key);
        if (animate_state_it != shared_data_.end()) {
            try {
                animate = std::any_cast<STypeState::AnimateInState>(animate_state_it->second);
            } catch (const std::bad_any_cast&) {
                animate = {};
            }
        }
        animate.enabled = safety.animate_in_was_enabled;
        shared_data_[animate_key] = animate;

        STypeState::PreviewState preview;
        const auto preview_key = std::type_index(typeid(STypeState::PreviewState));
        auto preview_state_it = shared_data_.find(preview_key);
        if (preview_state_it != shared_data_.end()) {
            try {
                preview = std::any_cast<STypeState::PreviewState>(preview_state_it->second);
            } catch (const std::bad_any_cast&) {
                preview = {};
            }
        }
        preview.enabled = safety.preview_was_enabled;
        shared_data_[preview_key] = preview;

        safety.tripped = false;
        shared_data_[safety_key] = safety;
        return true;
    }

    struct ModuleControlFlags
    {
        std::atomic<bool> chroma_key_enabled{false};
        std::atomic<bool> point_tracker_enabled{true};
        std::atomic<bool> player_tracker_enabled{true};
        // Add more as needed
    };

    // Fast, lock-free access
    ModuleControlFlags &getModuleFlags() { return module_flags_; }
    const ModuleControlFlags &getModuleFlags() const { return module_flags_; }

private:
    // A single mutex protects all shared data in this object.
    std::mutex mtx_;

    // A map holding arbitrary data types, keyed by their unique type information.
    // This makes the class extensible without modification.
    std::map<std::type_index, std::any> shared_data_;

    ModuleControlFlags module_flags_; // No mutex needed - atomic operations
};
