# ======================================================================
#  mnemond -- Persistent, searchable memory for LLM agents via MCP
#
#  CMakeLists.txt -- Top-level build configuration
#
#  Follows autoconf conventions:
#    mkdir build && cd build
#    cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
#    make
#    make test
#    make install DESTDIR=/tmp/staging
#
#  Feature flags (pass -D<FLAG>=ON/OFF to cmake):
#    ENABLE_SYSTEMD  -- systemd sd_notify support  (auto-detected)
#    ENABLE_NUMA     -- NUMA-aware allocation       (auto-detected)
#    ENABLE_CURL     -- libcurl for extraction       (auto-detected)
# ======================================================================

cmake_minimum_required(VERSION 3.16)

# Version is tracked in the VERSION file (single source of truth)
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" MNEMON_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${MNEMON_VERSION_STRING}" MNEMON_VERSION_STRING)

project(mnemond
    VERSION   ${MNEMON_VERSION_STRING}
    LANGUAGES C CXX
    DESCRIPTION "Persistent searchable memory for LLM agents via MCP"
)

# C++ is only used for vendored usearch (third_party/usearch/lib.cpp).
# All core code is C11. No C++ in src/.

# Package metadata (autoconf equivalents)
set(PACKAGE_BUGREPORT "https://github.com/rdilley/MnemonAI/issues")
set(PACKAGE_URL       "https://github.com/rdilley/MnemonAI")
set(PACKAGE_TARNAME   "mnemond")

# ======================================================================
#  Build defaults
# ======================================================================

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Position-independent code (needed for shared libraries, good practice)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# RPATH: embed library search paths so binaries find llama.cpp/ggml libs
# without requiring LD_LIBRARY_PATH at runtime (build tree and install tree).
# Use RPATH (not RUNPATH) so transitive dependencies (e.g., libggml.so ->
# libggml-hip.so) inherit the search path. RUNPATH does not propagate.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
if(CMAKE_PREFIX_PATH)
    foreach(prefix ${CMAKE_PREFIX_PATH})
        list(APPEND CMAKE_BUILD_RPATH "${prefix}/lib")
        list(APPEND CMAKE_INSTALL_RPATH "${prefix}/lib")
    endforeach()
endif()
# Force DT_RPATH instead of DT_RUNPATH for transitive dependency resolution.
add_link_options("-Wl,--disable-new-dtags")

# Export compile_commands.json for editors / clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ======================================================================
#  GNU install directories (autoconf-compatible)
# ======================================================================

include(GNUInstallDirs)

# Autoconf-style directory variables
set(MNEMON_SYSCONFDIR   "${CMAKE_INSTALL_FULL_SYSCONFDIR}/mnemond"
    CACHE PATH "Configuration file directory")
set(MNEMON_LOCALSTATEDIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/mnemond"
    CACHE PATH "Persistent data directory")

# ======================================================================
#  Custom cmake modules
# ======================================================================

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ======================================================================
#  Compiler flags
# ======================================================================

include(CheckCCompilerFlag)

