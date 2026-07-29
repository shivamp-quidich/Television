#include "camera_trajectory_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <opencv2/imgproc.hpp>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMmToM = 0.001f;
constexpr float kMinSpanMetres = 2.0f;
constexpr float kReferenceHeightPx = 720.0f;

// Proportions taken from stype_sim/viz3d.py, expressed as a fraction of the
// framed trail so the visualization reads the same at studio and stadium scale
// (the reference uses 6 m and 8 m against a roughly 40 m trail). The frustum
// equivalents are operator-controlled and live in the options struct.
constexpr float kArrowLengthFraction = 0.20f;
constexpr int kFrustumRays = 8;
constexpr int kGridDivisionsPerSide = 5;

// Primitives at or above this alpha are painted opaque; the rest go through a
// single blended pass so the video stays readable underneath the web.
constexpr std::uint8_t kSolidAlphaThreshold = 200;

// Plot space, matching viz3d.py: x and y span the ground, z is height.
// World-to-plot is (world X, world Z, world Y) because FreeD uses world Y as up.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float length(const Vec3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

Vec3 normalize(const Vec3& a)
{
    const float len = length(a);
    return len > 1e-6f ? mul(a, 1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}

float deg2rad(float deg) { return deg * kPi / 180.0f; }

Vec3 positionOf(const STypeState::CameraData& cam)
{
    return {cam.x_mm * kMmToM, cam.z_mm * kMmToM, cam.y_mm * kMmToM};
}

struct CameraBasis {
    Vec3 look;
    Vec3 right;
    Vec3 down;
};

// Mirrors projection.build_camera_basis, then swaps world Y/Z into plot space.
CameraBasis basisOf(const STypeState::CameraData& cam)
{
    const float pan = deg2rad(cam.pan_deg);
    const float tilt = deg2rad(cam.tilt_deg);
    const float roll = deg2rad(cam.roll_deg);

    const float lx = std::sin(pan) * std::cos(tilt);
    const float ly = std::sin(tilt);
    const float lz = std::cos(pan) * std::cos(tilt);

    const float r0x = std::cos(pan);
    const float r0y = 0.0f;
    const float r0z = -std::sin(pan);

    // Rodrigues: rotate the pre-roll right vector around the look axis.
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
    const float cx = ly * r0z - lz * r0y;
    const float cy = lz * r0x - lx * r0z;
    const float cz = lx * r0y - ly * r0x;
    const float rx = r0x * cr + cx * sr;
    const float ry = r0y * cr + cy * sr;
    const float rz = r0z * cr + cz * sr;

    const float dx = ry * lz - rz * ly;
    const float dy = rz * lx - rx * lz;
    const float dz = rx * ly - ry * lx;

    const auto to_plot = [](float vx, float vy, float vz) { return Vec3{vx, vz, vy}; };
    return {normalize(to_plot(lx, ly, lz)),
            normalize(to_plot(rx, ry, rz)),
            normalize(to_plot(dx, dy, dz))};
}

// Isometric camera: plot z rises on screen, the ground axes fan out sideways.
struct IsoProjector {
    Vec3 center;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float scale = 1.0f;

    OverlayPoint operator()(const Vec3& p) const
    {
        const float dx = p.x - center.x;
        const float dy = p.y - center.y;
        const float dz = p.z - center.z;
        const float ix = (dx - dy) * 0.8660254f;
        const float iy = (dx + dy) * 0.5f - dz;
        return {origin_x + ix * scale, origin_y + iy * scale};
    }
};

// Rounds a spacing up to the nearest 1/2/5 x 10^n so the grid reads cleanly.
float niceStep(float value)
{
    if (value <= 0.0f)
        return 1.0f;
    const float base = std::pow(10.0f, std::floor(std::log10(value)));
    const float mantissa = value / base;
    const float multiplier = mantissa <= 1.0f ? 1.0f
                           : mantissa <= 2.0f ? 2.0f
                           : mantissa <= 5.0f ? 5.0f
                                              : 10.0f;
    return multiplier * base;
}

struct Bounds {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};

    void add(const Vec3& p)
    {
        min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
    Vec3 center() const
    {
        return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f, (min.z + max.z) * 0.5f};
    }
    float span() const
    {
        return std::max({max.x - min.x, max.y - min.y, max.z - min.z, kMinSpanMetres});
    }
};

} // namespace

