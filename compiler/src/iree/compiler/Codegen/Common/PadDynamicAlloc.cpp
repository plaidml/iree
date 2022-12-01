// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"

namespace mlir {
namespace iree_compiler {

/// If a value is defined by `%dim = affine_max(0, %src)` kind of op return
/// `%src` otherwise return `%dim`.
/// This is useful to handle common pattern generated by bufferization to
/// compute alloc sizes.
static Value skipAffineMaxZero(Value dim) {
  auto affineMax = dim.getDefiningOp<AffineMaxOp>();
  if (!affineMax) return dim;
  for (AffineExpr expr : affineMax.getMap().getResults()) {
    if (auto cst = expr.dyn_cast<AffineConstantExpr>()) {
      if (cst.getValue() == 0) continue;
    } else if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>()) {
      if (symExpr.getPosition() == 0) continue;
    }
    return dim;
  }
  return *affineMax.getSymbolOperands().begin();
}

static LogicalResult padAlloc(memref::AllocOp allocOp) {
  OpBuilder builder(allocOp);
  SmallVector<int64_t> shape = llvm::to_vector(allocOp.getType().getShape());
  SmallVector<OpFoldResult> sizes;
  size_t dynamicDimIdx = 0;
  for (int64_t &dimSize : shape) {
    if (dimSize != ShapedType::kDynamic) {
      sizes.push_back(builder.getIndexAttr(dimSize));
      continue;
    }
    Value dim = allocOp.getDynamicSizes()[dynamicDimIdx++];
    dim = skipAffineMaxZero(dim);
    auto ub = linalg::getConstantUpperBoundForIndex(dim);
    if (failed(ub)) {
      return allocOp.emitOpError(
          "unexpected allocation without upper bound shapes");
    }
    dimSize = *ub;
    sizes.push_back(dim);
  }
  if (dynamicDimIdx == 0) return success();
  Type elType = allocOp.getType().getElementType();
  MemRefType allocType = MemRefType::get(
      shape, elType, {}, allocOp.getType().getMemorySpaceAsInt());
  Location loc = allocOp.getLoc();
  Value paddedAlloc = builder.create<memref::AllocOp>(loc, allocType);
  SmallVector<OpFoldResult> offsets(shape.size(), builder.getIndexAttr(0));
  SmallVector<OpFoldResult> strides(shape.size(), builder.getIndexAttr(1));
  Value subview = builder.create<memref::SubViewOp>(loc, paddedAlloc, offsets,
                                                    sizes, strides);
  replaceMemrefUsesAndPropagateType(allocOp, subview, builder);
  allocOp->erase();
  return success();
}

namespace {

struct PadDynamicAllocPass : public PadDynamicAllocBase<PadDynamicAllocPass> {
  void runOnOperation() override {
    auto funcOp = getOperation();
    SmallVector<memref::AllocOp> sharedMemAllocs;
    // Collect all the alloc operations.
    funcOp.walk(
        [&](memref::AllocOp allocOp) { sharedMemAllocs.push_back(allocOp); });
    for (memref::AllocOp alloc : sharedMemAllocs) {
      if (failed(padAlloc(alloc))) return signalPassFailure();
    }
  }
};
}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>> createPadDynamicAlloc() {
  return std::make_unique<PadDynamicAllocPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
