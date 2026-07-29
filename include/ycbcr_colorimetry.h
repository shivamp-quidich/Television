#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

// The UYVY input stores component code values, not RGB pixels. Keep the
// matrix/range decision in one small, dependency-free definition so CPU views
// and GLSL views use the same convention.
enum class YCbCrMatrix : int {
    Rec601 = 0,
    Rec709 = 1,
};

enum class YCbCrMatrixSelection : int {
    Auto = 0,
    Rec601 = 1,
    Rec709 = 2,
};

enum class YCbCrRange : int {
    Legal = 0,
    Full = 1,
};

struct YCbCrColorimetry {
    YCbCrMatrix matrix = YCbCrMatrix::Rec709;
    YCbCrRange range = YCbCrRange::Legal;
};

inline constexpr int ycbcrMatrixUniformValue(YCbCrMatrix matrix)
{
    return matrix == YCbCrMatrix::Rec601 ? 0 : 1;
}

inline constexpr int ycbcrRangeUniformValue(YCbCrRange range)
{
    return range == YCbCrRange::Legal ? 0 : 1;
}

inline constexpr const char* ycbcrMatrixName(YCbCrMatrix matrix)
{
    return matrix == YCbCrMatrix::Rec601 ? "Rec.601" : "Rec.709";
}

inline constexpr const char* ycbcrMatrixSelectionName(YCbCrMatrixSelection selection)
{
    switch (selection) {
    case YCbCrMatrixSelection::Auto:   return "Auto";
    case YCbCrMatrixSelection::Rec601: return "Rec.601";
    case YCbCrMatrixSelection::Rec709: return "Rec.709";
    }
    return "Rec.709";
}

inline constexpr const char* ycbcrRangeName(YCbCrRange range)
{
    return range == YCbCrRange::Legal ? "legal" : "full";
}

inline constexpr YCbCrMatrix resolveYCbCrMatrix(YCbCrMatrixSelection selection,
                                                 int frame_height)
{
    switch (selection) {
    case YCbCrMatrixSelection::Rec601:
        return YCbCrMatrix::Rec601;
    case YCbCrMatrixSelection::Rec709:
        return YCbCrMatrix::Rec709;
    case YCbCrMatrixSelection::Auto:
        // DeckLink format-change metadata provides the active display mode,
        // not matrix coefficients. SD modes conventionally use Rec.601 and
        // HD/UHD modes use Rec.709; configuration can override this mapping.
        return frame_height > 0 && frame_height <= 576
            ? YCbCrMatrix::Rec601
            : YCbCrMatrix::Rec709;
    }
    return YCbCrMatrix::Rec709;
}

inline constexpr YCbCrColorimetry resolveYCbCrColorimetry(
    YCbCrMatrixSelection selection, YCbCrRange range, int frame_height)
{
    return {resolveYCbCrMatrix(selection, frame_height), range};
}

inline bool parseYCbCrMatrixSelection(std::string_view value,
                                      YCbCrMatrixSelection& selection)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });

    if (normalized == "auto") {
        selection = YCbCrMatrixSelection::Auto;
        return true;
    }
    if (normalized == "rec601" || normalized == "bt601" || normalized == "601") {
        selection = YCbCrMatrixSelection::Rec601;
        return true;
    }
    if (normalized == "rec709" || normalized == "bt709" || normalized == "709") {
        selection = YCbCrMatrixSelection::Rec709;
        return true;
    }
    return false;
}

inline bool parseYCbCrRange(std::string_view value, YCbCrRange& range)
{
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });

    if (normalized == "legal" || normalized == "limited" || normalized == "studio") {
        range = YCbCrRange::Legal;
        return true;
    }
    if (normalized == "full" || normalized == "full-range") {
        range = YCbCrRange::Full;
        return true;
    }
    return false;
}

inline std::array<float, 3> yCbCrToRgb(const std::array<float, 3>& ycbcr,
                                       const YCbCrColorimetry& colorimetry)
{
    float y = ycbcr[0];
    float cb = ycbcr[1] - 128.0f;
    float cr = ycbcr[2] - 128.0f;

    if (colorimetry.range == YCbCrRange::Legal) {
        y = (y - 16.0f) * (255.0f / 219.0f);
        cb *= 255.0f / 224.0f;
        cr *= 255.0f / 224.0f;
    }

    std::array<float, 3> rgb{};
    if (colorimetry.matrix == YCbCrMatrix::Rec601) {
        rgb = {y + 1.4020f * cr,
               y - 0.344136f * cb - 0.714136f * cr,
               y + 1.7720f * cb};
    } else {
        rgb = {y + 1.5748f * cr,
               y - 0.1873f * cb - 0.4681f * cr,
               y + 1.8556f * cb};
    }

    for (float& component : rgb) {
        component = std::clamp(component, 0.0f, 255.0f);
    }
    return rgb;
}
