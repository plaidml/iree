// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/local/executable_plugin.h"

extern int iree_xsmm_brgemm_dispatch(void* context, void* params,
                                         void* reserved);
extern int iree_xsmm_gemm_dispatch(void* context, void* params,
                                         void* reserved);
extern int iree_xsmm_unary_dispatch(void* context, void* params,
                                    void* reserved);
extern int iree_xsmm_binary_dispatch(void* context, void* params,
                                    void* reserved);

extern int iree_xsmm_brgemm_invoke(void* context, void* params,
                                       void* reserved);
extern int iree_xsmm_gemm_invoke(void* context, void* params,
                                       void* reserved);
extern int iree_xsmm_unary_invoke(void* context, void* params, void* reserved);
extern int iree_xsmm_binary_invoke(void* context, void* params, void* reserved);

static iree_hal_executable_plugin_status_t tpp_import_plugin_load(
    const iree_hal_executable_plugin_environment_v0_t* environment,
    size_t param_count, const iree_hal_executable_plugin_string_pair_t* params,
    void** out_self) {
  *out_self = NULL;  // no state in this plugin
  return iree_hal_executable_plugin_ok_status();
}

static void tpp_import_plugin_unload(void* self) {}

static iree_hal_executable_plugin_status_t tpp_import_provider_resolve(
    void* self, const iree_hal_executable_plugin_resolve_params_v0_t* params,
    iree_hal_executable_plugin_resolution_t* out_resolution) {

  *out_resolution = 0;
  bool any_required_not_found = false;

  void **out_fn_ptrs = params->out_fn_ptrs;
  void **out_fn_contexts = params->out_fn_contexts;

  for (size_t i = 0; i < params->count; i++) {
    const char *symbol_name = params->symbol_names[i];
    bool is_optional =
        iree_hal_executable_plugin_import_is_optional(symbol_name);
    if (is_optional) ++symbol_name;

    if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_brgemm_dispatch") == 0) {
      out_fn_ptrs[i] = iree_xsmm_brgemm_dispatch;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_gemm_dispatch") == 0) {
      out_fn_ptrs[i] = iree_xsmm_gemm_dispatch;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_unary_dispatch") == 0) {
      out_fn_ptrs[i] = iree_xsmm_unary_dispatch;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_binary_dispatch") == 0) {
      out_fn_ptrs[i] = iree_xsmm_binary_dispatch;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_brgemm_invoke") == 0) {
      out_fn_ptrs[i] = iree_xsmm_brgemm_invoke;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_gemm_invoke") == 0) {
      out_fn_ptrs[i] = iree_xsmm_gemm_invoke;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_unary_invoke") == 0) {
      out_fn_ptrs[i] = iree_xsmm_unary_invoke;
      out_fn_contexts[i] = NULL;
    } else if (iree_hal_executable_plugin_strcmp(symbol_name,
                                          "xsmm_binary_invoke") == 0) {
      out_fn_ptrs[i] = iree_xsmm_binary_invoke;
      out_fn_contexts[i] = NULL;
    } else {
      if (is_optional) {
        *out_resolution |=
            IREE_HAL_EXECUTABLE_PLUGIN_RESOLUTION_MISSING_OPTIONAL;
      } else {
        any_required_not_found = true;
      }
    }
  }

  return any_required_not_found
             ? iree_hal_executable_plugin_status_from_code(
                   IREE_HAL_EXECUTABLE_PLUGIN_STATUS_NOT_FOUND)
             : iree_hal_executable_plugin_ok_status();
}

IREE_HAL_EXECUTABLE_PLUGIN_EXPORT const iree_hal_executable_plugin_header_t**
iree_hal_executable_plugin_query(
    iree_hal_executable_plugin_version_t max_version, void* reserved) {
  static const iree_hal_executable_plugin_header_t header = {
      // Declares what library version is present: newer runtimes may support
      // loading older plugins but newer plugins cannot load on older runtimes.
      .version = IREE_HAL_EXECUTABLE_PLUGIN_VERSION_LATEST,
      // Name and description are used for tracing/logging/diagnostics.
      .name = "tpp_import",
      .description = "Plugin to resolve XSMM APIs used by TPP passes ",
      // Standalone plugins must declare that they are standalone so that the
      // runtime can verify support.
      .features = IREE_HAL_EXECUTABLE_PLUGIN_FEATURE_STANDALONE,
      // Standalone plugins don't support sanitizers.
      .sanitizer = IREE_HAL_EXECUTABLE_PLUGIN_SANITIZER_NONE,
  };
  static const iree_hal_executable_plugin_v0_t plugin = {
      .header = &header,
      .load = tpp_import_plugin_load,
      .unload = tpp_import_plugin_unload,
      .resolve = tpp_import_provider_resolve,
  };
  return max_version <= IREE_HAL_EXECUTABLE_PLUGIN_VERSION_LATEST
             ? (const iree_hal_executable_plugin_header_t**)&plugin
             : NULL;
}
