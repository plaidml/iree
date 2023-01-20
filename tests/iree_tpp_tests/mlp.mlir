// RUN: iree-compile --iree-hal-target-backends=llvm-cpu %s \
// RUN: -o %s.vmfb --iree-llvmcpu-enable-tpp-lowering-pipeline \
// RUN: --iree-llvmcpu-stack-allocation-limit=2097152

// RUN: iree-run-module --device=local-task --module=%s.vmfb \
// RUN: --function=entry --task_worker_stack_size=2097152 \
// RUN: --input="4x8xf32=1.0" --input="8x2xf32=2.0" \
// RUN: --input="1x2xf32=3.0" | \
// RUN: FileCheck %s

// RUN: rm %s.vmfb

#map0 = affine_map<(d0, d1) -> (0, d1)>
#map1 = affine_map<(d0, d1) -> (d0, d1)>

!arg0_tensor_t = tensor<4x8xf32>
!arg1_tensor_t = tensor<8x2xf32>
!arg2_tensor_t = tensor<1x2xf32>
!out_tensor_t = tensor<4x2xf32>

func.func @entry(%arg0: !arg0_tensor_t, %arg1: !arg1_tensor_t, %arg2: !arg2_tensor_t) -> !out_tensor_t {
  %output = tensor.empty() : !out_tensor_t
  %1 = linalg.generic {indexing_maps = [#map0, #map1], iterator_types = ["parallel", "parallel"]} ins(%arg2 : !arg2_tensor_t) outs(%output : !out_tensor_t) {
    ^bb0(%arg9: f32, %arg10: f32):
      linalg.yield %arg9 : f32
  } -> !out_tensor_t
  %2 = linalg.matmul ins(%arg0, %arg1 : !arg0_tensor_t, !arg1_tensor_t) outs(%1 : !out_tensor_t) -> !out_tensor_t
  %c0 = arith.constant 0.0 : f32
  %3 = linalg.generic {indexing_maps = [#map1], iterator_types = ["parallel", "parallel"]} outs(%2 : !out_tensor_t) {
    ^bb0(%arg9: f32):
      %16 = arith.maxf %arg9, %c0 : f32
      linalg.yield %16 : f32
  } -> !out_tensor_t
  return %3 : !out_tensor_t
}

//
// CHECK: EXEC @entry
// CHECK: result[0]: hal.buffer_view 
// CHECK: 4x2xf32=[19 19][19 19][19 19][19 19]
//