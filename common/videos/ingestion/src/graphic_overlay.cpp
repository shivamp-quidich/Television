#include "graphic_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kVirtualCamDist = 1000.0f;

bool isImageExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".bmp" || ext == ".webp" || ext == ".tga";
}

cv::Point3d bilinearWorld(const std::array<cv::Point3d, 4>& corners,
                          float u01, float v01)
{
    // corners: TL, TR, BR, BL — u,v in [0,1]
    const cv::Point3d top = corners[0] * (1.0 - u01) + corners[1] * u01;
    const cv::Point3d bot = corners[3] * (1.0 - u01) + corners[2] * u01;
    return top * (1.0 - v01) + bot * v01;
}

} // namespace

GraphicOverlay::~GraphicOverlay()
{
    clearAll();
}

void GraphicOverlay::destroyTexture(GLuint& texture)
{
    if (texture != 0) {
        glDeleteTextures(1, &texture);
        texture = 0;
    }
}

void GraphicOverlay::destroyPending_()
{
    destroyTexture(pending_texture_);
    pending_width_ = 0;
    pending_height_ = 0;
    pending_rgba_.release();
    pending_name_.clear();
}

void GraphicOverlay::refreshFileList(const std::string& data_dir)
{
    files_.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec))
        return;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        if (!isImageExtension(entry.path()))
            continue;
        files_.push_back(entry.path().string());
    }
    std::sort(files_.begin(), files_.end());
}

bool GraphicOverlay::uploadTexture(const cv::Mat& bgra, GLuint& texture,
                                   int& width, int& height, std::string& error)
{
    if (bgra.empty() || bgra.type() != CV_8UC4) {
        error = "expected BGRA image data";
        return false;
    }
    cv::Mat upload;
    cv::cvtColor(bgra, upload, cv::COLOR_BGRA2RGBA);
    destroyTexture(texture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, upload.cols, upload.rows, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, upload.data);
    glBindTexture(GL_TEXTURE_2D, 0);
    width = upload.cols;
    height = upload.rows;
    error.clear();
    return true;
}

bool GraphicOverlay::selectFile(int index, std::string& error)
{
    if (index < 0 || index >= static_cast<int>(files_.size())) {
        error = "no image selected";
        return false;
    }
    cv::Mat loaded = cv::imread(files_[static_cast<std::size_t>(index)], cv::IMREAD_UNCHANGED);
    if (loaded.empty()) {
        error = "failed to load " + files_[static_cast<std::size_t>(index)];
        return false;
    }
    cv::Mat rgba;
    if (loaded.channels() == 4)
        rgba = loaded;
    else if (loaded.channels() == 3)
        cv::cvtColor(loaded, rgba, cv::COLOR_BGR2BGRA);
    else if (loaded.channels() == 1)
        cv::cvtColor(loaded, rgba, cv::COLOR_GRAY2BGRA);
    else {
        error = "unsupported channel count";
        return false;
    }
    GLuint texture = 0;
    int width = 0, height = 0;
    if (!uploadTexture(rgba, texture, width, height, error))
        return false;
    destroyPending_();
    pending_texture_ = texture;
    pending_width_ = width;
    pending_height_ = height;
    pending_rgba_ = std::move(rgba);
    pending_name_ = std::filesystem::path(files_[static_cast<std::size_t>(index)]).filename().string();
    return true;
}

bool GraphicOverlay::placeAt(float center_u, float center_v, float width_fraction)
{
    if (pending_texture_ == 0 || pending_rgba_.empty())
        return false;
    PlacedGraphic item;
    item.name = pending_name_;
    item.image_width = pending_width_;
    item.image_height = pending_height_;
    item.rgba = pending_rgba_.clone();
    item.center_u = std::clamp(center_u, 0.0f, 1.0f);
    item.center_v = std::clamp(center_v, 0.0f, 1.0f);
    item.width_fraction = std::clamp(width_fraction, 0.02f, 1.0f);
    std::string error;
    if (!uploadTexture(item.rgba, item.texture, item.image_width, item.image_height, error))
        return false;
    placed_.push_back(std::move(item));
    destroyPending_();
    return true;
}

