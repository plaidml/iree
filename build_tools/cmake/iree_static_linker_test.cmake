# Copyright 2021 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# iree_static_linker_test()
#
# Creates a test test for the statically linked libraries generated by the
# llvm-cpu compiler target backend and executed using the local-sync runtime
# HAL driver.
#
# Parameters:
#   NAME: Name of the target
#   SRC: mlir source file to be compiled to an IREE module.
#   INPUT_TYPE: Module input type, assuming native c/c++ data types
#   HAL_TYPE: HAL data type, see runtime/src/iree/hal/buffer_view.h for possible
#       options.
#   STATIC_LIB_PREFIX: llvm static library prefix.
#   ENTRY_FUNCTION: Entry function call.
#   EMITC: Uses EmitC to output C code instead of VM bytecode.
#   COMPILER_FLAGS: additional flags to pass to the compiler. Bytecode output
#       format and backend flags are passed automatically.
#   FUNCTION_INPUTS: Module input in the format of mxnxcx<datatype>.
#   LABELS: Additional labels to apply to the test. The package path and
#       "driver=local-sync" are added automatically.
#   TARGET_CPU_FEATURES: If specified, a string passed as argument to
#       --iree-llvmcpu-target-cpu-features.
#
# Example:
#   iree_static_linker_test(
#     NAME
#       edge_detection_test
#     SRC
#       "edge_detection.mlir"
#     STATIC_LIB_PREFIX
#       edge_detection_linked_llvm_cpu
#     ENTRY_FUNCTION
#       "edge_detect_sobel_operator"
#     FUNCTION_INPUTS
#       "1x128x128xf32"
#     COMPILER_FLAGS
#       "--iree-input-type=mhlo"
#   )
function(iree_static_linker_test)
  if(NOT IREE_BUILD_TESTS)
    return()
  endif()

  # See comment in iree_check_test about this condition.
  if(NOT IREE_BUILD_COMPILER AND NOT IREE_HOST_BIN_DIR)
    return()
  endif()

  if(NOT (IREE_TARGET_BACKEND_LLVM_CPU OR IREE_HOST_BIN_DIR) OR
     NOT IREE_HAL_DRIVER_LOCAL_SYNC)
    return()
  endif()

  cmake_parse_arguments(
    _RULE
    "EMITC"
    "NAME;SRC;DRIVER;STATIC_LIB_PREFIX;ENTRY_FUNCTION;INPUT_TYPE;HAL_TYPE"
    "COMPILER_FLAGS;LABELS;TARGET_CPU_FEATURES;FUNCTION_INPUTS"
    ${ARGN}
  )

  if(_RULE_EMITC AND
     NOT (IREE_OUTPUT_FORMAT_C OR IREE_HOST_BIN_DIR))
    return()
  endif()

  iree_package_name(_PACKAGE_NAME)
  iree_package_ns(_PACKAGE_NS)
  set(_NAME "${_PACKAGE_NAME}_${_RULE_NAME}")

  # Set up static library name.
  set(_O_FILE_NAME "${_RULE_NAME}.o")
  set(_H_FILE_NAME "${_RULE_NAME}.h")

  # Set common iree-compile flags
  set(_COMPILER_ARGS ${_RULE_COMPILER_FLAGS})
  list(APPEND _COMPILER_ARGS "--iree-hal-target-backends=llvm-cpu")
  if(_RULE_TARGET_CPU_FEATURES)
    list(APPEND _COMPILER_ARGS "--iree-llvmcpu-target-cpu-features=${_RULE_TARGET_CPU_FEATURES}")
  endif()

  if(_RULE_EMITC)
    set(_C_FILE_NAME "${_RULE_NAME}_emitc.h")
    iree_c_module(
      NAME
        ${_RULE_NAME}_emitc
      SRC
        "${_RULE_SRC}"
      FLAGS
        ${_COMPILER_ARGS}
      H_FILE_OUTPUT
        "${_C_FILE_NAME}"
      STATIC_LIB_PATH
        "${_O_FILE_NAME}"
      NO_RUNTIME

    )
  else()  # bytecode module path
    # Generate the embed data with the bytecode module.
    set(_MODULE_NAME "${_RULE_NAME}_module")
    set(_EMBED_H_FILE_NAME ${_MODULE_NAME}_c.h)
    iree_bytecode_module(
      NAME
        ${_MODULE_NAME}
      SRC
        "${_RULE_SRC}"
      FLAGS
        ${_COMPILER_ARGS}
      STATIC_LIB_PATH
        "${_O_FILE_NAME}"
      C_IDENTIFIER
        "${_NAME}"
      PUBLIC
    )
  endif(_RULE_EMITC)

  set(_LIB_NAME "${_NAME}_lib")
  add_library(${_LIB_NAME}
    STATIC
    ${_O_FILE_NAME}
  )
  SET_TARGET_PROPERTIES(
    ${_LIB_NAME}
    PROPERTIES
    LINKER_LANGUAGE C
  )

  # If any of these arguments are not specified, only compilation will be
  # tested.
  if(NOT _RULE_FUNCTION_INPUTS OR
     NOT _RULE_ENTRY_FUNCTION OR
     NOT _RULE_STATIC_LIB_PREFIX)
    return()
  endif()

  # Set alias for this static library to be used later in the function.
  add_library(${_PACKAGE_NS}::${_RULE_NAME}_lib ALIAS ${_LIB_NAME})

  # Process module input configs to be used in the c template.
  # Example:
  #   FUNCTION_INPUTS "1x28x28x1xf32"
  #     _INPUT_NUM  1
  #     _INPUT_DIM_STR 4
  #     _INPUT_SIZE_STR 768
  #     _INPUT_SHAPE_STR of {1, 28, 28, 1},
  #   FUNCTION_INPUTS "4xf32,f32"
  #     _INPUT_NUM  2
  #     _INPUT_DIM_STR 1, 1
  #     _INPUT_SIZE_STR 4, 1
  #     _INPUT_SHAPE_STR of {4}, {1},
  string(REPLACE "," ";"  _INPUTS_LIST "${_RULE_FUNCTION_INPUTS}")
  list(LENGTH _INPUTS_LIST _INPUT_NUM)

  set(_INPUT_DIM_LIST)
  set(_INPUT_SIZE_LIST)
  set(_INPUT_SHAPE_STR)
  set(_INPUT_DIM_MAX 0)
  foreach(_INPUT_ENTRY ${_INPUTS_LIST})
    # Separate the function input into shape (m,n,c) and format
    string(REPLACE "x" ";" _INPUT_ENTRY_LIST "${_INPUT_ENTRY}")
    list(POP_BACK _INPUT_ENTRY_LIST _INPUT_FORMAT)

    # Process shape to compute the size (number of elements) and dimention.
    # The results are stored into lists to support multiple inputs
    if(NOT _INPUT_ENTRY_LIST)  # single entry input
      set(_INPUT_ENTRY_LIST "1")
    endif()

    string(REPLACE ";" ", " _INPUT_ENTRY_STR "${_INPUT_ENTRY_LIST}")
    set(_INPUT_SHAPE_STR "${_INPUT_SHAPE_STR}\{${_INPUT_ENTRY_STR}\}, ")

    list(LENGTH _INPUT_ENTRY_LIST _INPUT_DIM)
    list(APPEND _INPUT_DIM_LIST ${_INPUT_DIM})
    if(_INPUT_DIM GREATER _INPUT_DIM_MAX)
      set(_INPUT_DIM_MAX ${_INPUT_DIM})
    endif()
    set(_INPUT_SIZE 1)
    foreach(_INPUT_DIM_VAL ${_INPUT_ENTRY_LIST})
      math(EXPR _INPUT_SIZE "${_INPUT_SIZE} * ${_INPUT_DIM_VAL}")
    endforeach()
    list(APPEND _INPUT_SIZE_LIST ${_INPUT_SIZE})
  endforeach(_INPUT_ENTRY)
  string(REPLACE ";" ", " _INPUT_DIM_STR "${_INPUT_DIM_LIST}")
  string(REPLACE ";" ", " _INPUT_SIZE_STR "${_INPUT_SIZE_LIST}")

  # Process input and HAL data format based on the last input entry.
  set(IREE_INPUT_TYPE)
  set(IREE_HAL_TYPE)
  if("${_INPUT_FORMAT}" STREQUAL "f32")
    set(IREE_INPUT_TYPE "float")
    set(IREE_HAL_TYPE "IREE_HAL_ELEMENT_TYPE_FLOAT_32")
  elseif("${_INPUT_FORMAT}" STREQUAL "i32")
    set(IREE_INPUT_TYPE "int32_t")
    set(IREE_HAL_TYPE "IREE_HAL_ELEMENT_TYPE_SINT_32")
  elseif("${_INPUT_FORMAT}" STREQUAL "i16")
    set(IREE_INPUT_TYPE "int16_t")
    set(IREE_HAL_TYPE "IREE_HAL_ELEMENT_TYPE_SINT_16")
  elseif("${_INPUT_FORMAT}" STREQUAL "i8")
    set(IREE_INPUT_TYPE "int8_t")
    set(IREE_HAL_TYPE "IREE_HAL_ELEMENT_TYPE_SINT_8")
  elseif("${_INPUT_FORMAT}" STREQUAL "ui8")
    set(IREE_INPUT_TYPE "uint8_t")
    set(IREE_HAL_TYPE "IREE_HAL_ELEMENT_TYPE_UINT_8")
  else()
    message(SEND_ERROR "Unsupported format ${_INPUT_FORMAT}")
  endif()

  # Generate the source file.
  # TODO(scotttodd): Move to build time instead of configure time?
  set(IREE_STATIC_LIB_HDR "\"${_H_FILE_NAME}\"")
  set(IREE_STATIC_LIB_QUERY_FN "${_RULE_STATIC_LIB_PREFIX}_library_query")
  set(IREE_MODULE_HDR "${_EMBED_H_FILE_NAME}")
  set(IREE_MODULE_CREATE_FN "${_NAME}_create\(\)")
  set(IREE_EMITC_HDR "${_C_FILE_NAME}")
  set(IREE_MODULE_MAIN_FN "\"module.${_RULE_ENTRY_FUNCTION}\"")
  set(IREE_INPUT_NUM "${_INPUT_NUM}")
  set(IREE_INPUT_DIM_MAX "${_INPUT_DIM_MAX}")
  set(IREE_INPUT_DIM_ARR "\{${_INPUT_DIM_STR}\}")
  set(IREE_INPUT_SIZE_ARR "\{${_INPUT_SIZE_STR}\}")
  set(IREE_INPUT_SHAPE_ARR "\{${_INPUT_SHAPE_STR}\}")
  set(IREE_EXE_NAME "\"${_RULE_NAME}\"")
  configure_file(
    "${IREE_ROOT_DIR}/build_tools/cmake/static_linker_test.c.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${_RULE_NAME}.c"
  )

  iree_cc_binary(
    NAME
      ${_RULE_NAME}_run
    SRCS
      "${CMAKE_CURRENT_BINARY_DIR}/${_RULE_NAME}.c"
    DEPS
      ::${_RULE_NAME}_lib
      iree::runtime
      iree::hal::drivers::local_sync::sync_driver
      iree::hal::local::loaders::static_library_loader
  )

  if(_RULE_EMITC)
    target_link_libraries(${_NAME}_run
        PRIVATE
          iree_vm_shims_emitc
          ${_NAME}_emitc
    )
    target_compile_definitions(${_NAME}_run
      PRIVATE
      EMITC_IMPLEMENTATION=\"${_C_FILE_NAME}\"
    )
  else()
    target_link_libraries(${_NAME}_run PRIVATE ${_NAME}_module_c)
  endif()

  add_dependencies(iree-test-deps "${_NAME}_run")

  iree_native_test(
    NAME
      ${_RULE_NAME}
    SRC
      ::${_RULE_NAME}_run
    DRIVER
      local-sync
    LABELS
      ${_RULE_LABELS}
      ${_RULE_TARGET_CPU_FEATURES}
  )
endfunction()
