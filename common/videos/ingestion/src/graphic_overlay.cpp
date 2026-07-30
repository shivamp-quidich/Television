#include "graphic_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

bool isImageExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".bmp" || ext == ".webp" || ext == ".tga";
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

    cv::Mat loaded = cv::imread(files_[index], cv::IMREAD_UNCHANGED);
    if (loaded.empty()) {
        error = "failed to load " + files_[index];
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
        error = "unsupported channel count in " + files_[index];
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
    pending_name_ = std::filesystem::path(files_[index]).filename().string();
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

void GraphicOverlay::clearPending()
{
    destroyPending_();
}

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

void GraphicOverlay::drawPreview(ImDrawList* draw_list,
                                 const ImVec2& video_min,
                                 const ImVec2& video_max,
                                 const ImVec2& mouse,
                                 const GraphicOverlayOptions& options) const
{
    if (!draw_list || !options.enabled)
        return;

    const float video_w = video_max.x - video_min.x;
    const float video_h = video_max.y - video_min.y;
    if (video_w < 1.0f || video_h < 1.0f)
        return;

    draw_list->PushClipRect(video_min, video_max, true);

    const auto drawOne = [&](GLuint texture, int iw, int ih,
                             float center_x, float center_y, float width_fraction,
                             ImU32 poly_color) {
        float disp_w = 0.0f, disp_h = 0.0f;
        displaySizeFor(iw, ih, video_w, width_fraction, disp_w, disp_h);
        if (disp_w < 1.0f || disp_h < 1.0f || texture == 0)
            return;
        const ImVec2 p0(center_x - disp_w * 0.5f, center_y - disp_h * 0.5f);
        const ImVec2 p1(center_x + disp_w * 0.5f, center_y + disp_h * 0.5f);
        draw_list->AddImage(reinterpret_cast<void*>(static_cast<intptr_t>(texture)),
                            p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
        const ImVec2 corners[5] = {
            p0, ImVec2(p1.x, p0.y), p1, ImVec2(p0.x, p1.y), p0,
        };
        draw_list->AddPolyline(corners, 5, poly_color, ImDrawFlags_None, 2.0f);
    };

    for (const auto& item : placed_) {
        drawOne(item.texture, item.image_width, item.image_height,
                video_min.x + item.center_u * video_w,
                video_min.y + item.center_v * video_h,
                item.width_fraction,
                IM_COL32(80, 220, 120, 200));
    }

    if (pending_texture_ != 0) {
        const float cx = std::clamp(mouse.x, video_min.x, video_max.x);
        const float cy = std::clamp(mouse.y, video_min.y, video_max.y);
        drawOne(pending_texture_, pending_width_, pending_height_,
                cx, cy, options.width_fraction,
                IM_COL32(255, 200, 40, 240));
    }

    draw_list->PopClipRect();
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

    const int src_x = dst_rect.x - x0;
    const int src_y = dst_rect.y - y0;
    cv::Mat src = resized(cv::Rect(src_x, src_y, dst_rect.width, dst_rect.height));
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

void GraphicOverlay::burnIntoBgr(cv::Mat& bgr, const GraphicOverlayOptions& options) const
{
    if (!options.enabled || !options.burn_into_sdi || placed_.empty())
        return;
    if (bgr.empty() || bgr.type() != CV_8UC3)
        return;

    for (const auto& item : placed_)
        alphaBlit(bgr, item.rgba, item.center_u, item.center_v, item.width_fraction);
}