cv::Point2f GraphicOverlay::applyPlaneTransform(const cv::Point2f& local,
                                                const GraphicGridTransform& t)
{
    const float rad = t.rotation_deg * kPi / 180.0f;
    float rx = local.x * std::cos(rad) - local.y * std::sin(rad);
    float ry = local.x * std::sin(rad) + local.y * std::cos(rad);
    rx *= t.depth_z;
    ry *= t.depth_z;
    if (std::abs(t.pitch_deg) > 0.01f) {
        const float pitch_rad = t.pitch_deg * kPi / 180.0f;
        const float z_offset = ry * std::sin(pitch_rad);
        const float perspective_scale = kVirtualCamDist / (kVirtualCamDist - z_offset);
        rx *= perspective_scale;
        ry = (ry * std::cos(pitch_rad)) * perspective_scale;
    }
    if (std::abs(t.roll_deg) > 0.01f) {
        const float roll_rad = t.roll_deg * kPi / 180.0f;
        const float z_offset = rx * std::sin(roll_rad);
        const float perspective_scale = kVirtualCamDist / (kVirtualCamDist - z_offset);
        rx = (rx * std::cos(roll_rad)) * perspective_scale;
        ry *= perspective_scale;
    }
    return {rx, ry};
}

std::array<cv::Point2f, 4> GraphicOverlay::alignmentQuad(const GraphicGridTransform& t,
                                                         int video_w, int video_h)
{
    const std::array<cv::Point2f, 4> plane = {{
        {-t.grid_width * 0.5f, -t.grid_height * 0.5f},
        { t.grid_width * 0.5f, -t.grid_height * 0.5f},
        { t.grid_width * 0.5f,  t.grid_height * 0.5f},
        {-t.grid_width * 0.5f,  t.grid_height * 0.5f},
    }};
    const float cx = static_cast<float>(video_w) * 0.5f + t.offset_x;
    const float cy = static_cast<float>(video_h) * 0.5f + t.offset_y;
    std::array<cv::Point2f, 4> quad{};
    for (std::size_t i = 0; i < 4; ++i) {
        const cv::Point2f rel = applyPlaneTransform(plane[i], t);
        quad[i] = {rel.x + cx, rel.y + cy};
    }
    return quad;
}

std::vector<cv::Point2f> GraphicOverlay::alignmentGridPoints(const GraphicGridTransform& t,
                                                             int video_w, int video_h)
{
    const int cols = std::max(2, t.grid_cols);
    const int rows = std::max(2, t.grid_rows);
    const float cx = static_cast<float>(video_w) * 0.5f + t.offset_x;
    const float cy = static_cast<float>(video_h) * 0.5f + t.offset_y;
    std::vector<cv::Point2f> points;
    points.reserve(static_cast<std::size_t>(cols * rows));
    for (int r = 0; r < rows; ++r) {
        const float v = static_cast<float>(r) / static_cast<float>(rows - 1);
        for (int c = 0; c < cols; ++c) {
            const float u = static_cast<float>(c) / static_cast<float>(cols - 1);
            const cv::Point2f local((u - 0.5f) * t.grid_width, (v - 0.5f) * t.grid_height);
            const cv::Point2f rel = applyPlaneTransform(local, t);
            points.emplace_back(rel.x + cx, rel.y + cy);
        }
    }
    return points;
}

cv::Mat GraphicOverlay::planeHomography(const GraphicGridTransform& t,
                                        int video_w, int video_h)
{
    const auto quad = alignmentQuad(t, video_w, video_h);
    const cv::Point2f src[4] = {
        {-t.grid_width * 0.5f, -t.grid_height * 0.5f},
        { t.grid_width * 0.5f, -t.grid_height * 0.5f},
        { t.grid_width * 0.5f,  t.grid_height * 0.5f},
        {-t.grid_width * 0.5f,  t.grid_height * 0.5f},
    };
    const cv::Point2f dst[4] = {quad[0], quad[1], quad[2], quad[3]};
    try {
        return cv::getPerspectiveTransform(src, dst);
    } catch (const cv::Exception&) {
        return {};
    }
}

const GraphicGridTransform& GraphicOverlay::activeGrid(const GraphicOverlayOptions& options) const
{
    if (options.align_state == GridAlignState::Applied && applied_grid_valid_)
        return applied_grid_;
    return options.grid;
}