# POSIX feature macros -- required for strdup, realpath, clock_gettime, etc.
# Without these, strict C11 mode treats POSIX functions as implicit (returning int),
# which truncates 64-bit pointers to 32-bit on x86_64 and causes segfaults.
add_compile_definitions(
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

# Warnings -- treat like autoconf AC_PROG_CC_C11 + careful flags
set(MNEMON_C_FLAGS
    -Wall -Wextra -Wpedantic
    -Wshadow -Wstrict-prototypes -Wmissing-prototypes
    -Wimplicit-function-declaration
    -Werror=implicit-function-declaration
    -Wwrite-strings -Wcast-qual
    -Wno-unused-parameter
)

foreach(flag ${MNEMON_C_FLAGS})
    string(MAKE_C_IDENTIFIER "HAS_FLAG_${flag}" flag_var)
    check_c_compiler_flag(${flag} ${flag_var})
    if(${flag_var})
        add_compile_options(${flag})
    endif()
endforeach()

# Hardening flags
check_c_compiler_flag("-fstack-protector-strong" HAS_STACK_PROTECTOR)
if(HAS_STACK_PROTECTOR)
    add_compile_options(-fstack-protector-strong)
endif()

add_compile_definitions(_FORTIFY_SOURCE=2)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(_DEBUG)
endif()

# ======================================================================
#  Header checks (autoconf AC_CHECK_HEADERS equivalent)
# ======================================================================

include(CheckIncludeFile)

check_include_file(dlfcn.h      HAVE_DLFCN_H)
check_include_file(errno.h      HAVE_ERRNO_H)
check_include_file(fcntl.h      HAVE_FCNTL_H)
check_include_file(getopt.h     HAVE_GETOPT_H)
check_include_file(inttypes.h   HAVE_INTTYPES_H)
check_include_file(memory.h     HAVE_MEMORY_H)
check_include_file(pthread.h    HAVE_PTHREAD_H)
check_include_file(signal.h     HAVE_SIGNAL_H)
check_include_file(stdint.h     HAVE_STDINT_H)
check_include_file(stdlib.h     HAVE_STDLIB_H)
check_include_file(string.h     HAVE_STRING_H)
check_include_file(strings.h    HAVE_STRINGS_H)
check_include_file(sys/mman.h   HAVE_SYS_MMAN_H)
check_include_file(sys/stat.h   HAVE_SYS_STAT_H)
check_include_file(sys/types.h  HAVE_SYS_TYPES_H)
check_include_file(sys/wait.h   HAVE_SYS_WAIT_H)
check_include_file(syslog.h     HAVE_SYSLOG_H)
check_include_file(unistd.h     HAVE_UNISTD_H)

# ======================================================================
#  Function checks (autoconf AC_CHECK_FUNCS equivalent)
# ======================================================================

include(CheckFunctionExists)
include(CheckSymbolExists)

check_function_exists(clock_gettime  HAVE_CLOCK_GETTIME)
check_function_exists(daemon         HAVE_DAEMON)
check_function_exists(fork           HAVE_FORK)
check_function_exists(getopt_long    HAVE_GETOPT_LONG)
check_function_exists(mmap           HAVE_MMAP)
check_function_exists(posix_memalign HAVE_POSIX_MEMALIGN)
check_function_exists(realpath       HAVE_REALPATH)
check_function_exists(setsid         HAVE_SETSID)
check_function_exists(strndup        HAVE_STRNDUP)
check_function_exists(strerror_r     HAVE_STRERROR_R)

# dlopen may be in libdl
include(CheckLibraryExists)
check_library_exists(dl dlopen "" HAVE_LIBDL)
if(HAVE_LIBDL)
    set(HAVE_DLOPEN 1)
    set(DL_LIBRARY dl)
else()
    check_function_exists(dlopen HAVE_DLOPEN)
    set(DL_LIBRARY "")
endif()

# ======================================================================
#  Threading (autoconf ACX_PTHREAD equivalent)
# ======================================================================

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ======================================================================
#  Required dependencies -- prefer system, fall back to vendored
# ======================================================================

# SQLite3: try system first, fall back to vendored amalgamation
option(USE_SYSTEM_SQLITE3 "Use system SQLite3 instead of vendored" ON)
if(USE_SYSTEM_SQLITE3)
    find_package(SQLite3 QUIET)
endif()
if(NOT SQLite3_FOUND)
    set(USE_SYSTEM_SQLITE3 OFF)
    message(STATUS "System SQLite3 not found -- using vendored amalgamation")
    add_subdirectory(third_party/sqlite3)
    add_library(SQLite::SQLite3 ALIAS sqlite3)
endif()

# llama.cpp (system install required)
# Build from source: https://github.com/ggerganov/llama.cpp
# Set LLAMA_ROOT or CMAKE_PREFIX_PATH if installed to a non-standard prefix.
#
# llama.cpp provides its own cmake config (llama-config.cmake) which creates
# an imported target named "llama". We also support discovery via our
# FindLlama.cmake module which creates "Llama::Llama".
find_package(llama QUIET CONFIG)
if(llama_FOUND)
    message(STATUS "Found llama.cpp via cmake config")
    if(NOT TARGET Llama::Llama)
        add_library(Llama::Llama ALIAS llama)
    endif()
else()
    find_package(Llama REQUIRED)
endif()

# ======================================================================
#  Vendored dependencies
# ======================================================================

add_subdirectory(third_party/cJSON)
add_subdirectory(third_party/lmdb)
add_subdirectory(third_party/usearch)

# ======================================================================
#  Optional dependencies (autoconf AC_ARG_ENABLE / AC_ARG_WITH equiv.)
# ======================================================================

find_package(PkgConfig QUIET)

# --- systemd ---
option(ENABLE_SYSTEMD "Enable systemd sd_notify support" ON)
if(ENABLE_SYSTEMD AND PkgConfig_FOUND)
    pkg_check_modules(SYSTEMD QUIET libsystemd)
    if(SYSTEMD_FOUND)
        set(HAVE_LIBSYSTEMD 1)
    else()
        set(ENABLE_SYSTEMD OFF)
    endif()
else()
    set(ENABLE_SYSTEMD OFF)
endif()

# --- libmicrohttpd (HTTP transport) ---
option(ENABLE_HTTP "Enable HTTP/SSE transport for network MCP access" ON)
if(ENABLE_HTTP AND PkgConfig_FOUND)
    pkg_check_modules(MHD QUIET libmicrohttpd)
    if(MHD_FOUND)
        set(HAVE_LIBMICROHTTPD 1)
    else()
        message(WARNING "libmicrohttpd not found -- HTTP transport disabled. "
                "Install: sudo apt-get install libmicrohttpd-dev")
        set(ENABLE_HTTP OFF)
    endif()
else()
    set(ENABLE_HTTP OFF)
endif()

# --- NUMA ---
option(ENABLE_NUMA "Enable NUMA-aware memory allocation" ON)
if(ENABLE_NUMA AND PkgConfig_FOUND)
    pkg_check_modules(NUMA QUIET numa)
    if(NOT NUMA_FOUND)
        # Fall back to library search
        find_library(NUMA_LIBRARY NAMES numa)
        find_path(NUMA_INCLUDE_DIR NAMES numa.h)
        if(NUMA_LIBRARY AND NUMA_INCLUDE_DIR)
            set(NUMA_FOUND TRUE)
            set(NUMA_LIBRARIES ${NUMA_LIBRARY})
            set(NUMA_INCLUDE_DIRS ${NUMA_INCLUDE_DIR})
        endif()
    endif()
    if(NUMA_FOUND)
        set(HAVE_LIBNUMA 1)
    else()
        set(ENABLE_NUMA OFF)
    endif()
else()
    set(ENABLE_NUMA OFF)
endif()

# --- libcurl ---
option(ENABLE_CURL "Enable libcurl for entity extraction HTTP" ON)
if(ENABLE_CURL)
    # Try standard find first, then multiarch path (Debian/Ubuntu)
    find_package(CURL QUIET)
    if(NOT CURL_FOUND)
        find_path(CURL_INCLUDE_DIR NAMES curl/curl.h
            PATHS /usr/include/x86_64-linux-gnu /usr/include)
        find_library(CURL_LIBRARY NAMES curl)
        if(CURL_INCLUDE_DIR AND CURL_LIBRARY)
            set(CURL_FOUND TRUE)
            set(CURL_INCLUDE_DIRS ${CURL_INCLUDE_DIR})
            set(CURL_LIBRARIES ${CURL_LIBRARY})
            add_library(CURL::libcurl UNKNOWN IMPORTED)
            set_target_properties(CURL::libcurl PROPERTIES
                IMPORTED_LOCATION "${CURL_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")
        endif()
    endif()
    if(CURL_FOUND)
        set(HAVE_LIBCURL 1)
    else()
        message(WARNING "libcurl not found -- entity extraction disabled. "
                "Install: sudo apt-get install libcurl4-openssl-dev")
        set(ENABLE_CURL OFF)
    endif()
else()
    set(ENABLE_CURL OFF)
endif()

# Math library (may be implicit)
check_library_exists(m sqrt "" HAVE_LIBM)
if(HAVE_LIBM)
    set(M_LIBRARY m)
else()
    set(M_LIBRARY "")
endif()

# ======================================================================
#  SIMD detection (autoconf-style compiler feature checks)
# ======================================================================

check_c_compiler_flag("-mavx2" HAVE_AVX2)
check_c_compiler_flag("-mavx512f" HAVE_AVX512)


# ======================================================================
#  Generate config.h (autoconf AC_CONFIG_HEADERS equivalent)
# ======================================================================

configure_file(
    "${CMAKE_SOURCE_DIR}/src/config.h.in"
    "${CMAKE_BINARY_DIR}/config.h"
    @ONLY
)

# Make config.h findable
include_directories("${CMAKE_BINARY_DIR}")

# ======================================================================
#  SIMD object libraries
# ======================================================================

add_library(simd_scalar OBJECT simd/distance_scalar.c)
target_include_directories(simd_scalar PRIVATE
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/simd"
)
target_compile_definitions(simd_scalar PRIVATE HAVE_CONFIG_H)

set(SIMD_OBJECTS $<TARGET_OBJECTS:simd_scalar>)

if(HAVE_AVX2)
    add_library(simd_avx2 OBJECT simd/distance_avx2.c)
    target_compile_options(simd_avx2 PRIVATE -mavx2 -mfma)
    target_compile_definitions(simd_avx2 PRIVATE HAVE_CONFIG_H HAVE_AVX2)
    target_include_directories(simd_avx2 PRIVATE
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/simd"
    )
    list(APPEND SIMD_OBJECTS $<TARGET_OBJECTS:simd_avx2>)
endif()

if(HAVE_AVX512)
    add_library(simd_avx512 OBJECT simd/distance_avx512.c)
    target_compile_options(simd_avx512 PRIVATE -mavx512f -mavx512bw)
    target_compile_definitions(simd_avx512 PRIVATE HAVE_CONFIG_H HAVE_AVX512)
    target_include_directories(simd_avx512 PRIVATE
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_SOURCE_DIR}/simd"
    )
    list(APPEND SIMD_OBJECTS $<TARGET_OBJECTS:simd_avx512>)
