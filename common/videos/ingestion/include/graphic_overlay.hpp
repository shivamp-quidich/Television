#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <imgui.h>
#include <opencv2/core.hpp>

#include "app_config.hpp"
#include "stype_csv_overlay.hpp"

// Screen-space alignment grid matching sponsor_tracker GridTransform.
struct GraphicGridTransform {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float depth_z = 1.0f;
    float rotation_deg = 0.0f; // Z
    float pitch_deg = 0.0f;    // X
    float roll_deg = 0.0f;     // Y — field plane
    float grid_width = 1920.0f;
    float grid_height = 1080.0f;
    int grid_cols = 20;
    int grid_rows = 16;

    void reset()
    {
        offset_x = offset_y = 0.0f;
        depth_z = 1.0f;
        rotation_deg = pitch_deg = roll_deg = 0.0f;
    }
};

struct GraphicLiveTransform {
    float scale = 1.0f;
    float yaw_deg = 0.0f;
    float pitch_deg = 0.0f;
    float roll_deg = 0.0f;

    void reset()
    {
        scale = 1.0f;
        yaw_deg = pitch_deg = roll_deg = 0.0f;
    }
};

enum class GridAlignState {
    Idle,      // no plane locked
    Aligning,  // show grid, edit transform
    Applied,   // grid hidden; plane kept for placement + world follow
};

struct GraphicOverlayOptions {
    bool enabled = true;
    bool burn_into_sdi = true;
    bool plane_grid = true;
    GridAlignState align_state = GridAlignState::Idle;
    // When Applied, show_grid stays false; Aligning forces it on.
    bool show_grid = false;
    float width_fraction = 0.2f;
    float graphic_height_px = 150.0f;
    GraphicGridTransform grid;
    GraphicLiveTransform live;
    std::string data_dir = AppConfig().graphic_data_dir;
};

struct PlacedGraphic {
    std::string name;
    GLuint texture = 0;
    int image_width = 0;
    int image_height = 0;
    cv::Mat rgba;

    float center_u = 0.5f;
    float center_v = 0.5f;
    float width_fraction = 0.2f;

    // Plane-local placement on the applied alignment grid.
    bool plane_space = false;
    float plane_cx = 0.0f;
    float plane_cy = 0.0f;
    float plane_width = 200.0f;
    float plane_height = 200.0f;

    // World-anchored corners (mm) when placement used a locked world plane.
    // Updated each frame by projecting with the current UDP/CSV pose.
    bool world_anchored = false;
    std::array<cv::Point3d, 4> world_corners{}; // TL,TR,BR,BL
};

class GraphicOverlay {
public:
    GraphicOverlay() = default;
    ~GraphicOverlay();

    GraphicOverlay(const GraphicOverlay&) = delete;
    GraphicOverlay& operator=(const GraphicOverlay&) = delete;

    void refreshFileList(const std::string& data_dir);
    const std::vector<std::string>& files() const { return files_; }

    bool selectFile(int index, std::string& error);
    bool placeAt(float center_u, float center_v, float width_fraction);

    // Places pending graphic on the locked/applied plane (or live aligning plane).
    bool placeAtPlane(float video_x, float video_y, int frame_w, int frame_h,
                      const GraphicOverlayOptions& options);

    // Align → Apply: freeze grid, hide dots, optionally bake world plane from pose.
    void startAlignment(GraphicOverlayOptions& options);
    bool applyAlignment(GraphicOverlayOptions& options,
                        int frame_w, int frame_h,
                        const stype::Record* pose,
                        const stype::OverlayOptions* overlay_options,
                        std::string& status);
    // After Apply without pose: bake world plane once UDP/CSV arrives (no UI change).
    bool tryLockWorldWithPose(int frame_w, int frame_h,
                              const stype::Record& pose,
                              const stype::OverlayOptions& overlay_options,
                              const GraphicLiveTransform& live);
    void clearAlignment(GraphicOverlayOptions& options);

