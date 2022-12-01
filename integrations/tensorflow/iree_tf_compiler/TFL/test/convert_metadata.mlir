// RUN: iree-opt-tflite --split-input-file --pass-pipeline='builtin.module(iree-tflite-convert-module-metadata,func.func(iree-tflite-convert-function-metadata))' %s | FileCheck %s

module attributes {tfl.schema_version = 3 : i32} {
  // CHECK: func.func @main(
  // CHECK-SAME: %arg0: tensor<?xf32> {iree.identifier = "input0"},
  // CHECK-SAME: %arg1: tensor<?xf32> {iree.identifier = "input1"}
  // CHECK-SAME: ) -> (
  // CHECK-SAME: tensor<?xf32> {iree.identifier = "output0"},
  // CHECK-SAME: tensor<?xf32> {iree.identifier = "output1"})
  func.func @main(%arg0: tensor<?xf32>, %arg1: tensor<?xf32>) -> (tensor<?xf32>, tensor<?xf32>) attributes {
    tf.entry_function = {inputs = "input0,input1", outputs = "output0,output1"}
  } {
    return %arg0, %arg1 : tensor<?xf32>, tensor<?xf32>
  }
}
