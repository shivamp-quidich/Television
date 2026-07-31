#include "stype_csv_overlay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <opencv2/imgproc.hpp>

namespace stype {
namespace {

constexpr double kPi = 3.14159265358979323846;

double radians(double degrees)
{
    return degrees * kPi / 180.0;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool splitCsvLine(const std::string& line, std::vector<std::string>& fields)
{
    fields.clear();
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        const char ch = line[i];
        if (ch == '"')
        {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"')
            {
                field += ch;
                ++i;
            }
            else
            {
                quoted = !quoted;
            }
        }
        else if (ch == ',' && !quoted)
        {
            fields.push_back(trim(field));
            field.clear();
        }
        else
        {
            field += ch;
        }
    }
    fields.push_back(trim(field));
    return !quoted;
}

bool parseInteger(const std::string& text, std::int64_t& value)
{
    try
    {
        std::size_t used = 0;
        value = std::stoll(trim(text), &used);
        return used == trim(text).size();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool parseDouble(const std::string& text, double& value)
{
    try
    {
        const std::string clean = trim(text);
        std::size_t used = 0;
        value = std::stod(clean, &used);
        return used == clean.size() && std::isfinite(value);
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool parseBool(const std::string& text, bool& value)
{
    const std::string clean = trim(text);
    if (clean == "1" || clean == "true" || clean == "TRUE")
    {
        value = true;
        return true;
    }
    if (clean == "0" || clean == "false" || clean == "FALSE")
    {
        value = false;
        return true;
    }
    return false;
}

struct Camera {
    double fx = 0.0;
    double fy = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    double px_per_mm = 0.0;
    double k1 = 0.0;
    double k2 = 0.0;
    double cam_x = 0.0;
    double cam_y = 0.0;
    double cam_z = 0.0;
    std::array<double, 3> right{};
    std::array<double, 3> down{};
    std::array<double, 3> look{};
};

double dot(const std::array<double, 3>& a, double x, double y, double z)
{
    return a[0] * x + a[1] * y + a[2] * z;
}

Camera buildCamera(const Record& record, const OverlayOptions& options, int width, int height)
{
    Camera camera;
    const double image_width = static_cast<double>(width);
    const double image_height = static_cast<double>(height);

    double hfov = record.hfov_deg;
    if (!record.hasDirectFov())
    {
        const double vfov = std::clamp(options.fallback_vfov_deg, 1.0, 179.0);
        hfov = 2.0 * std::atan(std::tan(radians(vfov) * 0.5) * image_width / image_height)
             * 180.0 / kPi;
    }
    camera.fx = (image_width * 0.5) / std::tan(radians(hfov) * 0.5);
    camera.fy = camera.fx;
    camera.cx = image_width * 0.5;
    camera.cy = image_height * 0.5;

    if (record.hasDirectFov() && record.hf_pa_width_mm > 0.0 && record.hf_ar > 0.0)
    {
        const double pa_height_mm = record.hf_pa_width_mm / record.hf_ar;
        camera.cx -= record.hf_cx_mm * image_width / record.hf_pa_width_mm;
        camera.cy -= record.hf_cy_mm * image_height / pa_height_mm;
        camera.px_per_mm = image_width / record.hf_pa_width_mm;
        if (options.apply_distortion)
        {
            camera.k1 = record.hf_k1;
            camera.k2 = record.hf_k2;
        }
    }

    const double pan = radians(record.pan_deg);
    const double tilt = radians(record.tilt_deg);
    const double roll = radians(record.roll_deg);
    camera.look = {std::sin(pan) * std::cos(tilt),
                   std::sin(tilt),
                   std::cos(pan) * std::cos(tilt)};

    const std::array<double, 3> right_without_roll = {std::cos(pan), 0.0, -std::sin(pan)};
    const std::array<double, 3> cross = {
        camera.look[1] * right_without_roll[2] - camera.look[2] * right_without_roll[1],
        camera.look[2] * right_without_roll[0] - camera.look[0] * right_without_roll[2],
        camera.look[0] * right_without_roll[1] - camera.look[1] * right_without_roll[0]};
    const double cos_roll = std::cos(roll);
    const double sin_roll = std::sin(roll);
    for (int i = 0; i < 3; ++i)
        camera.right[i] = right_without_roll[i] * cos_roll + cross[i] * sin_roll;
    camera.down = {
        camera.right[1] * camera.look[2] - camera.right[2] * camera.look[1],
        camera.right[2] * camera.look[0] - camera.right[0] * camera.look[2],
        camera.right[0] * camera.look[1] - camera.right[1] * camera.look[0]};

    camera.cam_x = record.x_mm;
    camera.cam_y = record.y_mm;
    camera.cam_z = record.z_mm;
    return camera;
}

bool project(const Camera& camera, double x, double y, double z, cv::Point& point)
{
    const double dx = x - camera.cam_x;
    const double dy = y - camera.cam_y;
    const double dz = z - camera.cam_z;
    const double z_camera = dot(camera.look, dx, dy, dz);
    if (z_camera <= 1.0)
        return false;

    double u = camera.fx * dot(camera.right, dx, dy, dz) / z_camera + camera.cx;
    double v = camera.fy * dot(camera.down, dx, dy, dz) / z_camera + camera.cy;
    if (camera.px_per_mm > 0.0 && (camera.k1 != 0.0 || camera.k2 != 0.0))
    {
        // HF k1/k2 map distorted radius to ideal radius. Solve the inverse
        // mapping with a bounded binary search, as in stype_player.py.
        const double ux = (u - camera.cx) / camera.px_per_mm;
        const double uy = (v - camera.cy) / camera.px_per_mm;
        const double ideal_radius = std::hypot(ux, uy);
        if (ideal_radius > 1e-12)
        {
            auto mappedRadius = [&camera](double radius) {
                const double radius2 = radius * radius;
                return radius * (1.0 + camera.k1 * radius2 + camera.k2 * radius2 * radius2);
            };
            double upper = std::max(1.0, ideal_radius);
            for (int i = 0; i < 32 && mappedRadius(upper) < ideal_radius; ++i)
                upper *= 2.0;
            if (!std::isfinite(upper) || mappedRadius(upper) < ideal_radius)
                return false;
            double lower = 0.0;
            for (int i = 0; i < 40; ++i)
            {
                const double mid = (lower + upper) * 0.5;
                if (mappedRadius(mid) < ideal_radius)
                    lower = mid;
                else
                    upper = mid;
            }
            const double ratio = (lower + upper) * 0.5 / ideal_radius;
            u = camera.cx + ux * ratio * camera.px_per_mm;
            v = camera.cy + uy * ratio * camera.px_per_mm;
        }
    }

    if (u < static_cast<double>(std::numeric_limits<int>::min()) ||
        u > static_cast<double>(std::numeric_limits<int>::max()) ||
        v < static_cast<double>(std::numeric_limits<int>::min()) ||
        v > static_cast<double>(std::numeric_limits<int>::max()))
        return false;
    point = cv::Point(cvRound(u), cvRound(v));
    return true;
}

// void drawText(cv::Mat& bgr, const Record& record, bool direct_fov)
// {
//     const std::array<std::string, 4> lines = {
//         "Stype " + std::string(record.stype_valid ? "VALID" : "INVALID") +
//             "  frame " + std::to_string(record.frame_id) +
//             "  t=" + std::to_string(record.timestamp_ms) + " ms",
//         cv::format("Pan %+6.2f  Tilt %+6.2f  Roll %+6.2f deg",
//                    record.pan_deg, record.tilt_deg, record.roll_deg),
//         cv::format("X %+7.3f  Y %+7.3f  Z %+7.3f m",
//                    record.x_mm / 1000.0, record.y_mm / 1000.0, record.z_mm / 1000.0),
//         direct_fov
//             ? cv::format("HFOV %.2f deg  zoom %lld  focus %lld",
//                          record.hfov_deg, static_cast<long long>(record.zoom_raw),
//                          static_cast<long long>(record.focus_raw))
//             : "HFOV unavailable: using fallback projection"};
//     constexpr int margin = 12;
//     constexpr int line_height = 22;
//     cv::rectangle(bgr, cv::Rect(5, 5, std::min(bgr.cols - 10, 560),
//                                 margin + line_height * static_cast<int>(lines.size())),
//                   cv::Scalar(0, 0, 0), cv::FILLED);
//     const cv::Scalar status_color = record.stype_valid ? cv::Scalar(80, 255, 80)
//                                                         : cv::Scalar(0, 180, 255);
//     for (std::size_t i = 0; i < lines.size(); ++i)
//     {
//         cv::putText(bgr, lines[i], cv::Point(margin, margin + 8 + line_height * static_cast<int>(i)),
//                     cv::FONT_HERSHEY_SIMPLEX, 0.52, i == 0 ? status_color : cv::Scalar(240, 240, 240),
//                     1, cv::LINE_AA);
//     }
// }

} // namespace

Record applyAlignment(const Record& record, const AlignmentAdjust& alignment)
{
    Record out = record;
    out.pan_deg = record.pan_deg * static_cast<double>(alignment.sign_pan) + alignment.add_pan_deg;
    out.tilt_deg = record.tilt_deg * static_cast<double>(alignment.sign_tilt) + alignment.add_tilt_deg;
    out.roll_deg = record.roll_deg * static_cast<double>(alignment.sign_roll) + alignment.add_roll_deg;
    out.x_mm = record.x_mm * static_cast<double>(alignment.sign_x) + alignment.add_x_mm;
    out.y_mm = record.y_mm * static_cast<double>(alignment.sign_y) + alignment.add_y_mm;
    out.z_mm = record.z_mm * static_cast<double>(alignment.sign_z) + alignment.add_z_mm;
    return out;
}

bool projectWorldPoint(const Record& record, const OverlayOptions& options,
                       int image_width, int image_height,
                       double x_mm, double y_mm, double z_mm,
                       int& u, int& v)
{
    if (image_width <= 0 || image_height <= 0)
        return false;
    const Record aligned = applyAlignment(record, options.alignment);
    const Camera camera = buildCamera(aligned, options, image_width, image_height);
    cv::Point point;
    if (!project(camera, x_mm, y_mm, z_mm, point))
        return false;
    u = point.x;
    v = point.y;
    return true;
}

bool unprojectToGround(const Record& record, const OverlayOptions& options,
                       int image_width, int image_height,
                       double pixel_u, double pixel_v,
                       double& x_mm, double& y_mm, double& z_mm)
{
    if (image_width <= 0 || image_height <= 0)
        return false;

    const Record aligned = applyAlignment(record, options.alignment);
    const Camera camera = buildCamera(aligned, options, image_width, image_height);

    double u = pixel_u;
    double v = pixel_v;
    // Clicks are in the distorted image. HF k1/k2 map distorted→ideal radius;
    // convert to the ideal pinhole ray before intersecting the ground.
    if (camera.px_per_mm > 0.0 && (camera.k1 != 0.0 || camera.k2 != 0.0))
    {
        const double ux = (u - camera.cx) / camera.px_per_mm;
        const double uy = (v - camera.cy) / camera.px_per_mm;
        const double distorted_radius = std::hypot(ux, uy);
        if (distorted_radius > 1e-12)
        {
            const double radius2 = distorted_radius * distorted_radius;
            const double ideal_radius = distorted_radius *
                (1.0 + camera.k1 * radius2 + camera.k2 * radius2 * radius2);
            if (!std::isfinite(ideal_radius))
                return false;
            const double ratio = ideal_radius / distorted_radius;
            u = camera.cx + ux * ratio * camera.px_per_mm;
            v = camera.cy + uy * ratio * camera.px_per_mm;
        }
    }

    // Ideal pinhole ray in the camera frame (right, down, look).
    const double x_cam = (u - camera.cx) / camera.fx;
    const double y_cam = (v - camera.cy) / camera.fy;
    const double z_cam = 1.0;
    const double dir_x = camera.right[0] * x_cam + camera.down[0] * y_cam + camera.look[0] * z_cam;
    const double dir_y = camera.right[1] * x_cam + camera.down[1] * y_cam + camera.look[1] * z_cam;
    const double dir_z = camera.right[2] * x_cam + camera.down[2] * y_cam + camera.look[2] * z_cam;
    if (std::abs(dir_y) < 1e-9)
        return false;

    // Intersect camera ray with world ground plane Y = 0.
    const double t = -camera.cam_y / dir_y;
    if (t <= 0.0)
        return false;

    x_mm = camera.cam_x + t * dir_x;
    y_mm = 0.0;
    z_mm = camera.cam_z + t * dir_z;
    return std::isfinite(x_mm) && std::isfinite(z_mm);
}

bool loadCsv(const std::string& path, Records& records, std::string* error)
{
    std::ifstream input(path);
    if (!input)
    {
        if (error)
            *error = "Cannot open Stype CSV: " + path;
        return false;
    }

    std::string line;
    std::vector<std::string> fields;
    if (!std::getline(input, line) || !splitCsvLine(line, fields))
    {
        if (error)
            *error = "CSV has no valid header";
        return false;
    }
    if (!fields.empty() && fields.front().size() >= 3 &&
        static_cast<unsigned char>(fields.front()[0]) == 0xEF)
        fields.front().erase(0, 3); // UTF-8 BOM

    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < fields.size(); ++i)
        columns.emplace(fields[i], i);
    static const std::array<const char*, 18> required = {
        "frame_id", "timestamp_ms", "stype_valid", "pan_deg", "tilt_deg", "roll_deg",
        "x_mm", "y_mm", "z_mm", "zoom_raw", "focus_raw", "hfov_deg", "hf_ar", "hf_k1",
        "hf_k2", "hf_cx_mm", "hf_cy_mm", "hf_pa_width_mm"};
    for (const char* name : required)
    {
        if (columns.find(name) == columns.end())
        {
            if (error)
                *error = std::string("CSV is missing required column: ") + name;
            return false;
        }
    }

    Records loaded;
    std::size_t line_number = 1;
    while (std::getline(input, line))
    {
        ++line_number;
        if (trim(line).empty())
            continue;
        if (!splitCsvLine(line, fields) || fields.size() != columns.size())
        {
            if (error)
                *error = "Malformed CSV row " + std::to_string(line_number);
            return false;
        }
        const auto field = [&fields, &columns](const char* name) -> const std::string& {
            return fields[columns.at(name)];
        };
        Record record;
        const bool valid =
            parseInteger(field("frame_id"), record.frame_id) &&
            parseInteger(field("timestamp_ms"), record.timestamp_ms) &&
            parseBool(field("stype_valid"), record.stype_valid) &&
            parseDouble(field("pan_deg"), record.pan_deg) &&
            parseDouble(field("tilt_deg"), record.tilt_deg) &&
            parseDouble(field("roll_deg"), record.roll_deg) &&
            parseDouble(field("x_mm"), record.x_mm) &&
            parseDouble(field("y_mm"), record.y_mm) &&
            parseDouble(field("z_mm"), record.z_mm) &&
            parseInteger(field("zoom_raw"), record.zoom_raw) &&
            parseInteger(field("focus_raw"), record.focus_raw) &&
            parseDouble(field("hfov_deg"), record.hfov_deg) &&
            parseDouble(field("hf_ar"), record.hf_ar) &&
            parseDouble(field("hf_k1"), record.hf_k1) &&
            parseDouble(field("hf_k2"), record.hf_k2) &&
            parseDouble(field("hf_cx_mm"), record.hf_cx_mm) &&
            parseDouble(field("hf_cy_mm"), record.hf_cy_mm) &&
            parseDouble(field("hf_pa_width_mm"), record.hf_pa_width_mm);
        if (!valid)
        {
            if (error)
                *error = "Invalid value in CSV row " + std::to_string(line_number);
            return false;
        }
        loaded.push_back(record);
    }
    records = std::move(loaded);
    return true;
}

void drawOverlay(cv::Mat bgr, const Record& record, const OverlayOptions& options)
{
    if (bgr.empty() || bgr.type() != CV_8UC3 || options.gizmo_length_mm <= 0.0)
        return;

    const Record aligned = applyAlignment(record, options.alignment);
    const Camera camera = buildCamera(aligned, options, bgr.cols, bgr.rows);
    cv::Point origin;
    if (!project(camera, 0.0, 0.0, 0.0, origin))
    {
        // drawText(bgr, aligned, aligned.hasDirectFov());
        cv::putText(bgr, "World origin behind camera", cv::Point(12, bgr.rows - 16),
                    cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 200, 255), 2, cv::LINE_AA);
        return;
    }

    struct Axis {
        double x;
        double y;
        double z;
        cv::Scalar color;
        const char* label;
    };
    const double length = options.gizmo_length_mm;
    const std::array<Axis, 3> axes = {{
        {length, 0.0, 0.0, cv::Scalar(0, 0, 255), "X"},
        {0.0, length, 0.0, cv::Scalar(0, 255, 0), "Y"},
        {0.0, 0.0, length, cv::Scalar(255, 100, 0), "Z"}}};
    for (const Axis& axis : axes)
    {
        cv::Point end;
        if (!project(camera, axis.x, axis.y, axis.z, end))
            continue;
        cv::line(bgr, origin, end, axis.color, 3, cv::LINE_AA);
        cv::circle(bgr, end, 6, axis.color, cv::FILLED, cv::LINE_AA);
        cv::putText(bgr, axis.label, end + cv::Point(8, 5), cv::FONT_HERSHEY_SIMPLEX,
                    0.8, axis.color, 2, cv::LINE_AA);
    }

    cv::circle(bgr, origin, 7, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
    cv::circle(bgr, origin, 7, cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
}

} // namespace stype
