cmake_minimum_required(VERSION 3.20)

# CMake 3.30 removed the legacy FindBoost module. libtorrent 2.1's exported
# package uses find_dependency(Boost); select config-mode Boost discovery on
# newer CMake while remaining compatible with older Raspberry Pi OS CMake.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(macha VERSION 0.90.58 LANGUAGES CXX)

# README.md states the version under its header as *vX.Y.Z*. Refuse to
# configure when it disagrees with the project version, so a release cannot
# ship with a stale README.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/README.md" MACHA_README_VERSION
     REGEX "^\\*v[0-9]+\\.[0-9]+\\.[0-9]+\\*$" LIMIT_COUNT 1)
if(NOT MACHA_README_VERSION STREQUAL "*v${PROJECT_VERSION}*")
    message(FATAL_ERROR "README.md version line '${MACHA_README_VERSION}' does not match "
                        "project version ${PROJECT_VERSION}; update it to *v${PROJECT_VERSION}*")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# macha_core is a shared library so that it, the macha executable and every
# future dlopen'd subsystem plugin (see TODO/archive/2026-09-05-subsystem-plugin-isolation-plan.md)
# share exactly one copy of its classes and global/static state -- linking it
# statically into more than one of those would duplicate that state, which is
# a correctness hazard, not a style question. Executables that link it locate
# it at a path relative to themselves at runtime (see INSTALL_RPATH below) so
# installation is not tied to a fixed prefix.
if(APPLE)
    set(MACHA_RPATH_ORIGIN "@loader_path")
else()
    set(MACHA_RPATH_ORIGIN "$ORIGIN")
endif()

# A subsystem plugin (see TODO/archive/2026-09-05-subsystem-plugin-isolation-plan.md)
# is always built from the same source tree, same commit, in the same CMake
# invocation as macha_core -- never distributed or versioned independently --
# so a plugin whose embedded build stamp doesn't match core's own is a build
# skew (e.g. a partial rsync deploy), not a supported "different build"
# scenario, and must be refused rather than loaded. MACHA_BUILD_COMMIT gives
# that stamp real precision: it changes on every commit, not just on a version
# bump, catching skew a version-number comparison alone would miss.
find_package(Git QUIET)
set(MACHA_BUILD_COMMIT "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MACHA_BUILD_COMMIT_OUTPUT
        RESULT_VARIABLE MACHA_BUILD_COMMIT_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(MACHA_BUILD_COMMIT_RESULT EQUAL 0 AND MACHA_BUILD_COMMIT_OUTPUT)
        set(MACHA_BUILD_COMMIT "${MACHA_BUILD_COMMIT_OUTPUT}")
    endif()
endif()

# Needed early: macha_core and every plugin are private shared libraries, not
# public system libraries, so they install under their own subdirectory of
# the standard libdir (e.g. /usr/lib/macha/), never into bindir alongside the
# executables -- mixing .so files into a directory that's on PATH and
# conventionally executable-only is exactly the kind of thing that trips up
# package managers and anyone auditing what's in bin.
include(GNUInstallDirs)
# Deliberately "lib/macha", not "${CMAKE_INSTALL_LIBDIR}/macha": on Debian
# and derivatives (e.g. Raspberry Pi OS), GNUInstallDirs resolves
# CMAKE_INSTALL_LIBDIR to a multiarch path (lib/aarch64-linux-gnu, etc), which
# is the right convention for a public system library that might need to
# coexist with other architectures' copies side by side, but macha_core is
# private (nothing outside this project links it) and only ever one arch per
# install -- multiarch nesting would just make this directory needlessly
# hard to find/document for no actual benefit.
set(MACHA_PRIVATE_LIBDIR "lib/macha" CACHE STRING
    "Directory macha_core installs to (private shared library, not a public system one)")
set(MACHA_PLUGIN_INSTALL_DIR "${MACHA_PRIVATE_LIBDIR}/plugins" CACHE STRING
    "Directory subsystem plugins install to and Config::plugin_path defaults to")
# The default baked into the binary must be an absolute path good for however
# this build is actually installed (CMAKE_INSTALL_PREFIX at configure time),
# not something computed from the running executable's own location --
# that's what led to the bindir mistake above in the first place.
if(IS_ABSOLUTE "${MACHA_PLUGIN_INSTALL_DIR}")
    set(MACHA_PLUGIN_INSTALL_DIR_FULL "${MACHA_PLUGIN_INSTALL_DIR}")
else()
    set(MACHA_PLUGIN_INSTALL_DIR_FULL "${CMAKE_INSTALL_PREFIX}/${MACHA_PLUGIN_INSTALL_DIR}")
endif()
# Relative hop from an installed executable (bindir) to the private libdir,
# for INSTALL_RPATH, computed between full paths so it's correct regardless
# of how many path segments CMAKE_INSTALL_BINDIR/LIBDIR actually are on this
# platform (e.g. Debian multiarch's lib/<triplet>).
file(RELATIVE_PATH MACHA_BINDIR_TO_PRIVATE_LIBDIR
     "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
     "${CMAKE_INSTALL_PREFIX}/${MACHA_PRIVATE_LIBDIR}")

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/macha_version.hpp @ONLY)