endif()

# ======================================================================
#  Main executable
# ======================================================================

set(MNEMON_SOURCES
    src/main.c
    src/mnemon_err.c
    src/config.c
    src/daemon.c
    src/log.c
    src/id.c
    src/mcp_stdio.c
    src/mcp_http.c
    src/mcp_dispatch.c
    src/mcp_tools.c
    src/storage.c
    src/graph.c
    src/fts.c
    src/vector.c
    src/embed.c
    src/search.c
    src/temporal.c
    src/memory.c
    src/consolidate.c
    src/import.c
    src/extract.c
    src/secret.c
    src/hardware.c
    src/threads.c
    src/admit.c
    src/audit.c
    src/model_mgr.c
    src/honeypot.c
    src/sse.c
)

add_executable(mnemond ${MNEMON_SOURCES} ${SIMD_OBJECTS})

target_compile_definitions(mnemond PRIVATE HAVE_CONFIG_H)

target_include_directories(mnemond PRIVATE
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/simd"
)

target_link_libraries(mnemond PRIVATE
    cjson
    lmdb
    usearch
    SQLite::SQLite3
    Llama::Llama
    Threads::Threads
    ${M_LIBRARY}
    ${DL_LIBRARY}
)

if(ENABLE_SYSTEMD)
    target_compile_definitions(mnemond PRIVATE ENABLE_SYSTEMD)
    target_include_directories(mnemond PRIVATE ${SYSTEMD_INCLUDE_DIRS})
    target_link_libraries(mnemond PRIVATE ${SYSTEMD_LIBRARIES})
