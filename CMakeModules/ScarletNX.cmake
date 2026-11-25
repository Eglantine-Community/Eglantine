# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

if (NOT LINUX OR ANDROID OR NOT ARCHITECTURE_x86_64 OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "ScarletNX currently requires a native x86-64 Linux build")
endif()

include(ExternalProject)
include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_program(SCARLETNX_CLANG NAMES clang REQUIRED)
find_program(SCARLETNX_CLANGXX NAMES clang++ REQUIRED)
find_program(SCARLETNX_PATCH NAMES patch REQUIRED)
find_program(SCARLETNX_LLVM_CONFIG
    NAMES llvm-config llvm-config-23 llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19
    DOC "llvm-config for the ScarletNX CPU recompiler" REQUIRED)

execute_process(COMMAND "${SCARLETNX_LLVM_CONFIG}" --libfiles core native passes
    COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE snx_llvm_libs OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${SCARLETNX_LLVM_CONFIG}" --system-libs core native passes
    COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE snx_llvm_system_libs OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(snx_llvm_libs UNIX_COMMAND "${snx_llvm_libs}")
separate_arguments(snx_llvm_system_libs UNIX_COMMAND "${snx_llvm_system_libs}")

set(snx_prefix "${PROJECT_BINARY_DIR}/scarletnx")
set(snx_build "${snx_prefix}/build")
set(snx_source "${snx_prefix}/source")
set(snx_archive "${snx_build}/recompiler-library/${CMAKE_STATIC_LIBRARY_PREFIX}scarletnx-recompiler${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(snx_patch "${PROJECT_SOURCE_DIR}/externals/scarletnx/fastmem.patch")
set(snx_profile_patch "${PROJECT_SOURCE_DIR}/externals/scarletnx/profile.patch")
set(snx_inprocess_patch "${PROJECT_SOURCE_DIR}/externals/scarletnx/inprocess.patch")
set(snx_target_reuse_patch "${PROJECT_SOURCE_DIR}/externals/scarletnx/target-reuse.patch")

# Only the CPU library uses Clang; the rest of Eglantine retains its selected compiler.
ExternalProject_Add(scarletnx-project
    PREFIX "${snx_prefix}"
    SOURCE_DIR "${snx_source}"
    BINARY_DIR "${snx_build}"
    URL https://codeload.github.com/scarletnx/ScarletNX/tar.gz/f16810ce9c
    URL_HASH SHA512=6fabe5d61bdcb01f4409793219fae9c40d558cf8c962c1529ab8edf4bc32b265cba9be23217b3b9104784239aeeff0ed86a90068219cc9e5c20ce0183a9d2cf1
    PATCH_COMMAND "${SCARLETNX_PATCH}" --batch --forward --fuzz=0 -p1 -i "${snx_patch}"
          COMMAND "${SCARLETNX_PATCH}" --batch --forward --fuzz=0 -p1 -i "${snx_profile_patch}"
          COMMAND "${SCARLETNX_PATCH}" --batch --forward --fuzz=0 -p1 -i "${snx_inprocess_patch}"
          COMMAND "${SCARLETNX_PATCH}" --batch --forward --fuzz=0 -p1 -i "${snx_target_reuse_patch}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    LIST_SEPARATOR |
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=${SCARLETNX_CLANG}
        -DCMAKE_CXX_COMPILER=${SCARLETNX_CLANGXX}
        "-DCMAKE_C_COMPILER_LAUNCHER=$<JOIN:${CMAKE_C_COMPILER_LAUNCHER},|>"
        -DSNX_LLVM_CONFIG=${SCARLETNX_LLVM_CONFIG}
        -DSNX_BUILD_NX=OFF
        -DSNX_BUILD_GNUABI=OFF
        -DSNX_BUILD_3DS=OFF
        -DSNX_BUILD_VITA=OFF
        -DSNX_BUILD_AGB=OFF
        -DSNX_BUILD_GUI=OFF
        -DSNX_BUILD_TESTING=OFF
        -DSNX_DEPLOY=OFF
        -DSNX_A64_DISASM=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --parallel 2
                  --target scarletnx-recompiler
    BUILD_BYPRODUCTS "${snx_archive}"
    INSTALL_COMMAND "")
# Re-extract before applying an updated patch to avoid patching an already modified tree.
ExternalProject_Add_StepDependencies(scarletnx-project download
    "${snx_patch}" "${snx_profile_patch}" "${snx_inprocess_patch}" "${snx_target_reuse_patch}")

add_library(ScarletNX::recompiler STATIC IMPORTED GLOBAL)
set_target_properties(ScarletNX::recompiler PROPERTIES
    IMPORTED_LOCATION "${snx_archive}"
    INTERFACE_LINK_LIBRARIES "${snx_llvm_libs};${snx_llvm_system_libs};Threads::Threads;${CMAKE_DL_LIBS};m")
add_dependencies(ScarletNX::recompiler scarletnx-project)

set(SCARLETNX_INCLUDE_DIRS "${snx_build}/recompiler-library/include;${snx_source}/src")
install(FILES "${snx_source}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/eglantine/scarletnx")
