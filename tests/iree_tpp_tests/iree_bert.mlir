// RUN: iree-compile --iree-hal-target-backends=llvm-cpu \
// RUN: %s.frozen.linalg -o %s.vmfb \
// RUN: --iree-llvmcpu-enable-tpp-lowering-pipeline \
// RUN: --iree-llvmcpu-stack-allocation-limit=4816897

// RUN: iree-run-module --device=local-task --module=%s.vmfb \
// RUN: --executable_plugin=${IREE_BINARY_DIR}/samples/tpp_import/tpp_import_x86_64.so \
// RUN: --function=serving_default --task_worker_stack_size=481689700 \
// RUN: --input="32x8xi32=1" --input="32x8xi32=2" --input="32x8xi32=3" \
// RUN: --output_max_element_count=8 | FileCheck %s

// RUN: rm %s.vmfb

// CHECK: EXEC @serving_default
// CHECK: result[0]: hal.buffer_view
// CHECK{LITERAL}: 32x128xf32=[-0.999989 -0.0727283 -0.999813 0.935952 -0.997543 0.862308 -0.998703 -0.429097...]
// CHECK: result[1]: hal.buffer_view
// CHECK{LITERAL}: 32x8x128xf32=[[-0.476127 0.0938244 -4.54325 -1.59823 0.788881 -0.0364897 0.6787 0.212291...]
// CHECK: result[2]: hal.buffer_view
// CHECK{LITERAL}: 32x8x128xf32=[[-0.485089 -0.290502 -2.64978 -4.06134 1.02866 -1.03537 -0.273451 -0.423676...]
// CHECK: result[3]: hal.buffer_view
// CHECK{LITERAL}: 32x128xf32=[-0.999989 -0.0727283 -0.999813 0.935952 -0.997543 0.862308 -0.998703 -0.429097...]
// CHECK: result[4]: hal.buffer_view
// CHECK{LITERAL}: 32x8x128xf32=[[-0.485089 -0.290502 -2.64978 -4.06134 1.02866 -1.03537 -0.273451 -0.423676...]