endif()

if(ENABLE_NUMA)
    target_compile_definitions(mnemond PRIVATE ENABLE_NUMA)
    target_include_directories(mnemond PRIVATE ${NUMA_INCLUDE_DIRS})
    target_link_libraries(mnemond PRIVATE ${NUMA_LIBRARIES})
endif()

if(ENABLE_CURL)
    target_compile_definitions(mnemond PRIVATE ENABLE_CURL)
    target_link_libraries(mnemond PRIVATE CURL::libcurl)
endif()

if(ENABLE_HTTP)
    target_compile_definitions(mnemond PRIVATE ENABLE_HTTP)
    target_include_directories(mnemond PRIVATE ${MHD_INCLUDE_DIRS})
    target_link_libraries(mnemond PRIVATE ${MHD_LIBRARIES})
endif()

if(HAVE_AVX2)
    target_compile_definitions(mnemond PRIVATE HAVE_AVX2)
endif()

if(HAVE_AVX512)
    target_compile_definitions(mnemond PRIVATE HAVE_AVX512)
endif()

# ======================================================================
#  Core library (for tests to link against without main.c)
# ======================================================================

set(MNEMON_LIB_SOURCES
    src/mnemon_err.c
    src/config.c
    src/daemon.c
    src/log.c
    src/id.c
    src/mcp_stdio.c
    src/mcp_http.c
    src/mcp_dispatch.c
    src/mcp_tools.c
    src/storage.c
    src/graph.c
    src/fts.c
    src/vector.c
    src/embed.c
    src/search.c
    src/temporal.c
    src/memory.c
    src/consolidate.c
    src/import.c
    src/extract.c
    src/secret.c
    src/hardware.c
    src/threads.c
    src/admit.c
    src/audit.c
    src/model_mgr.c
    src/honeypot.c
    src/sse.c
)

