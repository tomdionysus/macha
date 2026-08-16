cmake_minimum_required(VERSION 3.20)
project(macha VERSION 0.8.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/macha_version.hpp @ONLY)

option(MACHA_ENABLE_FUSE "Build the FUSE mount adapter when available" ON)
option(MACHA_BUILD_TESTS "Build tests" ON)
option(MACHA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(PkgConfig REQUIRED)

# Homebrew versioned FFmpeg formulae are keg-only, so their .pc files are not
# necessarily visible to pkg-config. Discover a suitable Homebrew prefix on
# macOS before asking FindPkgConfig; ordinary system/pkg-config discovery is
# unchanged on other platforms.
if(APPLE)
    find_program(MACHA_BREW_EXECUTABLE brew HINTS /usr/local/bin /opt/homebrew/bin)
    if(MACHA_BREW_EXECUTABLE)
        foreach(MACHA_FFMPEG_FORMULA ffmpeg@7 ffmpeg)
            execute_process(
                COMMAND ${MACHA_BREW_EXECUTABLE} --prefix ${MACHA_FFMPEG_FORMULA}
                RESULT_VARIABLE MACHA_BREW_RESULT
                OUTPUT_VARIABLE MACHA_FFMPEG_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(MACHA_BREW_RESULT EQUAL 0 AND
               EXISTS "${MACHA_FFMPEG_PREFIX}/lib/pkgconfig/libavformat.pc")
                list(PREPEND CMAKE_PREFIX_PATH "${MACHA_FFMPEG_PREFIX}")
                set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH TRUE)
                message(STATUS "Using Homebrew FFmpeg libraries from ${MACHA_FFMPEG_PREFIX}")
                break()
            endif()
        endforeach()
    endif()
endif()

# The in-process media engine uses public APIs present in FFmpeg 6 and later.
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavformat>=60 libavcodec>=60 libavutil>=58 libswscale>=7 libswresample>=4)

if(TARGET yaml-cpp::yaml-cpp)
    set(MACHA_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(MACHA_YAML_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "yaml-cpp package found but no CMake target is available")
endif()

set(MACHA_CORE_SOURCES
    src/types.cpp
    src/codec.cpp
    src/crypto.cpp
    src/config.cpp
    src/log.cpp
    src/diagnostics.cpp
    src/placement.cpp
    src/membership.cpp
    src/local_store.cpp
    src/storage_pool.cpp
    src/persistent_cache.cpp
    src/metadata.cpp
    src/net.cpp
    src/cluster.cpp
    src/distributed_store.cpp
    src/replica_selector.cpp
    src/metadata_manager.cpp
    src/json.cpp
    src/http.cpp
    src/media_segments.cpp
    src/media_timestamps.cpp
    src/media_vod.cpp
    src/media_engine.cpp
    src/playback.cpp
    src/catalogue.cpp
    src/catalogue_api.cpp
    src/media_catalogue.cpp
    src/hydration.cpp
    src/filesystem.cpp
    src/service.cpp
)

add_library(macha_core STATIC ${MACHA_CORE_SOURCES})
target_include_directories(macha_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(macha_core PUBLIC
    OpenSSL::Crypto Threads::Threads CURL::libcurl ${MACHA_YAML_TARGET} PkgConfig::LIBAV)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(macha_core PRIVATE -Wall -Wextra -Wpedantic)
    if(MACHA_WARNINGS_AS_ERRORS)
        target_compile_options(macha_core PRIVATE -Werror)
    endif()
endif()

add_executable(macha src/main.cpp)
target_link_libraries(macha PRIVATE macha_core)

set(MACHA_FUSE_FOUND OFF)
if(MACHA_ENABLE_FUSE)
    pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
    if(TARGET PkgConfig::FUSE3)
        set(MACHA_FUSE_FOUND ON)
        target_sources(macha PRIVATE src/fuse_adapter.cpp)
        target_link_libraries(macha PRIVATE PkgConfig::FUSE3)
    endif()

    if(APPLE AND NOT MACHA_FUSE_FOUND)
        find_path(FUSE3_INCLUDE_DIR fuse.h
            HINTS
                /usr/local/include/fuse3
                /opt/homebrew/include/fuse3
                /Library/Filesystems/macfuse.fs/Contents/Resources/include/fuse3
        )
        find_library(FUSE3_LIBRARY
            NAMES fuse3 fuse
            HINTS
                /usr/local/lib
                /opt/homebrew/lib
                /Library/Filesystems/macfuse.fs/Contents/Resources/lib
        )
        if(FUSE3_INCLUDE_DIR AND FUSE3_LIBRARY)
            set(MACHA_FUSE_FOUND ON)
            target_sources(macha PRIVATE src/fuse_adapter.cpp)
            target_include_directories(macha PRIVATE ${FUSE3_INCLUDE_DIR})
            target_link_libraries(macha PRIVATE ${FUSE3_LIBRARY})
        endif()
    endif()
endif()

if(NOT MACHA_FUSE_FOUND)
    target_sources(macha PRIVATE src/fuse_stub.cpp)
    if(MACHA_ENABLE_FUSE)
        message(STATUS "FUSE3/macFUSE development files not found; building service without mount support")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(macha PRIVATE -Wall -Wextra -Wpedantic)
    if(MACHA_WARNINGS_AS_ERRORS)
        target_compile_options(macha PRIVATE -Werror)
    endif()
endif()

if(MACHA_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_executable(macha-tests tests/test_main.cpp)
    target_link_libraries(macha-tests PRIVATE macha_core)
    target_compile_definitions(macha-tests PRIVATE MACHA_HAVE_YAML_CPP=1)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(macha-tests PRIVATE -Wall -Wextra -Wpedantic)
        if(MACHA_WARNINGS_AS_ERRORS)
            target_compile_options(macha-tests PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME macha-tests COMMAND macha-tests)
    set_tests_properties(macha-tests PROPERTIES TIMEOUT 60)
endif()

include(GNUInstallDirs)
install(TARGETS macha RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES systemd/macha.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system OPTIONAL)
install(FILES README.md ARCHITECTURE.md SECURITY.md CHANGELOG.md CONTRIBUTING.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES systemd/macha.conf.example macha.yaml.example
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples OPTIONAL)
if(APPLE)
    install(FILES macos/macha.plist.example
            DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples OPTIONAL)
endif()
