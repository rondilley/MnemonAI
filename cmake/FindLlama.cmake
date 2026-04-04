#[=======================================================================[.rst:
FindLlama
---------

Find the llama.cpp library (C API).

Imported Targets
^^^^^^^^^^^^^^^^

``Llama::Llama``
  The llama.cpp library, if found.

Result Variables
^^^^^^^^^^^^^^^^

``LLAMA_FOUND``
  True if llama.cpp was found.
``LLAMA_INCLUDE_DIRS``
  Include directories for llama.cpp.
``LLAMA_LIBRARIES``
  Libraries to link against.
``LLAMA_VERSION``
  Version string, if available.

Hints
^^^^^

``LLAMA_ROOT``
  Environment or CMake variable pointing to llama.cpp install prefix.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(_LLAMA QUIET llama)
endif()

# Find header
find_path(LLAMA_INCLUDE_DIR
    NAMES llama.h
    HINTS
        ${LLAMA_ROOT}
        $ENV{LLAMA_ROOT}
        ${_LLAMA_INCLUDE_DIRS}
    PATH_SUFFIXES include
)

# Find library
find_library(LLAMA_LIBRARY
    NAMES llama
    HINTS
        ${LLAMA_ROOT}
        $ENV{LLAMA_ROOT}
        ${_LLAMA_LIBRARY_DIRS}
    PATH_SUFFIXES lib lib64
)

# Version from pkg-config if available
if(_LLAMA_VERSION)
    set(LLAMA_VERSION ${_LLAMA_VERSION})
endif()

find_package_handle_standard_args(Llama
    REQUIRED_VARS LLAMA_LIBRARY LLAMA_INCLUDE_DIR
    VERSION_VAR LLAMA_VERSION
)

if(LLAMA_FOUND AND NOT TARGET Llama::Llama)
    add_library(Llama::Llama UNKNOWN IMPORTED)
    set_target_properties(Llama::Llama PROPERTIES
        IMPORTED_LOCATION "${LLAMA_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LLAMA_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(LLAMA_INCLUDE_DIR LLAMA_LIBRARY)

set(LLAMA_INCLUDE_DIRS ${LLAMA_INCLUDE_DIR})
set(LLAMA_LIBRARIES ${LLAMA_LIBRARY})