void GraphicOverlay::startAlignment(GraphicOverlayOptions& options)
{
    if (applied_grid_valid_)
        options.grid = applied_grid_;
    options.align_state = GridAlignState::Aligning;
    options.show_grid = true;
    options.plane_grid = true;
}

bool GraphicOverlay::bakeWorldPlane(const GraphicGridTransform& grid, int frame_w, int frame_h,
                                    const stype::Record& pose,
                                    const stype::OverlayOptions& overlay_options)
{
    const auto quad = alignmentQuad(grid, frame_w, frame_h);
    std::array<cv::Point3d, 4> world{};
    for (int i = 0; i < 4; ++i) {
        double x = 0.0, y = 0.0, z = 0.0;
        if (!stype::unprojectToGround(pose, overlay_options, frame_w, frame_h,
                                      quad[static_cast<std::size_t>(i)].x,
                                      quad[static_cast<std::size_t>(i)].y,
                                      x, y, z)) {
            world_plane_valid_ = false;
            return false;
        }
        world[static_cast<std::size_t>(i)] = {x, y, z};
    }
    world_plane_corners_ = world;
    world_grid_width_ = grid.grid_width;
    world_grid_height_ = grid.grid_height;
    world_plane_valid_ = true;
    return true;
}

bool GraphicOverlay::applyAlignment(GraphicOverlayOptions& options,
                                    int frame_w, int frame_h,
                                    const stype::Record* pose,
                                    const stype::OverlayOptions* overlay_options,
                                    std::string& status)
{
    GraphicGridTransform grid = options.grid;
    if (grid.grid_width < 50.0f)
        grid.grid_width = static_cast<float>(std::max(frame_w, 1));
    if (grid.grid_height < 50.0f)
        grid.grid_height = static_cast<float>(std::max(frame_h, 1));

    applied_grid_ = grid;
    applied_grid_valid_ = true;
    options.grid = grid;
    options.align_state = GridAlignState::Applied;
    options.show_grid = false; // hide dots; plane kept for graphics

    if (pose != nullptr && overlay_options != nullptr && frame_w > 0 && frame_h > 0 &&
        bakeWorldPlane(grid, frame_w, frame_h, *pose, *overlay_options)) {
        // Re-anchor already-placed plane graphics into world space.
        for (auto& item : placed_) {
            if (!item.plane_space)
                continue;
            if (planeRectToWorld(item.plane_cx, item.plane_cy, item.plane_width, item.plane_height,
                                 options.live, item.world_corners)) {
                item.world_anchored = true;
            }
        }
        status = "Grid applied — hidden; graphics follow UDP/world origin";
        return true;
    }

    world_plane_valid_ = false;
    for (auto& item : placed_)
        item.world_anchored = false;
    status = "Grid applied — hidden (screen plane). Waiting for UDP pose to lock world";
    return true;
}

void GraphicOverlay::clearAlignment(GraphicOverlayOptions& options)
{
    options.align_state = GridAlignState::Idle;
    options.show_grid = false;
    applied_grid_valid_ = false;
    world_plane_valid_ = false;
    for (auto& item : placed_)
        item.world_anchored = false;
}

bool GraphicOverlay::tryLockWorldWithPose(int frame_w, int frame_h,
                                          const stype::Record& pose,
                                          const stype::OverlayOptions& overlay_options,
                                          const GraphicLiveTransform& live)
{
    if (!applied_grid_valid_ || world_plane_valid_ || frame_w <= 0 || frame_h <= 0)
        return false;
    if (!bakeWorldPlane(applied_grid_, frame_w, frame_h, pose, overlay_options))
        return false;
    for (auto& item : placed_) {
        if (!item.plane_space)
            continue;
        if (planeRectToWorld(item.plane_cx, item.plane_cy, item.plane_width, item.plane_height,
                             live, item.world_corners)) {
            item.world_anchored = true;
        }
    }
    return true;
}