CameraTrajectoryGeometry buildCameraTrajectoryGeometry(
    float min_x, float min_y, float max_x, float max_y,
    const std::deque<STypeState::CameraData>& trail,
    const STypeState::CameraData& camera,
    const CameraTrajectoryOverlayOptions& options)
{
    CameraTrajectoryGeometry geometry;
    const float width = max_x - min_x;
    const float height = max_y - min_y;
    if (!options.enabled || width < 64.0f || height < 64.0f)
        return geometry;

    const float px_scale = height / kReferenceHeightPx;
    const float thin = std::max(1.0f, 1.2f * px_scale);
    const float medium = std::max(1.5f, 2.0f * px_scale);
    const float thick = std::max(2.0f, 3.2f * px_scale);

    // Keep the drawing inside the picture-safe area rather than the frame edge.
    const float inset_x = width * 0.06f;
    const float inset_y = height * 0.08f;
    const float content_min_x = min_x + inset_x;
    const float content_max_x = max_x - inset_x;
    const float content_min_y = min_y + inset_y;
    const float content_max_y = max_y - inset_y;
    const float content_w = std::max(content_max_x - content_min_x, 1.0f);
    const float content_h = std::max(content_max_y - content_min_y, 1.0f);

    const Vec3 position = positionOf(camera);
    const CameraBasis basis = basisOf(camera);
    const float ground_z = options.ground_plane_y_mm * kMmToM;

    // The view is framed by the trail alone, the way viz3d fixes its axis limits
    // in the static layer. The camera marker, arrow and cone then move across a
    // stationary scene instead of dragging the world around with them.
    Bounds trail_bounds;
    for (const auto& c : trail)
        trail_bounds.add(positionOf(c));
    if (trail.empty())
        trail_bounds.add(position);

    const float span = trail_bounds.span();
    const float frustum_length =
        span * std::clamp(options.frustum_length_percent, 0.5f, 200.0f) * 0.01f;
    const float arrow_length = span * kArrowLengthFraction;
    const float grid_step = niceStep(span / static_cast<float>(kGridDivisionsPerSide));

    // viz3d's axis padding, applied to the same plot axes.
    Bounds view = trail_bounds;
    const float pad_x = std::max(5.0f, (trail_bounds.max.x - trail_bounds.min.x) * 0.12f + 2.0f);
    const float pad_y = std::max(5.0f, (trail_bounds.max.y - trail_bounds.min.y) * 0.12f + 2.0f);
    const float pad_z = std::max(2.0f, (trail_bounds.max.z - trail_bounds.min.z) * 0.15f + 1.0f);
    view.min = {view.min.x - pad_x, view.min.y - pad_y, view.min.z - pad_z};
    view.max = {view.max.x + pad_x, view.max.y + pad_y, view.max.z + pad_z};

    IsoProjector project;
    project.center = view.center();
    project.scale = 1.0f;

    // Fit the padded view box, not the live pose, so the scale holds still.
    Bounds iso;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 world{(corner & 1) ? view.max.x : view.min.x,
                         (corner & 2) ? view.max.y : view.min.y,
                         (corner & 4) ? view.max.z : view.min.z};
        const OverlayPoint q = project(world);
        iso.add({q.x, q.y, 0.0f});
    }

    // Hold the anchor away from the edges so every side keeps real room. A floor
    // on the room itself would instead let the drawing spill out of frame.
    const float anchor_margin_x = content_w * 0.12f;
    const float anchor_margin_y = content_h * 0.12f;
    const float anchor_x = std::clamp(min_x + options.anchor_u * width,
                                      content_min_x + anchor_margin_x,
                                      content_max_x - anchor_margin_x);
    const float anchor_y = std::clamp(min_y + options.anchor_v * height,
                                      content_min_y + anchor_margin_y,
                                      content_max_y - anchor_margin_y);

    // Largest scale that still keeps the whole view box inside the content box.
    const float room_right = content_max_x - anchor_x;
    const float room_left = anchor_x - content_min_x;
    const float room_down = content_max_y - anchor_y;
    const float room_up = anchor_y - content_min_y;

    float scale = std::numeric_limits<float>::max();
    if (iso.max.x > 1e-6f)
        scale = std::min(scale, room_right / iso.max.x);
    if (iso.min.x < -1e-6f)
        scale = std::min(scale, room_left / -iso.min.x);
    if (iso.max.y > 1e-6f)
        scale = std::min(scale, room_down / iso.max.y);
    if (iso.min.y < -1e-6f)
        scale = std::min(scale, room_up / -iso.min.y);
    project.scale = (scale < std::numeric_limits<float>::max()) ? scale : 1.0f;
    project.origin_x = anchor_x;
    project.origin_y = anchor_y;

    const auto line = [&geometry](OverlayPoint a, OverlayPoint b, OverlayColor color, float t) {
        geometry.lines.push_back({a, b, color, t});
    };

    if (options.show_grid) {
        const OverlayColor grid_color{165, 180, 195, 55};
        const float pad = grid_step;
        const float gx0 = std::floor(std::min(trail_bounds.min.x, 0.0f) / grid_step) * grid_step - pad;
        const float gx1 = std::ceil(std::max(trail_bounds.max.x, 0.0f) / grid_step) * grid_step + pad;
        const float gy0 = std::floor(std::min(trail_bounds.min.y, 0.0f) / grid_step) * grid_step - pad;
        const float gy1 = std::ceil(std::max(trail_bounds.max.y, 0.0f) / grid_step) * grid_step + pad;
        for (float x = gx0; x <= gx1 + 0.5f * grid_step; x += grid_step)
            line(project({x, gy0, ground_z}), project({x, gy1, ground_z}), grid_color, thin);
        for (float y = gy0; y <= gy1 + 0.5f * grid_step; y += grid_step)
            line(project({gx0, y, ground_z}), project({gx1, y, ground_z}), grid_color, thin);

        const Vec3 origin{0.0f, 0.0f, 0.0f};
        const float axis_length = std::max(span * 0.15f, grid_step);
        line(project(origin), project({axis_length, 0.0f, 0.0f}), OverlayColor{238, 68, 68, 220}, medium);
        line(project(origin), project({0.0f, axis_length, 0.0f}), OverlayColor{68, 110, 238, 220}, medium);
        line(project(origin), project({0.0f, 0.0f, axis_length}), OverlayColor{68, 238, 68, 220}, medium);
        geometry.circles.push_back({project(origin), std::max(3.0f, 4.0f * px_scale),
                                    OverlayColor{255, 221, 0, 230}, true, 1.0f});
    }

    // Trail shaded along the "cool" colormap, cyan for the oldest sample
    // through magenta for the newest.
    if (trail.size() > 1) {
        const std::size_t count = trail.size();
        for (std::size_t i = 1; i < count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count - 1);
            OverlayColor color;
            color.r = static_cast<std::uint8_t>(255.0f * t);
            color.g = static_cast<std::uint8_t>(255.0f * (1.0f - t));
            color.b = 255;
            color.a = static_cast<std::uint8_t>(90.0f + 130.0f * t);
            line(project(positionOf(trail[i - 1])), project(positionOf(trail[i])), color, medium);
        }
    }

    const OverlayPoint camera_px = project(position);

    // Drop line to the ground plane gives the isometric view a height reference.
    line(project({position.x, position.y, ground_z}), camera_px, OverlayColor{255, 255, 255, 90}, thin);

    if (options.show_frustum) {
        const float half_angle = deg2rad(std::clamp(options.frustum_half_angle_deg, 0.5f, 89.0f));
        const OverlayColor web{255, 102, 0, 90};
        const OverlayColor rim_color{255, 102, 0, 130};
        OverlayPoint rim[kFrustumRays];
        for (int i = 0; i < kFrustumRays; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kFrustumRays);
            const Vec3 edge = add(mul(basis.look, std::cos(half_angle)),
                                  mul(add(mul(basis.right, std::cos(a)),
                                          mul(basis.down, std::sin(a))),
                                      std::sin(half_angle)));
            rim[i] = project(add(position, mul(normalize(edge), frustum_length)));
        }
        for (int i = 0; i < kFrustumRays; ++i) {
            line(camera_px, rim[i], web, thin);
            line(rim[i], rim[(i + 1) % kFrustumRays], rim_color, thin);
        }
    }

    // Heading arrow with a simple head drawn in projected space.
    const OverlayPoint tip_px = project(add(position, mul(basis.look, arrow_length)));
    const OverlayColor arrow_color{255, 102, 0, 240};
    line(camera_px, tip_px, arrow_color, thick);
    {
        const float dx = tip_px.x - camera_px.x;
        const float dy = tip_px.y - camera_px.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-3f) {
            const float ux = dx / len;
            const float uy = dy / len;
            const float head = std::max(8.0f, 14.0f * px_scale);
            line(tip_px, {tip_px.x - ux * head + uy * head * 0.5f, tip_px.y - uy * head - ux * head * 0.5f},
                 arrow_color, thick);
            line(tip_px, {tip_px.x - ux * head - uy * head * 0.5f, tip_px.y - uy * head + ux * head * 0.5f},
                 arrow_color, thick);
        }
    }

    geometry.circles.push_back({camera_px, std::max(4.0f, 6.0f * px_scale),
                                OverlayColor{255, 102, 0, 255}, true, 1.0f});
    geometry.circles.push_back({camera_px, std::max(8.0f, 12.0f * px_scale),
                                OverlayColor{255, 170, 60, 170}, false, medium});

    return geometry;
}

