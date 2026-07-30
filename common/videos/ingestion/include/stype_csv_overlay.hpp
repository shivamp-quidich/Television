#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace stype {

// One row from a Stype HF tracking CSV. Positions are millimetres; angles are
// degrees. The names intentionally match the CSV columns.
struct Record {
    std::int64_t frame_id = 0;
    std::int64_t timestamp_ms = 0;
    bool stype_valid = false;
    double pan_deg = 0.0;
    double tilt_deg = 0.0;
    double roll_deg = 0.0;
    double x_mm = 0.0;
    double y_mm = 0.0;
    double z_mm = 0.0;
    std::int64_t zoom_raw = 0;
    std::int64_t focus_raw = 0;
    double hfov_deg = 0.0;
    double hf_ar = 0.0;
    double hf_k1 = 0.0;
    double hf_k2 = 0.0;
    double hf_cx_mm = 0.0;
    double hf_cy_mm = 0.0;
    double hf_pa_width_mm = 0.0;

    bool hasDirectFov() const noexcept { return hfov_deg > 0.5 && hfov_deg < 180.0; }
};

using Records = std::vector<Record>;

// Loads a CSV atomically: records is unchanged if false is returned. error, if
// supplied, receives an actionable parse/open error.
bool loadCsv(const std::string& path, Records& records, std::string* error = nullptr);

struct AlignmentAdjust {
    // Per-axis sign (+1 / -1). Clicking + or - in the Apply Alignment panel
    // sets the corresponding sign; used every frame when projecting the origin.
    int sign_pan = +1;
    int sign_tilt = +1;
    int sign_roll = +1;
    int sign_x = +1;
    int sign_y = +1;
    int sign_z = +1;

    // Additive offsets applied after the sign (degrees / millimetres).
    double add_pan_deg = 0.0;
    double add_tilt_deg = 0.0;
    double add_roll_deg = 0.0;
    double add_x_mm = 0.0;
    double add_y_mm = 0.0;
    double add_z_mm = 0.0;

    double angle_step_deg = 0.5;
    double position_step_mm = 50.0;

    void reset()
    {
        sign_pan = sign_tilt = sign_roll = +1;
        sign_x = sign_y = sign_z = +1;
        add_pan_deg = add_tilt_deg = add_roll_deg = 0.0;
        add_x_mm = add_y_mm = add_z_mm = 0.0;
    }
};

struct OverlayOptions {
    double gizmo_length_mm = 500.0;
    double fallback_vfov_deg = 25.0;
    bool apply_distortion = true;
    AlignmentAdjust alignment;
};

// Applies sign + offset to a tracking row (does not mutate the CSV source).
Record applyAlignment(const Record& record, const AlignmentAdjust& alignment);

// Draws the world-origin XYZ axes and a compact tracking readout in-place on a
// BGR frame. A direct Stype HF horizontal FOV is used when present; otherwise
// fallback_vfov_deg supplies a simple pinhole camera approximation.
void drawOverlay(cv::Mat bgr, const Record& record,
                 const OverlayOptions& options = OverlayOptions{});

} // namespace stype
