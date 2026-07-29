#if 0 // Replaced by the GLFW/OpenGL/ImGui operator application below.
#if 0
#include "decklink_capture.hpp"
#include "logger.h"

#include <climits>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int parseDeviceIndex(const char* argument, const char* name)
{
    char* end = nullptr;
    const long value = std::strtol(argument, &end, 10);
    if (*argument == '\0' || *end != '\0' || value < 0 || value > INT_MAX)
    {
        throw std::runtime_error(std::string("Invalid ") + name + " device index: " + argument);
    }

    return static_cast<int>(value);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 3)
    {
        std::cerr << "Usage: decklink_passthrough [input-device=0] [output-device=1]\n";
        return EXIT_FAILURE;
    }

    int inputDevice = 0;
    int outputDevice = 1;
    try
    {
        if (argc >= 2)
        {
            inputDevice = parseDeviceIndex(argv[1], "input");
        }
        if (argc == 3)
        {
            outputDevice = parseDeviceIndex(argv[2], "output");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    initLogger();
    DeckLinkCapture capture;
    if (!capture.initialize(inputDevice) || !capture.startCapture())
    {
        std::cerr << "Unable to start capture on DeckLink input " << inputDevice << '\n';
        return EXIT_FAILURE;
    }

    cv::namedWindow("DeckLink input", cv::WINDOW_NORMAL);
    std::cout << "Capturing from device " << inputDevice << ", previewing, and sending to device "
              << outputDevice << ". Press q or Esc to stop.\n";

    bool outputReady = false;
    int outputWidth = 0;
    int outputHeight = 0;
    int observedWidth = 0;
    int observedHeight = 0;
    int stableFrameCount = 0;
    auto lastStatus = std::chrono::steady_clock::now();
    uint64_t frameCount = 0;

    while (true)
    {
        std::vector<uint8_t> frameBytes;
        int width = 0;
        int height = 0;
        if (!capture.getFrameBuffer(frameBytes, width, height))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const int pitch = capture.getLastFramePitch();
        const size_t expectedSize = static_cast<size_t>(pitch) * static_cast<size_t>(height);
        if (width <= 0 || height <= 0 || pitch <= 0 || frameBytes.size() < expectedSize)
        {
            std::cerr << "Ignoring invalid capture frame (" << width << 'x' << height
                      << ", pitch=" << pitch << ", bytes=" << frameBytes.size() << ")\n";
            continue;
        }

        cv::Mat uyvy(height, width, CV_8UC2, frameBytes.data(), pitch);
        cv::Mat bgr;
        cv::cvtColor(uyvy, bgr, cv::COLOR_YUV2BGR_UYVY);
        cv::imshow("DeckLink input", bgr);

        int modeWidth = 0;
        int modeHeight = 0;
        capture.getCurrentDisplayModeResolution(modeWidth, modeHeight);
        if (modeWidth == 0 || modeHeight == 0 || width != modeWidth || height != modeHeight)
        {
            // Preview pre-detection and stale buffered frames, but never forward
            // them into output configured for a different DeckLink display mode.
            observedWidth = 0;
            observedHeight = 0;
            stableFrameCount = 0;
            const int key = cv::waitKey(1) & 0xFF;
            if (key == 'q' || key == 27)
            {
                break;
            }
            continue;
        }

        // The SDK can deliver one frame from the previous mode before its format
        // change callback restarts capture. Require two same-size frames before
        // configuring output, so a stale frame cannot select the wrong SDI mode.
        if (width == observedWidth && height == observedHeight)
        {
            ++stableFrameCount;
        }
        else
        {
            observedWidth = width;
            observedHeight = height;
            stableFrameCount = 1;
        }

        // Reinitialize output only after the replacement signal is stable, so raw
        // UYVY is never sent to a mismatched DeckLink display mode.
        if (!outputReady || width != outputWidth || height != outputHeight)
        {
            if (stableFrameCount < 2)
            {
                const int key = cv::waitKey(1) & 0xFF;
                if (key == 'q' || key == 27)
                {
                    break;
                }
                continue;
            }

            if (!capture.initializeOutput(outputDevice))
            {
                std::cerr << "Unable to initialize DeckLink output " << outputDevice << '\n';
                break;
            }
            outputReady = true;
            outputWidth = width;
            outputHeight = height;
            std::cout << "Output initialized for " << width << 'x' << height
                      << " (" << capture.getOutputDisplayModeName() << ")\n";
        }

        if (!capture.sendFrameToDecklink(frameBytes.data(), expectedSize, width, height, pitch))
        {
            std::cerr << "Failed to send frame to DeckLink output\n";
        }

        ++frameCount;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatus >= std::chrono::seconds(5))
        {
            std::cout << "Forwarded " << frameCount << " frames; input mode "
                      << capture.getDisplayModeName() << '\n';
            lastStatus = now;
        }

        const int key = cv::waitKey(1) & 0xFF;
        if (key == 'q' || key == 27)
        {
            break;
        }
    }

    capture.disableOutput(0);
    capture.stopCapture();
    cv::destroyAllWindows();
    return 0;
}
#endif