bool GraphicOverlay::planeRectToWorld(float plane_cx, float plane_cy, float plane_w, float plane_h,
                                      const GraphicLiveTransform& live,
                                      std::array<cv::Point3d, 4>& out_world) const
{
    if (!world_plane_valid_)
        return false;

    const float scale = std::max(0.05f, live.scale);
    const float half_w = std::max(1.0f, plane_w) * 0.5f * scale;
    const float half_h = std::max(1.0f, plane_h) * 0.5f * scale;

    // Apply graphic yaw/pitch/roll in plane-local space (same as screen path).
    GraphicGridTransform gfx;
    gfx.rotation_deg = live.yaw_deg;
    gfx.pitch_deg = live.pitch_deg;
    gfx.roll_deg = live.roll_deg;
    gfx.depth_z = 1.0f;

    const std::array<cv::Point2f, 4> local = {{
        {-half_w, -half_h}, { half_w, -half_h},
        { half_w,  half_h}, {-half_w,  half_h},
    }};

    const float gw = std::max(1.0f, world_grid_width_);
    const float gh = std::max(1.0f, world_grid_height_);

    for (int i = 0; i < 4; ++i) {
        const cv::Point2f rel = applyPlaneTransform(local[static_cast<std::size_t>(i)], gfx);
        const float px = plane_cx + rel.x;
        const float py = plane_cy + rel.y;
        const float u01 = (px / gw) + 0.5f;
        const float v01 = (py / gh) + 0.5f;
        out_world[static_cast<std::size_t>(i)] =
            bilinearWorld(world_plane_corners_, std::clamp(u01, 0.0f, 1.0f),
                          std::clamp(v01, 0.0f, 1.0f));
    }
    return true;
}

bool GraphicOverlay::projectWorldCorners(const std::array<cv::Point3d, 4>& world,
                                         const stype::Record& pose,
                                         const stype::OverlayOptions& overlay_options,
                                         int frame_w, int frame_h,
                                         std::array<cv::Point2f, 4>& out_uv) const
{
    for (int i = 0; i < 4; ++i) {
        int u = 0, v = 0;
        if (!stype::projectWorldPoint(pose, overlay_options, frame_w, frame_h,
                                      world[static_cast<std::size_t>(i)].x,
                                      world[static_cast<std::size_t>(i)].y,
                                      world[static_cast<std::size_t>(i)].z,
                                      u, v)) {
            return false;
        }
        out_uv[static_cast<std::size_t>(i)] =
            cv::Point2f(static_cast<float>(u), static_cast<float>(v));
    }
    return true;
}

std::array<cv::Point2f, 4> GraphicOverlay::screenPlacementQuad(
    const GraphicGridTransform& grid,
    const GraphicLiveTransform& live,
    float plane_cx, float plane_cy, float plane_w, float plane_h,
    int video_w, int video_h) const
{
    const float scale = std::max(0.05f, live.scale);
    const float half_w = std::max(1.0f, plane_w) * 0.5f * scale;
    const float half_h = std::max(1.0f, plane_h) * 0.5f * scale;

    GraphicGridTransform gfx = grid;
    gfx.rotation_deg = live.yaw_deg;
    gfx.pitch_deg = live.pitch_deg;
    gfx.roll_deg = live.roll_deg;
    gfx.depth_z = 1.0f;
    gfx.offset_x = 0.0f;
    gfx.offset_y = 0.0f;

    const std::array<cv::Point2f, 4> local = {{
        {-half_w, -half_h}, { half_w, -half_h},
        { half_w,  half_h}, {-half_w,  half_h},
    }};
    std::vector<cv::Point2f> plane_pts;
    plane_pts.reserve(4);
    for (const auto& p : local) {
        const cv::Point2f rel = applyPlaneTransform(p, gfx);
        plane_pts.emplace_back(plane_cx + rel.x, plane_cy + rel.y);
    }

    std::array<cv::Point2f, 4> out = {{
        {plane_cx - half_w, plane_cy - half_h},
        {plane_cx + half_w, plane_cy - half_h},
        {plane_cx + half_w, plane_cy + half_h},
        {plane_cx - half_w, plane_cy + half_h},
    }};
    const cv::Mat H = planeHomography(grid, video_w, video_h);
    if (H.empty())
        return out;
    std::vector<cv::Point2f> image_pts;
    cv::perspectiveTransform(plane_pts, image_pts, H);
    if (image_pts.size() == 4) {
        for (std::size_t i = 0; i < 4; ++i)
            out[i] = image_pts[i];
    }
    return out;
}

