// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>

#include "iree/base/api.h"
#include "iree/hal/local/executable_loader.h"

extern int iree_xsmm_brgemm_dispatch_f32(void* context, void* params,
                                         void* reserved);
extern int iree_xsmm_matmul_dispatch_f32(void* context, void* params,
                                         void* reserved);
extern int iree_xsmm_unary_dispatch(void* context, void* params,
                                    void* reserved);

extern int iree_xsmm_brgemm_invoke_f32(void* context, void* params,
                                       void* reserved);
extern int iree_xsmm_matmul_invoke_f32(void* context, void* params,
                                       void* reserved);
extern int iree_xsmm_unary_invoke(void* context, void* params, void* reserved);

static iree_status_t iree_samples_tpp_import_provider_resolve(
    void* self, iree_string_view_t symbol_name, void** out_fn_ptr,
    void** out_fn_context) {
  // Xsmm dispatch API.
  if (iree_string_view_equal(symbol_name,
                             IREE_SV("xsmm_brgemm_dispatch_f32"))) {
    *out_fn_ptr = iree_xsmm_brgemm_dispatch_f32;
    return iree_ok_status();
  }
  if (iree_string_view_equal(symbol_name,
                             IREE_SV("xsmm_matmul_dispatch_f32"))) {
    *out_fn_ptr = iree_xsmm_matmul_dispatch_f32;
    return iree_ok_status();
  }
  if (iree_string_view_equal(symbol_name, IREE_SV("xsmm_unary_dispatch"))) {
    *out_fn_ptr = iree_xsmm_unary_dispatch;
    return iree_ok_status();
  }

  // Xsmm invoke API.
  if (iree_string_view_equal(symbol_name, IREE_SV("xsmm_brgemm_invoke_f32"))) {
    *out_fn_ptr = iree_xsmm_brgemm_invoke_f32;
    return iree_ok_status();
  }
  if (iree_string_view_equal(symbol_name, IREE_SV("xsmm_matmul_invoke_f32"))) {
    *out_fn_ptr = iree_xsmm_matmul_invoke_f32;
    return iree_ok_status();
  }
  if (iree_string_view_equal(symbol_name, IREE_SV("xsmm_unary_invoke"))) {
    *out_fn_ptr = iree_xsmm_unary_invoke;
    return iree_ok_status();
  }
  return iree_status_from_code(IREE_STATUS_NOT_FOUND);
}

iree_hal_executable_import_provider_t iree_samples_tpp_import_provider(void) {
  iree_hal_executable_import_provider_t import_provider = {
      .self = NULL,
      .resolve = iree_samples_tpp_import_provider_resolve,
  };
  return import_provider;
}
