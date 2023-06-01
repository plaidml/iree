// RUN: iree-compile --iree-hal-target-backends=llvm-cpu %s \
// RUN: -o %s.vmfb --iree-llvmcpu-enable-tpp-lowering-pipeline \
// RUN: --iree-llvmcpu-stack-allocation-limit=2097152

// RUN: iree-run-module --device=local-task --module=%s.vmfb \
// RUN: --executable_plugin=${IREE_BINARY_DIR}/samples/tpp_import/tpp_import_x86_64.so \
// RUN: --function=entry --task_worker_stack_size=2097152 \
// RUN: | FileCheck %s

// RUN: rm %s.vmfb

!out_tensor_t = tensor<4x4xf32>
!in0_tensor_t = tensor<1x4x8xf32>
!in1_tensor_t = tensor<1x8x4xf32>

func.func @brgemmtpp(%A: !in0_tensor_t,
                     %B: !in1_tensor_t) -> !out_tensor_t {
  %C = tensor.empty() : !out_tensor_t
  %D = linalg.batch_reduce_matmul ins(%A, %B: !in0_tensor_t, !in1_tensor_t) outs(%C: !out_tensor_t) -> !out_tensor_t
  return %D: !out_tensor_t
}

func.func @entry() -> tensor<4x4xf32> {
  %c0 = arith.constant 0 : index
  %d1 = arith.constant -1.0 : f32

  // Initialize various tensors.
  %da = arith.constant dense<[[
        [ 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1, 8.1 ],
        [ 1.2, 2.2, 3.2, 4.2, 5.2, 6.2, 7.2, 8.2 ],
        [ 1.3, 2.3, 3.3, 4.3, 5.3, 6.3, 7.3, 8.3 ],
        [ 1.4, 2.4, 3.4, 4.4, 5.4, 6.4, 7.4, 8.4 ]
  ]]> : tensor<1x4x8xf32>

  %db = arith.constant dense<[[
        [ 10.1, 11.1, 12.1, 13.1 ],
        [ 10.2, 11.2, 12.2, 13.2 ],
        [ 10.3, 11.3, 12.3, 13.3 ],
        [ 10.4, 11.4, 12.4, 13.4 ],
        [ 10.5, 11.5, 12.5, 13.5 ],
        [ 10.6, 11.6, 12.6, 13.6 ],
        [ 10.7, 11.7, 12.7, 13.7 ],
        [ 10.8, 11.8, 12.8, 13.8 ]
  ]]> : tensor<1x8x4xf32>

  // Call kernel.
  %0 = call @brgemmtpp(%da, %db) : (tensor<1x4x8xf32>, tensor<1x8x4xf32>) -> tensor<4x4xf32>
  return %0 : tensor<4x4xf32>
}
//
// CHECK: EXEC @entry
// CHECK: result[0]: hal.buffer_view 
// CHECK: 4x4xf32=[388.76 425.56 462.36 499.16][397.12 434.72 472.32 509.92][405.48 443.88 482.28 520.68][413.84 453.04 492.24 531.44]
//