#include "decklink_capture.hpp"
#include "logger.h"
#include "stype_csv_overlay.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cfloat>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <opencv2/imgproc.hpp>

namespace {

int parseDeviceIndex(const char* argument, const char* name)
{
    char* end = nullptr;
    const long value = std::strtol(argument, &end, 10);
    if (*argument == '\0' || *end != '\0' || value < 0 || value > INT_MAX)
        throw std::runtime_error(std::string("Invalid ") + name + " device index: " + argument);
    return static_cast<int>(value);
}

std::vector<std::filesystem::path> csvFiles(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::is_directory(directory))
        return files;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().extension() == ".csv")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

void uploadFrame(GLuint texture, const cv::Mat& bgr)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, bgr.cols, bgr.rows, 0,
                 GL_BGR, GL_UNSIGNED_BYTE, bgr.data);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 3)
    {
        std::cerr << "Usage: decklink_passthrough [input-device=0] [output-device=1]\n";
        return EXIT_FAILURE;
    }

    int inputDevice = 0;
    int outputDevice = 1;
    try
    {
        if (argc >= 2) inputDevice = parseDeviceIndex(argv[1], "input");
        if (argc == 3) outputDevice = parseDeviceIndex(argv[2], "output");
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    if (!glfwInit())
    {
        std::cerr << "Unable to initialize GLFW\n";
        return EXIT_FAILURE;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Televison", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK)
    {
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    GLuint previewTexture = 0;
    glGenTextures(1, &previewTexture);
    glBindTexture(GL_TEXTURE_2D, previewTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    initLogger();
    DeckLinkCapture capture;
    if (!capture.initialize(inputDevice) || !capture.startCapture())
    {
        std::cerr << "Unable to start DeckLink input " << inputDevice << '\n';
        glDeleteTextures(1, &previewTexture);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    const std::filesystem::path csvDirectory = "csv";
    std::vector<std::filesystem::path> files = csvFiles(csvDirectory);
    stype::Records records;
    int selectedFile = -1;
    int csvOffset = 0;
    std::string csvStatus = "Select a CSV from csv/";
    if (!files.empty())
    {
        selectedFile = 0;
        stype::loadCsv(files.front().string(), records, &csvStatus);
        if (!records.empty())
            csvStatus = "Loaded " + std::to_string(records.size()) + " tracking rows";
    }

    bool outputReady = false;
    int outputWidth = 0, outputHeight = 0;
    int stableWidth = 0, stableHeight = 0, stableFrames = 0;
    std::uint64_t videoFrame = 0;
    cv::Mat preview;
    stype::Record activeRecord;
    bool hasActiveRecord = false;
    std::string outputStatus = "Waiting for input format";

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        std::vector<std::uint8_t> frameBytes;
        int width = 0, height = 0;
        if (capture.getFrameBuffer(frameBytes, width, height))
        {
            const int pitch = capture.getLastFramePitch();
            const size_t frameSize = static_cast<size_t>(pitch) * static_cast<size_t>(height);
            int modeWidth = 0, modeHeight = 0;
            capture.getCurrentDisplayModeResolution(modeWidth, modeHeight);
            if (width > 0 && height > 0 && pitch > 0 && frameBytes.size() >= frameSize)
            {
                cv::Mat uyvy(height, width, CV_8UC2, frameBytes.data(), pitch);
                cv::cvtColor(uyvy, preview, cv::COLOR_YUV2BGR_UYVY);
                hasActiveRecord = false;
                const auto row = static_cast<std::int64_t>(videoFrame) + csvOffset;
                if (row >= 0 && row < static_cast<std::int64_t>(records.size()))
                {
                    activeRecord = records[static_cast<size_t>(row)];
                    hasActiveRecord = true;
                    stype::drawOverlay(preview, activeRecord);
                }

                uploadFrame(previewTexture, preview);
                ++videoFrame;

                const bool modeMatches = modeWidth == width && modeHeight == height;
                if (modeMatches)
                {
                    stableFrames = (width == stableWidth && height == stableHeight) ? stableFrames + 1 : 1;
                    stableWidth = width;
                    stableHeight = height;
                }
                else
                {
                    stableFrames = 0;
                }

                if (modeMatches && stableFrames >= 2 &&
                    (!outputReady || width != outputWidth || height != outputHeight))
                {
                    outputReady = capture.initializeOutput(outputDevice);
                    if (outputReady)
                    {
                        outputWidth = width;
                        outputHeight = height;
                        outputStatus = "Output " + std::to_string(capture.getActiveOutputDeviceIndex()) + ": " +
                                       capture.getOutputDisplayModeName();
                    }
                    else
                    {
                        outputStatus = "DeckLink output initialization failed";
                    }
                }

                if (outputReady && modeMatches && width == outputWidth && height == outputHeight)
                {
                    cv::Mat outputUyvy(height, width, CV_8UC2);
                    cv::cvtColor(preview, outputUyvy, cv::COLOR_BGR2YUV_UYVY);
                    if (!capture.sendFrameToDecklink(outputUyvy.data,
                                                     outputUyvy.total() * outputUyvy.elemSize(),
                                                     width, height,
                                                     static_cast<int>(outputUyvy.step)))
                        outputStatus = "DeckLink output frame send failed";
                }
            }
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float panelWidth = 390.0f;
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - panelWidth, viewport->WorkSize.y));
        ImGui::Begin("Live DeckLink Video", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        if (!preview.empty())
        {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float scale = std::min(available.x / preview.cols, available.y / preview.rows);
            ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(previewTexture)),
                         ImVec2(preview.cols * scale, preview.rows * scale));
        }
        else
        {
            ImGui::TextUnformatted("Waiting for DeckLink video input...");
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - panelWidth, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(panelWidth, viewport->WorkSize.y));
        ImGui::Begin("Tracking Control", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::Text("Input %d  Output %d", inputDevice, outputDevice);
        ImGui::TextWrapped("%s", outputStatus.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Tracking CSV");
        if (ImGui::Button("Refresh CSV list"))
        {
            files = csvFiles(csvDirectory);
            if (selectedFile >= static_cast<int>(files.size())) selectedFile = -1;
        }
        if (ImGui::BeginCombo("CSV file", selectedFile >= 0 ? files[selectedFile].filename().c_str() : "(none)"))
        {
            for (int i = 0; i < static_cast<int>(files.size()); ++i)
            {
                const bool selected = i == selectedFile;
                if (ImGui::Selectable(files[i].filename().c_str(), selected))
                {
                    stype::Records loaded;
                    std::string error;
                    if (stype::loadCsv(files[i].string(), loaded, &error))
                    {
                        records = std::move(loaded);
                        selectedFile = i;
                        videoFrame = 0;
                        csvStatus = "Loaded " + std::to_string(records.size()) + " rows";
                    }
                    else
                    {
                        csvStatus = error;
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload") && selectedFile >= 0)
        {
            stype::Records loaded;
            if (stype::loadCsv(files[selectedFile].string(), loaded, &csvStatus))
                records = std::move(loaded);
        }
        ImGui::TextWrapped("%s", csvStatus.c_str());
        ImGui::SliderInt("CSV frame offset", &csvOffset, -500, 500);
        ImGui::Text("Video frame: %llu", static_cast<unsigned long long>(videoFrame));

        if (hasActiveRecord)
        {
            ImGui::Separator();
            ImGui::Text("CSV row %lld", static_cast<long long>(activeRecord.frame_id));
            ImGui::Text("Pan %.2f  Tilt %.2f  Roll %.2f", activeRecord.pan_deg, activeRecord.tilt_deg, activeRecord.roll_deg);
            ImGui::Text("X %.3f  Y %.3f  Z %.3f m", activeRecord.x_mm / 1000.0,
                        activeRecord.y_mm / 1000.0, activeRecord.z_mm / 1000.0);
            ImGui::Text("HFOV %.2f  Zoom %lld  Focus %lld", activeRecord.hfov_deg,
                        static_cast<long long>(activeRecord.zoom_raw), static_cast<long long>(activeRecord.focus_raw));
        }
        if (!records.empty())
        {
            std::vector<float> pan, tilt, roll, x, y, z;
            pan.reserve(records.size()); tilt.reserve(records.size()); roll.reserve(records.size());
            x.reserve(records.size()); y.reserve(records.size()); z.reserve(records.size());
            for (const auto& record : records)
            {
                pan.push_back(static_cast<float>(record.pan_deg)); tilt.push_back(static_cast<float>(record.tilt_deg));
                roll.push_back(static_cast<float>(record.roll_deg)); x.push_back(static_cast<float>(record.x_mm / 1000.0));
                y.push_back(static_cast<float>(record.y_mm / 1000.0)); z.push_back(static_cast<float>(record.z_mm / 1000.0));
            }
            const int marker = std::clamp(static_cast<int>(videoFrame) + csvOffset, 0, static_cast<int>(records.size()) - 1);
            ImGui::Separator();
            ImGui::TextUnformatted("CSV values");
            ImGui::PlotLines("Pan", pan.data(), static_cast<int>(pan.size()), marker, nullptr, -180.0f, 180.0f, ImVec2(-1, 70));
            ImGui::PlotLines("Tilt", tilt.data(), static_cast<int>(tilt.size()), marker, nullptr, -180.0f, 180.0f, ImVec2(-1, 70));
            ImGui::PlotLines("Roll", roll.data(), static_cast<int>(roll.size()), marker, nullptr, -180.0f, 180.0f, ImVec2(-1, 70));
            ImGui::PlotLines("X (m)", x.data(), static_cast<int>(x.size()), marker, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 55));
            ImGui::PlotLines("Y (m)", y.data(), static_cast<int>(y.size()), marker, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 55));
            ImGui::PlotLines("Z (m)", z.data(), static_cast<int>(z.size()), marker, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 55));
        }
        ImGui::End();

        ImGui::Render();
        int displayWidth = 0, displayHeight = 0;
        glfwGetFramebufferSize(window, &displayWidth, &displayHeight);
        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.06f, 0.07f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    capture.disableOutput(0);
    capture.stopCapture();
    glDeleteTextures(1, &previewTexture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
#endif

#include "camera_trajectory_overlay.hpp"
#include "gpu_uyvy_preview.hpp"
#include "ingest.hpp"
#include "logger.h"
#include "udp_receiver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgproc.hpp>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <cuda_runtime.h>

namespace {
// Reports a failure when it first appears and stays silent until it clears, so a
// persistent per-frame failure cannot flood the log.
void reportFailure(bool& latched, bool failed, const char* message)
{
    if (failed && !latched)
        getModuleLogger("app")->warn(message);
    latched = failed;
}

bool copyFrameToHost(const FrameData& frame, std::vector<uint8_t>& host)
{
    const auto& uyvy = *frame.uyvy_frame;
    host.resize(uyvy.size);
    return cudaMemcpy2D(host.data(), uyvy.pitch, uyvy.d_data, uyvy.pitch,
                        uyvy.pitch, uyvy.height, cudaMemcpyDeviceToHost) == cudaSuccess;
}

// Paints the tracking visualization into the staged UYVY bytes so the DeckLink
// output carries the same picture the operator sees in the preview.
bool burnOverlayIntoStagedFrame(std::vector<uint8_t>& host, int width, int height, int pitch,
                                const std::deque<STypeState::CameraData>& trail,
                                const STypeState::CameraData& camera,
                                const CameraTrajectoryOverlayOptions& options,
                                cv::Mat& bgr_scratch)
{
    if (width <= 0 || height <= 0 || pitch < width * 2)
        return false;
    if (host.size() < static_cast<std::size_t>(pitch) * static_cast<std::size_t>(height))
        return false;

    cv::Mat uyvy(height, width, CV_8UC2, host.data(), static_cast<std::size_t>(pitch));
    cv::cvtColor(uyvy, bgr_scratch, cv::COLOR_YUV2BGR_UYVY);
    drawCameraTrajectoryOnBgr(bgr_scratch, trail, camera, options);
    cv::cvtColor(bgr_scratch, uyvy, cv::COLOR_BGR2YUV_UYVY);
    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    const int input_device = argc > 1 ? std::atoi(argv[1]) : 0;
    const int output_device = argc > 2 ? std::atoi(argv[2]) : 1;
    if (!glfwInit())
        return EXIT_FAILURE;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Televison", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    initLogger();
    SharedState shared_state;
    IngestConfig config;
    config.input_device = input_device;
    config.output_device = output_device;
    auto ingest = CreateIngest(config);
    if (!ingest || !ingest->initialize(config) || !ingest->startCapture()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }
    ingest->setSharedState(&shared_state);

    GpuUyvyPreview preview;
    UdpReceiver udp_receiver;
    constexpr const char* kUdpBindIp = "127.0.0.1";
    constexpr const char* kUdpRawOutput = "/home/quidich/Televison/udpOut/udp_raw.txt";
    int udp_port = 6305;
    udp_receiver.start(kUdpBindIp, udp_port, kUdpRawOutput);

    std::uint64_t frame_id = 0;
    std::string status = "Waiting for DeckLink input";
    bool output_ready = false;
    int stable_width = 0;
    int stable_height = 0;
    int stable_frame_count = 0;
    std::vector<uint8_t> output_staging;
    bool preview_failing = false;
    bool staging_failing = false;
    bool output_send_failing = false;

    CameraTrajectoryOverlayOptions trajectory_options;
    bool burn_overlay_into_output = true;
    std::deque<STypeState::CameraData> camera_trail;
    std::uint64_t trail_packet_count = 0;
    cv::Mat overlay_scratch;
    bool overlay_burn_failing = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Sampled once per iteration so the SDI burn-in and the preview overlay
        // always visualize the same pose.
        const UdpReceiver::Status udp = udp_receiver.status();
        if (udp.last_packet_valid && udp.packets_received != trail_packet_count) {
            trail_packet_count = udp.packets_received;
            camera_trail.push_back(udp.last_camera);
            while (camera_trail.size() > static_cast<std::size_t>(std::max(2, trajectory_options.max_trail_points)))
                camera_trail.pop_front();
        }
        const bool overlay_available = trajectory_options.enabled && udp.last_packet_valid;

        std::shared_ptr<FrameData> frame;
        if (ingest->getFrame(frame) && frame && frame->uyvy_frame) {
            frame->id = static_cast<int>(frame_id++);
            frame->timestamp = std::chrono::steady_clock::now();
            frame->sync_timestamp_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            const bool preview_updated = preview.update(*frame->uyvy_frame);

            const IngestVideoInfo info = ingest->getVideoInfo();
            VideoInputState::Config video_config;
            video_config.width = info.width;
            video_config.height = info.height;
            video_config.display_mode_name = info.display_mode_name;
            // getFrame() consumes the capture's frame-available flag, so
            // info.signal_detected is false for the frame we are holding.
            // Holding a decoded frame is itself proof of an input signal.
            video_config.signal_detected = true;
            video_config.interlaced = info.interlaced;
            video_config.ideal_fps = info.ideal_fps;
            video_config.fps_threshold = info.fps_threshold;
            shared_state.setData(video_config);

            const bool dimensions_match_mode = info.width == frame->uyvy_frame->width &&
                                               info.height == frame->uyvy_frame->height;
            if (dimensions_match_mode) {
                stable_frame_count = (stable_width == info.width && stable_height == info.height)
                    ? stable_frame_count + 1 : 1;
                stable_width = info.width;
                stable_height = info.height;
            } else {
                stable_frame_count = 0;
            }
            if (!output_ready && stable_frame_count >= 2)
                output_ready = ingest->initializeOutput(output_device);
            if (output_ready && dimensions_match_mode && preview_updated) {
                const bool staged = copyFrameToHost(*frame, output_staging);
                reportFailure(staging_failing, !staged, "Device-to-host staging copy failed");
                if (staged) {
                    bool burn_failed = false;
                    if (burn_overlay_into_output && overlay_available) {
                        burn_failed = !burnOverlayIntoStagedFrame(
                            output_staging, frame->uyvy_frame->width, frame->uyvy_frame->height,
                            frame->uyvy_frame->pitch, camera_trail, udp.last_camera,
                            trajectory_options, overlay_scratch);
                    }
                    reportFailure(overlay_burn_failing, burn_failed,
                                  "Trajectory burn-in skipped; sending the clean frame");
                    const bool sent = ingest->sendFrameToOutput(
                        output_staging.data(), output_staging.size(),
                        frame->uyvy_frame->width, frame->uyvy_frame->height,
                        frame->uyvy_frame->pitch);
                    reportFailure(output_send_failing, !sent, "DeckLink output rejected a frame");
                }
            }
            reportFailure(preview_failing, !preview_updated,
                          "GPU preview update failed; SDI output is gated on it");
            status = info.display_mode_name + "  GPU FrameData #" + std::to_string(frame->id) +
                     (preview_updated ? "" : "  (preview update failed)");
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float controls_width = 390.0f;
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x - controls_width, viewport->WorkSize.y));
        ImGui::Begin("GPU DeckLink Preview", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        if (preview.rgbTexture()) {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float scale = std::min(available.x / preview.width(), available.y / preview.height());
            ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(preview.rgbTexture())),
                         ImVec2(preview.width() * scale, preview.height() * scale));
            const ImVec2 image_min = ImGui::GetItemRectMin();
            const ImVec2 image_max = ImGui::GetItemRectMax();
            // Clicking the picture re-places the scene. Storing the position
            // normalized keeps the preview and the SDI frame in agreement.
            if (trajectory_options.enabled && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                trajectory_options.anchor_u =
                    std::clamp((mouse.x - image_min.x) / std::max(image_max.x - image_min.x, 1.0f), 0.0f, 1.0f);
                trajectory_options.anchor_v =
                    std::clamp((mouse.y - image_min.y) / std::max(image_max.y - image_min.y, 1.0f), 0.0f, 1.0f);
            }
            // The preview draws the overlay with ImGui rather than reading back
            // the burned frame, so it mirrors what SDI carries at zero extra cost.
            if (overlay_available)
                drawCameraTrajectoryOverlay(ImGui::GetWindowDrawList(), image_min, image_max,
                                            camera_trail, udp.last_camera, trajectory_options);
        } else {
            ImGui::TextUnformatted("Waiting for GPU FrameData...");
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - controls_width, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(controls_width, viewport->WorkSize.y));
        ImGui::Begin("Application Status", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::TextUnformatted("CSV tracking and frame overlays are disabled.");
        ImGui::Separator();
        ImGui::TextUnformatted("UDP receive");
        ImGui::Text("Bind IP  %s  (local)", kUdpBindIp);
        ImGui::InputInt("Port", &udp_port);
        if (udp_port < 1) udp_port = 1;
        if (udp_port > 65535) udp_port = 65535;
        if (ImGui::Button("Apply UDP port"))
            udp_receiver.start(kUdpBindIp, udp_port, kUdpRawOutput);

        ImGui::Text("Listening  %s", udp.listening ? "yes" : "no");
        ImGui::Text("Packets  %llu", static_cast<unsigned long long>(udp.packets_received));
        ImGui::Text("Last size  %d bytes", udp.last_packet_size);
        ImGui::TextWrapped("UDP text file  %s", udp.raw_output_path.c_str());
        if (!udp.last_error.empty())
            ImGui::TextWrapped("Error: %s", udp.last_error.c_str());
        if (udp.last_packet_valid) {
            const auto& cam = udp.last_camera;
            // Pose telemetry is reported here rather than drawn on the picture,
            // matching viz3d's read-out bar. Y is the height axis, Z is depth.
            ImGui::Text("Cam %d   zoom %d   focus %d", cam.camera_id, cam.zoom_raw, cam.focus_raw);
            ImGui::Text("pan %8.2f   tilt %7.2f   roll %6.2f deg",
                        cam.pan_deg, cam.tilt_deg, cam.roll_deg);
            ImGui::Text("X %7.1f   Y %6.1f   Z(depth) %7.1f m",
                        cam.x_mm * 0.001f, cam.y_mm * 0.001f, cam.z_mm * 0.001f);
            ImGui::Text("dist-to-origin %.1f m",
                        std::sqrt(cam.x_mm * cam.x_mm + cam.z_mm * cam.z_mm) * 0.001f);
        } else if (udp.packets_received > 0) {
            ImGui::TextUnformatted("Last packet was not a valid FreeD D1 frame.");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Trajectory overlay");
        ImGui::Checkbox("Show overlay", &trajectory_options.enabled);
        ImGui::Checkbox("Burn into SDI output", &burn_overlay_into_output);
        ImGui::Checkbox("Ground grid", &trajectory_options.show_grid);
        ImGui::DragFloat("Ground plane", &trajectory_options.ground_plane_y_mm,
                         10.0f, -100000.0f, 100000.0f, "%.0f mm");
        ImGui::Checkbox("Frustum web", &trajectory_options.show_frustum);
        ImGui::BeginDisabled(!trajectory_options.show_frustum);
        ImGui::SliderFloat("Cone angle", &trajectory_options.frustum_half_angle_deg,
                           1.0f, 60.0f, "%.1f deg half");
        ImGui::SliderFloat("Web length", &trajectory_options.frustum_length_percent,
                           1.0f, 100.0f, "%.0f%% of trail");
        if (ImGui::Button("Reset frustum")) {
            const CameraTrajectoryOverlayOptions defaults;
            trajectory_options.frustum_half_angle_deg = defaults.frustum_half_angle_deg;
            trajectory_options.frustum_length_percent = defaults.frustum_length_percent;
        }
        ImGui::EndDisabled();
        ImGui::SliderInt("Trail length", &trajectory_options.max_trail_points, 2, 2000);
        ImGui::Text("Trail samples  %d", static_cast<int>(camera_trail.size()));
        if (ImGui::Button("Clear trail"))
            camera_trail.clear();
        ImGui::TextWrapped("Click the video to place the scene. Anchor %.2f, %.2f",
                           trajectory_options.anchor_u, trajectory_options.anchor_v);
        if (ImGui::Button("Centre scene")) {
            const CameraTrajectoryOverlayOptions defaults;
            trajectory_options.anchor_u = defaults.anchor_u;
            trajectory_options.anchor_v = defaults.anchor_v;
        }
        ImGui::End();

        ImGui::Render();
        int display_width = 0, display_height = 0;
        glfwGetFramebufferSize(window, &display_width, &display_height);
        glViewport(0, 0, display_width, display_height);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    ingest->disableOutput(0);
    ingest->stopCapture();
    udp_receiver.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}