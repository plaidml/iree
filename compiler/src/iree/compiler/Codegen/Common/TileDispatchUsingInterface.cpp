// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <deque>

#include "iree/compiler/Codegen/Common/Transforms.h"
#include "iree/compiler/Codegen/Interfaces/PartitionableLoopsInterface.h"
#include "iree/compiler/Codegen/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/ViewLikeInterfaceUtils.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/SCF/Utils/Utils.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Transforms/Transforms.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/TilingInterface.h"
#include "mlir/Transforms/TopologicalSortUtils.h"

#define DEBUG_TYPE "tile-dispatch-using-interface"

namespace mlir {
namespace iree_compiler {

/// Method to check if a value is zero
static bool isZero(Value val) {
  OpFoldResult ofr = getAsOpFoldResult(val);
  return isConstantIntValue(ofr, 0);
}

/// Helper method to adjust the interchange vector to match the iteration
/// domain.
static SmallVector<unsigned> fillInterchangeVector(
    ArrayRef<unsigned> interchangeVector, size_t iterationDomainSize) {
  SmallVector<unsigned> filledVector = llvm::to_vector(interchangeVector);
  if (filledVector.size() < iterationDomainSize) {
    auto range = llvm::seq<unsigned>(filledVector.size(), iterationDomainSize);
    filledVector.append(range.begin(), range.end());
  }
  if (filledVector.size() > iterationDomainSize)
    filledVector.resize(iterationDomainSize);
  return filledVector;
}

/// Helper method to apply permutation to a vector
template <typename T>
static SmallVector<T> applyPermutationToVector(const SmallVector<T> &vector,
                                               ArrayRef<unsigned> interchange) {
  assert(interchange.size() == vector.size());
  return llvm::to_vector(
      llvm::map_range(interchange, [&](unsigned val) { return vector[val]; }));
}
/// Helper method to apply to invert a permutation.
static SmallVector<unsigned> invertPermutationVector(
    ArrayRef<unsigned> interchange) {
  SmallVector<unsigned> inversion(interchange.size());
  for (auto pos : llvm::enumerate(interchange)) {
    inversion[pos.value()] = pos.index();
  }
  return inversion;
}
/// Method to check if an interchange vector is a permutation.
static bool isPermutation(ArrayRef<unsigned> interchange) {
  llvm::SmallDenseSet<unsigned, 4> seenVals;
  for (auto val : interchange) {
    if (seenVals.count(val)) return false;
    seenVals.insert(val);
  }
  return seenVals.size() == interchange.size();
}

/// Given the `lb` and `step` of a loop, return the lower bound and step to use
/// for a distributed loop. Replace the iteration domain to
/// - lb_partitioned = lb + procId * step
/// - step_partitioned = step * nprocs
static std::tuple<Value, Value> getDistributeLBAndStep(OpBuilder &b,
                                                       Location loc, Value lb,
                                                       Value step, Value procId,
                                                       Value nprocs) {
  AffineExpr s0, s1, s2;
  bindSymbols(b.getContext(), s0, s1, s2);
  auto offsetMap = AffineMap::get(0, 3, {s0 + s1 * s2});
  auto stepMap = AffineMap::get(0, 2, {s0 * s1});
  Value distributeLB =
      makeComposedAffineApply(b, loc, offsetMap, ValueRange{lb, procId, step});
  Value distributeStep =
      makeComposedAffineApply(b, loc, stepMap, ValueRange{step, nprocs});
  return {distributeLB, distributeStep};
}

//===----------------------------------------------------------------------===//
// TileDispatchUsingSCFForOp pattern implementation.
//===----------------------------------------------------------------------===//

/// This implementation mirrors the implementation in
/// include/mlir/Dialect/SCF/Transforms/TileUsingInterface.h and
/// lib/Dialect/SCF/Transforms/TileUsingInterface.cpp. It is adapted to do
/// distribution and also use `flow.dispatch.tensor.load/store` instead of
/// `tensor.extract_slice/insert_slice`.

namespace {
// Till `scf.for` can take an `OpFoldResult` for lb, ub and step, use of `Range`
// with `OpFoldResult` causes lots of `OpFoldResult` -> `Value` conversion.
// This struct is a mirror of `Range` with `Value` type fields.
struct RangeVal {
  Value offset;
  Value size;
  Value stride;
};
}  // namespace

/// Generate an empty loop nest that represents the tiled loop nest shell.
/// - `loopRanges` specifies the lb, ub and step of the untiled iteration space.
/// - `tileSizeVals` is the tile sizes to use. Zero represent untiled loops.
/// - In `offsets` and `sizes` return the multi-dimensional offset and size of
/// the
///   tile processed within the inner most loop.
static SmallVector<scf::ForOp> generateTileLoopNest(
    OpBuilder &builder, Location loc, ArrayRef<RangeVal> loopRanges,
    ArrayRef<Value> tileSizeVals,
    ArrayRef<linalg::DistributionMethod> distributionMethod,
    ArrayRef<linalg::ProcInfo> procInfo, SmallVector<OpFoldResult> &offsets,
    SmallVector<OpFoldResult> &sizes) {
  assert(!loopRanges.empty() && "expected at least one loop range");
  assert(loopRanges.size() == tileSizeVals.size() &&
         "expected as many tile sizes as loop ranges");
  assert(loopRanges.size() == distributionMethod.size() &&
         "expected as many entries in distribution method list as number of "
         "loop ranges");
  OpBuilder::InsertionGuard guard(builder);
  SmallVector<scf::ForOp> loops;
  offsets.resize(loopRanges.size());
  sizes.resize(loopRanges.size());

  // The tile size to use (to avoid out of bounds access) is  minimum of
  // `tileSize` and `ub - iv`, where `iv` is the induction variable
  // of the tiled loop.
  AffineExpr s0, s1, d0;
  bindDims(builder.getContext(), d0);
  bindSymbols(builder.getContext(), s0, s1);
  AffineMap minMap = AffineMap::get(1, 2, {s0, s1 - d0}, builder.getContext());
  auto createBoundedTileSize = [&](Value iv, Value tileSize,
                                   Value size) -> OpFoldResult {
    if (isConstantIntValue(getAsOpFoldResult(tileSize), 1)) {
      return builder.getIndexAttr(1);
    }
    return makeComposedFoldedAffineMin(
        builder, loc, minMap, ArrayRef<OpFoldResult>{iv, tileSize, size});
  };

  unsigned procDim = 0;
  for (auto loopRange : llvm::enumerate(loopRanges)) {
    auto index = loopRange.index();
    Value lb = loopRange.value().offset;
    Value ub = loopRange.value().size;
    Value step = tileSizeVals[index];

    // No loops if tile size is zero. Set offset and size to the loop
    // offset and size.
    if (matchPattern(tileSizeVals[index], m_Zero())) {
      offsets[index] = lb;
      sizes[index] = ub;
      continue;
    }

    auto method = distributionMethod[index];
    if (method != linalg::DistributionMethod::None) {
      std::tie(lb, step) = getDistributeLBAndStep(builder, loc, lb, step,
                                                  procInfo[procDim].procId,
                                                  procInfo[procDim].nprocs);
      procDim++;
    }

    if (method == linalg::DistributionMethod::CyclicNumProcsEqNumIters) {
      offsets[index] = getAsOpFoldResult(lb);
      sizes[index] = createBoundedTileSize(lb, tileSizeVals[index], ub);
      continue;
    }

    auto loop = builder.create<scf::ForOp>(
        loc, lb, ub, step, ValueRange{},
        [&](OpBuilder &bodyBuilder, Location bodyLoc, Value iv,
            ValueRange /*iterArgs*/) {
          sizes[index] = createBoundedTileSize(iv, tileSizeVals[index], ub);
          builder.create<scf::YieldOp>(loc);
        });
    offsets[index] = loop.getInductionVar();
    loops.push_back(loop);
    builder.setInsertionPoint(loop.getBody()->getTerminator());
  }
  return loops;
}

/// Replace the `flow.dispatch.tensor.store` of the `untiledValue` with a tiled
/// `flow.dispatch.tensor.store` that writes only a tile of the result at
/// offsets given by `tiledOffsets` and sizes given by `tiledSizes`, using
/// `tiledValue` as the source.
static LogicalResult replaceStoreWithTiledVersion(
    RewriterBase &rewriter, OpResult untiledValue, OpResult tiledValue,
    ArrayRef<OpFoldResult> tileOffsets, ArrayRef<OpFoldResult> tileSizes) {
  SmallVector<IREE::Flow::DispatchTensorStoreOp> storeOps;
  for (OpOperand &use : untiledValue.getUses()) {
    auto storeOp = dyn_cast<IREE::Flow::DispatchTensorStoreOp>(use.getOwner());
    if (storeOp && storeOp.getValue() == use.get()) {
      storeOps.push_back(storeOp);
    }
  }
  if (storeOps.empty()) return success();
  if (storeOps.size() != 1) {
    return rewriter.notifyMatchFailure(untiledValue.getOwner(),
                                       "expected a single store for the op");
  }
  auto storeOp = storeOps.front();

  SmallVector<OpFoldResult> tileStrides(tileOffsets.size(),
                                        rewriter.getIndexAttr(1));
  SmallVector<OpFoldResult> combinedOffsets, combinedSizes, combinedStrides;
  SliceAndDynamicDims clonedSliceAndVals =
      cloneOffsetsSizesAndStrides(rewriter, storeOp);

  if (failed(mergeOffsetsSizesAndStrides(
          rewriter, storeOp.getLoc(), clonedSliceAndVals.offsets,
          clonedSliceAndVals.sizes, clonedSliceAndVals.strides,
          storeOp.getDroppedDims(), tileOffsets, tileSizes, tileStrides,
          combinedOffsets, combinedSizes, combinedStrides))) {
    return rewriter.notifyMatchFailure(
        storeOp, "failed to create tiled flow.dispatch.tensor.store op");
  }

  rewriter.create<IREE::Flow::DispatchTensorStoreOp>(
      storeOp.getLoc(), tiledValue, storeOp.getTarget(),
      clonedSliceAndVals.dynamicDims, combinedOffsets, combinedSizes,
      combinedStrides);
  rewriter.eraseOp(storeOp);
  return success();
}

/// Replace all `flow.dispatch.tensor.store` operations that use values produced
/// by `untiledOp` as source, with tiled stores, with tiled values produced by
/// `tiledOp`.
static LogicalResult replaceAllStoresWithTiledVersion(
    RewriterBase &rewriter, TilingInterface untiledOp,
    ArrayRef<OpFoldResult> offsets, ArrayRef<OpFoldResult> sizes,
    Operation *tiledOp) {
  for (auto result : llvm::enumerate(untiledOp->getResults())) {
    SmallVector<OpFoldResult> resultOffsets, resultSizes;
    if (failed(untiledOp.getResultTilePosition(rewriter, result.index(),
                                               offsets, sizes, resultOffsets,
                                               resultSizes))) {
      return rewriter.notifyMatchFailure(
          untiledOp, "failed to rewrite destructive update");
    }
    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(tiledOp->getBlock()->getTerminator());
    if (failed(replaceStoreWithTiledVersion(
            rewriter, result.value().cast<OpResult>(),
            tiledOp->getResult(result.index()).cast<OpResult>(), resultOffsets,
            resultSizes))) {
      return failure();
    }
  }
  return success();
}

namespace {

/// Result of the tiled operation.
struct TilingResult {
  Operation *tiledOp;
  SmallVector<scf::ForOp> loops;
  llvm::SmallBitVector tiledLoops;
  SmallVector<OpFoldResult> tileOffsets;
  SmallVector<OpFoldResult> tileSizes;
};

/// Pattern to tile an op that implements the `TilingInterface` using
/// `scf.for`+`flow.dispatch.tensor.load/store` for iterating over the tiles.
struct TileDispatchUsingSCFForOp
    : public OpInterfaceRewritePattern<TilingInterface> {
  /// Construct a generic pattern applied to all TilingInterface ops.
  TileDispatchUsingSCFForOp(MLIRContext *context,
                            linalg::LinalgTilingOptions options,
                            IREE::LinalgExt::LinalgTransformationFilter filter,
                            PatternBenefit benefit = 1)
      : OpInterfaceRewritePattern<TilingInterface>(context, benefit),
        options(std::move(options)),
        filter(std::move(filter)) {}

  /// Construct a generic pattern applied to `opName`.
  TileDispatchUsingSCFForOp(StringRef opName, MLIRContext *context,
                            linalg::LinalgTilingOptions options,
                            IREE::LinalgExt::LinalgTransformationFilter filter,
                            PatternBenefit benefit = 1)
      : OpInterfaceRewritePattern<TilingInterface>(context, benefit),
        options(std::move(options)),
        filter(std::move(filter)) {}

  /// `matchAndRewrite` implementation that returns the significant transformed
  /// pieces of IR.
  FailureOr<TilingResult> returningMatchAndRewrite(
      TilingInterface op, PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(TilingInterface op,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(op, rewriter);
  }

 private:
  /// Options to control tiling.
  linalg::LinalgTilingOptions options;

  /// Filter to control transformation.
  IREE::LinalgExt::LinalgTransformationFilter filter;
};
}  // namespace

FailureOr<TilingResult> TileDispatchUsingSCFForOp::returningMatchAndRewrite(
    TilingInterface op, PatternRewriter &rewriter) const {
  // Check for the filter and abort if needed.
  if (failed(filter.checkAndNotify(rewriter, op))) {
    return failure();
  }

  OpBuilder::InsertionGuard guard(rewriter);
  rewriter.setInsertionPointAfter(op);

  if (!options.tileSizeComputationFunction) {
    return rewriter.notifyMatchFailure(
        op, "missing tile size computation function");
  }

  // 1. Get the range of the loops that are represented by the operation.
  SmallVector<Range> iterationDomainOfr = op.getIterationDomain(rewriter);
  Location loc = op.getLoc();
  size_t numLoops = iterationDomainOfr.size();
  if (numLoops == 0) {
    return rewriter.notifyMatchFailure(
        op, "unable to tile op with no iteration domain");
  }
  auto iterationDomain =
      llvm::to_vector(llvm::map_range(iterationDomainOfr, [&](Range r) {
        return RangeVal{
            getValueOrCreateConstantIndexOp(rewriter, loc, r.offset),
            getValueOrCreateConstantIndexOp(rewriter, loc, r.size),
            getValueOrCreateConstantIndexOp(rewriter, loc, r.stride)};
      }));

  // 2. Materialize the tile sizes. Enforce the convention that "tiling by zero"
  // skips tiling a particular dimension. This convention is significantly
  // simpler to handle instead of adjusting affine maps to account for missing
  // dimensions.
  SmallVector<Value> tileSizeVector =
      options.tileSizeComputationFunction(rewriter, op);
  if (tileSizeVector.size() < numLoops) {
    auto zero = rewriter.create<arith::ConstantIndexOp>(op.getLoc(), 0);
    tileSizeVector.append(numLoops - tileSizeVector.size(), zero);
  }
  tileSizeVector.resize(numLoops);

  TilingResult tilingResult;
  tilingResult.tiledLoops.resize(numLoops, false);
  for (auto tileSize : llvm::enumerate(tileSizeVector)) {
    if (!isZero(tileSize.value())) {
      tilingResult.tiledLoops.set(tileSize.index());
    }
  }

  if (!tilingResult.tiledLoops.any()) {
    // Replace the filter on the untiled op itself.
    filter.replaceLinalgTransformationFilter(rewriter, op);
    return tilingResult;
  }

  SmallVector<Operation *> tiledImplementation;
  {
    SmallVector<OpFoldResult> offsets, sizes;
    // If there is an interchange specified, permute the iteration domain and
    // the tile sizes.
    SmallVector<unsigned> interchangeVector;
    if (!options.interchangeVector.empty()) {
      interchangeVector = fillInterchangeVector(options.interchangeVector,
                                                iterationDomain.size());
    }
    if (!interchangeVector.empty()) {
      if (!isPermutation(interchangeVector)) {
        return rewriter.notifyMatchFailure(
            op,
            "invalid intechange vector, not a permutation of the entire "
            "iteration space");
      }

      iterationDomain =
          applyPermutationToVector(iterationDomain, interchangeVector);
      tileSizeVector =
          applyPermutationToVector(tileSizeVector, interchangeVector);
    }

    // If there is distribution specified, adjust the loop ranges. Note that
    // permutation happens *before* distribution.
    SmallVector<linalg::DistributionMethod> distributionMethods(
        iterationDomain.size(), linalg::DistributionMethod::None);
    SmallVector<linalg::ProcInfo> procInfo;
    if (options.distribution) {
      SmallVector<utils::IteratorType> iteratorTypes =
          op.getLoopIteratorTypes();

      // The parallel loops that are tiled are partitionable loops.
      SmallVector<Range> parallelLoopRanges;
      SmallVector<unsigned> partitionedLoopIds;
      for (auto iteratorType : llvm::enumerate(iteratorTypes)) {
        if (iteratorType.value() == utils::IteratorType::parallel &&
            !isZero(tileSizeVector[iteratorType.index()])) {
          parallelLoopRanges.push_back(
              iterationDomainOfr[iteratorType.index()]);
          partitionedLoopIds.push_back(iteratorType.index());
        }
      }

      // Query the callback to get the {procId, nprocs} to use.
      procInfo =
          options.distribution->procInfo(rewriter, loc, parallelLoopRanges);

      for (auto it : llvm::enumerate(partitionedLoopIds)) {
        distributionMethods[it.value()] =
            procInfo[it.index()].distributionMethod;
      }
    }

    // 3. Materialize an empty loop nest that iterates over the tiles. These
    // loops for now do not return any values even if the original operation has
    // results.
    tilingResult.loops =
        generateTileLoopNest(rewriter, loc, iterationDomain, tileSizeVector,
                             distributionMethods, procInfo, offsets, sizes);

    if (!interchangeVector.empty()) {
      auto inversePermutation = invertPermutationVector(interchangeVector);
      offsets = applyPermutationToVector(offsets, inversePermutation);
      sizes = applyPermutationToVector(sizes, inversePermutation);
    }

    LLVM_DEBUG({
      if (!tilingResult.loops.empty()) {
        llvm::errs() << "LoopNest shell :\n";
        tilingResult.loops.front().dump();
        llvm::errs() << "\n";
      }
    });

    // 4. Generate the tiled implementation within the inner most loop.
    if (!tilingResult.loops.empty())
      rewriter.setInsertionPoint(
          tilingResult.loops.back().getBody()->getTerminator());
    tiledImplementation = op.getTiledImplementation(rewriter, offsets, sizes);
    tilingResult.tiledOp = tiledImplementation.back();

    LLVM_DEBUG({
      if (!tilingResult.loops.empty()) {
        llvm::errs() << "After tiled implementation :\n";
        tilingResult.loops.front().dump();
        llvm::errs() << "\n";
      }
    });
    std::swap(tilingResult.tileOffsets, offsets);
    std::swap(tilingResult.tileSizes, sizes);
  }

  // Update the filter.
  for (auto tiledOp : tiledImplementation) {
    filter.replaceLinalgTransformationFilter(rewriter, tiledOp);
  }

  if (op->getNumResults() == 0) {
    rewriter.eraseOp(op);
    return tilingResult;
  }

  // Rewrite all `flow.dispatch.tensor.store` operation with tiled version
  // of the store. Its valid to this for all stores of the root untiled op.
  if (failed(replaceAllStoresWithTiledVersion(
          rewriter, op, tilingResult.tileOffsets, tilingResult.tileSizes,
          tilingResult.tiledOp))) {
    return failure();
  }
  return tilingResult;
}

//===----------------------------------------------------------------------===//
// TileAndFuseDispatchUsingSCFForOp pattern implementation.
//===----------------------------------------------------------------------===//

namespace {

/// Result of tile and fuse operations.
struct TileAndFuseResult {
  SmallVector<Operation *> tiledAndFusedOps;
  SmallVector<scf::ForOp> loops;
};

struct TileAndFuseDispatchUsingSCFForOp
    : public OpInterfaceRewritePattern<TilingInterface> {
  /// Construct a generic pattern applied to all TilingInterface ops.
  TileAndFuseDispatchUsingSCFForOp(
      MLIRContext *context, linalg::LinalgTilingOptions options,
      IREE::LinalgExt::LinalgTransformationFilter filter,
      PatternBenefit benefit = 1)
      : OpInterfaceRewritePattern<TilingInterface>(context, benefit),
        tilingPattern(context, options, filter) {}

  /// Construct a generic pattern applied to `opName`.
  TileAndFuseDispatchUsingSCFForOp(
      StringRef opName, MLIRContext *context,
      linalg::LinalgTilingOptions options,
      IREE::LinalgExt::LinalgTransformationFilter filter,
      PatternBenefit benefit = 1)
      : OpInterfaceRewritePattern<TilingInterface>(context, benefit),
        tilingPattern(context, options, filter) {}

  FailureOr<TileAndFuseResult> returningMatchAndRewrite(
      TilingInterface op, PatternRewriter &rewriter) const;

  LogicalResult matchAndRewrite(TilingInterface op,
                                PatternRewriter &rewriter) const override {
    return returningMatchAndRewrite(op, rewriter);
  }

 private:
  TileDispatchUsingSCFForOp tilingPattern;
};
}  // namespace

/// Find all producers to fuse and return them in sorted order;
static SmallVector<Operation *> getAllFusableProducers(TilingInterface op) {
  llvm::SetVector<Operation *> producers;
  std::deque<Operation *> worklist;
  worklist.push_back(op);

  while (!worklist.empty()) {
    Operation *currOp = worklist.front();
    worklist.pop_front();
    for (OpOperand &operand : currOp->getOpOperands()) {
      auto tilingInterfaceProducer =
          operand.get().getDefiningOp<TilingInterface>();
      if (!tilingInterfaceProducer ||
          producers.count(tilingInterfaceProducer)) {
        continue;
      }
      worklist.push_back(tilingInterfaceProducer);
      producers.insert(tilingInterfaceProducer);
    }
  }

  SmallVector<Operation *> sortedOps(producers.begin(), producers.end());
  mlir::computeTopologicalSorting(sortedOps);
  return sortedOps;
}

/// Return all slices that are used to access a tile of the producer. Assume
/// that `tiledOps` are in "reverse" order of their appearance in the IR.
static SmallVector<tensor::ExtractSliceOp> getAllFusableProducerUses(
    Operation *untiledOp, ArrayRef<Operation *> tiledOps) {
  SmallVector<tensor::ExtractSliceOp> sliceOps;
  for (auto tiledOp : llvm::reverse(tiledOps)) {
    for (OpOperand &operand : llvm::reverse(tiledOp->getOpOperands())) {
      auto sliceOp = operand.get().getDefiningOp<tensor::ExtractSliceOp>();
      if (!sliceOp || sliceOp.getSource().getDefiningOp() != untiledOp)
        continue;
      sliceOps.push_back(sliceOp);
    }
  }
  return sliceOps;
}

FailureOr<TileAndFuseResult>
TileAndFuseDispatchUsingSCFForOp::returningMatchAndRewrite(
    TilingInterface op, PatternRewriter &rewriter) const {
  TileAndFuseResult tileAndFuseResult;
  auto fusableProducers = getAllFusableProducers(op);
  // Apply the tiling pattern.
  FailureOr<TilingResult> tilingResult =
      tilingPattern.returningMatchAndRewrite(op, rewriter);
  if (failed(tilingResult)) {
    return failure();
  }
  // If there is no tiling then there is nothing to do for fusion.
  if (!tilingResult->tiledLoops.any()) {
    return tileAndFuseResult;
  }
  tileAndFuseResult.tiledAndFusedOps.push_back(tilingResult->tiledOp);
  tileAndFuseResult.loops.append(tilingResult->loops);

  // Get all ops to tile and fuse.
  auto fusableProducersRef = llvm::makeArrayRef(fusableProducers);
  while (!fusableProducersRef.empty()) {
    auto fusableProducer = cast<TilingInterface>(fusableProducersRef.back());
    fusableProducersRef = fusableProducersRef.drop_back();

    // Find a slice that is used to access the producer. Get all the slice ops.
    // It is assumed that the slice ops are returned in-order, so that you
    // could use the first slice as the insertion point.
    auto sliceOps = getAllFusableProducerUses(
        fusableProducer, tileAndFuseResult.tiledAndFusedOps);
    if (sliceOps.empty()) continue;
    tensor::ExtractSliceOp sliceOp = sliceOps.front();
    OpBuilder::InsertionGuard g(rewriter);
    rewriter.setInsertionPoint(sliceOp);

    // Generate the tiled implementation of the producer.
    FailureOr<Value> tiledProducerVal =
        tensor::replaceExtractSliceWithTiledProducer(
            rewriter, sliceOp, sliceOp.getSource().cast<OpResult>());
    if (failed(tiledProducerVal)) {
      return rewriter.notifyMatchFailure(sliceOp,
                                         "fusion along slice op failed");
    }
    auto tiledProducer = tiledProducerVal->getDefiningOp<TilingInterface>();
    if (!tiledProducer) {
      return rewriter.notifyMatchFailure(
          tiledProducer,
          "expected tiled implementation to implement TilingInterface as well");
    }
    if (tiledProducer->getNumResults() != fusableProducer->getNumResults()) {
      return rewriter.notifyMatchFailure(fusableProducer,
                                         "fused operation expected to produce "
                                         "an op with same number of results");
    }

    // 2b. Assume that the tile sizes used are such that all tiled loops are
    //     "common parallel loops" for the consumer and all pulled in
    //     producers. So using the tile size of the tiled consumer op, and the
    //     information about which loops are tiled and which arent, compute
    //     the tile sizes to use for the producer as well.
    SmallVector<OpFoldResult> producerOffset, producerSizes;
    SmallVector<Range> producerIterationDomain =
        fusableProducer.getIterationDomain(rewriter);
    for (auto range : llvm::enumerate(producerIterationDomain)) {
      if (range.index() < tilingResult->tiledLoops.size() &&
          tilingResult->tiledLoops.test(range.index())) {
        producerOffset.push_back(tilingResult->tileOffsets[range.index()]);
        producerSizes.push_back(tilingResult->tileSizes[range.index()]);
      } else {
        producerOffset.push_back(range.value().offset);
        producerSizes.push_back(range.value().size);
      }
    }

    // 2c. Finally replace any `flow.dispatch.tensor.store` operation with
    //     tiled version of the operation. It is only valid to do this under the
    //     above assumption that the producer and consumer share the loops
    //     that can be tiled.
    if (failed(replaceAllStoresWithTiledVersion(rewriter, fusableProducer,
                                                producerOffset, producerSizes,
                                                tiledProducer))) {
      return failure();
    }
    // Replace all uses of the slices processed in this step with values from
    // the producer.
    for (auto sliceOp : sliceOps) {
      unsigned resultNumber =
          sliceOp.getSource().cast<OpResult>().getResultNumber();
      rewriter.replaceOp(sliceOp, tiledProducer->getResult(resultNumber));
    }
    tileAndFuseResult.tiledAndFusedOps.push_back(tiledProducer);
  }

  return tileAndFuseResult;
}

namespace {

//===----------------------------------------------------------------------===//
// SwapExtractSliceWithTiledProducer
//===----------------------------------------------------------------------===//

/// Pattern to swap a `tilinginterface op` -> `tensor.extract_slice` with
/// `tensor.extract_slice` of operands of the op -> tiled `tilinginterface
/// op`.
struct SwapExtractSliceWithTiledProducer
    : public OpRewritePattern<tensor::ExtractSliceOp> {
  using OpRewritePattern<tensor::ExtractSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    OpResult producer = sliceOp.getSource().dyn_cast<OpResult>();
    if (!producer) {
      return rewriter.notifyMatchFailure(sliceOp, "source uses bb arg");
    }
    FailureOr<Value> tiledProducer =
        tensor::replaceExtractSliceWithTiledProducer(rewriter, sliceOp,
                                                     producer);
    if (failed(tiledProducer)) {
      return failure();
    }
    // Replace all uses of the producer within the
    rewriter.replaceOp(sliceOp, tiledProducer.value());
    return success();
  }
};

//===----------------------------------------------------------------------===//
// SwapExtractSliceWithDispatchTensorLoad
//===----------------------------------------------------------------------===//

/// Pattern to swap `flow.dispatch.tensor.load` -> `tensor.extract_slice` with
/// `flow.dispatch.tensor.load` of the slice.
struct SwapExtractSliceWithDispatchTensorLoad
    : public OpRewritePattern<tensor::ExtractSliceOp> {
  using OpRewritePattern<tensor::ExtractSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    auto loadOp =
        sliceOp.getSource().getDefiningOp<IREE::Flow::DispatchTensorLoadOp>();
    if (!loadOp) return failure();

    SmallVector<OpFoldResult> combinedOffsets, combinedSizes, combinedStrides;
    if (failed(mergeOffsetsSizesAndStrides(
            rewriter, loadOp.getLoc(), loadOp, sliceOp, loadOp.getDroppedDims(),
            combinedOffsets, combinedSizes, combinedStrides))) {
      return rewriter.notifyMatchFailure(
          sliceOp, "failed to fold offsets, sizes and strides with load op");
    }
    rewriter.replaceOpWithNewOp<IREE::Flow::DispatchTensorLoadOp>(
        sliceOp, sliceOp.getType(), loadOp.getSource(), loadOp.getSourceDims(),
        combinedOffsets, combinedSizes, combinedStrides);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// SwapExtractSliceWithInitTensor
//===----------------------------------------------------------------------===//

/// Pattern to swap `init_tensor` -> `tensor.extract_slice` with
/// `init_tensor` of the slice.
struct SwapExtractSliceWithInitTensor
    : public OpRewritePattern<tensor::ExtractSliceOp> {
  using OpRewritePattern<tensor::ExtractSliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExtractSliceOp sliceOp,
                                PatternRewriter &rewriter) const override {
    auto emptyTensorOp = sliceOp.getSource().getDefiningOp<tensor::EmptyOp>();
    if (!emptyTensorOp) return failure();

    SmallVector<OpFoldResult> mixedSizes = sliceOp.getMixedSizes();
    if (mixedSizes.size() != sliceOp.getType().getRank()) {
      SmallVector<OpFoldResult> rankReducedMixedSizes;
      rankReducedMixedSizes.reserve(sliceOp.getType().getRank());
      auto droppedDims = sliceOp.getDroppedDims();
      for (auto size : llvm::enumerate(mixedSizes)) {
        if (droppedDims.test(size.index())) continue;
        rankReducedMixedSizes.push_back(size.value());
      }
      std::swap(mixedSizes, rankReducedMixedSizes);
    }
    rewriter.replaceOpWithNewOp<tensor::EmptyOp>(
        sliceOp, mixedSizes, sliceOp.getType().getElementType());
    return success();
  }
};

}  // namespace

void populateTileAndDistributeToWorkgroupsPatterns(
    RewritePatternSet &patterns, linalg::LinalgTilingOptions options,
    IREE::LinalgExt::LinalgTransformationFilter filter) {
  MLIRContext *context = patterns.getContext();
  patterns.insert<TileAndFuseDispatchUsingSCFForOp>(context, std::move(options),
                                                    std::move(filter));
  patterns.insert<SwapExtractSliceWithDispatchTensorLoad,
                  SwapExtractSliceWithInitTensor,
                  SwapExtractSliceWithTiledProducer>(context);
}

}  // namespace iree_compiler
}  // namespace mlir
