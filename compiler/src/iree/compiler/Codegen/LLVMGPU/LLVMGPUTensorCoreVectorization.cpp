// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree-dialects/Dialect/LinalgExt/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Dialect/LoweringConfig.h"
#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Utils/MarkerUtils.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Dialect/Vector/Transforms/VectorTransforms.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"

using mlir::iree_compiler::IREE::LinalgExt::LinalgVectorizationPattern;
using mlir::iree_compiler::IREE::LinalgExt::VectorizationPatterns;

namespace mlir {
namespace iree_compiler {

/// Flag defined in Passes.cpp.
extern llvm::cl::opt<bool> llvmgpuUseMMASync;

//====---------------------------------------------------------------------===//
// Patterns for vectorization
//====---------------------------------------------------------------------===//

static void populateVectorizationPatterns(RewritePatternSet &patterns) {
  IREE::LinalgExt::LinalgTransformationFilter f(
      StringAttr::get(patterns.getContext(), getVectorizeMarker()));
  VectorizationPatterns<linalg::FillOp, linalg::GenericOp>::insert(patterns, f);
  patterns.add<LinalgVectorizationPattern>(
      patterns.getContext(), f.addOpFilter<linalg::ContractionOpInterface>());
  vector::populateVectorTransferPermutationMapLoweringPatterns(patterns);
  vector::populateVectorReductionToContractPatterns(patterns);
}

static Optional<SmallVector<int64_t>> unrollOrder(Operation *op) {
  auto contract = dyn_cast<vector::ContractionOp>(op);
  if (!contract) return llvm::None;
  SmallVector<int64_t> order;
  // Pick an unrolling order that will allow tensorcore operation to reuse LHS
  // register. This is needed to get good performance on sm_80 target.
  // First make reduction the outer dimensions.
  for (auto iter : llvm::enumerate(contract.getIteratorTypes())) {
    if (vector::isReductionIterator(iter.value())) {
      order.push_back(iter.index());
    }
  }

  llvm::SmallDenseSet<int64_t> dims;
  for (AffineExpr expr : contract.getIndexingMapsArray()[0].getResults()) {
    dims.insert(expr.cast<AffineDimExpr>().getPosition());
  }
  // Then parallel dimensions that are part of Lhs as we want to re-use Lhs.
  for (auto iter : llvm::enumerate(contract.getIteratorTypes())) {
    if (vector::isParallelIterator(iter.value()) && dims.count(iter.index())) {
      order.push_back(iter.index());
    }
  }
  // Then the remaining parallel loops.
  for (auto iter : llvm::enumerate(contract.getIteratorTypes())) {
    if (vector::isParallelIterator(iter.value()) && !dims.count(iter.index())) {
      order.push_back(iter.index());
    }
  }
  return order;
}

// Merge transpose op into the transfer read op. Transpose are not supported on
// MMA types but MMA load can transpose the matrix when loading.
struct CombineTransferReadOpBroadcast final
    : public OpRewritePattern<vector::BroadcastOp> {
  using OpRewritePattern<vector::BroadcastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(vector::BroadcastOp op,
                                PatternRewriter &rewriter) const override {
    auto transferReadOp =
        op.getSource().getDefiningOp<vector::TransferReadOp>();
    if (!transferReadOp || transferReadOp.getMask() ||
        transferReadOp.hasOutOfBoundsDim()) {
      return failure();
    }
    int64_t rankDiff =
        op.getVectorType().getRank() - transferReadOp.getVectorType().getRank();
    SmallVector<AffineExpr> exprs(rankDiff, rewriter.getAffineConstantExpr(0));
    ArrayRef<AffineExpr> originalExpr =
        transferReadOp.getPermutationMap().getResults();
    exprs.append(originalExpr.begin(), originalExpr.end());
    AffineMap newMap =
        AffineMap::get(transferReadOp.getPermutationMap().getNumDims(),
                       transferReadOp.getPermutationMap().getNumSymbols(),
                       exprs, op.getContext());
    ArrayAttr inBounds = rewriter.getBoolArrayAttr(
        SmallVector<bool>(op.getVectorType().getRank(), true));
    rewriter.replaceOpWithNewOp<vector::TransferReadOp>(
        op, op.getType(), transferReadOp.getSource(),
        transferReadOp.getIndices(), newMap, transferReadOp.getPadding(),
        transferReadOp.getMask(), inBounds);
    return success();
  }
};

static Optional<SmallVector<int64_t>> getGPUTCNativeVectorSize(Operation *op) {
  // Currently hardcode the size of wmma operation. When more cases are
  // supported this should be picked based on what the backend supports.
  int64_t m = 16;
  int64_t n = llvmgpuUseMMASync ? 8 : 16;
  if (auto contract = dyn_cast<vector::ContractionOp>(op)) {
    int64_t k;
    if (llvmgpuUseMMASync)
      k = contract.getLhsType().getElementType().isF16() ? 8 : 4;
    else
      k = contract.getLhsType().getElementType().isF16() ? 16 : 8;
    SmallVector<int64_t> nativeSize(contract.getIteratorTypes().size() - 3, 1);
    nativeSize.append({m, n, k});
    return nativeSize;
  }
  if (auto writeOp = dyn_cast<vector::TransferWriteOp>(op)) {
    SmallVector<int64_t> nativeSize(writeOp.getVectorType().getRank() - 2, 1);
    nativeSize.append({m, n});
    return nativeSize;
  }
  if (auto readOp = dyn_cast<vector::TransferReadOp>(op)) {
    // Transfer read ops may need different shapes based on how they are being
    // used. For simplicity just match the shape used by the extract strided op.
    VectorType sliceType;
    for (Operation *users : op->getUsers()) {
      auto extract = dyn_cast<vector::ExtractStridedSliceOp>(users);
      if (!extract) return llvm::None;
      auto vecType = extract.getResult().getType().cast<VectorType>();
      if (sliceType && sliceType != vecType) return llvm::None;
      sliceType = vecType;
    }
    return llvm::to_vector<>(sliceType.getShape());
  }
  if ((OpTrait::hasElementwiseMappableTraits(op) && op->getNumResults() == 1)) {
    if (auto vecType = op->getResultTypes()[0].dyn_cast<VectorType>()) {
      SmallVector<int64_t> nativeSize(vecType.getRank() - 2, 1);
      // Map elementwise ops to the output shape.
      nativeSize.append({m, n});
      return nativeSize;
    }
  }
  return llvm::None;
}

static void populateVectorUnrollPatterns(RewritePatternSet &patterns) {
  vector::populateVectorUnrollPatterns(
      patterns, vector::UnrollVectorOptions()
                    .setNativeShapeFn(getGPUTCNativeVectorSize)
                    .setUnrollTraversalOrderFn(unrollOrder));
}

namespace {
struct LLVMGPUTensorCoreVectorizationPass
    : public LLVMGPUTensorCoreVectorizationBase<
          LLVMGPUTensorCoreVectorizationPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<vector::VectorDialect>();
  }
  void runOnOperation() override {
    auto funcOp = getOperation();
    MLIRContext *context = &getContext();
    {
      // Step 1. Vectorize
      RewritePatternSet vectorizationPatterns(context);
      populateVectorizationPatterns(vectorizationPatterns);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(vectorizationPatterns)))) {
        return signalPassFailure();
      }

      // Fold consumer add ops into the contraction op itself.
      RewritePatternSet canonicalizationPatterns(context);
      vector::ContractionOp::getCanonicalizationPatterns(
          canonicalizationPatterns, context);
      canonicalizationPatterns.insert<CombineTransferReadOpBroadcast>(
          funcOp.getContext());
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(canonicalizationPatterns)))) {
        return signalPassFailure();
      }

      RewritePatternSet vectorUnrollPatterns(context);
      populateVectorUnrollPatterns(vectorUnrollPatterns);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(vectorUnrollPatterns)))) {
        return signalPassFailure();
      }
    }
  }
};
}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createLLVMGPUTensorCoreVectorizationPass() {
  return std::make_unique<LLVMGPUTensorCoreVectorizationPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