bool GraphicOverlay::placeAtPlane(float video_x, float video_y, int frame_w, int frame_h,
                                  const GraphicOverlayOptions& options)
{
    if (pending_texture_ == 0 || pending_rgba_.empty() || frame_w <= 0 || frame_h <= 0)
        return false;
    if (options.align_state != GridAlignState::Aligning &&
        options.align_state != GridAlignState::Applied) {
        return false;
    }

    const GraphicGridTransform& grid = activeGrid(options);
    const cv::Mat H = planeHomography(grid, frame_w, frame_h);
    if (H.empty())
        return false;
    cv::Mat H_inv;
    if (!cv::invert(H, H_inv, cv::DECOMP_SVD))
        return false;

    std::vector<cv::Point2f> click = {{video_x, video_y}};
    std::vector<cv::Point2f> plane;
    cv::perspectiveTransform(click, plane, H_inv);
    if (plane.empty())
        return false;

    const float aspect = (pending_height_ > 0)
                             ? static_cast<float>(pending_width_) / static_cast<float>(pending_height_)
                             : 1.0f;
    const float height = std::max(10.0f, options.graphic_height_px);
    const float width = height * aspect;

    PlacedGraphic item;
    item.name = pending_name_;
    item.image_width = pending_width_;
    item.image_height = pending_height_;
    item.rgba = pending_rgba_.clone();
    item.plane_space = true;
    item.plane_cx = plane[0].x;
    item.plane_cy = plane[0].y;
    item.plane_width = width;
    item.plane_height = height;

    if (world_plane_valid_ &&
        planeRectToWorld(item.plane_cx, item.plane_cy, item.plane_width, item.plane_height,
                         options.live, item.world_corners)) {
        item.world_anchored = true;
    }

    std::string error;
    if (!uploadTexture(item.rgba, item.texture, item.image_width, item.image_height, error))
        return false;
    placed_.push_back(std::move(item));
    destroyPending_();
    return true;
}

void GraphicOverlay::clearPending() { destroyPending_(); }

void GraphicOverlay::removePlaced(int index)
{
    if (index < 0 || index >= static_cast<int>(placed_.size()))
        return;
    destroyTexture(placed_[static_cast<std::size_t>(index)].texture);
    placed_.erase(placed_.begin() + index);
}

void GraphicOverlay::clearAll()
{
    destroyPending_();
    for (auto& item : placed_)
        destroyTexture(item.texture);
    placed_.clear();
}

void GraphicOverlay::displaySizeFor(int image_w, int image_h, float video_width_px,
                                    float width_fraction, float& out_w, float& out_h)
{
    if (image_w <= 0 || image_h <= 0 || video_width_px <= 0.0f) {
        out_w = out_h = 0.0f;
        return;
    }
    const float fraction = std::clamp(width_fraction, 0.02f, 1.0f);
    out_w = video_width_px * fraction;
    out_h = out_w * (static_cast<float>(image_h) / static_cast<float>(image_w));
}

void GraphicOverlay::warpAlphaBlit(cv::Mat& bgr, const cv::Mat& bgra, const cv::Point2f dst[4])
{
    if (bgr.empty() || bgra.empty() || bgra.type() != CV_8UC4 || bgr.type() != CV_8UC3)
        return;
    const cv::Point2f src[4] = {
        {0.0f, 0.0f},
        {static_cast<float>(bgra.cols), 0.0f},
        {static_cast<float>(bgra.cols), static_cast<float>(bgra.rows)},
        {0.0f, static_cast<float>(bgra.rows)},
    };
    const cv::Mat H = cv::getPerspectiveTransform(src, dst);
    cv::Mat warped;
    cv::warpPerspective(bgra, warped, H, bgr.size(),
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0, 0));
    for (int y = 0; y < bgr.rows; ++y) {
        const cv::Vec4b* src_row = warped.ptr<cv::Vec4b>(y);
        cv::Vec3b* dst_row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const float a = src_row[x][3] / 255.0f;
            if (a <= 0.0f)
                continue;
            const float ia = 1.0f - a;
            dst_row[x][0] = static_cast<std::uint8_t>(src_row[x][0] * a + dst_row[x][0] * ia);
            dst_row[x][1] = static_cast<std::uint8_t>(src_row[x][1] * a + dst_row[x][1] * ia);
            dst_row[x][2] = static_cast<std::uint8_t>(src_row[x][2] * a + dst_row[x][2] * ia);
        }
    }
}