option(MACHA_BUILD_SERVER "Build the macha server executable" ON)
option(MACHA_ENABLE_FUSE "Build the FUSE mount adapter when available" ON)
option(MACHA_BUILD_TESTS "Build tests" ON)
option(MACHA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(MACHA_TEST_COVERAGE "Instrument the backend test build for source/branch coverage" OFF)

# Sanitizers are a whole-program property, not a per-target one: macha_core is
# a shared library that the executables link and every subsystem plugin
# dlopen's, so instrumenting only some of those would leave the interposed
# allocator and the shadow memory disagreeing across the boundary. These flags
# therefore go on at directory scope, before any target exists, rather than
# through target_compile_options like MACHA_TEST_COVERAGE does.
set(MACHA_SANITIZE "" CACHE STRING
    "Sanitizers to build with: empty (none), address, undefined, address,undefined, or thread")
if(MACHA_SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR "MACHA_SANITIZE requires GCC, Clang or AppleClang")
    endif()
    # ThreadSanitizer maintains its own shadow state for every memory access and
    # cannot share a process with AddressSanitizer's; the compilers reject the
    # combination too, but late and less legibly than this does.
    if(MACHA_SANITIZE MATCHES "thread" AND MACHA_SANITIZE MATCHES "address")
        message(FATAL_ERROR
            "MACHA_SANITIZE=thread cannot be combined with address; build them separately")
    endif()
    # -g and a frame pointer are what turn a sanitizer report from an address
    # into a stack trace naming the owning subsystem, which is the entire point
    # of running one against this codebase's lock ordering.
    add_compile_options(-fsanitize=${MACHA_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${MACHA_SANITIZE})
    message(STATUS "Sanitizers enabled: ${MACHA_SANITIZE}")
endif()

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
# Response compression for the API and the web client. zlib is already a
# transitive dependency of both OpenSSL and libcurl; this names it directly
# because src/http/http_compression.cpp calls deflate() itself.
find_package(ZLIB REQUIRED)
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
        # The exported target adds TORRENT_USE_ASSERTS to any consumer built
        # in Debug. The macro changes the layout of libtorrent's classes, and
        # the installed library was built without it, so a Debug plugin read
        # the library's objects at the wrong offsets (torrent_status came
        # back without its info hashes). The macro must match the library,
        # not our build type: drop it.
        get_target_property(_macha_lt_definitions LibtorrentRasterbar::torrent-rasterbar
                            INTERFACE_COMPILE_DEFINITIONS)
        if(_macha_lt_definitions)
            list(REMOVE_ITEM _macha_lt_definitions "$<$<CONFIG:Debug>:TORRENT_USE_ASSERTS>")
            set_target_properties(LibtorrentRasterbar::torrent-rasterbar PROPERTIES
                                  INTERFACE_COMPILE_DEFINITIONS "${_macha_lt_definitions}")
        endif()
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
    src/process_allocator.cpp
    src/log.cpp
    src/supervised.cpp
    src/coverage.cpp
    src/diagnostics.cpp
    src/contract/work.cpp
    src/contract/horizon.cpp
    src/contract/gates.cpp
    src/ledger/node_horizon_builder.cpp
    src/ledger/availability.cpp
    src/ledger/predicate_query.cpp
    src/observation.cpp
    src/durable_file.cpp
    src/write_behind.cpp
    src/json.cpp
    src/subsystem/subsystem.cpp
    src/subsystem/subsystem_registry.cpp
    src/subsystem/subsystem_supervisor.cpp
    src/storage/durability_domain.cpp
    src/storage/presence_index.cpp
    src/storage/local_store.cpp
    src/storage/storage_pool.cpp
    src/storage/retention.cpp
    src/storage/sealed_journal.cpp
    src/ledger/object_trie.cpp
    src/ledger/held_ledger.cpp
    src/storage/unreferenced_since.cpp
    src/ledger/retention_ledger.cpp
    src/storage/persistent_cache.cpp
    src/cluster/telemetry.cpp
    src/cluster/transcode_rates.cpp
    src/cluster/activity_clocks.cpp
    src/cluster/node_resources.cpp
    src/cluster/node_identity.cpp
    src/cluster/storage_server.cpp
    src/cluster/control_objects.cpp
    src/cluster/local_state.cpp
    src/cluster/local_services.cpp
    src/cluster/public_connectivity.cpp
    src/cluster/placement.cpp
    src/cluster/membership.cpp
    src/cluster/net.cpp
    src/cluster/cluster.cpp
    src/cluster/distributed_store.cpp
    src/cluster/replica_selector.cpp
    src/metadata/metadata.cpp
    src/metadata/metadata_merge.cpp
    src/metadata/namespace_tree.cpp
    src/metadata/namespace_control_store.cpp
    src/metadata/metadata_manager.cpp
    src/metadata/metadata_server.cpp
    src/auth/session.cpp
    src/auth/users.cpp
    src/auth/accounts.cpp
    src/http/http.cpp
    src/http/http_compression.cpp
    src/api/status_api.cpp
    src/api/catalogue_api.cpp
    src/api/files_api.cpp
    src/api/paging.cpp
    src/api/item_availability.cpp
    src/api/manage_api.cpp
    src/api/session_api.cpp
    src/api/users_api.cpp
    src/api/web_api.cpp
    src/api/acquisition_api.cpp
    src/filesystem/macos_unicode.cpp
    src/filesystem/hydration.cpp
    src/filesystem/filesystem.cpp
    src/fuse/fuse_journal.cpp
    src/fuse/fuse_frontend.cpp
    src/fuse/fuse_mountpoint.cpp
    src/fuse/fuse_subsystem.cpp
    src/media/media_metadata_registry.cpp
    src/media/ffmpeg_log_base.cpp
    src/media/media_segments.cpp
    src/media/media_timestamps.cpp
    src/media/media_vod.cpp
    src/media/subtitle_text.cpp
    src/media/media_containers.cpp
    src/media/media_engine_common.cpp
    src/playback/playback.cpp
    src/catalogue/media_information.cpp
    src/catalogue/catalogue.cpp
    src/catalogue/catalogue_hints.cpp
    src/catalogue/catalogue_installer.cpp
    src/catalogue/media_catalogue.cpp
    src/acquisition/cluster_jobs.cpp
    src/acquisition/ingest.cpp
    src/torrent/torrent_coordinator.cpp
    src/torrent/torrent_extent_journal.cpp
    src/torrent/torrent_common.cpp
    src/torrent/torrent_request.cpp
    src/service/maintenance_clock.cpp
    src/service/claim_walk.cpp
    src/service/availability_service.cpp
    src/service/maintenance.cpp
    src/service/service.cpp
    src/service/node_services.cpp
)

add_library(macha_core SHARED ${MACHA_CORE_SOURCES})
target_include_directories(macha_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(macha_core PUBLIC
    OpenSSL::Crypto Threads::Threads ${MACHA_CURL_TARGET} ZLIB::ZLIB ${CMAKE_DL_LIBS})
if(MACHA_MINIUPNPC_TARGET)
    target_link_libraries(macha_core PUBLIC ${MACHA_MINIUPNPC_TARGET})
    target_compile_definitions(macha_core PUBLIC MACHA_HAVE_MINIUPNPC=1)
    message(STATUS "UPnP port mapping enabled (${MACHA_MINIUPNPC_TARGET})")
else()
    message(STATUS "miniupnpc not found; UPnP port mapping unavailable at runtime")
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
# Clang checks the thread-safety annotations (src/contract/thread_safety.hpp)
# and refuses a violation, in the core and in each plugin; GCC ignores them.
function(macha_thread_safety target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(${target} PRIVATE -Wthread-safety -Werror=thread-safety)
    endif()
endfunction()
macha_thread_safety(macha_core)

# BitTorrent acquisition is a subsystem plugin, not part of macha_core: the
# whole libtorrent linkage lives behind a dlopen boundary so that a node
# without the plugin file simply has no torrent capability at runtime, and so
# an ordinary fault in it degrades that subsystem rather than the process (see
# TODO/archive/2026-09-05-subsystem-plugin-isolation-plan.md, Phase 1). MODULE, not
# SHARED: it is only ever dlopen'd, never linked against.
set(MACHA_PLUGIN_BUILD_DIR "${CMAKE_BINARY_DIR}/plugins")
# From the installed plugin directory back to the private libdir holding
# libmacha_core, so every plugin resolves core wherever the prefix is.
file(RELATIVE_PATH MACHA_PLUGINDIR_TO_PRIVATE_LIBDIR
     "${MACHA_PLUGIN_INSTALL_DIR_FULL}"
     "${CMAKE_INSTALL_PREFIX}/${MACHA_PRIVATE_LIBDIR}")
if(MACHA_LIBTORRENT_TARGET)
    add_library(macha-torrent MODULE src/torrent/torrent_manager.cpp src/torrent/torrent_session_policy.cpp src/torrent/torrent_plugin.cpp
        src/torrent/torrent_disk_io.cpp)
    target_link_libraries(macha-torrent PRIVATE macha_core ${MACHA_LIBTORRENT_TARGET})
    set_target_properties(macha-torrent PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${MACHA_PLUGIN_BUILD_DIR}"
        INSTALL_RPATH "${MACHA_RPATH_ORIGIN}/${MACHA_PLUGINDIR_TO_PRIVATE_LIBDIR}"
        BUILD_WITH_INSTALL_RPATH FALSE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(macha-torrent PRIVATE -Wall -Wextra -Wpedantic)
        macha_thread_safety(macha-torrent)
        if(MACHA_WARNINGS_AS_ERRORS)
            target_compile_options(macha-torrent PRIVATE -Werror)
        endif()
    endif()
    message(STATUS "BitTorrent acquisition plugin enabled (${MACHA_LIBTORRENT_TARGET})")
else()
    message(STATUS "libtorrent-rasterbar >= 2.0 not found; the BitTorrent acquisition plugin will not be built")
endif()

add_executable(macha-metadata-repair tools/metadata_inspect.cpp)
target_link_libraries(macha-metadata-repair PRIVATE macha_core)
add_executable(macha-metadata-dump tools/metadata_history_dump.cpp)
target_link_libraries(macha-metadata-dump PRIVATE macha_core)
add_executable(macha-namespace-migrate tools/namespace_migrate.cpp)
target_link_libraries(macha-namespace-migrate PRIVATE macha_core)
add_executable(macha-users tools/users_admin.cpp)
target_link_libraries(macha-users PRIVATE macha_core)
add_executable(macha-recover tools/recover.cpp)
target_link_libraries(macha-recover PRIVATE macha_core)
# libmacha_core installs to its own private libdir, not bindir (see
# MACHA_PRIVATE_LIBDIR above) -- this rpath is the relative hop from an
# installed executable's bindir to it, so it resolves regardless of prefix.
set_target_properties(macha-metadata-repair macha-metadata-dump macha-namespace-migrate macha-users macha-recover PROPERTIES
    INSTALL_RPATH "${MACHA_RPATH_ORIGIN}/${MACHA_BINDIR_TO_PRIVATE_LIBDIR}"
    BUILD_WITH_INSTALL_RPATH FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(macha-metadata-repair PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(macha-metadata-dump PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(macha-namespace-migrate PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(macha-users PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(macha-recover PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MACHA_BUILD_SERVER)
    add_executable(macha
        src/service/main.cpp
        src/config.cpp
        src/media/ffmpeg_log.cpp
        src/media/media_engine.cpp
        src/media/media_metadata_ffmpeg.cpp
        src/service/service_config.cpp)
    target_link_libraries(macha PRIVATE macha_core ${MACHA_YAML_TARGET} PkgConfig::LIBAV)
    set_target_properties(macha PROPERTIES
        INSTALL_RPATH "${MACHA_RPATH_ORIGIN}/${MACHA_BINDIR_TO_PRIVATE_LIBDIR}"
        BUILD_WITH_INSTALL_RPATH FALSE)

    # The FUSE mount adapter is a subsystem plugin, not part of the executable:
    # libfuse3 (plus its kernel driver) is the widest native crash surface in
    # the process, and behind a dlopen boundary a node without the plugin file
    # simply has no mount capability at runtime. The boundary is libfuse
    # ALONE -- FuseFrontend, the journal and the mountpoint helpers are core's
    # own code and stay in macha_core, so core keeps its concrete types and
    # the FUSE tests keep linking it directly. See
    # TODO/archive/2026-09-14-fuse-supervised-subsystem-plan.md, decision 2.
    set(MACHA_FUSE_FOUND OFF)
    if(MACHA_ENABLE_FUSE)
        pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
        if(TARGET PkgConfig::FUSE3)
            set(MACHA_FUSE_FOUND ON)
            add_library(macha-fuse MODULE src/fuse/fuse_adapter.cpp src/fuse/fuse_plugin.cpp)
            target_link_libraries(macha-fuse PRIVATE macha_core)
            # On macOS, /usr/local/include is searched by Clang before -isystem
            # directories.  If both legacy FUSE 2 headers and FUSE 3 are installed,
            # PkgConfig::FUSE3's system include therefore lets <fuse.h> resolve to
            # FUSE 2 while <fuse_lowlevel.h> resolves to FUSE 3.  Put the explicit
            # FUSE 3 directory first as a normal include path so a clean configure is
            # deterministic (an older CMake cache happened to conceal this).
            if(APPLE)
                target_include_directories(macha-fuse BEFORE PRIVATE ${FUSE3_INCLUDE_DIRS})
                target_link_directories(macha-fuse PRIVATE ${FUSE3_LIBRARY_DIRS})
                target_link_libraries(macha-fuse PRIVATE ${FUSE3_LIBRARIES})
                target_compile_options(macha-fuse PRIVATE ${FUSE3_CFLAGS_OTHER})
                target_link_options(macha-fuse PRIVATE ${FUSE3_LDFLAGS_OTHER})
            else()
                target_link_libraries(macha-fuse PRIVATE PkgConfig::FUSE3)
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
                add_library(macha-fuse MODULE src/fuse/fuse_adapter.cpp src/fuse/fuse_plugin.cpp)
                target_link_libraries(macha-fuse PRIVATE macha_core ${FUSE3_LIBRARY})
                target_include_directories(macha-fuse PRIVATE ${FUSE3_INCLUDE_DIR})
            endif()
        endif()
    endif()

    if(MACHA_FUSE_FOUND)
        set_target_properties(macha-fuse PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${MACHA_PLUGIN_BUILD_DIR}"
            INSTALL_RPATH "${MACHA_RPATH_ORIGIN}/${MACHA_PLUGINDIR_TO_PRIVATE_LIBDIR}"
            BUILD_WITH_INSTALL_RPATH FALSE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(macha-fuse PRIVATE -Wall -Wextra -Wpedantic)
            macha_thread_safety(macha-fuse)
            if(MACHA_WARNINGS_AS_ERRORS)
                target_compile_options(macha-fuse PRIVATE -Werror)
            endif()
        endif()
        # The executable no longer links libfuse at all.
        add_dependencies(macha macha-fuse)
        message(STATUS "FUSE mount plugin enabled")
    elseif(MACHA_ENABLE_FUSE)
        # No stub translation unit: with no plugin file there is no mount
        # driver, and the `fuse` subsystem is simply absent at runtime, the
        # same way a node without libmacha-torrent has no torrent capability.
        message(STATUS "FUSE3/macFUSE development files not found; building service without mount support")
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

    # Real dlopen'd plugins exercising SubsystemSupervisor end to end (see
    # TODO/archive/2026-09-05-subsystem-plugin-isolation-plan.md), not in-process
    # mocks. Each is a MODULE library -- never linked into an executable,
    # only ever dlopen'd -- and links macha_core so it shares the same
    # Subsystem/SubsystemPluginEntry types and build-identity stamp.
    foreach(plugin_name IN ITEMS test_plugin_ok test_plugin_faulting test_plugin_mismatched_abi
                                test_plugin_no_entry_symbol test_plugin_declining
                                test_plugin_faulting_after_start test_plugin_writes_on_stop)
        add_library(${plugin_name} MODULE tests/plugins/${plugin_name}.cpp)
        target_link_libraries(${plugin_name} PRIVATE macha_core)
        set_target_properties(${plugin_name} PROPERTIES PREFIX "")
    endforeach()

    # The portable suite owns all dependency-free behavioural tests. Concrete
    # yaml-cpp/libav adapter tests are registered separately below only when
    # those development dependencies are available. Both use the same runner.
    add_executable(macha-tests
        tests/main.cpp
        tests/test_framework.cpp
        tests/test_foundations.cpp
        tests/test_subsystem_supervisor.cpp
        tests/test_fuse_subsystem.cpp
        tests/test_storage_metadata.cpp
        tests/test_object_ledger.cpp
        tests/test_namespace_tree.cpp
        tests/test_namespace_migration.cpp
        tests/test_io_pressure.cpp
        tests/test_storage_v18.cpp
        tests/test_rpc_cluster.cpp
        tests/test_filesystem_fuse.cpp
        tests/test_hydration_catalogue.cpp
        tests/test_media_playback.cpp
        tests/test_session.cpp
        tests/test_users.cpp
        tests/test_web_api.cpp
        tests/test_http_server.cpp
        tests/test_http_compression.cpp
        tests/test_invariants.cpp
        tests/test_models.cpp
        tests/test_observation.cpp
        tests/test_baselines.cpp
        tests/test_maintenance_trace.cpp
        tests/test_presence.cpp
        tests/test_contract.cpp
        tests/test_absent_nodes.cpp
        tests/test_metadata_merge.cpp
        tests/test_ledger_gates.cpp
        tests/test_horizon_builder.cpp
        tests/test_availability.cpp
        tests/test_predicates.cpp
        tests/test_work_class.cpp
        tests/test_catalogue_maintenance.cpp
        tests/test_namespace_entries.cpp
        tests/test_claim_walk.cpp
        tests/test_root_parts.cpp
        tests/test_service_root.cpp
        tests/test_node_services.cpp)
    target_link_libraries(macha-tests PRIVATE macha_core)
    target_compile_definitions(macha-tests PRIVATE
        MACHA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        MACHA_TEST_PLUGIN_OK="$<TARGET_FILE:test_plugin_ok>"
        MACHA_TEST_PLUGIN_FAULTING="$<TARGET_FILE:test_plugin_faulting>"
        MACHA_TEST_PLUGIN_MISMATCHED_ABI="$<TARGET_FILE:test_plugin_mismatched_abi>"
        MACHA_TEST_PLUGIN_NO_ENTRY_SYMBOL="$<TARGET_FILE:test_plugin_no_entry_symbol>"
        MACHA_TEST_PLUGIN_DECLINING="$<TARGET_FILE:test_plugin_declining>"
        MACHA_TEST_PLUGIN_FAULTING_AFTER_START="$<TARGET_FILE:test_plugin_faulting_after_start>"
        MACHA_TEST_PLUGIN_WRITES_ON_STOP="$<TARGET_FILE:test_plugin_writes_on_stop>")
    add_dependencies(macha-tests test_plugin_ok test_plugin_faulting test_plugin_mismatched_abi
        test_plugin_no_entry_symbol test_plugin_declining test_plugin_faulting_after_start
        test_plugin_writes_on_stop)
    # Tests that need the real torrent subsystem dlopen it from the build
    # tree, exactly as production loads it from the install tree. Pointing a
    # Service's plugin_path here also keeps a test process from loading a
    # stale plugin out of the installed default directory.
    if(TARGET macha-torrent)
        target_compile_definitions(macha-tests PRIVATE
            MACHA_TEST_TORRENT_PLUGIN="$<TARGET_FILE:macha-torrent>"
            MACHA_TEST_PLUGIN_DIR="${MACHA_PLUGIN_BUILD_DIR}")
        add_dependencies(macha-tests macha-torrent)
    endif()
    if(TARGET macha-fuse)
        target_compile_definitions(macha-tests PRIVATE
            MACHA_TEST_FUSE_PLUGIN="$<TARGET_FILE:macha-fuse>")
        add_dependencies(macha-tests macha-fuse)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(macha-tests PRIVATE -Wall -Wextra -Wpedantic)
        if(MACHA_WARNINGS_AS_ERRORS)
            target_compile_options(macha-tests PRIVATE -Werror)
        endif()
    endif()
    if(MACHA_TEST_COVERAGE)
        # macha_core needs the coverage flags at LINK time as well as compile
        # time. It is a shared library, so the profile runtime it references
        # has to be linked into it -- without this the library does not link
        # at all (undefined __llvm_profile_runtime under Clang), which is why
        # this option had never produced a report.
        # Every case runs in a forked child that leaves through std::_Exit, so
        # the framework has to dump counters by hand; this tells it to.
        target_compile_definitions(macha-tests PRIVATE MACHA_COVERAGE=1)
        # Under Clang the library carries its own copy of the profile
        # runtime, which only the library can drive (src/coverage.hpp).
        target_compile_definitions(macha_core PRIVATE MACHA_COVERAGE=1)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(macha_core PRIVATE --coverage)
            target_link_options(macha_core PRIVATE --coverage)
            target_compile_options(macha-tests PRIVATE --coverage)
            target_link_options(macha-tests PRIVATE --coverage)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            # Atomic counters: in-process cluster tests run several nodes'
            # threads through the same code at once, and plain increments
            # lose counts, which llvm-cov then derives as negative branch
            # counts (shown as 18.4E) that wrap a real count to zero.
            target_compile_options(macha_core PRIVATE -fprofile-instr-generate -fcoverage-mapping
                -fprofile-update=atomic)
            target_link_options(macha_core PRIVATE -fprofile-instr-generate)
            target_compile_options(macha-tests PRIVATE -fprofile-instr-generate -fcoverage-mapping
                -fprofile-update=atomic)
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
            tests/test_transcode_timeline.cpp
            tests/test_lifecycle_record.cpp
            src/config.cpp
            src/media/ffmpeg_log.cpp
            src/media/media_engine.cpp
            src/media/media_metadata_ffmpeg.cpp)
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
            target_compile_definitions(macha-tests-runtime PRIVATE MACHA_COVERAGE=1)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                target_compile_options(macha-tests-runtime PRIVATE --coverage)
                target_link_options(macha-tests-runtime PRIVATE --coverage)
            elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
                target_compile_options(macha-tests-runtime PRIVATE -fprofile-instr-generate -fcoverage-mapping
                    -fprofile-update=atomic)
                target_link_options(macha-tests-runtime PRIVATE -fprofile-instr-generate)
            endif()
        endif()
        add_test(NAME macha-tests-runtime COMMAND macha-tests-runtime)
        set_tests_properties(macha-tests-runtime PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "yaml-cpp and/or FFmpeg development files not found; runtime-adapter tests will not be built")
    endif()
    # The torrent disk backend, driven through libtorrent's disk_interface as a
    # session drives it. Its own binary because it links libtorrent, which
    # macha_core and macha-tests deliberately never do.
    if(TARGET macha-torrent)
        add_executable(macha-tests-torrent
            tests/main.cpp
            tests/test_framework.cpp
            tests/test_torrent_disk_io.cpp
            src/torrent/torrent_disk_io.cpp
            src/torrent/torrent_session_policy.cpp)
        target_link_libraries(macha-tests-torrent PRIVATE macha_core ${MACHA_LIBTORRENT_TARGET})
        target_compile_definitions(macha-tests-torrent PRIVATE
            MACHA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
            target_compile_options(macha-tests-torrent PRIVATE -Wall -Wextra -Wpedantic)
            if(MACHA_WARNINGS_AS_ERRORS)
                target_compile_options(macha-tests-torrent PRIVATE -Werror)
            endif()
        endif()
        add_test(NAME macha-tests-torrent COMMAND macha-tests-torrent)
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
install(TARGETS macha-metadata-repair macha-metadata-dump macha-namespace-migrate macha-users macha-recover RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# macha_core is SHARED (see the add_library(macha_core SHARED ...) comment
# above), but it is a private library, not a public system one -- it installs
# under its own subdirectory of the standard libdir (MACHA_PRIVATE_LIBDIR),
# never into bindir alongside the executables. LIBRARY covers Linux/macOS
# .so/.dylib; RUNTIME is the Windows .dll case, kept for completeness even
# though no Windows target exists today.
install(TARGETS macha_core
        LIBRARY DESTINATION ${MACHA_PRIVATE_LIBDIR}
        RUNTIME DESTINATION ${MACHA_PRIVATE_LIBDIR})
# Subsystem plugins install here (Config::plugin_path defaults to it). The
# directory is created even when no plugin was built, so an operator can see
# where one would go and a scan of it is simply empty.
install(DIRECTORY DESTINATION ${MACHA_PLUGIN_INSTALL_DIR})
if(TARGET macha-torrent)
    install(TARGETS macha-torrent LIBRARY DESTINATION ${MACHA_PLUGIN_INSTALL_DIR})
endif()
if(TARGET macha-fuse)
    install(TARGETS macha-fuse LIBRARY DESTINATION ${MACHA_PLUGIN_INSTALL_DIR})
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
