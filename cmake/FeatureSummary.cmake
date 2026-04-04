#[=======================================================================[.rst:
FeatureSummary
--------------

Print an autoconf-style feature summary at the end of configuration.

Usage::

  include(cmake/FeatureSummary.cmake)
  mnemon_print_summary()

#]=======================================================================]

function(mnemon_print_summary)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS " ${PROJECT_NAME} ${PROJECT_VERSION} configuration summary")
    message(STATUS "============================================================")
    message(STATUS "")
    message(STATUS "  Build type ............ : ${CMAKE_BUILD_TYPE}")
    message(STATUS "  Install prefix ........ : ${CMAKE_INSTALL_PREFIX}")
    message(STATUS "  C compiler ............ : ${CMAKE_C_COMPILER}")
    message(STATUS "  C flags ............... : ${CMAKE_C_FLAGS}")
    message(STATUS "  C standard ............ : C${CMAKE_C_STANDARD}")
    message(STATUS "")
    message(STATUS "  Required dependencies:")
    message(STATUS "    SQLite3 ............. : ${SQLite3_VERSION}")
    message(STATUS "    llama.cpp ........... : ${LLAMA_FOUND}")
    message(STATUS "")
    message(STATUS "  Vendored dependencies:")
    message(STATUS "    cJSON ............... : bundled")
    message(STATUS "    LMDB ................ : bundled")
    message(STATUS "    usearch ............. : bundled (header-only)")
    message(STATUS "")
    message(STATUS "  Optional features:")

    if(ENABLE_SYSTEMD)
        message(STATUS "    systemd notify ...... : yes")
    else()
        message(STATUS "    systemd notify ...... : no")
    endif()

    if(ENABLE_NUMA)
        message(STATUS "    NUMA support ........ : yes")
    else()
        message(STATUS "    NUMA support ........ : no")
    endif()

    if(ENABLE_CURL)
        message(STATUS "    libcurl (extraction)  : yes")
    else()
        message(STATUS "    libcurl (extraction)  : no")
    endif()

    message(STATUS "")
    message(STATUS "  SIMD support:")

    if(HAVE_AVX2)
        message(STATUS "    AVX2 ................ : yes")
    else()
        message(STATUS "    AVX2 ................ : no")
    endif()

    if(HAVE_AVX512)
        message(STATUS "    AVX-512 ............. : yes")
    else()
        message(STATUS "    AVX-512 ............. : no")
    endif()

    message(STATUS "")
    message(STATUS "  Directories:")
    message(STATUS "    prefix .............. : ${CMAKE_INSTALL_PREFIX}")
    message(STATUS "    bindir .............. : ${CMAKE_INSTALL_FULL_BINDIR}")
    message(STATUS "    sysconfdir .......... : ${MNEMON_SYSCONFDIR}")
    message(STATUS "    localstatedir ....... : ${MNEMON_LOCALSTATEDIR}")
    message(STATUS "    mandir .............. : ${CMAKE_INSTALL_FULL_MANDIR}")
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "")
endfunction()
