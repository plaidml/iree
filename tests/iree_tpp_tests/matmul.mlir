// RUN: iree-compile --iree-hal-target-backends=llvm-cpu %s \
// RUN: -o %s.vmfb --iree-llvmcpu-enable-tpp-lowering-pipeline \
// RUN: --iree-llvmcpu-stack-allocation-limit=2097152

// RUN: iree-run-module --device=local-task --module=%s.vmfb \
// RUN: --function=entry --task_worker_stack_size=2097152 \
// RUN: --input="4x8xf32=1.0" --input="8x2xf32=2.0" | \
// RUN: FileCheck %s

// RUN: rm %s.vmfb

!arg0_tensor_t = tensor<4x8xf32>
!arg1_tensor_t = tensor<8x2xf32>
!out_tensor_t = tensor<4x2xf32>

func.func @entry(%A: !arg0_tensor_t, %B: !arg1_tensor_t) -> !out_tensor_t {
  %C = tensor.empty() : !out_tensor_t
  %D = linalg.matmul ins(%A, %B: !arg0_tensor_t, !arg1_tensor_t) outs(%C: !out_tensor_t) -> !out_tensor_t
  return %D : !out_tensor_t
}
//
// CHECK: EXEC @entry
// CHECK: result[0]: hal.buffer_view 
// CHECK: 4x2xf32=[16 16][16 16][16 16][16 16]
//