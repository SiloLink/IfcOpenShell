#
# Input variables:
# - `CGAL_INCLUDE_DIR`
# - `GMP_INCLUDE_DIR`
# - `GMP_LIBRARY_DIR`
# - `MPFR_INCLUDE_DIR`
# - `MPFR_LIBRARY_DIR`
# If input variables are not specified, try to find CGAL config.
# Input variables could also be provided as environment variables.
#
# Output targets:
# - `IFCOPENSHELL_CGAL`
#

if(TARGET IFCOPENSHELL_CGAL)
    return()
endif()

UNIFY_ENVVARS_AND_CACHE(CGAL_INCLUDE_DIR)
UNIFY_ENVVARS_AND_CACHE(GMP_INCLUDE_DIR)
UNIFY_ENVVARS_AND_CACHE(GMP_LIBRARY_DIR)
UNIFY_ENVVARS_AND_CACHE(MPFR_INCLUDE_DIR)
UNIFY_ENVVARS_AND_CACHE(MPFR_LIBRARY_DIR)

if(CGAL_INCLUDE_DIR)
    find_library(libGMP NAMES gmp mpir PATHS ${GMP_LIBRARY_DIR} NO_DEFAULT_PATH)
    find_library(libMPFR NAMES mpfr PATHS ${MPFR_LIBRARY_DIR} NO_DEFAULT_PATH)

    file(STRINGS "${CGAL_INCLUDE_DIR}/CGAL/version.h" CGAL_VERSION_LINE REGEX "^#define CGAL_VERSION ")
    string(REGEX REPLACE "^#define CGAL_VERSION ([0-9.]+).*$" "\\1" CGAL_VERSION "${CGAL_VERSION_LINE}")

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(
        CGAL
        REQUIRED_VARS CGAL_INCLUDE_DIR libGMP libMPFR
        VERSION_VAR CGAL_VERSION
    )

    add_library(CGAL::CGAL INTERFACE IMPORTED)
    target_include_directories(CGAL::CGAL INTERFACE "${CGAL_INCLUDE_DIR}")
    target_include_directories(CGAL::CGAL INTERFACE "${GMP_INCLUDE_DIR}" "${MPFR_INCLUDE_DIR}")
    target_link_libraries(CGAL::CGAL INTERFACE "${libMPFR}" "${libGMP}")
else()
    # Annoyingly this is producing CMP0167 boost warnings, because it's unsetting cmake policies
    # and using FindBoost module. But there's nothing we can do about it,
    # since everything happens in the scope of CGAL config. I guess it's be resolved in CGAL 6.1.0.
    #
    # CGAL's CGAL_TweakFindBoost.cmake (pulled in by `find_package(CGAL CONFIG)`)
    # overwrites `Boost_USE_STATIC_LIBS` with its own cached default, clobbering
    # whatever this project (or the user) already set. Setting `CGAL_TweakFindBoost` to avoid it.
    set(CGAL_TweakFindBoost ON)
    find_package(CGAL CONFIG)
    if(NOT CGAL_FOUND)
        message(
            FATAL_ERROR
            "CGAL_SUPPORT enabled, but CGAL_INCLUDE_DIR wasn't provided and CGAL package couldn't be found."
        )
    endif()
    message(STATUS "CGAL: found config at '${CGAL_DIR}'.")
endif()

# Adding another `IFCOPENSHELL_CGAL` target, because we want to add compile definitions to it,
# but in `CGALconfig.cmake` `CGAL::CGAL` is an alias, so you can't add properties to it.
add_library(IFCOPENSHELL_CGAL INTERFACE)
target_link_libraries(IFCOPENSHELL_CGAL INTERFACE CGAL::CGAL)
target_compile_definitions(IFCOPENSHELL_CGAL INTERFACE IFOPSH_WITH_CGAL)

# Both supplied include directories and package-config builds use the same
# ownership fix; never mutate a shared /deps or system CGAL installation.
get_target_property(cgal_target_includes CGAL::CGAL INTERFACE_INCLUDE_DIRECTORIES)
find_path(cgal_external_structure_include
    CGAL/Convex_decomposition_3/External_structure_builder.h
    HINTS ${CGAL_INCLUDE_DIR} ${CGAL_INCLUDE_DIRS} ${cgal_target_includes}
    NO_DEFAULT_PATH NO_CACHE REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/CGALExternalStructure.cmake")
set(cgal_corrected_include "${CMAKE_BINARY_DIR}/cgal-include")
ifcopenshell_cgal_external_structure("${cgal_external_structure_include}" "${cgal_corrected_include}")
target_include_directories(IFCOPENSHELL_CGAL BEFORE INTERFACE
    $<BUILD_INTERFACE:${cgal_corrected_include}>)
install(TARGETS IFCOPENSHELL_CGAL EXPORT ${IFCOPENSHELL_EXPORT_TARGETS})