void GraphicOverlay::alphaBlit(cv::Mat& bgr, const cv::Mat& bgra,
                               float center_u, float center_v, float width_fraction)
{
    if (bgr.empty() || bgra.empty() || bgr.type() != CV_8UC3 || bgra.type() != CV_8UC4)
        return;
    float disp_w = 0.0f, disp_h = 0.0f;
    displaySizeFor(bgra.cols, bgra.rows, static_cast<float>(bgr.cols),
                   width_fraction, disp_w, disp_h);
    const int out_w = std::max(1, static_cast<int>(std::lround(disp_w)));
    const int out_h = std::max(1, static_cast<int>(std::lround(disp_h)));
    const int cx = static_cast<int>(std::lround(center_u * bgr.cols));
    const int cy = static_cast<int>(std::lround(center_v * bgr.rows));
    const int x0 = cx - out_w / 2;
    const int y0 = cy - out_h / 2;
    cv::Mat resized;
    cv::resize(bgra, resized, cv::Size(out_w, out_h), 0, 0, cv::INTER_AREA);
    const cv::Rect frame_rect(0, 0, bgr.cols, bgr.rows);
    const cv::Rect dst_rect = cv::Rect(x0, y0, out_w, out_h) & frame_rect;
    if (dst_rect.empty())
        return;
    cv::Mat src = resized(cv::Rect(dst_rect.x - x0, dst_rect.y - y0, dst_rect.width, dst_rect.height));
    cv::Mat dst = bgr(dst_rect);
    for (int y = 0; y < dst_rect.height; ++y) {
        const cv::Vec4b* srow = src.ptr<cv::Vec4b>(y);
        cv::Vec3b* drow = dst.ptr<cv::Vec3b>(y);
        for (int x = 0; x < dst_rect.width; ++x) {
            const float a = srow[x][3] / 255.0f;
            if (a <= 0.0f)
                continue;
            drow[x][0] = static_cast<std::uint8_t>(srow[x][0] * a + drow[x][0] * (1.0f - a));
            drow[x][1] = static_cast<std::uint8_t>(srow[x][1] * a + drow[x][1] * (1.0f - a));
            drow[x][2] = static_cast<std::uint8_t>(srow[x][2] * a + drow[x][2] * (1.0f - a));
        }
    }
}

