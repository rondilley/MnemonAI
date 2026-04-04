#[=======================================================================[.rst:
CheckDependencies
-----------------

Checks for required and optional dependencies at configure time.
Prints actionable warnings with install commands for missing packages.

Called from the main CMakeLists.txt after all find_package() calls.

#]=======================================================================]

function(mnemon_check_dependencies)
    set(_missing "")
    set(_warnings "")

    # ---- Required: llama.cpp ----
    if(NOT TARGET Llama::Llama AND NOT TARGET llama)
        list(APPEND _missing "llama.cpp")
        list(APPEND _warnings
            "  llama.cpp NOT FOUND (required for embedding generation)")
        list(APPEND _warnings
            "    Install: git clone https://github.com/ggerganov/llama.cpp")
        list(APPEND _warnings
            "             cd llama.cpp && mkdir build && cd build")
        list(APPEND _warnings
            "             cmake .. -DCMAKE_INSTALL_PREFIX=$ENV{HOME}/.local && make -j && make install")
        list(APPEND _warnings
            "    Then: cmake .. -DCMAKE_PREFIX_PATH=$ENV{HOME}/.local")
    endif()

    # ---- Optional: libcurl ----
    if(NOT ENABLE_CURL)
        list(APPEND _warnings
            "  libcurl NOT FOUND (entity extraction + auto model download disabled)")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND _warnings
                "    Install: sudo apt-get install libcurl4-openssl-dev    # Debian/Ubuntu")
            list(APPEND _warnings
                "          or sudo dnf install libcurl-devel               # Fedora/RHEL")
        endif()
    endif()

    # ---- Optional: libsystemd ----
    if(NOT ENABLE_SYSTEMD)
        list(APPEND _warnings
            "  libsystemd NOT FOUND (sd_notify disabled -- not needed for --stdio mode)")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            list(APPEND _warnings
                "    Install: sudo apt-get install libsystemd-dev          # Debian/Ubuntu")
        endif()
    endif()

    # ---- Print results ----
    if(_missing)
        message(STATUS "")
        message(STATUS "!!! MISSING REQUIRED DEPENDENCIES !!!")
        foreach(_msg ${_warnings})
            message(STATUS "${_msg}")
        endforeach()
        message(STATUS "")
        message(FATAL_ERROR "Cannot continue without required dependencies.")
    endif()

    list(LENGTH _warnings _warn_count)
    if(_warn_count GREATER 0)
        message(STATUS "")
        message(STATUS "Optional dependencies not found (functionality reduced):")
        foreach(_msg ${_warnings})
            message(STATUS "${_msg}")
        endforeach()
        message(STATUS "")
    endif()
endfunction()