void drawCameraTrajectoryOverlay(
    ImDrawList* draw_list,
    const ImVec2& video_min,
    const ImVec2& video_max,
    const std::deque<STypeState::CameraData>& trail,
    const STypeState::CameraData& camera,
    const CameraTrajectoryOverlayOptions& options)
{
    if (!draw_list)
        return;
    const CameraTrajectoryGeometry geometry = buildCameraTrajectoryGeometry(
        video_min.x, video_min.y, video_max.x, video_max.y, trail, camera, options);
    if (geometry.lines.empty() && geometry.circles.empty())
        return;

    const auto to_imgui = [](const OverlayColor& c) { return IM_COL32(c.r, c.g, c.b, c.a); };
    draw_list->PushClipRect(video_min, video_max, true);
    for (const auto& l : geometry.lines)
        draw_list->AddLine(ImVec2(l.a.x, l.a.y), ImVec2(l.b.x, l.b.y), to_imgui(l.color), l.thickness);
    for (const auto& c : geometry.circles) {
        if (c.filled)
            draw_list->AddCircleFilled(ImVec2(c.center.x, c.center.y), c.radius, to_imgui(c.color), 24);
        else
            draw_list->AddCircle(ImVec2(c.center.x, c.center.y), c.radius, to_imgui(c.color), 24, c.thickness);
    }
    draw_list->PopClipRect();
}