void GraphicOverlay::drawPreview(ImDrawList* draw_list,
                                 const ImVec2& video_min,
                                 const ImVec2& video_max,
                                 const ImVec2& mouse,
                                 const GraphicOverlayOptions& options,
                                 int frame_width,
                                 int frame_height,
                                 const stype::Record* pose,
                                 const stype::OverlayOptions* overlay_options) const
{
    if (!draw_list || !options.enabled)
        return;
    const float video_w = video_max.x - video_min.x;
    const float video_h = video_max.y - video_min.y;
    if (video_w < 1.0f || video_h < 1.0f || frame_width <= 0 || frame_height <= 0)
        return;

    draw_list->PushClipRect(video_min, video_max, true);
    const auto toScreen = [&](const cv::Point2f& p) {
        return ImVec2(video_min.x + p.x / static_cast<float>(frame_width) * video_w,
                      video_min.y + p.y / static_cast<float>(frame_height) * video_h);
    };

    const GraphicGridTransform& grid = activeGrid(options);
    GraphicGridTransform grid_draw = grid;
    if (grid_draw.grid_width < 50.0f)
        grid_draw.grid_width = static_cast<float>(frame_width);
    if (grid_draw.grid_height < 50.0f)
        grid_draw.grid_height = static_cast<float>(frame_height);

    // Only draw the mesh while Aligning (hidden after Apply, but plane still used).
    const bool draw_grid = options.plane_grid &&
                           options.align_state == GridAlignState::Aligning &&
                           options.show_grid;
    if (draw_grid) {
        const auto points = alignmentGridPoints(grid_draw, frame_width, frame_height);
        const int cols = std::max(2, grid_draw.grid_cols);
        const int rows = std::max(2, grid_draw.grid_rows);
        const ImU32 dot = IM_COL32(80, 200, 255, 220);
        const ImU32 line = IM_COL32(60, 160, 220, 140);
        for (const auto& p : points)
            draw_list->AddCircleFilled(toScreen(p), 2.0f, dot);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols - 1; ++c)
                draw_list->AddLine(toScreen(points[static_cast<std::size_t>(r * cols + c)]),
                                   toScreen(points[static_cast<std::size_t>(r * cols + c + 1)]),
                                   line, 1.0f);
        for (int c = 0; c < cols; ++c)
            for (int r = 0; r < rows - 1; ++r)
                draw_list->AddLine(toScreen(points[static_cast<std::size_t>(r * cols + c)]),
                                   toScreen(points[static_cast<std::size_t>((r + 1) * cols + c)]),
                                   line, 1.0f);
        const auto outline = alignmentQuad(grid_draw, frame_width, frame_height);
        const ImVec2 o[5] = {
            toScreen(outline[0]), toScreen(outline[1]), toScreen(outline[2]),
            toScreen(outline[3]), toScreen(outline[0]),
        };
        draw_list->AddPolyline(o, 5, IM_COL32(255, 220, 80, 230), ImDrawFlags_None, 2.0f);
    }

    const auto drawScreen = [&](GLuint texture, int iw, int ih,
                                float center_x, float center_y, float width_fraction,
                                ImU32 poly_color) {
        float disp_w = 0.0f, disp_h = 0.0f;
        displaySizeFor(iw, ih, video_w, width_fraction, disp_w, disp_h);
        if (disp_w < 1.0f || disp_h < 1.0f || texture == 0)
            return;
        const ImVec2 p0(center_x - disp_w * 0.5f, center_y - disp_h * 0.5f);
        const ImVec2 p1(center_x + disp_w * 0.5f, center_y + disp_h * 0.5f);
        draw_list->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(texture)), p0, p1);
        const ImVec2 corners[5] = {p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), p0};
        draw_list->AddPolyline(corners, 5, poly_color, ImDrawFlags_None, 2.0f);
    };

    const auto drawQuad = [&](GLuint texture, const std::array<cv::Point2f, 4>& quad,
                              ImU32 poly_color) {
        if (texture == 0)
            return;
        const ImVec2 p0 = toScreen(quad[0]);
        const ImVec2 p1 = toScreen(quad[1]);
        const ImVec2 p2 = toScreen(quad[2]);
        const ImVec2 p3 = toScreen(quad[3]);
        draw_list->AddImageQuad(reinterpret_cast<void*>(static_cast<intptr_t>(texture)),
                                p0, p1, p2, p3);
        const ImVec2 outline[5] = {p0, p1, p2, p3, p0};
        draw_list->AddPolyline(outline, 5, poly_color, ImDrawFlags_None, 2.0f);
    };

    const bool can_world = pose != nullptr && overlay_options != nullptr && world_plane_valid_;

    for (const auto& item : placed_) {
        if (item.plane_space) {
            std::array<cv::Point2f, 4> quad{};
            bool ok = false;
            if (item.world_anchored && can_world) {
                std::array<cv::Point3d, 4> world = item.world_corners;
                // Refresh world corners from plane + live so scale/yaw/pitch/roll stay live.
                planeRectToWorld(item.plane_cx, item.plane_cy, item.plane_width, item.plane_height,
                                 options.live, world);
                ok = projectWorldCorners(world, *pose, *overlay_options,
                                         frame_width, frame_height, quad);
            }
            if (!ok) {
                quad = screenPlacementQuad(grid_draw, options.live,
                                           item.plane_cx, item.plane_cy,
                                           item.plane_width, item.plane_height,
                                           frame_width, frame_height);
            }
            drawQuad(item.texture, quad, IM_COL32(80, 220, 120, 200));
        } else {
            drawScreen(item.texture, item.image_width, item.image_height,
                       video_min.x + item.center_u * video_w,
                       video_min.y + item.center_v * video_h,
                       item.width_fraction, IM_COL32(80, 220, 120, 200));
        }
    }

    if (pending_texture_ != 0) {
        const bool allow_plane = options.plane_grid &&
            (options.align_state == GridAlignState::Aligning ||
             options.align_state == GridAlignState::Applied);
        if (allow_plane) {
            const float vx = std::clamp((mouse.x - video_min.x) / video_w, 0.0f, 1.0f) *
                             static_cast<float>(frame_width);
            const float vy = std::clamp((mouse.y - video_min.y) / video_h, 0.0f, 1.0f) *
                             static_cast<float>(frame_height);
            const cv::Mat H = planeHomography(grid_draw, frame_width, frame_height);
            cv::Mat H_inv;
            if (!H.empty() && cv::invert(H, H_inv, cv::DECOMP_SVD)) {
                std::vector<cv::Point2f> click = {{vx, vy}};
                std::vector<cv::Point2f> plane;
                cv::perspectiveTransform(click, plane, H_inv);
                if (!plane.empty()) {
                    const float aspect = (pending_height_ > 0)
                        ? static_cast<float>(pending_width_) / static_cast<float>(pending_height_)
                        : 1.0f;
                    const float height = std::max(10.0f, options.graphic_height_px);
                    std::array<cv::Point2f, 4> quad{};
                    bool ok = false;
                    if (can_world) {
                        std::array<cv::Point3d, 4> world{};
                        if (planeRectToWorld(plane[0].x, plane[0].y, height * aspect, height,
                                             options.live, world)) {
                            ok = projectWorldCorners(world, *pose, *overlay_options,
                                                     frame_width, frame_height, quad);
                        }
                    }
                    if (!ok) {
                        quad = screenPlacementQuad(grid_draw, options.live,
                                                   plane[0].x, plane[0].y,
                                                   height * aspect, height,
                                                   frame_width, frame_height);
                    }
                    drawQuad(pending_texture_, quad, IM_COL32(255, 200, 40, 240));
                }
            }
        } else {
            drawScreen(pending_texture_, pending_width_, pending_height_,
                       std::clamp(mouse.x, video_min.x, video_max.x),
                       std::clamp(mouse.y, video_min.y, video_max.y),
                       options.width_fraction, IM_COL32(255, 200, 40, 240));
        }
    }

    draw_list->PopClipRect();
}

