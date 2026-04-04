#[=======================================================================[.rst:
FindLMDB
--------

Find the system-installed LMDB library (optional, for use instead of vendored).

Imported Targets
^^^^^^^^^^^^^^^^

``LMDB::LMDB``
  The LMDB library, if found.

Result Variables
^^^^^^^^^^^^^^^^

``LMDB_FOUND``
  True if LMDB was found on the system.
``LMDB_INCLUDE_DIRS``
  Include directories.
``LMDB_LIBRARIES``
  Libraries to link against.

#]=======================================================================]

include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(_LMDB QUIET lmdb)
endif()

find_path(LMDB_INCLUDE_DIR
    NAMES lmdb.h
    HINTS
        ${LMDB_ROOT}
        $ENV{LMDB_ROOT}
        ${_LMDB_INCLUDE_DIRS}
    PATH_SUFFIXES include
)

find_library(LMDB_LIBRARY
    NAMES lmdb
    HINTS
        ${LMDB_ROOT}
        $ENV{LMDB_ROOT}
        ${_LMDB_LIBRARY_DIRS}
    PATH_SUFFIXES lib lib64
)

find_package_handle_standard_args(LMDB
    REQUIRED_VARS LMDB_LIBRARY LMDB_INCLUDE_DIR
)

if(LMDB_FOUND AND NOT TARGET LMDB::LMDB)
    add_library(LMDB::LMDB UNKNOWN IMPORTED)
    set_target_properties(LMDB::LMDB PROPERTIES
        IMPORTED_LOCATION "${LMDB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LMDB_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(LMDB_INCLUDE_DIR LMDB_LIBRARY)

set(LMDB_INCLUDE_DIRS ${LMDB_INCLUDE_DIR})
set(LMDB_LIBRARIES ${LMDB_LIBRARY})
