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
    GLFWwindow* window = glfwCreateWindow(1600, 900, app_config.window_title.c_str(), nullptr, nullptr);
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

#include "app_config.hpp"
#include "gpu_uyvy_preview.hpp"
#include "graphic_overlay.hpp"
#include "ingest.hpp"
#include "logger.h"
#include "recording_playback.hpp"
#include "udp_receiver.hpp"
#include "udp_sender.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
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

// Converts the staged UYVY frame to BGR, burns all placed graphics, converts back.
bool burnGraphicIntoStagedFrame(std::vector<uint8_t>& host, int width, int height, int pitch,
                                const GraphicOverlay& graphic,
                                const GraphicOverlayOptions& options,
                                cv::Mat& bgr_scratch)
{
    if (width <= 0 || height <= 0 || pitch < width * 2)
        return false;
    if (host.size() < static_cast<std::size_t>(pitch) * static_cast<std::size_t>(height))
        return false;
    if (!options.burn_into_sdi || graphic.placedCount() == 0)
        return true;

    cv::Mat uyvy(height, width, CV_8UC2, host.data(), static_cast<std::size_t>(pitch));
    cv::cvtColor(uyvy, bgr_scratch, cv::COLOR_YUV2BGR_UYVY);
    graphic.burnIntoBgr(bgr_scratch, options);
    cv::cvtColor(bgr_scratch, uyvy, cv::COLOR_BGR2YUV_UYVY);
    return true;
}

std::string resolveDataDir()
{
    const char* candidates[] = {"data", "../data", "../../data"};
    for (const char* c : candidates) {
        if (std::filesystem::is_directory(c))
            return c;
    }
    return "data";
}

void uploadBgrTexture(GLuint texture, const cv::Mat& bgr)
{
    if (texture == 0 || bgr.empty() || bgr.type() != CV_8UC3)
        return;
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, bgr.cols, bgr.rows, 0,
                 GL_BGR, GL_UNSIGNED_BYTE, bgr.data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Same placement interaction for Live and Recording: click commits one graphic,
// then pending clears so the operator must select an image again.
void handleGraphicPlacementOnVideo(GraphicOverlay& graphic,
                                   GraphicOverlayOptions& options,
                                   int& selected_graphic,
                                   std::string& graphic_status,
                                   const ImVec2& image_min,
                                   const ImVec2& image_max,
                                   const ImVec2& mouse)
{
    if (graphic.hasPending() &&
        ImGui::IsItemHovered() && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const float video_w = std::max(image_max.x - image_min.x, 1.0f);
        const float video_h = std::max(image_max.y - image_min.y, 1.0f);
        const float u = std::clamp((mouse.x - image_min.x) / video_w, 0.0f, 1.0f);
        const float v = std::clamp((mouse.y - image_min.y) / video_h, 0.0f, 1.0f);
        const std::string placed_name = graphic.pendingName();
        if (graphic.placeAt(u, v, options.width_fraction)) {
            selected_graphic = -1;
            graphic_status = "Placed " + placed_name + "  (" +
                             std::to_string(graphic.placedCount()) +
                             " total) — select an image to place another";
        }
    }
    graphic.drawPreview(ImGui::GetWindowDrawList(), image_min, image_max, mouse, options);
}

// Converts a BGR frame to pitched UYVY and sends it to DeckLink SDI, resizing
// when the recording resolution differs from the enabled output mode.
bool sendBgrToSdi(IIngest& ingest,
                  const cv::Mat& bgr_in,
                  int out_w,
                  int out_h,
                  cv::Mat& resize_scratch,
                  cv::Mat& uyvy_scratch,
                  bool& send_failing)
{
    if (bgr_in.empty() || bgr_in.type() != CV_8UC3 || out_w <= 0 || out_h <= 0)
        return false;

    const cv::Mat* src = &bgr_in;
    if (bgr_in.cols != out_w || bgr_in.rows != out_h) {
        cv::resize(bgr_in, resize_scratch, cv::Size(out_w, out_h), 0, 0, cv::INTER_AREA);
        src = &resize_scratch;
    }

    cv::cvtColor(*src, uyvy_scratch, cv::COLOR_BGR2YUV_UYVY);
    const int pitch = out_w * 2;
    const bool sent = ingest.sendFrameToOutput(
        uyvy_scratch.data,
        static_cast<size_t>(uyvy_scratch.total() * uyvy_scratch.elemSize()),
        out_w, out_h, pitch);
    reportFailure(send_failing, !sent, "DeckLink output rejected a recording frame");
    return sent;
}

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Single + button: each click multiplies that axis sign by -1 (+1 <-> -1).
bool drawSignToggleRow(const char* label, int& sign)
{
    bool changed = false;
    ImGui::PushID("sign");
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%-4s", label);
    ImGui::SameLine();

    const bool positive = sign > 0;
    if (positive)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.25f, 1.0f));
    else
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.20f, 0.15f, 1.0f));
    if (ImGui::Button("+")) {
        sign = -sign;
        if (sign == 0)
            sign = +1;
        changed = true;
    }
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::Text("%+d", sign);
    ImGui::PopID();
    ImGui::PopID();
    return changed;
}

