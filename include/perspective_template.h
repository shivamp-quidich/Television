#pragma once

#include <array>
#include <string>
#include <opencv2/core.hpp>

namespace perspective_template
{
    constexpr int kTemplatePointCount = 10;
    constexpr float kPitchWidthMeters = 3.05f;   // Across the wicket (10 ft)
    constexpr float kPitchLengthMeters = 20.12f; // Between bowling creases (22 yards)
    constexpr float kPoppingCreaseOffsetMeters = 1.22f; // Distance from bowling crease towards center
    constexpr float kMetricToCentimeter = 100.0f;

    struct TemplatePoint
    {
        const char *label;
        cv::Point2f model; // Units: centimeters relative to pitch origin (top-left)
    };

    inline const std::array<TemplatePoint, kTemplatePointCount> &getTemplatePoints()
    {
        static const std::array<TemplatePoint, kTemplatePointCount> kPoints = []()
        {
            const float pitch_width = kPitchWidthMeters * kMetricToCentimeter;
            const float pitch_length = kPitchLengthMeters * kMetricToCentimeter;
            const float popping_crease_offset = kPoppingCreaseOffsetMeters * kMetricToCentimeter;
            const float top_popping_y = popping_crease_offset;
            const float bottom_popping_y = pitch_length - popping_crease_offset;
            const float stump_center_x = pitch_width * 0.5f;

            std::array<TemplatePoint, kTemplatePointCount> result{};
            result[0] = {"Pitch: Top-Left", cv::Point2f(0.0f, 0.0f)};
            result[1] = {"Pitch: Top-Right", cv::Point2f(pitch_width, 0.0f)};
            result[2] = {"Pitch: Bottom-Right", cv::Point2f(pitch_width, pitch_length)};
            result[3] = {"Pitch: Bottom-Left", cv::Point2f(0.0f, pitch_length)};
            result[4] = {"Popping Crease: Top-Left", cv::Point2f(0.0f, top_popping_y)};
            result[5] = {"Popping Crease: Top-Right", cv::Point2f(pitch_width, top_popping_y)};
            result[6] = {"Popping Crease: Bottom-Right", cv::Point2f(pitch_width, bottom_popping_y)};
            result[7] = {"Popping Crease: Bottom-Left", cv::Point2f(0.0f, bottom_popping_y)};
            result[8] = {"Stumps: Top-Center", cv::Point2f(stump_center_x, 0.0f)};
            result[9] = {"Stumps: Bottom-Center", cv::Point2f(stump_center_x, pitch_length)};
            return result;
        }();
        return kPoints;
    }

    inline const std::array<int, 4> &getCornerIndices()
    {
        static const std::array<int, 4> kCornerIndices = {0, 1, 2, 3};
        return kCornerIndices;
    }

    inline const std::array<int, 2> &getTopPoppingCreaseIndices()
    {
        static const std::array<int, 2> kTopPoppingIndices = {4, 5};
        return kTopPoppingIndices;
    }

    inline const std::array<int, 2> &getBottomPoppingCreaseIndices()
    {
        static const std::array<int, 2> kBottomPoppingIndices = {7, 6};
        return kBottomPoppingIndices;
    }

    inline const std::array<int, 2> &getStumpCenterIndices()
    {
        static const std::array<int, 2> kStumpIndices = {8, 9};
        return kStumpIndices;
    }

    inline float getPitchLengthCentimeters()
    {
        return kPitchLengthMeters * kMetricToCentimeter;
    }

    inline float getPitchWidthCentimeters()
    {
        return kPitchWidthMeters * kMetricToCentimeter;
    }

    inline float getPitchLengthMeters()
    {
        return kPitchLengthMeters;
    }

    inline float getPitchWidthMeters()
    {
        return kPitchWidthMeters;
    }

    inline float getPoppingCreaseOffsetMeters()
    {
        return kPoppingCreaseOffsetMeters;
    }
}
