cmake_minimum_required(VERSION 3.21)
project(岩心光谱智能解译系统 VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(APP_TARGET_NAME core_spectrum_interpretation_app)
set(APP_DISPLAY_NAME "岩心光谱智能解译系统")
set(APP_EXECUTABLE_NAME "${APP_DISPLAY_NAME}")

# ---------------------------------------------------------------------------
# Qt6
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Charts Concurrent)

# ---------------------------------------------------------------------------
# Eigen3  (header-only — find system package or fall back to bundled)
# ---------------------------------------------------------------------------
find_package(Eigen3 QUIET)
if(NOT Eigen3_FOUND)
    # Try vcpkg / conan layout
    find_path(EIGEN3_INCLUDE_DIR NAMES Eigen/Core
              HINTS ${CMAKE_SOURCE_DIR}/third_party/eigen
                    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
              PATH_SUFFIXES eigen3)
    if(EIGEN3_INCLUDE_DIR)
        add_library(Eigen3::Eigen INTERFACE IMPORTED)
        target_include_directories(Eigen3::Eigen INTERFACE ${EIGEN3_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "Eigen3 not found. Install via vcpkg (eigen) or set EIGEN3_INCLUDE_DIR.")
    endif()
endif()

# ---------------------------------------------------------------------------
# OSQP  (static lib + headers)
# ---------------------------------------------------------------------------
find_package(osqp QUIET)
if(NOT osqp_FOUND)
    find_library(OSQP_LIB NAMES osqp osqpstatic
                 HINTS ${CMAKE_SOURCE_DIR}/third_party/osqp/lib
                       ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib)
    find_path(OSQP_INCLUDE_DIR NAMES osqp/osqp.h osqp.h
              HINTS ${CMAKE_SOURCE_DIR}/third_party/osqp/include
                    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include)
    if(OSQP_LIB AND OSQP_INCLUDE_DIR)
        add_library(osqp::osqp STATIC IMPORTED)
        set_target_properties(osqp::osqp PROPERTIES
            IMPORTED_LOCATION ${OSQP_LIB}
            INTERFACE_INCLUDE_DIRECTORIES ${OSQP_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "OSQP not found. Install via vcpkg (osqp) or set OSQP_LIB / OSQP_INCLUDE_DIR.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/spectralanalyzer.h
    src/spectralanalyzer.cpp
    src/spectrallibrary.h
    src/spectrumdialog.h
    src/envidataset.h
    src/envidataset.cpp
    src/osqp_eigen_utils.h
)

set(SPECTRAL_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/resources/光谱数据库")

if(WIN32)
    qt_add_executable(${APP_TARGET_NAME} WIN32 ${SOURCES})
else()
    qt_add_executable(${APP_TARGET_NAME} ${SOURCES})
endif()

target_include_directories(${APP_TARGET_NAME} PRIVATE
    src
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include
)

target_compile_definitions(${APP_TARGET_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(${APP_TARGET_NAME} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Charts
    Qt6::Concurrent
    Eigen3::Eigen
    osqp::osqp
)

set_target_properties(${APP_TARGET_NAME} PROPERTIES
    OUTPUT_NAME "${APP_EXECUTABLE_NAME}"
)

if(EXISTS "${SPECTRAL_LIBRARY_DIR}")
    add_custom_command(TARGET ${APP_TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${APP_TARGET_NAME}>/光谱数据库"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${SPECTRAL_LIBRARY_DIR}"
                "$<TARGET_FILE_DIR:${APP_TARGET_NAME}>/光谱数据库"
        COMMENT "Copying built-in spectral library..."
    )
endif()

# ---------------------------------------------------------------------------
# Windows: run windeployqt automatically after build
# ---------------------------------------------------------------------------
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS ${Qt6_DIR}/../../../bin)
    if(WINDEPLOYQT)
        add_custom_command(TARGET ${APP_TARGET_NAME} POST_BUILD
            COMMAND ${WINDEPLOYQT}
                --no-translations
                --no-system-d3d-compiler
                $<TARGET_FILE:${APP_TARGET_NAME}>
            COMMENT "Running windeployqt..."
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${APP_TARGET_NAME} DESTINATION .)
if(EXISTS "${SPECTRAL_LIBRARY_DIR}")
    install(DIRECTORY "${SPECTRAL_LIBRARY_DIR}/" DESTINATION "光谱数据库")
endif()