add_library(mnemon_core STATIC ${MNEMON_LIB_SOURCES} ${SIMD_OBJECTS})
target_compile_definitions(mnemon_core PRIVATE HAVE_CONFIG_H)
target_include_directories(mnemon_core PUBLIC
    "${CMAKE_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/simd"
)
target_link_libraries(mnemon_core PUBLIC
    cjson
    lmdb
    usearch
    SQLite::SQLite3
    Llama::Llama
    Threads::Threads
    ${M_LIBRARY}
    ${DL_LIBRARY}
)
if(ENABLE_SYSTEMD)
    target_compile_definitions(mnemon_core PRIVATE ENABLE_SYSTEMD)
    target_link_libraries(mnemon_core PUBLIC ${SYSTEMD_LIBRARIES})
endif()
if(ENABLE_NUMA)
    target_compile_definitions(mnemon_core PRIVATE ENABLE_NUMA)
    target_link_libraries(mnemon_core PUBLIC ${NUMA_LIBRARIES})
endif()
if(ENABLE_CURL)
    target_compile_definitions(mnemon_core PRIVATE ENABLE_CURL)
    target_link_libraries(mnemon_core PUBLIC CURL::libcurl)
endif()
if(ENABLE_HTTP)
    target_compile_definitions(mnemon_core PRIVATE ENABLE_HTTP)
    target_include_directories(mnemon_core PUBLIC ${MHD_INCLUDE_DIRS})
    target_link_libraries(mnemon_core PUBLIC ${MHD_LIBRARIES})
endif()
if(HAVE_AVX2)
    target_compile_definitions(mnemon_core PRIVATE HAVE_AVX2)
endif()
if(HAVE_AVX512)
    target_compile_definitions(mnemon_core PRIVATE HAVE_AVX512)
endif()

# ======================================================================
#  Tests (autoconf AC_CONFIG_TESTDIR / make check equivalent)
# ======================================================================

include(CTest)

