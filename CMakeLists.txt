cmake_minimum_required(VERSION 3.20)

# CMake 3.30 removed the legacy FindBoost module. libtorrent 2.1's exported
# package uses find_dependency(Boost); select config-mode Boost discovery on
# newer CMake while remaining compatible with older Raspberry Pi OS CMake.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(macha VERSION 0.21.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/macha_version.hpp @ONLY)

option(MACHA_BUILD_SERVER "Build the macha server executable" ON)
option(MACHA_ENABLE_FUSE "Build the FUSE mount adapter when available" ON)
option(MACHA_BUILD_TESTS "Build tests" ON)
option(MACHA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(MACHA_TEST_COVERAGE "Instrument the backend test build for source/branch coverage" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(MACHA_INSTALL_SYSTEMD_DEFAULT ON)
else()
    set(MACHA_INSTALL_SYSTEMD_DEFAULT OFF)
endif()
option(MACHA_INSTALL_SYSTEMD "Install the systemd service unit" ${MACHA_INSTALL_SYSTEMD_DEFAULT})

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(yaml-cpp QUIET)

# Some environments expose a static CURL::libcurl without its transitive link
# dependencies. Prefer pkg-config's complete imported target when available.
pkg_check_modules(LIBCURL QUIET IMPORTED_TARGET libcurl)
if(TARGET PkgConfig::LIBCURL)
    set(MACHA_CURL_TARGET PkgConfig::LIBCURL)
else()
    set(MACHA_CURL_TARGET CURL::libcurl)
endif()

# Homebrew versioned FFmpeg is keg-only and libtorrent's native CMake package
# lives beneath its formula prefix. Discover both prefixes up front on macOS so
# ordinary find_package/pkg-config calls work on Intel and Apple Silicon. Other
# platforms retain their normal system discovery paths.
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

        execute_process(
            COMMAND ${MACHA_BREW_EXECUTABLE} --prefix libtorrent-rasterbar
            RESULT_VARIABLE MACHA_BREW_LIBTORRENT_RESULT
            OUTPUT_VARIABLE MACHA_LIBTORRENT_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(MACHA_BREW_LIBTORRENT_RESULT EQUAL 0)
            list(PREPEND CMAKE_PREFIX_PATH "${MACHA_LIBTORRENT_PREFIX}")
            set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH TRUE)
            message(STATUS "Using Homebrew libtorrent from ${MACHA_LIBTORRENT_PREFIX}")
        endif()

        execute_process(
            COMMAND ${MACHA_BREW_EXECUTABLE} --prefix miniupnpc
            RESULT_VARIABLE MACHA_BREW_MINIUPNPC_RESULT
            OUTPUT_VARIABLE MACHA_MINIUPNPC_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(MACHA_BREW_MINIUPNPC_RESULT EQUAL 0)
            list(PREPEND CMAKE_PREFIX_PATH "${MACHA_MINIUPNPC_PREFIX}")
            set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH TRUE)
            message(STATUS "Using Homebrew miniupnpc from ${MACHA_MINIUPNPC_PREFIX}")
        endif()
    endif()
endif()

# The in-process media engine uses public APIs present in FFmpeg 6 and later.
pkg_check_modules(LIBAV QUIET IMPORTED_TARGET
    libavformat>=60 libavcodec>=60 libavutil>=58 libswscale>=7 libswresample>=4)

# UPnP IGD discovery/port mapping is optional at build time. Prefer the
# library's native CMake package when available (including Homebrew), with
# pkg-config as a fallback for distributions that only ship miniupnpc.pc.
set(MACHA_MINIUPNPC_TARGET "")
find_package(miniupnpc CONFIG QUIET)
if(TARGET miniupnpc::miniupnpc)
    set(MACHA_MINIUPNPC_TARGET miniupnpc::miniupnpc)
else()
    pkg_check_modules(MINIUPNPC QUIET IMPORTED_TARGET miniupnpc)
    if(TARGET PkgConfig::MINIUPNPC)
        set(MACHA_MINIUPNPC_TARGET PkgConfig::MINIUPNPC)
    endif()
endif()

# BitTorrent acquisition is optional at build time. Prefer libtorrent's native
# CMake package: it carries the exact compile definitions and transitive Boost,
# OpenSSL and platform-framework requirements used to build the installed
# library. pkg-config remains the fallback for distributions that only ship a
# .pc file.
set(MACHA_LIBTORRENT_TARGET "")
find_package(LibtorrentRasterbar CONFIG QUIET)
if(TARGET LibtorrentRasterbar::torrent-rasterbar)
    if(DEFINED LibtorrentRasterbar_VERSION AND LibtorrentRasterbar_VERSION VERSION_LESS "2.0")
        message(STATUS "Ignoring libtorrent-rasterbar ${LibtorrentRasterbar_VERSION}; Macha requires >= 2.0")
    else()
        set(MACHA_LIBTORRENT_TARGET LibtorrentRasterbar::torrent-rasterbar)
    endif()
endif()
if(NOT MACHA_LIBTORRENT_TARGET)
    pkg_check_modules(LIBTORRENT QUIET IMPORTED_TARGET libtorrent-rasterbar>=2.0)
    if(TARGET PkgConfig::LIBTORRENT)
        set(MACHA_LIBTORRENT_TARGET PkgConfig::LIBTORRENT)
    endif()
endif()

set(MACHA_YAML_TARGET "")
if(TARGET yaml-cpp::yaml-cpp)
    set(MACHA_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(MACHA_YAML_TARGET yaml-cpp)
endif()

if(MACHA_BUILD_SERVER AND NOT MACHA_YAML_TARGET)
    message(FATAL_ERROR "yaml-cpp is required to build the macha server (set MACHA_BUILD_SERVER=OFF for dependency-light tests)")
endif()
if(MACHA_BUILD_SERVER AND NOT TARGET PkgConfig::LIBAV)
    message(FATAL_ERROR "FFmpeg development libraries are required to build the macha server (set MACHA_BUILD_SERVER=OFF for dependency-light tests)")
endif()

set(MACHA_CORE_SOURCES
    src/types.cpp
    src/codec.cpp
    src/crypto.cpp
    src/config_base.cpp
    src/log.cpp
    src/ffmpeg_log_base.cpp
    src/diagnostics.cpp
    src/telemetry.cpp
    src/status_api.cpp
    src/public_connectivity.cpp
    src/durable_file.cpp
    src/durability_domain.cpp
    src/placement.cpp
    src/membership.cpp
    src/local_store.cpp
    src/storage_pool.cpp
    src/retention.cpp
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
    src/subtitle_text.cpp
    src/media_engine_common.cpp
    src/playback.cpp
    src/catalogue.cpp
    src/catalogue_hints.cpp
    src/catalogue_api.cpp
    src/manage_api.cpp
    src/acquisition_api.cpp
    src/media_catalogue.cpp
    src/ingest.cpp
    src/torrent.cpp
    src/macos_unicode.cpp
    src/hydration.cpp
    src/filesystem.cpp
    src/fuse_journal.cpp
    src/fuse_frontend.cpp
    src/fuse_mountpoint.cpp
    src/service.cpp
)

add_library(macha_core STATIC ${MACHA_CORE_SOURCES})
target_include_directories(macha_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(macha_core PUBLIC
    OpenSSL::Crypto Threads::Threads ${MACHA_CURL_TARGET})
if(MACHA_MINIUPNPC_TARGET)
    target_link_libraries(macha_core PUBLIC ${MACHA_MINIUPNPC_TARGET})
    target_compile_definitions(macha_core PUBLIC MACHA_HAVE_MINIUPNPC=1)
    message(STATUS "UPnP port mapping enabled (${MACHA_MINIUPNPC_TARGET})")
else()
    message(STATUS "miniupnpc not found; UPnP port mapping unavailable at runtime")
endif()
if(MACHA_LIBTORRENT_TARGET)
    target_link_libraries(macha_core PUBLIC ${MACHA_LIBTORRENT_TARGET})
    target_compile_definitions(macha_core PUBLIC MACHA_HAVE_LIBTORRENT=1)
    message(STATUS "BitTorrent acquisition enabled (${MACHA_LIBTORRENT_TARGET})")
else()
    message(STATUS "libtorrent-rasterbar >= 2.0 not found; building generic ingest without BitTorrent acquisition")
endif()
if(APPLE)
    find_library(MACHA_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(macha_core PUBLIC ${MACHA_COREFOUNDATION_FRAMEWORK})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(macha_core PRIVATE -Wall -Wextra -Wpedantic)
    if(MACHA_WARNINGS_AS_ERRORS)
        target_compile_options(macha_core PRIVATE -Werror)
    endif()
endif()

if(MACHA_BUILD_SERVER)
    add_executable(macha
        src/main.cpp
        src/config.cpp
        src/ffmpeg_log.cpp
        src/media_engine.cpp
        src/media_metadata_ffmpeg.cpp
        src/service_config.cpp)
    target_link_libraries(macha PRIVATE macha_core ${MACHA_YAML_TARGET} PkgConfig::LIBAV)

    set(MACHA_FUSE_FOUND OFF)
    if(MACHA_ENABLE_FUSE)
        pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
if(TARGET PkgConfig::FUSE3)
    set(MACHA_FUSE_FOUND ON)
    target_sources(macha PRIVATE src/fuse_adapter.cpp)
    # On macOS, /usr/local/include is searched by Clang before -isystem
    # directories.  If both legacy FUSE 2 headers and FUSE 3 are installed,
    # PkgConfig::FUSE3's system include therefore lets <fuse.h> resolve to
    # FUSE 2 while <fuse_lowlevel.h> resolves to FUSE 3.  Put the explicit
    # FUSE 3 directory first as a normal include path so a clean configure is
    # deterministic (an older CMake cache happened to conceal this).
    if(APPLE)
        target_include_directories(macha BEFORE PRIVATE ${FUSE3_INCLUDE_DIRS})
        target_link_directories(macha PRIVATE ${FUSE3_LIBRARY_DIRS})
        target_link_libraries(macha PRIVATE ${FUSE3_LIBRARIES})
        target_compile_options(macha PRIVATE ${FUSE3_CFLAGS_OTHER})
        target_link_options(macha PRIVATE ${FUSE3_LDFLAGS_OTHER})
    else()
        target_link_libraries(macha PRIVATE PkgConfig::FUSE3)
    endif()
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

endif()

if(MACHA_BUILD_TESTS)
    include(CTest)
    enable_testing()

    # The portable suite owns all dependency-free behavioural tests. Concrete
    # yaml-cpp/libav adapter tests are registered separately below only when
    # those development dependencies are available. Both use the same runner.
    add_executable(macha-tests
        tests/main.cpp
        tests/test_framework.cpp
        src/media_engine_stub.cpp
        src/media_metadata_stub.cpp
        tests/test_foundations.cpp
        tests/test_storage_metadata.cpp
        tests/test_storage_v18.cpp
        tests/test_rpc_cluster.cpp
        tests/test_filesystem_fuse.cpp
        tests/test_hydration_catalogue.cpp
        tests/test_media_playback.cpp
        tests/test_invariants.cpp
        tests/test_models.cpp)
    target_link_libraries(macha-tests PRIVATE macha_core)
    target_compile_definitions(macha-tests PRIVATE
        MACHA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(macha-tests PRIVATE -Wall -Wextra -Wpedantic)
        if(MACHA_WARNINGS_AS_ERRORS)
            target_compile_options(macha-tests PRIVATE -Werror)
        endif()
    endif()
    if(MACHA_TEST_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(macha_core PRIVATE --coverage)
            target_compile_options(macha-tests PRIVATE --coverage)
            target_link_options(macha-tests PRIVATE --coverage)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            target_compile_options(macha_core PRIVATE -fprofile-instr-generate -fcoverage-mapping)
            target_compile_options(macha-tests PRIVATE -fprofile-instr-generate -fcoverage-mapping)
            target_link_options(macha-tests PRIVATE -fprofile-instr-generate)
        else()
            message(FATAL_ERROR "MACHA_TEST_COVERAGE requires GCC, Clang or AppleClang")
        endif()
    endif()

    add_test(NAME macha-tests COMMAND macha-tests)
    # Individual cases have stricter child-process deadlines. This is only a
    # suite-level deadman for CI infrastructure failures.
    set_tests_properties(macha-tests PROPERTIES TIMEOUT 300)

    # Runtime-adapter tests are intentionally separate. A development machine
    # without yaml-cpp/FFmpeg can still compile and execute the large portable
    # suite above; when both dependencies exist this executable exercises the
    # concrete configuration/media adapters as well.
    if(MACHA_YAML_TARGET AND TARGET PkgConfig::LIBAV)
        add_executable(macha-tests-runtime
            tests/main.cpp
            tests/test_framework.cpp
            tests/test_runtime_dependencies.cpp
            src/config.cpp
            src/ffmpeg_log.cpp
            src/media_engine.cpp
            src/media_metadata_ffmpeg.cpp)
        target_link_libraries(macha-tests-runtime PRIVATE
            macha_core ${MACHA_YAML_TARGET} PkgConfig::LIBAV)
        target_compile_definitions(macha-tests-runtime PRIVATE
            MACHA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(macha-tests-runtime PRIVATE -Wall -Wextra -Wpedantic)
            if(MACHA_WARNINGS_AS_ERRORS)
                target_compile_options(macha-tests-runtime PRIVATE -Werror)
            endif()
        endif()
        if(MACHA_TEST_COVERAGE)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(macha-tests-runtime PRIVATE --coverage)
                target_link_options(macha-tests-runtime PRIVATE --coverage)
            elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
                target_compile_options(macha-tests-runtime PRIVATE -fprofile-instr-generate -fcoverage-mapping)
                target_link_options(macha-tests-runtime PRIVATE -fprofile-instr-generate)
            endif()
        endif()
        add_test(NAME macha-tests-runtime COMMAND macha-tests-runtime)
        set_tests_properties(macha-tests-runtime PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "yaml-cpp and/or FFmpeg development files not found; runtime-adapter tests will not be built")
    endif()
endif()

include(GNUInstallDirs)

if(MACHA_INSTALL_SYSTEMD)
    set(MACHA_CONFIG_INSTALL_DIR_DEFAULT "/etc/macha")
else()
    set(MACHA_CONFIG_INSTALL_DIR_DEFAULT "${CMAKE_INSTALL_SYSCONFDIR}/macha")
endif()
set(MACHA_CONFIG_INSTALL_DIR "${MACHA_CONFIG_INSTALL_DIR_DEFAULT}" CACHE STRING
    "Directory containing the installed Macha configuration")
set(MACHA_SYSTEMD_UNIT_INSTALL_DIR_DEFAULT "lib/systemd/system")
set(MACHA_SYSTEMD_UNIT_INSTALL_DIR_LEGACY "${CMAKE_INSTALL_LIBDIR}/systemd/system")
if(DEFINED MACHA_SYSTEMD_UNIT_INSTALL_DIR
   AND MACHA_SYSTEMD_UNIT_INSTALL_DIR STREQUAL MACHA_SYSTEMD_UNIT_INSTALL_DIR_LEGACY
   AND NOT MACHA_SYSTEMD_UNIT_INSTALL_DIR STREQUAL MACHA_SYSTEMD_UNIT_INSTALL_DIR_DEFAULT)
    # Before 0.21.0 this default incorrectly inherited Debian's multiarch
    # CMAKE_INSTALL_LIBDIR (for example lib/aarch64-linux-gnu), a directory
    # systemd does not search.  Migrate that exact cached default but retain
    # explicit operator overrides.
    set(MACHA_SYSTEMD_UNIT_INSTALL_DIR "${MACHA_SYSTEMD_UNIT_INSTALL_DIR_DEFAULT}" CACHE STRING
        "Directory containing installed systemd system units" FORCE)
else()
    set(MACHA_SYSTEMD_UNIT_INSTALL_DIR "${MACHA_SYSTEMD_UNIT_INSTALL_DIR_DEFAULT}" CACHE STRING
        "Directory containing installed systemd system units")
endif()

function(macha_full_install_path output path)
    if(IS_ABSOLUTE "${path}")
        set(${output} "${path}" PARENT_SCOPE)
    else()
        set(${output} "${CMAKE_INSTALL_PREFIX}/${path}" PARENT_SCOPE)
    endif()
endfunction()

macha_full_install_path(MACHA_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
macha_full_install_path(MACHA_CONFIG_DIR "${MACHA_CONFIG_INSTALL_DIR}")
macha_full_install_path(MACHA_SYSTEMD_UNIT_DIR "${MACHA_SYSTEMD_UNIT_INSTALL_DIR}")
set(MACHA_CONFIG_FILE "${MACHA_CONFIG_DIR}/macha.yaml")
set(MACHA_CONFIG_EXAMPLE "${MACHA_CONFIG_DIR}/macha.yaml.example")

if(TARGET macha)
    install(TARGETS macha RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MACHA_INSTALL_SYSTEMD)
    if(NOT UNIX)
        message(FATAL_ERROR "MACHA_INSTALL_SYSTEMD requires a Unix-like target")
    endif()
    configure_file(systemd/macha.service
                   ${CMAKE_CURRENT_BINARY_DIR}/generated/systemd/macha.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/systemd/macha.service
            DESTINATION ${MACHA_SYSTEMD_UNIT_INSTALL_DIR})
endif()

install(FILES macha.yaml.example
        DESTINATION ${MACHA_CONFIG_INSTALL_DIR})
configure_file(cmake/install_config.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/install_config.cmake @ONLY)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/generated/install_config.cmake)

install(FILES README.md ARCHITECTURE.md SECURITY.md CHANGELOG.md CONTRIBUTING.md LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES macha.yaml.example
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples OPTIONAL)
if(APPLE)
    install(FILES macos/macha.plist.example
            DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples OPTIONAL)
endif()

configure_file(cmake/cmake_uninstall.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Uninstall Macha (configuration and runtime data are preserved)")

message(STATUS "Macha configuration file: ${MACHA_CONFIG_FILE}")
if(MACHA_INSTALL_SYSTEMD)
    message(STATUS "Macha systemd unit: ${MACHA_SYSTEMD_UNIT_DIR}/macha.service")
endif()
