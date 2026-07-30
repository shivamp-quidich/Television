#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <GL/glew.h>
#include <imgui.h>
#include <opencv2/core.hpp>

#include "app_config.hpp"

// OpenGL graphic overlays picked from data/. While placing, a polygon the size
// of the scaled image follows the mouse; each click adds another placed graphic.
struct GraphicOverlayOptions {
    bool enabled = true;
    bool burn_into_sdi = true;
    // Default display size as a fraction of the video width when placing.
    float width_fraction = 0.2f;
    std::string data_dir = AppConfig().graphic_data_dir;
};

struct PlacedGraphic {
    std::string name;
    GLuint texture = 0;
    int image_width = 0;
    int image_height = 0;
    cv::Mat rgba; // CV_8UC4 BGRA for SDI burn-in
    float center_u = 0.5f;
    float center_v = 0.5f;
    float width_fraction = 0.2f;
};

class GraphicOverlay {
public:
    GraphicOverlay() = default;
    ~GraphicOverlay();

    GraphicOverlay(const GraphicOverlay&) = delete;
    GraphicOverlay& operator=(const GraphicOverlay&) = delete;

    void refreshFileList(const std::string& data_dir);
    const std::vector<std::string>& files() const { return files_; }

    // Loads files_[index] as the pending (follow-mouse) graphic.
    bool selectFile(int index, std::string& error);

    // Freezes the pending graphic at (center_u, center_v), then clears pending
    // so the operator must select an image again before placing another.
    bool placeAt(float center_u, float center_v, float width_fraction);

    void clearPending();
    void removePlaced(int index);
    void clearAll();

    bool hasPending() const { return pending_texture_ != 0; }
    int pendingWidth() const { return pending_width_; }
    int pendingHeight() const { return pending_height_; }
    const std::string& pendingName() const { return pending_name_; }

    const std::vector<PlacedGraphic>& placed() const { return placed_; }
    int placedCount() const { return static_cast<int>(placed_.size()); }

    // Draws every placed graphic, plus the follow-mouse pending one with its
    // size polygon.
    void drawPreview(ImDrawList* draw_list,
                     const ImVec2& video_min,
                     const ImVec2& video_max,
                     const ImVec2& mouse,
                     const GraphicOverlayOptions& options) const;

    // Burns all placed graphics into a BGR frame for SDI output.
    void burnIntoBgr(cv::Mat& bgr, const GraphicOverlayOptions& options) const;

private:
    static void displaySizeFor(int image_w, int image_h, float video_width_px,
                               float width_fraction, float& out_w, float& out_h);
    static void destroyTexture(GLuint& texture);
    static bool uploadTexture(const cv::Mat& bgra, GLuint& texture,
                              int& width, int& height, std::string& error);
    static void alphaBlit(cv::Mat& bgr, const cv::Mat& bgra,
                          float center_u, float center_v, float width_fraction);

    void destroyPending_();

    std::vector<std::string> files_;

    GLuint pending_texture_ = 0;
    int pending_width_ = 0;
    int pending_height_ = 0;
    cv::Mat pending_rgba_;
    std::string pending_name_;

    std::vector<PlacedGraphic> placed_;
};
