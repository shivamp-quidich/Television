# Locates the Blackmagic DeckLink SDK.
#
# The SDK's DeckLinkAPIDispatch.cpp dlopen()s libDeckLinkAPI.so by soname at run
# time, so the library is not a link input — only libdl is. libDeckLinkAPI.so is
# still located here so a missing Desktop Video driver is reported at configure
# time, and so a driver installed outside the loader's default directories can be
# reached via RPATH.
#
# Input variables (cache or environment):
#   DECKLINK_SDK_DIR      SDK root containing include/DeckLinkAPI.h.
#                         Defaults to third_party/Decklink-SDK in this project.
#   DECKLINK_LIBRARY_DIR  Directory containing libDeckLinkAPI.so.
#
# Result:
#   DeckLink::API                  interface target (headers + dl + RPATH)
#   DeckLinkAPI_DISPATCH_SOURCE    path to DeckLinkAPIDispatch.cpp, which every
#                                  consumer must compile into exactly one target
#   DeckLinkAPI_LIBRARY            resolved libDeckLinkAPI.so, or NOTFOUND

find_path(DeckLinkAPI_INCLUDE_DIR
    NAMES DeckLinkAPI.h
    HINTS
        ${DECKLINK_SDK_DIR}
        ENV DECKLINK_SDK_DIR
        ${CMAKE_CURRENT_LIST_DIR}/../third_party/Decklink-SDK
    PATH_SUFFIXES include Linux/include
)

find_file(DeckLinkAPI_DISPATCH_SOURCE
    NAMES DeckLinkAPIDispatch.cpp
    HINTS ${DeckLinkAPI_INCLUDE_DIR}
    NO_DEFAULT_PATH
)

find_library(DeckLinkAPI_LIBRARY
    NAMES DeckLinkAPI
    HINTS
        ${DECKLINK_LIBRARY_DIR}
        ENV DECKLINK_LIBRARY_DIR
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeckLinkAPI
    REQUIRED_VARS DeckLinkAPI_INCLUDE_DIR DeckLinkAPI_DISPATCH_SOURCE
)

if (DeckLinkAPI_FOUND AND NOT TARGET DeckLink::API)
    add_library(DeckLink::API INTERFACE IMPORTED)
    target_include_directories(DeckLink::API INTERFACE ${DeckLinkAPI_INCLUDE_DIR})
    target_link_libraries(DeckLink::API INTERFACE ${CMAKE_DL_LIBS})

    if (DeckLinkAPI_LIBRARY)
        get_filename_component(_decklink_lib_dir "${DeckLinkAPI_LIBRARY}" DIRECTORY)
        # Only non-default loader directories need an RPATH entry.
        if (NOT _decklink_lib_dir IN_LIST CMAKE_C_IMPLICIT_LINK_DIRECTORIES AND
            NOT _decklink_lib_dir IN_LIST CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES)
            target_link_options(DeckLink::API INTERFACE "LINKER:-rpath,${_decklink_lib_dir}")
        endif()
        unset(_decklink_lib_dir)
    else()
        message(WARNING
            "libDeckLinkAPI.so was not found. The build will succeed, but capture "
            "and playback fail at run time until the Blackmagic Desktop Video "
            "driver is installed, or DECKLINK_LIBRARY_DIR points at the library.")
    endif()
endif()

mark_as_advanced(DeckLinkAPI_INCLUDE_DIR DeckLinkAPI_DISPATCH_SOURCE DeckLinkAPI_LIBRARY)
