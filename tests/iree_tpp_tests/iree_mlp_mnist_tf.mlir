// RUN: iree-compile --iree-hal-target-backends=llvm-cpu \
// RUN: %s.frozen.mhlo -o %s.vmfb --iree-input-type=mhlo \
// RUN: --iree-llvmcpu-enable-tpp-lowering-pipeline \
// RUN: --iree-llvmcpu-stack-allocation-limit=4816897

// RUN: iree-run-module --device=local-task --module=%s.vmfb \
// RUN: --function=serving_default --task_worker_stack_size=481689700 \
// RUN: --input=@%s.test_image_label_4.npy | \
// RUN: FileCheck %s

// RUN: rm %s.vmfb

// Since we are feeding the input image for label 4, expected output is label 4.

// CHECK: EXEC @serving_default
// CHECK: result[0]: hal.buffer_view
// CHECK: 1xi32=4