if(BUILD_TESTING)
    set(MNEMON_TESTS graph fts vector search mcp temporal secret storage)

    foreach(test_name ${MNEMON_TESTS})
        add_executable(test_${test_name} test/test_${test_name}.c)
        target_compile_definitions(test_${test_name} PRIVATE HAVE_CONFIG_H)
        target_include_directories(test_${test_name} PRIVATE
            "${CMAKE_BINARY_DIR}"
            "${CMAKE_SOURCE_DIR}/src"
            "${CMAKE_SOURCE_DIR}/simd"
        )
        target_link_libraries(test_${test_name} PRIVATE
            mnemon_core
        )
        add_test(
            NAME    ${test_name}
            COMMAND test_${test_name}
        )
    endforeach()

    # ------------------------------------------------------------------
    #  Python integration tests (require mnemond binary + Python 3)
    # ------------------------------------------------------------------
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        # MCP client compatibility (all 35 tools, stdio transport)
        add_test(
            NAME    mcp_client
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/test/test_mcp_client.py"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(mcp_client PROPERTIES
            LABELS "python;integration"
            TIMEOUT 120
        )

        # MCP HTTP transport
        add_test(
            NAME    mcp_http
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/test/test_mcp_http.py"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(mcp_http PROPERTIES
            LABELS "python;integration"
            TIMEOUT 120
        )

        # MCP HTTP transport with the real embedding model loaded (validates
        # vector/semantic paths that mcp_http stubs with model_path=none).
        # Skips (exit 77) when the model is absent, so it never fails on CI.
        add_test(
            NAME    mcp_http_embeddings
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/test/test_all_tools_http.py"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(mcp_http_embeddings PROPERTIES
            LABELS "python;integration;embeddings"
            SKIP_RETURN_CODE 77
            TIMEOUT 240
        )

        # Performance benchmarks
        add_test(
            NAME    mcp_perf
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/test/test_mcp_perf.py"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(mcp_perf PROPERTIES
            LABELS "python;benchmark"
            TIMEOUT 300
        )

        # LongMemEval benchmark -- quick smoke test (no agent, no judges)
        # Full benchmark: python3 ../test/test_longmemeval.py generate
        add_test(
            NAME    longmemeval
            COMMAND "${CMAKE_COMMAND}" -E env
                    "MNEMON_LONGMEMEVAL_AGENT=none"
                    "MNEMON_LONGMEMEVAL_JUDGES=none"
                    "MNEMON_LONGMEMEVAL_LIMIT=10"
                    "${Python3_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/test/test_longmemeval.py"
                    "generate"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(longmemeval PROPERTIES
            LABELS "python;benchmark;longmemeval"
            TIMEOUT 60
        )
    endif()
endif()

# ======================================================================
#  Installation (autoconf make install / DESTDIR equivalent)
# ======================================================================

# Binary
install(TARGETS mnemond
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# Configuration file
install(FILES etc/mnemond.conf.example
    DESTINATION "${MNEMON_SYSCONFDIR}"
    RENAME mnemond.conf.example
)

# systemd service file (if systemd is available)
if(ENABLE_SYSTEMD)
    set(SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
        CACHE PATH "systemd unit file directory")
    install(FILES etc/mnemond.service
        DESTINATION "${SYSTEMD_UNIT_DIR}"
    )
endif()

# Man page
install(FILES doc/mnemond.1
    DESTINATION "${CMAKE_INSTALL_MANDIR}/man1"
)

# ======================================================================
#  Uninstall target (autoconf make uninstall equivalent)
# ======================================================================

if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake"
    )
endif()

# ======================================================================
#  CPack (autoconf make dist / make dist-gz equivalent)
# ======================================================================

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT "${PACKAGE_BUGREPORT}")
set(CPACK_SOURCE_GENERATOR "TGZ;TXZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\\\.git/"
    "/build/"
    "/\\\\.claude/"
    "\\\\.key\\\\.txt$"
    "/venv/"
    "/models/"
)
set(CPACK_GENERATOR "TGZ")

include(CPack)

# ======================================================================
#  Dependency check (warns about missing optional deps with install cmds)
# ======================================================================

include(cmake/CheckDependencies.cmake)
mnemon_check_dependencies()

# ======================================================================
#  Feature summary (autoconf-style configure output)
# ======================================================================

include(cmake/FeatureSummary.cmake)
mnemon_print_summary()
