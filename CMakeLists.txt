cmake_minimum_required(VERSION 3.16)
project(ras3d VERSION 0.1.2 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── compiler flags ─────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -O2)
add_compile_definitions(_GNU_SOURCE)

# ── OpenMP ─────────────────────────────────────────────────────────────────
find_package(OpenMP REQUIRED)

# ── libtiff ────────────────────────────────────────────────────────────────
find_package(TIFF REQUIRED)

# ── libgeotiff ─────────────────────────────────────────────────────────────
find_path(GEOTIFF_INCLUDE_DIR geotiff.h
    PATHS /usr/include/geotiff /usr/local/include/geotiff /usr/include)
find_library(GEOTIFF_LIBRARY NAMES geotiff)
if(NOT GEOTIFF_INCLUDE_DIR OR NOT GEOTIFF_LIBRARY)
    message(FATAL_ERROR "libgeotiff not found. Install libgeotiff-dev.")
endif()

# ── libhdf5 ────────────────────────────────────────────────────────────────
find_package(HDF5 REQUIRED COMPONENTS C HL)

# ── libm ───────────────────────────────────────────────────────────────────
find_library(MATH_LIB m)

# ── library sources ────────────────────────────────────────────────────────
set(RAS3D_SOURCES
    src/api.c
    src/grass_compat.c
    src/region.c
    src/backend_geotiff.c
    src/backend_hdf5.c
)

# Shared library with SONAME libras3d.so.0
add_library(ras3d_shared SHARED ${RAS3D_SOURCES})
set_target_properties(ras3d_shared PROPERTIES
    OUTPUT_NAME   ras3d
    VERSION       ${PROJECT_VERSION}
    SOVERSION     0
)

add_library(ras3d_static STATIC ${RAS3D_SOURCES})
set_target_properties(ras3d_static PROPERTIES OUTPUT_NAME ras3d)

foreach(tgt ras3d_shared ras3d_static)
    target_include_directories(${tgt} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_include_directories(${tgt} PRIVATE
        ${GEOTIFF_INCLUDE_DIR}
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(${tgt} PRIVATE
        TIFF::TIFF
        ${GEOTIFF_LIBRARY}
        ${HDF5_C_LIBRARIES}
        ${HDF5_HL_LIBRARIES}
        OpenMP::OpenMP_C
        ${MATH_LIB}
    )
    target_compile_definitions(${tgt} PRIVATE ${HDF5_DEFINITIONS})
endforeach()

# ── tests ──────────────────────────────────────────────────────────────────
option(BUILD_TESTING "Build data-path-dependent tests" ON)
if(BUILD_TESTING)
    enable_testing()
    foreach(test_name test_geotiff test_hdf5)
        add_executable(${test_name} tests/${test_name}.c)
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${GEOTIFF_INCLUDE_DIR}
            ${HDF5_INCLUDE_DIRS}
        )
        target_link_libraries(${test_name} PRIVATE ras3d_static)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# Self-contained tests (no external data): always built.
enable_testing()
add_executable(test_write_georef tests/test_write_georef.c)
target_include_directories(test_write_georef PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${GEOTIFF_INCLUDE_DIR}
)
target_link_libraries(test_write_georef PRIVATE ras3d_static TIFF::TIFF
    ${GEOTIFF_LIBRARY} ${MATH_LIB})
add_test(NAME test_write_georef COMMAND test_write_georef)

# ── install ────────────────────────────────────────────────────────────────
include(GNUInstallDirs)   # must come before configure_file so includedir is set

# ── pkg-config file ────────────────────────────────────────────────────────
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/ras3d.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/ras3d.pc
    @ONLY
)

install(TARGETS ras3d_shared ras3d_static
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ras3d DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ras3d.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
install(FILES python/ras3d.py python/ras3d_grass_shim.py python/ras3d_write.py
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/python3/dist-packages
)

# ── summary ────────────────────────────────────────────────────────────────
message(STATUS "ras3d ${PROJECT_VERSION} configuration:")
message(STATUS "  TIFF       : ${TIFF_LIBRARIES}")
message(STATUS "  GeoTIFF    : ${GEOTIFF_LIBRARY}")
message(STATUS "  HDF5       : ${HDF5_C_LIBRARIES}")
message(STATUS "  OpenMP     : ${OpenMP_C_FLAGS}")
message(STATUS "  Install to : ${CMAKE_INSTALL_PREFIX}")
