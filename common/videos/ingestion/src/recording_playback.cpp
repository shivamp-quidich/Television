#include "recording_playback.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <opencv2/imgproc.hpp>

namespace {

bool isVideoExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp4" || ext == ".mkv" || ext == ".mov" || ext == ".avi";
}

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::vector<RecordingPair> listRecordingPairs(const std::string& recordings_dir)
{
    std::vector<RecordingPair> pairs;
    std::error_code ec;
    const std::filesystem::path root(recordings_dir);
    if (!std::filesystem::is_directory(root, ec))
        return pairs;

    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        const auto& entry = *it;
        if (!entry.is_regular_file(file_ec) && !entry.is_symlink(file_ec))
            continue;
        if (!isVideoExtension(entry.path()))
            continue;

        const std::string stem = entry.path().stem().string();
        // Prefer {stem}_stype.csv (stype_handover layout); also accept {stem}.csv.
        std::filesystem::path csv_path = entry.path().parent_path() / (stem + "_stype.csv");
        if (!std::filesystem::is_regular_file(csv_path, file_ec)) {
            csv_path = entry.path().parent_path() / (stem + ".csv");
            if (!std::filesystem::is_regular_file(csv_path, file_ec))
                continue;
        }

        RecordingPair pair;
        pair.name = entry.path().filename().string(); // e.g. sdi_….mp4
        pair.video_path = entry.path().string();
        pair.csv_path = csv_path.string();
        pairs.push_back(std::move(pair));
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const RecordingPair& a, const RecordingPair& b) { return a.name < b.name; });
    return pairs;
}

RecordingPlayback::~RecordingPlayback()
{
    close();
}

void RecordingPlayback::close()
{
    if (capture_.isOpened())
        capture_.release();
    records_.clear();
    name_.clear();
    frame_count_ = 0;
    frame_index_ = 0;
    width_ = 0;
    height_ = 0;
    frame_bgr_.release();
    display_.release();
    has_record_ = false;
    playing = false;
    last_advance_ms_ = 0;
}

bool RecordingPlayback::readCurrentFrame_(std::string& error)
{
    if (!capture_.read(frame_bgr_) || frame_bgr_.empty()) {
        error = "end of video or read failed";
        return false;
    }
    if (frame_bgr_.channels() == 1)
        cv::cvtColor(frame_bgr_, frame_bgr_, cv::COLOR_GRAY2BGR);
    else if (frame_bgr_.channels() == 4)
        cv::cvtColor(frame_bgr_, frame_bgr_, cv::COLOR_BGRA2BGR);
    width_ = frame_bgr_.cols;
    height_ = frame_bgr_.rows;
    applyOverlay_();
    error.clear();
    return true;
}

void RecordingPlayback::applyOverlay_()
{
    display_ = frame_bgr_.clone();
    has_record_ = false;
    const auto row = static_cast<std::int64_t>(frame_index_) + csv_offset;
    if (row >= 0 && row < static_cast<std::int64_t>(records_.size())) {
        active_record_ = records_[static_cast<std::size_t>(row)];
        has_record_ = true;
        if (show_world_origin)
            stype::drawOverlay(display_, active_record_, overlay_options);
    }
}

bool RecordingPlayback::open(const RecordingPair& pair, std::string& error)
{
    close();
    if (!stype::loadCsv(pair.csv_path, records_, &error))
        return false;

    capture_.open(pair.video_path);
    if (!capture_.isOpened()) {
        error = "cannot open video: " + pair.video_path;
        records_.clear();
        return false;
    }

    name_ = pair.name;
    frame_count_ = static_cast<int>(capture_.get(cv::CAP_PROP_FRAME_COUNT));
    fps_ = capture_.get(cv::CAP_PROP_FPS);
    if (!(fps_ > 1.0 && fps_ < 240.0))
        fps_ = 25.0;
    frame_index_ = 0;
    playing = false;
    last_advance_ms_ = nowMs();

    if (!readCurrentFrame_(error)) {
        close();
        return false;
    }
    return true;
}

bool RecordingPlayback::seek(int frame_index)
{
    if (!capture_.isOpened())
        return false;
    const int clamped = std::clamp(frame_index, 0, std::max(0, frame_count_ - 1));
    capture_.set(cv::CAP_PROP_POS_FRAMES, clamped);
    frame_index_ = clamped;
    std::string error;
    if (!readCurrentFrame_(error)) {
        playing = false;
        return false;
    }
    last_advance_ms_ = nowMs();
    return true;
}

void RecordingPlayback::refreshOverlay()
{
    if (!frame_bgr_.empty())
        applyOverlay_();
}

bool RecordingPlayback::tick()
{
    if (!capture_.isOpened() || !playing)
        return false;

    const std::int64_t now = nowMs();
    const std::int64_t interval_ms = static_cast<std::int64_t>(1000.0 / fps_);
    if (now - last_advance_ms_ < interval_ms)
        return false;
    last_advance_ms_ = now;

    const int next = frame_index_ + 1;
    if (frame_count_ > 0 && next >= frame_count_) {
        playing = false;
        return false;
    }

    frame_index_ = next;
    std::string error;
    if (!readCurrentFrame_(error)) {
        playing = false;
        frame_index_ = std::max(0, frame_index_ - 1);
        return false;
    }
    return true;
}