    bool alignmentApplied() const { return world_plane_valid_ || applied_grid_valid_; }
    bool worldPlaneValid() const { return world_plane_valid_; }

    void clearPending();
    void removePlaced(int index);
    void clearAll();

    bool hasPending() const { return pending_texture_ != 0; }
    int pendingWidth() const { return pending_width_; }
    int pendingHeight() const { return pending_height_; }
    const std::string& pendingName() const { return pending_name_; }

    const std::vector<PlacedGraphic>& placed() const { return placed_; }
    int placedCount() const { return static_cast<int>(placed_.size()); }

    void drawPreview(ImDrawList* draw_list,
                     const ImVec2& video_min,
                     const ImVec2& video_max,
                     const ImVec2& mouse,
                     const GraphicOverlayOptions& options,
                     int frame_width,
                     int frame_height,
                     const stype::Record* pose = nullptr,
                     const stype::OverlayOptions* overlay_options = nullptr) const;

    void burnIntoBgr(cv::Mat& bgr, const GraphicOverlayOptions& options,
                     const stype::Record* pose = nullptr,
                     const stype::OverlayOptions* overlay_options = nullptr) const;

    static cv::Point2f applyPlaneTransform(const cv::Point2f& local,
                                           const GraphicGridTransform& t);
    static std::array<cv::Point2f, 4> alignmentQuad(const GraphicGridTransform& t,
                                                    int video_w, int video_h);
    static std::vector<cv::Point2f> alignmentGridPoints(const GraphicGridTransform& t,
                                                        int video_w, int video_h);
    static cv::Mat planeHomography(const GraphicGridTransform& t,
                                   int video_w, int video_h);

private:
    static void displaySizeFor(int image_w, int image_h, float video_width_px,
                               float width_fraction, float& out_w, float& out_h);
    static void destroyTexture(GLuint& texture);
    static bool uploadTexture(const cv::Mat& bgra, GLuint& texture,
                              int& width, int& height, std::string& error);
    static void alphaBlit(cv::Mat& bgr, const cv::Mat& bgra,
                          float center_u, float center_v, float width_fraction);
    static void warpAlphaBlit(cv::Mat& bgr, const cv::Mat& bgra,
                              const cv::Point2f dst[4]);

    const GraphicGridTransform& activeGrid(const GraphicOverlayOptions& options) const;

    bool bakeWorldPlane(const GraphicGridTransform& grid, int frame_w, int frame_h,
                        const stype::Record& pose,
                        const stype::OverlayOptions& overlay_options);

    // Map plane-local (cx,cy,w,h) + live transform → 4 world corners using baked plane.
    bool planeRectToWorld(float plane_cx, float plane_cy, float plane_w, float plane_h,
                          const GraphicLiveTransform& live,
                          std::array<cv::Point3d, 4>& out_world) const;

    bool projectWorldCorners(const std::array<cv::Point3d, 4>& world,
                             const stype::Record& pose,
                             const stype::OverlayOptions& overlay_options,
                             int frame_w, int frame_h,
                             std::array<cv::Point2f, 4>& out_uv) const;

    std::array<cv::Point2f, 4> screenPlacementQuad(
        const GraphicGridTransform& grid,
        const GraphicLiveTransform& live,
        float plane_cx, float plane_cy, float plane_w, float plane_h,
        int video_w, int video_h) const;

    void destroyPending_();

    std::vector<std::string> files_;

    GLuint pending_texture_ = 0;
    int pending_width_ = 0;
    int pending_height_ = 0;
    cv::Mat pending_rgba_;
    std::string pending_name_;

    std::vector<PlacedGraphic> placed_;

    // Frozen after Apply (grid stays usable while hidden).
    bool applied_grid_valid_ = false;
    GraphicGridTransform applied_grid_{};

    // World plane from Apply + UDP/CSV pose (TL,TR,BR,BL on ground Y≈0).
    bool world_plane_valid_ = false;
    float world_grid_width_ = 1920.0f;
    float world_grid_height_ = 1080.0f;
    std::array<cv::Point3d, 4> world_plane_corners_{};
};