void drawCameraTrajectoryOnBgr(
    cv::Mat& bgr,
    const std::deque<STypeState::CameraData>& trail,
    const STypeState::CameraData& camera,
    const CameraTrajectoryOverlayOptions& options)
{
    if (bgr.empty() || bgr.type() != CV_8UC3)
        return;
    const CameraTrajectoryGeometry geometry = buildCameraTrajectoryGeometry(
        0.0f, 0.0f, static_cast<float>(bgr.cols), static_cast<float>(bgr.rows),
        trail, camera, options);
    if (geometry.lines.empty() && geometry.circles.empty())
        return;

    const auto to_scalar = [](const OverlayColor& c) {
        return cv::Scalar(static_cast<double>(c.b), static_cast<double>(c.g), static_cast<double>(c.r));
    };
    // Grid and origin lines can run far outside the frame; clamp before the
    // float-to-int conversion so OpenCV's clipping sees sane coordinates.
    const auto to_point = [](const OverlayPoint& p) {
        return cv::Point(static_cast<int>(std::lround(std::clamp(p.x, -1e6f, 1e6f))),
                         static_cast<int>(std::lround(std::clamp(p.y, -1e6f, 1e6f))));
    };
    const auto stroke = [](float thickness) { return std::max(1, static_cast<int>(std::lround(thickness))); };

    // OpenCV drawing is opaque, so translucent primitives are painted on a copy
    // of the region they cover and blended back in one pass.
    cv::Rect blend_roi;
    for (const auto& l : geometry.lines) {
        if (l.color.a >= kSolidAlphaThreshold)
            continue;
        const cv::Rect r = cv::Rect(to_point(l.a), to_point(l.b)) +
                           cv::Size(stroke(l.thickness) * 2, stroke(l.thickness) * 2);
        blend_roi = blend_roi.empty() ? r : (blend_roi | r);
    }
    blend_roi &= cv::Rect(0, 0, bgr.cols, bgr.rows);

    if (!blend_roi.empty()) {
        cv::Mat region = bgr(blend_roi);
        cv::Mat overlay = region.clone();
        for (const auto& l : geometry.lines) {
            if (l.color.a >= kSolidAlphaThreshold)
                continue;
            cv::line(overlay, to_point(l.a) - blend_roi.tl(), to_point(l.b) - blend_roi.tl(),
                     to_scalar(l.color), stroke(l.thickness), cv::LINE_AA);
        }
        cv::addWeighted(overlay, 0.6, region, 0.4, 0.0, region);
    }

    for (const auto& l : geometry.lines) {
        if (l.color.a < kSolidAlphaThreshold)
            continue;
        cv::line(bgr, to_point(l.a), to_point(l.b), to_scalar(l.color), stroke(l.thickness), cv::LINE_AA);
    }
    for (const auto& c : geometry.circles) {
        cv::circle(bgr, to_point(c.center), std::max(1, static_cast<int>(std::lround(c.radius))),
                   to_scalar(c.color), c.filled ? cv::FILLED : stroke(c.thickness), cv::LINE_AA);
    }
}
