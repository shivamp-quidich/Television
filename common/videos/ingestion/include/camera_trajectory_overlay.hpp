#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include <imgui.h>
#include <opencv2/core.hpp>

#include "shared_state.h"

struct CameraTrajectoryOverlayOptions {
    bool enabled = true;
    bool show_grid = true;
    bool show_frustum = true;
    // Cone half-angle, and ray length as a percentage of the framed trail span
    // so the web keeps its proportions at any world scale. The defaults are the
    // stype_sim/viz3d.py values (14 degrees, 6 m against a ~40 m trail).
    float frustum_half_angle_deg = 14.0f;
    float frustum_length_percent = 15.0f;
    int max_trail_points = 1000;
    // Height of the ground plane the grid and the camera drop line sit on,
    // matching viz3d's world_origin_y_mm.
    float ground_plane_y_mm = 0.0f;
    // Screen position of the scene centre, normalized against the video
    // rectangle so the preview and the full-size SDI frame place it identically.
    float anchor_u = 0.5f;
    float anchor_v = 0.5f;
};

struct OverlayColor {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;
};

struct OverlayPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct OverlayLine {
    OverlayPoint a;
    OverlayPoint b;
    OverlayColor color;
    float thickness = 1.0f;
};

struct OverlayCircle {
    OverlayPoint center;
    float radius = 1.0f;
    OverlayColor color;
    bool filled = true;
    float thickness = 1.0f;
};

// Resolution-independent drawing primitives for one tracking pose. Building the
// geometry separately lets the on-screen preview and the SDI frame render the
// identical visualization at their own pixel sizes. Pose telemetry is not part
// of the drawing; it is reported in the control panel instead.
struct CameraTrajectoryGeometry {
    std::vector<OverlayLine> lines;
    std::vector<OverlayCircle> circles;
};

CameraTrajectoryGeometry buildCameraTrajectoryGeometry(
    float min_x, float min_y, float max_x, float max_y,
    const std::deque<STypeState::CameraData>& trail,
    const STypeState::CameraData& camera,
    const CameraTrajectoryOverlayOptions& options);

// Draws over the ImGui preview image without modifying video pixels.
void drawCameraTrajectoryOverlay(
    ImDrawList* draw_list,
    const ImVec2& video_min,
    const ImVec2& video_max,
    const std::deque<STypeState::CameraData>& trail,
    const STypeState::CameraData& camera,
    const CameraTrajectoryOverlayOptions& options = {});

// Burns the same visualization into a BGR frame so it survives to SDI output.
void drawCameraTrajectoryOnBgr(
    cv::Mat& bgr,
    const std::deque<STypeState::CameraData>& trail,
    const STypeState::CameraData& camera,
    const CameraTrajectoryOverlayOptions& options = {});