// Draw a video texture centered both horizontally and vertically in the window.
void drawCenteredVideo(GLuint texture, float video_w, float video_h,
                       GraphicOverlay& graphic,
                       GraphicOverlayOptions& options,
                       int& selected_graphic,
                       std::string& graphic_status)
{
    if (texture == 0 || video_w < 1.0f || video_h < 1.0f)
        return;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float scale = std::min(available.x / video_w, available.y / video_h);
    const ImVec2 size(video_w * scale, video_h * scale);
    const float pad_x = std::max(0.0f, (available.x - size.x) * 0.5f);
    const float pad_y = std::max(0.0f, (available.y - size.y) * 0.5f);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + pad_x,
                               ImGui::GetCursorPosY() + pad_y));
    ImGui::Image(reinterpret_cast<void*>(static_cast<intptr_t>(texture)), size);
    const ImVec2 image_min = ImGui::GetItemRectMin();
    const ImVec2 image_max = ImGui::GetItemRectMax();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    handleGraphicPlacementOnVideo(graphic, options, selected_graphic, graphic_status,
                                  image_min, image_max, mouse);
}
} // namespace

int main(int argc, char* argv[])
{
    initLogger();

    AppConfig app_config;
    const std::string config_path = AppConfig::resolvePath(argc, argv);
    std::string config_error;
    if (std::filesystem::exists(config_path)) {
        if (!app_config.load(config_path, config_error))
            getModuleLogger("app")->warn("{}", config_error);
        else
            getModuleLogger("app")->info("Loaded config {}", app_config.path);
    } else {
        // First run: write the defaults so operators have a file to edit.
        if (app_config.save(config_path, config_error)) {
            app_config.path = config_path;
            getModuleLogger("app")->info("Wrote default config {}", config_path);
        } else {
            getModuleLogger("app")->warn("{}", config_error);
        }
    }

    // Optional CLI overrides: trailing integer args are input/output devices.
    // --config PATH is consumed by resolvePath and skipped here.
    std::vector<int> device_args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--config") {
            ++i;
            continue;
        }
        if (arg.rfind("--config=", 0) == 0)
            continue;
        try {
            std::size_t idx = 0;
            const int value = std::stoi(arg, &idx);
            if (idx == arg.size())
                device_args.push_back(value);
        } catch (...) {
        }
    }
    if (device_args.size() >= 1)
        app_config.input_device = device_args[0];
    if (device_args.size() >= 2)
        app_config.output_device = device_args[1];

    const int input_device = app_config.input_device;
    const int output_device = app_config.output_device;

    if (!glfwInit())
        return EXIT_FAILURE;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(app_config.window_width, app_config.window_height,
                                          app_config.window_title.c_str(), nullptr, nullptr);
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
    UdpSender udp_sender;
    std::string udp_bind_ip = app_config.udp_bind_ip;
    std::string udp_raw_output = app_config.udp_raw_output_path;
    std::string udp_send_ip = app_config.udp_send_ip;
    int udp_port = app_config.udp_port;
    int udp_send_port = app_config.udp_send_port;
    udp_receiver.start(udp_bind_ip, udp_port, udp_raw_output);
    {
        std::string send_error;
        if (!udp_sender.configure(udp_send_ip, udp_send_port, send_error))
            getModuleLogger("app")->warn("UDP sender: {}", send_error);
    }

    struct UdpPlotSample {
        float pan = 0.0f;
        float tilt = 0.0f;
        float roll = 0.0f;
        float x_m = 0.0f;
        float y_m = 0.0f;
        float z_m = 0.0f;
    };
    std::deque<UdpPlotSample> udp_plot_history;
    struct DelayedUdpPacket {
        STypeState::CameraData camera;
        std::int64_t received_ms = 0;
    };
    std::deque<DelayedUdpPacket> udp_recv_delay_buf;
    constexpr std::size_t kUdpPlotCapacity = 600;
    std::uint64_t last_udp_packets_seen = 0;
    bool recording_udp_send_enabled = true;
    int udp_recv_delay_ms = app_config.udp_recv_delay_ms;
    STypeState::CameraData udp_delayed_camera;
    bool udp_delayed_valid = false;

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

    std::string config_status = app_config.path.empty()
        ? "No config file loaded"
        : ("Config  " + app_config.path);

    GraphicOverlay graphic;
    GraphicOverlayOptions graphic_options;
    graphic_options.data_dir = resolveDataDir();
    graphic.refreshFileList(graphic_options.data_dir);
    int selected_graphic = -1;
    std::string graphic_status = "Pick an image from " + graphic_options.data_dir;
    cv::Mat graphic_scratch;
    bool graphic_burn_failing = false;

    enum class SourceMode { Live = 0, Recording = 1 };
    SourceMode source_mode = SourceMode::Live;
    RecordingPlayback recording;
    std::vector<RecordingPair> recording_pairs =
        listRecordingPairs(app_config.recordings_dir);
    int selected_recording = -1;
    std::string recording_status =
        std::to_string(recording_pairs.size()) + " videos (.mp4/.mkv/…) with CSV in " +
        app_config.recordings_dir;
    getModuleLogger("app")->info("Recording scan: {} pairs in {}",
                                 recording_pairs.size(), app_config.recordings_dir);
    GLuint recording_texture = 0;
    glGenTextures(1, &recording_texture);
    glBindTexture(GL_TEXTURE_2D, recording_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    bool recording_display_dirty = false;
    cv::Mat recording_sdi_bgr;
    cv::Mat recording_resize_scratch;
    cv::Mat recording_uyvy_scratch;
    std::int64_t last_recording_sdi_ms = 0;
    bool recording_sdi_enabled = true;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const UdpReceiver::Status udp = udp_receiver.status();
        const std::int64_t now_ms = steadyNowMs();
        if (udp.packets_received > last_udp_packets_seen && udp.last_packet_valid) {
            last_udp_packets_seen = udp.packets_received;
            udp_recv_delay_buf.push_back(DelayedUdpPacket{udp.last_camera, now_ms});
        }
        // Release packets only after recv_delay_ms have elapsed since arrival.
        const std::int64_t delay_ms = std::max(0, udp_recv_delay_ms);
        while (!udp_recv_delay_buf.empty() &&
               now_ms - udp_recv_delay_buf.front().received_ms >= delay_ms) {
            udp_delayed_camera = udp_recv_delay_buf.front().camera;
            udp_recv_delay_buf.pop_front();
            udp_delayed_valid = true;
            udp_plot_history.push_back(UdpPlotSample{
                udp_delayed_camera.pan_deg, udp_delayed_camera.tilt_deg,
                udp_delayed_camera.roll_deg,
                udp_delayed_camera.x_mm * 0.001f,
                udp_delayed_camera.y_mm * 0.001f,
                udp_delayed_camera.z_mm * 0.001f});
            while (udp_plot_history.size() > kUdpPlotCapacity)
                udp_plot_history.pop_front();
        }

        if (source_mode == SourceMode::Recording) {
            const bool advanced = recording.tick();
            if (advanced)
                recording_display_dirty = true;

            // Internal UDP send only while recording is playing (CSV pose -> Stype HF).
            if (recording_udp_send_enabled && recording.playing && recording.hasRecord() && advanced) {
                const auto aligned = stype::applyAlignment(
                    recording.activeRecord(), recording.overlay_options.alignment);
                udp_sender.sendRecord(aligned);
            }

            if (recording_display_dirty && !recording.display().empty()) {
                uploadBgrTexture(recording_texture, recording.display());
                recording_display_dirty = false;
            }

            if (recording_sdi_enabled && recording.isOpen() && !recording.display().empty()) {
                if (!output_ready)
                    output_ready = ingest->initializeOutput(output_device);

                const IngestVideoInfo info = ingest->getVideoInfo();
                const int out_w = stable_width > 0 ? stable_width
                    : (info.width > 0 ? info.width : recording.width());
                const int out_h = stable_height > 0 ? stable_height
                    : (info.height > 0 ? info.height : recording.height());

                const std::int64_t now_ms = steadyNowMs();
                const std::int64_t interval_ms = recording.fps() > 1.0
                    ? static_cast<std::int64_t>(1000.0 / recording.fps())
                    : 40;
                if (output_ready && out_w > 0 && out_h > 0 &&
                    now_ms - last_recording_sdi_ms >= interval_ms) {
                    last_recording_sdi_ms = now_ms;
                    recording_sdi_bgr = recording.display().clone();
                    if (graphic_options.enabled && graphic_options.burn_into_sdi)
                        graphic.burnIntoBgr(recording_sdi_bgr, graphic_options);
                    sendBgrToSdi(*ingest, recording_sdi_bgr, out_w, out_h,
                                 recording_resize_scratch, recording_uyvy_scratch,
                                 output_send_failing);
                    status = "Recording SDI  " + recording.name() + "  frame " +
                             std::to_string(recording.frameIndex()) + "/" +
                             std::to_string(recording.frameCount()) +
                             "  out " + std::to_string(out_w) + "x" + std::to_string(out_h);
                }
            }
        } else {
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
                        const bool burned = burnGraphicIntoStagedFrame(
                            output_staging, frame->uyvy_frame->width, frame->uyvy_frame->height,
                            frame->uyvy_frame->pitch, graphic, graphic_options, graphic_scratch);
                        reportFailure(graphic_burn_failing, !burned,
                                      "Graphic burn-in failed; sending the clean frame");
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
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        constexpr float align_width = 320.0f;
        constexpr float controls_width = 390.0f;
        const float center_width =
            std::max(200.0f, viewport->WorkSize.x - align_width - controls_width);

        // Left: Apply Alignment — pan/tilt/roll/x/y/z signs for origin.
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(align_width, viewport->WorkSize.y));
        ImGui::Begin("Apply Alignment", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextWrapped(
            "Manipulate tracking over every frame so the world origin sits correctly.");
        ImGui::Separator();
        auto& align = recording.overlay_options.alignment;
        bool alignment_changed = false;

        ImGui::TextUnformatted("Axis sign");
        ImGui::TextDisabled("Click + to multiply that axis by -1");
        alignment_changed |= drawSignToggleRow("Pan", align.sign_pan);
        alignment_changed |= drawSignToggleRow("Tilt", align.sign_tilt);
        alignment_changed |= drawSignToggleRow("Roll", align.sign_roll);
        alignment_changed |= drawSignToggleRow("X", align.sign_x);
        alignment_changed |= drawSignToggleRow("Y", align.sign_y);
        alignment_changed |= drawSignToggleRow("Z", align.sign_z);

        if (ImGui::Button("Reset alignment")) {
            align.reset();
            alignment_changed = true;
        }
        if (alignment_changed) {
            recording.refreshOverlay();
            recording_display_dirty = true;
        }
        if (recording.hasRecord()) {
            const auto& raw = recording.activeRecord();
            const auto shown = stype::applyAlignment(raw, align);
            ImGui::Separator();
            ImGui::TextUnformatted("Aligned pose (this frame)");
            ImGui::Text("Pan  %+8.2f  (raw %+.2f x %+d)",
                        shown.pan_deg, raw.pan_deg, align.sign_pan);
            ImGui::Text("Tilt %+8.2f  (raw %+.2f x %+d)",
                        shown.tilt_deg, raw.tilt_deg, align.sign_tilt);
            ImGui::Text("Roll %+8.2f  (raw %+.2f x %+d)",
                        shown.roll_deg, raw.roll_deg, align.sign_roll);
            ImGui::Text("X %+9.1f mm (raw %+.1f x %+d)",
                        shown.x_mm, raw.x_mm, align.sign_x);
            ImGui::Text("Y %+9.1f mm (raw %+.1f x %+d)",
                        shown.y_mm, raw.y_mm, align.sign_y);
            ImGui::Text("Z %+9.1f mm (raw %+.1f x %+d)",
                        shown.z_mm, raw.z_mm, align.sign_z);
        }
        ImGui::End();

        // Center: video preview (horizontally between panels, vertically centered).
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + align_width, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(center_width, viewport->WorkSize.y));
        ImGui::Begin("Video Preview", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        if (source_mode == SourceMode::Recording) {
            if (recording.isOpen() && !recording.display().empty()) {
                drawCenteredVideo(recording_texture,
                                  static_cast<float>(recording.width()),
                                  static_cast<float>(recording.height()),
                                  graphic, graphic_options, selected_graphic, graphic_status);
            } else {
                ImGui::TextUnformatted("Select a recording to play...");
            }
        } else if (preview.rgbTexture()) {
            drawCenteredVideo(preview.rgbTexture(),
                              static_cast<float>(preview.width()),
                              static_cast<float>(preview.height()),
                              graphic, graphic_options, selected_graphic, graphic_status);
        } else {
            ImGui::TextUnformatted("Waiting for GPU FrameData...");
        }
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + align_width + center_width, viewport->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(controls_width, viewport->WorkSize.y));
        ImGui::Begin("Application Status", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::TextWrapped("%s", config_status.c_str());
        ImGui::Text("DeckLink in %d  out %d", input_device, output_device);

        ImGui::Separator();
        ImGui::TextUnformatted("Source");
        int source_i = static_cast<int>(source_mode);
        if (ImGui::RadioButton("Live DeckLink", &source_i, 0)) {
            source_mode = SourceMode::Live;
            recording.playing = false;
            recording_udp_send_enabled = false;
            status = "Live DeckLink";
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Recording", &source_i, 1)) {
            source_mode = SourceMode::Recording;
            recording_udp_send_enabled = true;
            status = "Recording playback";
        }
        source_mode = static_cast<SourceMode>(source_i);

        ImGui::Separator();
        ImGui::TextUnformatted("Recording playback");
        ImGui::TextWrapped("%s", recording_status.c_str());
        {
            char dir_buf[512];
            std::snprintf(dir_buf, sizeof(dir_buf), "%s", app_config.recordings_dir.c_str());
            if (ImGui::InputText("Recordings dir", dir_buf, sizeof(dir_buf)))
                app_config.recordings_dir = dir_buf;
        }
        if (ImGui::Button("Refresh recordings")) {
            recording_pairs = listRecordingPairs(app_config.recordings_dir);
            if (selected_recording >= static_cast<int>(recording_pairs.size()))
                selected_recording = -1;
            recording_status = std::to_string(recording_pairs.size()) +
                               " videos (.mp4/.mkv/…) with CSV in " +
                               app_config.recordings_dir;
            getModuleLogger("app")->info("Recording scan: {} pairs in {}",
                                         recording_pairs.size(), app_config.recordings_dir);
        }
        if (recording_pairs.empty()) {
            ImGui::TextWrapped(
                "No paired videos found. Looking for .mp4/.mkv/.mov/.avi with a matching "
                "*_stype.csv (or .csv) under:\n%s",
                app_config.recordings_dir.c_str());
        } else {
            const char* rec_label = selected_recording >= 0
                ? recording_pairs[static_cast<std::size_t>(selected_recording)].name.c_str()
                : "(select .mp4 / .mkv …)";
            if (ImGui::BeginCombo("Video", rec_label)) {
                for (int i = 0; i < static_cast<int>(recording_pairs.size()); ++i) {
                    const bool selected = (i == selected_recording);
                    if (ImGui::Selectable(recording_pairs[static_cast<std::size_t>(i)].name.c_str(),
                                          selected)) {
                        selected_recording = i;
                        std::string error;
                        if (recording.open(recording_pairs[static_cast<std::size_t>(i)], error)) {
                            source_mode = SourceMode::Recording;
                            recording_display_dirty = true;
                            recording_status = "Loaded " + recording.name() + "  (" +
                                               std::to_string(recording.frameCount()) + " frames, " +
                                               std::to_string(recording.records().size()) + " CSV rows)";
                            status = "Recording  " + recording.name();
                        } else {
                            recording_status = error;
                        }
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (recording.isOpen()) {
            ImGui::Checkbox("SDI out", &recording_sdi_enabled);
            if (source_mode == SourceMode::Recording)
                ImGui::Checkbox("UDP send CSV while playing", &recording_udp_send_enabled);
            if (ImGui::Button(recording.playing ? "Pause" : "Play"))
                recording.playing = !recording.playing;
            ImGui::SameLine();
            if (ImGui::Button("Prev") && recording.seek(recording.frameIndex() - 1))
                recording_display_dirty = true;
            ImGui::SameLine();
            if (ImGui::Button("Next") && recording.seek(recording.frameIndex() + 1))
                recording_display_dirty = true;
            ImGui::SameLine();
            if (ImGui::Button("Restart") && recording.seek(0)) {
                recording.playing = true;
                recording_display_dirty = true;
            }

            int scrub = recording.frameIndex();
            if (ImGui::SliderInt("Frame", &scrub, 0, std::max(0, recording.frameCount() - 1))) {
                if (recording.seek(scrub))
                    recording_display_dirty = true;
            }
            if (ImGui::SliderInt("CSV offset", &recording.csv_offset, -500, 500)) {
                recording.refreshOverlay();
                recording_display_dirty = true;
            }
            if (ImGui::Checkbox("Show world origin", &recording.show_world_origin)) {
                recording.refreshOverlay();
                recording_display_dirty = true;
            }
            float gizmo_mm = static_cast<float>(recording.overlay_options.gizmo_length_mm);
            if (ImGui::SliderFloat("Gizmo length (mm)", &gizmo_mm, 50.0f, 5000.0f, "%.0f")) {
                recording.overlay_options.gizmo_length_mm = gizmo_mm;
                recording.refreshOverlay();
                recording_display_dirty = true;
            }
            if (ImGui::Checkbox("Apply k1/k2 distortion",
                                &recording.overlay_options.apply_distortion)) {
                recording.refreshOverlay();
                recording_display_dirty = true;
            }
            ImGui::Text("%.1f fps   %dx%d", recording.fps(), recording.width(), recording.height());
            if (recording.hasRecord()) {
                const auto& r = recording.activeRecord();
                ImGui::Separator();
                ImGui::Text("CSV frame %lld  %s",
                            static_cast<long long>(r.frame_id),
                            r.stype_valid ? "VALID" : "INVALID");
                ImGui::Text("Pan %.2f  Tilt %.2f  Roll %.2f",
                            r.pan_deg, r.tilt_deg, r.roll_deg);
                ImGui::Text("X %.3f  Y %.3f  Z %.3f m",
                            r.x_mm / 1000.0, r.y_mm / 1000.0, r.z_mm / 1000.0);
                ImGui::Text("HFOV %.2f  zoom %lld  focus %lld",
                            r.hfov_deg,
                            static_cast<long long>(r.zoom_raw),
                            static_cast<long long>(r.focus_raw));
            }
        }

        if (ImGui::Button("Save config")) {
            const std::string save_path = app_config.path.empty() ? config_path : app_config.path;
            app_config.udp_bind_ip = udp_bind_ip;
            app_config.udp_port = udp_port;
            app_config.udp_send_ip = udp_send_ip;
            app_config.udp_send_port = udp_send_port;
            app_config.udp_recv_delay_ms = udp_recv_delay_ms;
            app_config.udp_raw_output_path = udp_raw_output;
            if (app_config.save(save_path, config_error)) {
                app_config.path = save_path;
                config_status = "Saved  " + save_path;
            } else {
                config_status = config_error;
            }
        }
        ImGui::Separator();
        ImGui::TextUnformatted("UDP receive");
        char bind_ip_buf[64];
        std::snprintf(bind_ip_buf, sizeof(bind_ip_buf), "%s", udp_bind_ip.c_str());
        if (ImGui::InputText("Bind IP", bind_ip_buf, sizeof(bind_ip_buf)))
            udp_bind_ip = bind_ip_buf;
        ImGui::InputInt("Recv port", &udp_port);
        if (udp_port < 1) udp_port = 1;
        if (udp_port > 65535) udp_port = 65535;
        if (source_mode == SourceMode::Recording) {
            char send_ip_buf[64];
            std::snprintf(send_ip_buf, sizeof(send_ip_buf), "%s", udp_send_ip.c_str());
            if (ImGui::InputText("Send IP", send_ip_buf, sizeof(send_ip_buf)))
                udp_send_ip = send_ip_buf;
            ImGui::InputInt("Send port", &udp_send_port);
            if (udp_send_port < 1) udp_send_port = 1;
            if (udp_send_port > 65535) udp_send_port = 65535;
        }
        if (ImGui::SliderInt("Recv delay (ms)", &udp_recv_delay_ms, 0, 2000)) {
            // Keep buffered packets; only the release threshold changes.
        }
        ImGui::TextDisabled("Hold each UDP packet this many ms before use/plots");
        if (ImGui::Button("Apply UDP")) {
            udp_receiver.start(udp_bind_ip, udp_port, udp_raw_output);
            if (source_mode == SourceMode::Recording) {
                std::string send_error;
                if (!udp_sender.configure(udp_send_ip, udp_send_port, send_error))
                    getModuleLogger("app")->warn("UDP sender: {}", send_error);
            }
            last_udp_packets_seen = 0;
            udp_plot_history.clear();
            udp_recv_delay_buf.clear();
            udp_delayed_valid = false;
        }

        const auto send_status = udp_sender.status();
        ImGui::Text("Listening  %s", udp.listening ? "yes" : "no");
        ImGui::Text("Recv packets  %llu", static_cast<unsigned long long>(udp.packets_received));
        if (source_mode == SourceMode::Recording) {
            ImGui::Text("Send packets  %llu  %s",
                        static_cast<unsigned long long>(send_status.packets_sent),
                        (recording.playing && recording_udp_send_enabled) ? "(active)" : "(idle)");
        }
        ImGui::Text("Delay buffer  %d  (%d ms)",
                    static_cast<int>(udp_recv_delay_buf.size()),
                    std::max(0, udp_recv_delay_ms));
        ImGui::Text("Last size  %d bytes", udp.last_packet_size);
        ImGui::TextWrapped("UDP text file  %s", udp.raw_output_path.c_str());
        if (!udp.last_error.empty())
            ImGui::TextWrapped("Recv error: %s", udp.last_error.c_str());
        if (source_mode == SourceMode::Recording && !send_status.last_error.empty())
            ImGui::TextWrapped("Send error: %s", send_status.last_error.c_str());
        if (udp_delayed_valid) {
            const auto& cam = udp_delayed_camera;
            ImGui::TextUnformatted("Delayed Stype HF pose (synced)");
            ImGui::Text("Cam %d   pkt %d   zoom %d   focus %d",
                        cam.camera_id, cam.packet_no, cam.zoom_raw, cam.focus_raw);
            ImGui::Text("pan %8.2f   tilt %7.2f   roll %6.2f deg",
                        cam.pan_deg, cam.tilt_deg, cam.roll_deg);
            ImGui::Text("X %7.1f   Y %6.1f   Z(depth) %7.1f m",
                        cam.x_mm * 0.001f, cam.y_mm * 0.001f, cam.z_mm * 0.001f);
            ImGui::Text("dist-to-origin %.1f m",
                        std::sqrt(cam.x_mm * cam.x_mm + cam.z_mm * cam.z_mm) * 0.001f);
            ImGui::Text("HFOV %.2f deg   AR %.3f   PA width %.2f mm",
                        cam.hfov_deg, cam.hf_ar, cam.hf_pa_width_mm);
            ImGui::Text("k1 %.6f   k2 %.6f", cam.hf_k1, cam.hf_k2);
            ImGui::Text("cx %.3f mm   cy %.3f mm", cam.hf_cx_mm, cam.hf_cy_mm);
            if (STypeState::hasValidHfFov(cam))
                ImGui::TextUnformatted("HF optics  valid");
            else
                ImGui::TextUnformatted("HF optics  incomplete");
        } else if (udp.last_packet_valid) {
            ImGui::TextWrapped("Waiting for recv delay (%d ms)...",
                               std::max(0, udp_recv_delay_ms));
        } else if (udp.packets_received > 0) {
            ImGui::TextUnformatted("Last packet was not a valid Stype HF (0x0F) frame.");
        }

        if (!udp_plot_history.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("UDP received plots");
            std::vector<float> pan, tilt, roll, x, y, z;
            pan.reserve(udp_plot_history.size());
            tilt.reserve(udp_plot_history.size());
            roll.reserve(udp_plot_history.size());
            x.reserve(udp_plot_history.size());
            y.reserve(udp_plot_history.size());
            z.reserve(udp_plot_history.size());
            for (const auto& s : udp_plot_history) {
                pan.push_back(s.pan);
                tilt.push_back(s.tilt);
                roll.push_back(s.roll);
                x.push_back(s.x_m);
                y.push_back(s.y_m);
                z.push_back(s.z_m);
            }
            const int n = static_cast<int>(pan.size());
            ImGui::PlotLines("Pan", pan.data(), n, 0, nullptr, -180.0f, 180.0f, ImVec2(-1, 50));
            ImGui::PlotLines("Tilt", tilt.data(), n, 0, nullptr, -180.0f, 180.0f, ImVec2(-1, 50));
            ImGui::PlotLines("Roll", roll.data(), n, 0, nullptr, -180.0f, 180.0f, ImVec2(-1, 50));
            ImGui::PlotLines("X (m)", x.data(), n, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 40));
            ImGui::PlotLines("Y (m)", y.data(), n, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 40));
            ImGui::PlotLines("Z (m)", z.data(), n, 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(-1, 40));
            if (ImGui::Button("Clear UDP plots"))
                udp_plot_history.clear();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Graphic overlay");
        ImGui::TextUnformatted("Works on Live and Recording preview");
        ImGui::Checkbox("Show graphics", &graphic_options.enabled);
        ImGui::Checkbox("Burn into SDI", &graphic_options.burn_into_sdi);
        ImGui::SliderFloat("Width fraction", &graphic_options.width_fraction, 0.05f, 1.0f, "%.2f");
        ImGui::Text("Next size  %.0f%% of frame width", graphic_options.width_fraction * 100.0f);
        if (ImGui::Button("Refresh data/")) {
            graphic.refreshFileList(graphic_options.data_dir);
            graphic_status = std::to_string(graphic.files().size()) + " images in " +
                             graphic_options.data_dir;
        }
        ImGui::TextWrapped("%s", graphic_status.c_str());
        ImGui::Text("Placed  %d", graphic.placedCount());

        if (graphic.files().empty()) {
            ImGui::TextWrapped("No images in %s", graphic_options.data_dir.c_str());
        } else {
            const char* preview_label = (selected_graphic >= 0 &&
                                         selected_graphic < static_cast<int>(graphic.files().size()))
                ? nullptr
                : "(select image)";
            std::string preview_name;
            if (preview_label == nullptr) {
                preview_name = std::filesystem::path(graphic.files()[selected_graphic]).filename().string();
                preview_label = preview_name.c_str();
            }
            if (ImGui::BeginCombo("Image", preview_label)) {
                for (int i = 0; i < static_cast<int>(graphic.files().size()); ++i) {
                    const std::string name =
                        std::filesystem::path(graphic.files()[i]).filename().string();
                    const bool selected = (i == selected_graphic);
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        selected_graphic = i;
                        std::string error;
                        if (graphic.selectFile(i, error)) {
                            graphic_status = "Follow mouse — click video to place  (" +
                                             std::to_string(graphic.pendingWidth()) + "x" +
                                             std::to_string(graphic.pendingHeight()) + ")";
                        } else {
                            graphic_status = error;
                        }
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (graphic.hasPending()) {
            ImGui::Text("Pending  %s  %dx%d", graphic.pendingName().c_str(),
                        graphic.pendingWidth(), graphic.pendingHeight());
            ImGui::TextUnformatted("Yellow outline: next placement follows mouse");
            if (ImGui::Button("Cancel pending")) {
                graphic.clearPending();
                selected_graphic = -1;
                graphic_status = "Pending cancelled";
            }
        }

        if (graphic.placedCount() > 0) {
            ImGui::TextUnformatted("Placed graphics");
            int remove_index = -1;
            for (int i = 0; i < graphic.placedCount(); ++i) {
                const auto& item = graphic.placed()[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::Text("%d  %s  (%.2f, %.2f)  %.0f%%",
                            i + 1, item.name.c_str(),
                            item.center_u, item.center_v,
                            item.width_fraction * 100.0f);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                    remove_index = i;
                ImGui::PopID();
            }
            if (remove_index >= 0) {
                graphic.removePlaced(remove_index);
                graphic_status = "Removed placement  (" +
                                 std::to_string(graphic.placedCount()) + " remaining)";
            }
            if (ImGui::Button("Clear all graphics")) {
                graphic.clearAll();
                selected_graphic = -1;
                graphic_status = "Cleared all";
            }
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
    recording.close();
    if (recording_texture != 0)
        glDeleteTextures(1, &recording_texture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}