void GraphicOverlay::burnIntoBgr(cv::Mat& bgr, const GraphicOverlayOptions& options,
                                 const stype::Record* pose,
                                 const stype::OverlayOptions* overlay_options) const
{
    if (!options.enabled || !options.burn_into_sdi || placed_.empty())
        return;
    if (bgr.empty() || bgr.type() != CV_8UC3)
        return;

    const GraphicGridTransform& grid = activeGrid(options);
    GraphicGridTransform grid_draw = grid;
    if (grid_draw.grid_width < 50.0f)
        grid_draw.grid_width = static_cast<float>(bgr.cols);
    if (grid_draw.grid_height < 50.0f)
        grid_draw.grid_height = static_cast<float>(bgr.rows);

    const bool can_world = pose != nullptr && overlay_options != nullptr && world_plane_valid_;

    for (const auto& item : placed_) {
        if (item.rgba.empty())
            continue;
        if (item.plane_space) {
            std::array<cv::Point2f, 4> quad{};
            bool ok = false;
            if (item.world_anchored && can_world) {
                std::array<cv::Point3d, 4> world{};
                if (planeRectToWorld(item.plane_cx, item.plane_cy, item.plane_width, item.plane_height,
                                     options.live, world)) {
                    ok = projectWorldCorners(world, *pose, *overlay_options,
                                             bgr.cols, bgr.rows, quad);
                }
            }
            if (!ok) {
                quad = screenPlacementQuad(grid_draw, options.live,
                                           item.plane_cx, item.plane_cy,
                                           item.plane_width, item.plane_height,
                                           bgr.cols, bgr.rows);
            }
            const cv::Point2f dst[4] = {quad[0], quad[1], quad[2], quad[3]};
            warpAlphaBlit(bgr, item.rgba, dst);
        } else {
            alphaBlit(bgr, item.rgba, item.center_u, item.center_v, item.width_fraction);
        }
    }
}
