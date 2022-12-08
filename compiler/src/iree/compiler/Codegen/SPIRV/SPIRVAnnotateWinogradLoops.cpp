// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/SPIRV/Utils.h"
#include "iree/compiler/Codegen/Utils/GPUUtils.h"

namespace mlir {
namespace iree_compiler {

namespace {

class SPIRVAnnotateWinogradLoopsPass final
    : public SPIRVAnnotateWinogradLoopsBase<SPIRVAnnotateWinogradLoopsPass> {
 public:
  SPIRVAnnotateWinogradLoopsPass() = default;
  SPIRVAnnotateWinogradLoopsPass(const SPIRVAnnotateWinogradLoopsPass &pass) =
      default;

  void runOnOperation() override {
    func::FuncOp funcOp = getOperation();
    SmallVector<scf::ForOp, 4> forOps;
    funcOp.walk([&](scf::ForOp forOp) {
      if (!isTiledAndDistributedLoop(forOp)) forOps.push_back(forOp);
    });

    MLIRContext *context = &getContext();
    OpBuilder builder(context);
    const char *attrName = getSPIRVDistributeAttrName();
    for (auto forOp : llvm::enumerate(forOps)) {
      if (forOp.index() > kNumGPUDims) break;
      forOp.value()->setAttr(attrName, builder.getIndexAttr(forOp.index()));
    }
  }
};
}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createSPIRVAnnotateWinogradLoopsPass() {
  return std::make_unique<SPIRVAnnotateWinogradLoopsPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
