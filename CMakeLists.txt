cmake_minimum_required(VERSION 3.18)

project(Televison LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(CUDAToolkit REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui cudaimgproc)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)
find_package(glfw3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(DeckLinkAPI REQUIRED)

set(IMGUI_PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-package/usr")
set(IMGUI_INCLUDE_DIR "${IMGUI_PACKAGE_ROOT}/include/imgui")
set(IMGUI_LIBRARY "${IMGUI_PACKAGE_ROOT}/lib/x86_64-linux-gnu/libimgui.a")
set(IMGUI_BACKENDS_DIR "${IMGUI_PACKAGE_ROOT}/share/doc/libimgui-dev/examples/backends")
set(STB_PACKAGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb-package/usr")
set(STB_LIBRARY "${STB_PACKAGE_ROOT}/lib/x86_64-linux-gnu/libstb.a")

if (NOT EXISTS "${IMGUI_LIBRARY}" OR NOT EXISTS "${IMGUI_BACKENDS_DIR}/imgui_impl_glfw.cpp" OR
    NOT EXISTS "${STB_LIBRARY}")
    message(FATAL_ERROR "Project-local ImGui or stb package is incomplete")
endif()

add_library(stb_core STATIC IMPORTED GLOBAL)
set_target_properties(stb_core PROPERTIES IMPORTED_LOCATION "${STB_LIBRARY}")

add_library(imgui_core STATIC IMPORTED GLOBAL)
set_target_properties(imgui_core PROPERTIES
    IMPORTED_LOCATION "${IMGUI_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${IMGUI_INCLUDE_DIR}"
)
target_link_libraries(imgui_core INTERFACE stb_core)

add_library(imgui_glfw_opengl3 STATIC
    "${IMGUI_BACKENDS_DIR}/imgui_impl_glfw.cpp"
    "${IMGUI_BACKENDS_DIR}/imgui_impl_opengl3.cpp"
)
target_include_directories(imgui_glfw_opengl3 PUBLIC
    "${IMGUI_INCLUDE_DIR}"
    "${IMGUI_INCLUDE_DIR}/backends"
)
target_compile_definitions(imgui_glfw_opengl3 PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)
target_link_libraries(imgui_glfw_opengl3 PUBLIC
    imgui_core
    glfw
    OpenGL::GL
    GLEW::GLEW
)

message(STATUS "DeckLink SDK headers: ${DeckLinkAPI_INCLUDE_DIR}")
message(STATUS "DeckLink runtime library: ${DeckLinkAPI_LIBRARY}")

add_subdirectory(common/logger)
add_subdirectory(common/videos/ingestion)
