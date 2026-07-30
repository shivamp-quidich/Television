find_package(Threads REQUIRED)
find_package(CUDAToolkit REQUIRED)

option(STIQY_ENABLE_DELTACAST "Enable Deltacast ingestion backend" OFF)

add_library(ingestion STATIC
    src/decklink_capture.cpp
    src/ingest.cpp
    ${DeckLinkAPI_DISPATCH_SOURCE}
)

if (STIQY_ENABLE_DELTACAST)
    enable_language(CUDA)
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(spdlog REQUIRED)
    find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui cudaimgproc cudawarping)
    find_package(VideoMasterHD REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)

    target_sources(ingestion PRIVATE
        src/deltacast_capture.cu
        src/Tools.cpp
        src/Keyboard.cpp
    )
    target_compile_definitions(ingestion PUBLIC STIQY_ENABLE_DELTACAST=1)

    set_target_properties(ingestion PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        CUDA_STANDARD 11
        CUDA_ARCHITECTURES "75;80;86;89"
        LINKER_LANGUAGE CUDA
        POSITION_INDEPENDENT_CODE ON
    )

    target_compile_options(ingestion PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-rdc=true>
    )

    message(STATUS "VideoMasterHD include dir: ${VideoMasterHD_INCLUDE_DIR}")

    target_include_directories(ingestion PUBLIC
        ${VideoMasterHD_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS}
    )

    target_link_libraries(ingestion PUBLIC
        opencv_cudaimgproc
        opencv_cudawarping
        VideoMasterHD::Core
        CUDA::cuda_driver
        ${OPENGL_LIBRARIES}
        GLEW::GLEW
        spdlog::spdlog
    )

    # Some CMake/CUDA combinations don't provide the imported target CUDA::cudadevrt.
    # Link it if available, otherwise locate the static library directly.
    if (TARGET CUDA::cudadevrt)
        target_link_libraries(ingestion PUBLIC CUDA::cudadevrt)
    else()
        find_library(CUDADEVRT_LIBRARY
            NAMES cudadevrt libcudadevrt.a
            HINTS
                ${CUDAToolkit_LIBRARY_DIR}
                ${CUDAToolkit_LIBRARY_ROOT}
                ${CUDAToolkit_LIBRARY_ROOT}/lib64
                ${CUDAToolkit_LIBRARY_ROOT}/targets/x86_64-linux/lib
        )
        if (CUDADEVRT_LIBRARY)
            target_link_libraries(ingestion PUBLIC ${CUDADEVRT_LIBRARY})
        else()
            message(WARNING "Could not find cudadevrt (libcudadevrt.a). If you hit device-link errors, ensure the full CUDA toolkit is installed in the container.")
        endif()
    endif()

    option(BUILD_DELTACAST_TEST "Build deltacast test executable" ON)
    if (BUILD_DELTACAST_TEST)
        add_executable(deltacast_test
            src/main.cpp
        )

        set_target_properties(deltacast_test PROPERTIES
            CUDA_ARCHITECTURES "75;80;86;89"
        )

        target_link_libraries(deltacast_test
            ingestion
        )
    endif()
endif()

target_include_directories(ingestion PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/common/logger/include
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(ingestion PUBLIC
    logger
    DeckLink::API
    ${OpenCV_LIBS}
    ${CMAKE_DL_LIBS}
    Threads::Threads
    CUDA::cudart
    OpenGL::GL
    GLEW::GLEW
)

option(BUILD_DECKLINK_PASSTHROUGH "Build the DeckLink preview and SDI pass-through demo" ON)

if (BUILD_DECKLINK_PASSTHROUGH)
    add_executable(decklink_passthrough
        src/main.cpp
        src/gpu_uyvy_preview.cpp
        src/udp_receiver.cpp
        src/udp_sender.cpp
        src/app_config.cpp
        src/graphic_overlay.cpp
        src/stype_csv_overlay.cpp
        src/recording_playback.cpp
    )
    target_link_libraries(decklink_passthrough PRIVATE
        ingestion
        imgui_glfw_opengl3
        imgui_core
        glfw
        OpenGL::GL
        GLEW::GLEW
    )
endif()
