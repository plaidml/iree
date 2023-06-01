# Copyright 2022 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# set(_IMPORTS_NAME "iree_samples_tpp_import")
# add_library(${_IMPORTS_NAME} STATIC)
# target_sources(${_IMPORTS_NAME}
#   PRIVATE
#     tpp_import.c
# )
# target_link_libraries(${_IMPORTS_NAME}
#   tpp_c_runner_utils
#   iree_base_base
# )

function(tpp_import_library _ARCH)
  set(_NAME iree_samples_tpp_import_${_ARCH})
  if (_ARCH STREQUAL "arm_64")
    set(LLVM_ARCH "aarch64")
  else()
    set(LLVM_ARCH "${_ARCH}")
  endif()
  add_custom_command(
    OUTPUT
      tpp_import_${_ARCH}.o
    DEPENDS
      tpp_import.c
      ${IREE_CLANG_TARGET}
    COMMAND ${IREE_CLANG_TARGET}
      -target ${LLVM_ARCH}-unknown-unknown-eabi-elf
      -isystem ${IREE_BINARY_DIR}/third_party/llvm-project/llvm/lib/clang/17/include
      -std=c17
      -fPIC
      -ffreestanding
      -fvisibility=hidden
      -fno-plt
      -fno-rtti
      -fno-exceptions
      -fdata-sections
      -ffunction-sections
      -funique-section-names
      -g
      -I ${IREE_SOURCE_DIR}/runtime/src/
      -c ${CMAKE_CURRENT_SOURCE_DIR}/tpp_import.c
      -o ${CMAKE_CURRENT_BINARY_DIR}/tpp_import_${_ARCH}.o
    VERBATIM
  )
  add_custom_command(
    OUTPUT
      tpp_import_${_ARCH}.so
    DEPENDS
      tpp_import_${_ARCH}.o
      tpp_c_runner_utils
      ${IREE_LLD_TARGET}
    COMMAND ${IREE_LLD_TARGET}
      -flavor gnu
      --build-id=none
      -shared
      --no-undefined
      --no-allow-shlib-undefined
      --allow-multiple-definition
      --gc-sections
      -z now
      -z relro
      --discard-all
      --icf=all
      --ignore-data-address-equality
      --ignore-function-address-equality
      --hash-style=sysv
      --strip-debug
      ${CMAKE_CURRENT_BINARY_DIR}/tpp_import_${_ARCH}.o
      --rpath ${IREE_BINARY_DIR}/llvm-external-projects/mlir-tpp-dialects/runtime/
      --rpath ${IREE_BINARY_DIR}/llvm-external-projects/mlir-tpp-dialects/
      -L ${IREE_BINARY_DIR}/llvm-external-projects/mlir-tpp-dialects/runtime/
      -L ${IREE_BINARY_DIR}/llvm-external-projects/mlir-tpp-dialects/
      -o ${CMAKE_CURRENT_BINARY_DIR}/tpp_import_${_ARCH}.so
      -l tpp_c_runner_utils
      -l xsmm

    VERBATIM
  )
  add_custom_target(${_NAME} DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/tpp_import_${_ARCH}.so
    tpp_c_runner_utils
  )
endfunction()

# Build the tpp_import_*.so files for each architecture we target.
#tpp_import_library(arm_64)
tpp_import_library(x86_64)
