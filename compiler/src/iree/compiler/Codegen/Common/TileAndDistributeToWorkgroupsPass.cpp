// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

//=== TileAndDistributeToWorkgroupsPass.cpp - Tile to workgroups pass ----===//
//
// This pass distributes the operations within the module to workgroups. This
// pass is created to move tile and distribution out of flow level and into
// the backends. For now this is mostly a bridge pass to connect things during
// the transition, and eventually might just be deprecated in favor of a
// utility method.
//
//===---------------------------------------------------------------------===//

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtDialect.h"
#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree-dialects/Dialect/LinalgExt/Passes/Transforms.h"
#include "iree/compiler/Codegen/Common/Transforms.h"
#include "iree/compiler/Codegen/Dialect/LoweringConfig.h"
#include "iree/compiler/Codegen/Interfaces/PartitionableLoopsInterface.h"
#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Utils/Utils.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/HAL/IR/HALDialect.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Utils/Utils.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/SCF/Transforms/Transforms.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#define DEBUG_TYPE "iree-codegen-tile-and-distribute-to-workgroups"

namespace mlir {
namespace iree_compiler {

/// Method to return the configuration to use for first-level tile and
/// distribute. Returns the
/// - Root op of the dispatch. If no root op was found returns `nullptr`.
/// - tileSizes to use
/// - interchange
/// - loops to be partitioned (the tile sizes for the non-partitioned loop are
///   set to 0)
/// - static loop ranges - this is meant to be an optimization hint. It recovers
///   the static values that the workload of the dispatch corresponds to.
// TODO: Remove the use of static loop ranges. This is used to set the number of
// workgroups to a static value. Ideally this should not be done and the static
// and dyamic cases are handled the same way. When the tile+distribute moves
// away from using `scf.for` to using a construct that better captures
// distribution (like `scf.foreach_thread`) this information can be dropped.
static LogicalResult getTileAndDistributeConfig(
    ArrayRef<Operation *> computeOps, Operation *&dispatchRootOp,
    SmallVectorImpl<int64_t> &tileSizes,
    SmallVectorImpl<int64_t> &staticLoopRanges,
    SmallVectorImpl<int64_t> &interchange,
    SmallVectorImpl<unsigned> &partitionableLoops) {
  // Find the lowering configuration of the root operation.
  FailureOr<Operation *> rootOp = getLoweringConfigCarryingOp(computeOps);
  if (failed(rootOp)) {
    // Just return. All the in-out vectors are empty that should default
    // the number of workgroups to {1, 1, 1}
    dispatchRootOp = nullptr;
    return success();
  }
  dispatchRootOp = rootOp.value();

  auto partitionableLoopInterface =
      dyn_cast<PartitionableLoopsInterface>(*rootOp);
  if (!partitionableLoopInterface) {
    // Just return. All the in-out vectors are empty that should default
    // the number of workgroups to {1, 1, 1}
    return success();
  }

  partitionableLoops =
      partitionableLoopInterface.getPartitionableLoops(kNumMaxParallelDims);
  // For now assert that number of partitionable loops are less than the
  // supported max.
  // TODO(ravishankarm): Relax this restriction.
  if (partitionableLoops.size() > kNumMaxParallelDims) {
    return rootOp.value()->emitOpError(
               "expected number of partitionable loops to be less than or "
               "equal to ")
           << kNumMaxParallelDims;
  }

  IREE::Codegen::LoweringConfigAttr rootOpConfig = getLoweringConfig(*rootOp);
  if (!rootOpConfig) {
    return rootOp.value()->emitOpError(
        "unable to find configuration of root op to define workgroup count "
        "region");
  }
  tileSizes.assign(rootOpConfig.getTileSizeVals(0));
  interchange.assign(rootOpConfig.getTileInterchangeVals(0));

  // Set tile sizes of non-partitioned loops to 0.
  llvm::SmallDenseSet<unsigned> partitionableLoopsSet;
  partitionableLoopsSet.insert(partitionableLoops.begin(),
                               partitionableLoops.end());
  for (auto loopId : llvm::seq<unsigned>(0, tileSizes.size())) {
    if (partitionableLoopsSet.count(loopId)) continue;
    tileSizes[loopId] = 0;
  }

  if (auto linalgOp = dyn_cast<linalg::LinalgOp>(*rootOp)) {
    staticLoopRanges = linalgOp.getStaticLoopRanges();
  }
  staticLoopRanges.resize(tileSizes.size(), ShapedType::kDynamic);

  return success();
}

/// Get the materialization information from a `iree_linalg_ext.pack` operation.
static FailureOr<IREE::LinalgExt::MaterializeEncodingInfo>
getMaterializationInfo(IREE::LinalgExt::PackOp packOp) {
  IREE::LinalgExt::MaterializeEncodingInfo encodingInfo;
  SmallVector<OpFoldResult> mixedTileSizes = packOp.getMixedTiles();
  encodingInfo.innerTileSizes.reserve(mixedTileSizes.size());
  for (auto tileSize : mixedTileSizes) {
    if (tileSize.is<Value>()) {
      return packOp.emitOpError(
          "unhandled distribution of pack op with dynamic inner tile size");
    }
    encodingInfo.innerTileSizes.push_back(
        tileSize.get<Attribute>().cast<IntegerAttr>().getInt());
  }
  encodingInfo.innerDimsPos = llvm::to_vector(llvm::map_range(
      packOp.getInnerDimsPos(),
      [](Attribute attr) { return attr.cast<IntegerAttr>().getInt(); }));
  encodingInfo.outerDimsPerm = llvm::to_vector(llvm::map_range(
      packOp.getOuterDimsPerm(),
      [](Attribute attr) { return attr.cast<IntegerAttr>().getInt(); }));
  return encodingInfo;
}

//===---------------------------------------------------------------------===//
// Patterns to lower operations that are used to compute the number of
// workgroups.
//===---------------------------------------------------------------------===//

/// The `flow.dispatch.workgroup_count_from_dag_root` op is lowered to
/// a sequence of `affine.apply affine_map<()[s0, s1] -> ceildDiv(s0,
/// s1)>(workload, tileSize)`. for each of the dimensions. When tile size is
/// zero, number of workgroups is set to 1.
struct LowerDispatchWorkgroupCountForDagRootOp
    : OpRewritePattern<IREE::Flow::DispatchWorkgroupCountFromDagRootOp> {
  LowerDispatchWorkgroupCountForDagRootOp(MLIRContext *context,
                                          ArrayRef<int64_t> tileSizes,
                                          ArrayRef<int64_t> staticLoopRanges,
                                          ArrayRef<int64_t> interchange,
                                          ArrayRef<unsigned> partitionedLoops,
                                          PatternBenefit benefit = 1)
      : OpRewritePattern(context, benefit),
        givenTileSizes(tileSizes),
        givenStaticLoopRanges(staticLoopRanges),
        givenInterchange(interchange),
        partitionedLoops(partitionedLoops) {}

  LogicalResult matchAndRewrite(
      IREE::Flow::DispatchWorkgroupCountFromDagRootOp workgroupCountOp,
      PatternRewriter &rewriter) const override {
    auto workloadValues = workgroupCountOp.operands();
    SmallVector<OpFoldResult> tileSizes = llvm::to_vector(llvm::map_range(
        givenTileSizes,
        [&](int64_t v) -> OpFoldResult { return rewriter.getIndexAttr(v); }));

    Attribute zero = rewriter.getIndexAttr(0);
    tileSizes.resize(workloadValues.size(), zero);
    SmallVector<int64_t> staticLoopRanges = givenStaticLoopRanges;
    staticLoopRanges.resize(workloadValues.size(), ShapedType::kDynamic);
    Location loc = workgroupCountOp.getLoc();
    auto numTiles = llvm::to_vector(llvm::map_range(
        llvm::zip(workloadValues, staticLoopRanges, tileSizes),
        [&](std::tuple<Value, int64_t, OpFoldResult> p) -> OpFoldResult {
          auto tileSize = std::get<2>(p);
          if (isConstantIntValue(tileSize, 0)) {
            return rewriter.getIndexAttr(1);
          }

          int64_t staticLoopRange = std::get<1>(p);
          OpFoldResult workload =
              (staticLoopRange == ShapedType::kDynamic
                   ? OpFoldResult(std::get<0>(p))
                   : OpFoldResult(rewriter.getIndexAttr(staticLoopRange)));
          AffineExpr s0, s1;
          bindSymbols(rewriter.getContext(), s0, s1);
          SmallVector<OpFoldResult> mapOperands = {workload, tileSize};
          return makeComposedFoldedAffineApply(rewriter, loc, s0.ceilDiv(s1),
                                               mapOperands);
        }));
    // If there is interchange, first apply interchange on the number of tiles.
    if (!givenInterchange.empty()) {
      SmallVector<OpFoldResult> interchangedNumTiles = numTiles;
      for (auto interchangedLoop : llvm::enumerate(givenInterchange)) {
        interchangedNumTiles[interchangedLoop.value()] =
            numTiles[interchangedLoop.index()];
      }
      numTiles = interchangedNumTiles;
    }

    // Prune the numtiles for just the partitioned loops. Iterate in reverse
    // since the number of workgroups is specified from fastest varying to
    // slowest varying.
    SmallVector<Value> numWorkgroups;
    for (auto partitionedLoop : llvm::reverse(partitionedLoops)) {
      numWorkgroups.push_back(getValueOrCreateConstantIndexOp(
          rewriter, loc, numTiles[partitionedLoop]));
    }
    Value one = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    numWorkgroups.resize(kNumMaxParallelDims, one);
    rewriter.replaceOp(workgroupCountOp, numWorkgroups);
    return success();
  }

 private:
  /// Tile sizes specified for tile+distribute.
  SmallVector<int64_t> givenTileSizes;

  /// Static loop ranges of the distributed loops.
  // TODO: Remove this usage. This is just a WAR to help remove the unit-trip
  // distribution loops.
  SmallVector<int64_t> givenStaticLoopRanges;

  /// Interchange specified for tile+distribute.
  SmallVector<int64_t> givenInterchange;

  /// Loops that are partitioned.
  SmallVector<unsigned> partitionedLoops;
};

/// Pattern to lower a `flow.dispatch.workgroup_count_from_set_encoding` op.
/// At the Flow level this op uses the logical shape of the tensor
/// as the workload. This gets materialized into an physical tensor
/// Lower this operation accounting for the change of shape from
/// the logical shape to the physical shape. It lowers to
/// a `flow.dispatch.workgroup_count_from_root_dag` where the root
/// is the `pack` op that materialized the encoding.
struct LowerDispatchWorkgroupCountFromSetEncodingOp
    : public OpRewritePattern<
          IREE::Flow::DispatchWorkgroupCountFromSetEncodingOp> {
  LowerDispatchWorkgroupCountFromSetEncodingOp(
      MLIRContext *context,
      IREE::LinalgExt::MaterializeEncodingInfo encodingInfo,
      PatternBenefit benefit = 1)
      : OpRewritePattern(context, benefit),
        materializeEncodingInfo(std::move(encodingInfo)) {}

  LogicalResult matchAndRewrite(
      IREE::Flow::DispatchWorkgroupCountFromSetEncodingOp workgroupCountOp,
      PatternRewriter &rewriter) const override {
    ValueRange workload = workgroupCountOp.getOperands();
    // The workload represents the unpacked shape. Get the workload of the
    // packed shape.
    auto getAsOpFoldResults = [&](ArrayRef<int64_t> intVals) {
      return llvm::to_vector(llvm::map_range(
          intVals,
          [&](int64_t i) -> OpFoldResult { return rewriter.getIndexAttr(i); }));
    };
    SmallVector<OpFoldResult> resultShape =
        IREE::LinalgExt::PackOp::getResultShape(
            rewriter, workgroupCountOp.getLoc(), getAsOpFoldResult(workload),
            getAsOpFoldResults(materializeEncodingInfo.innerTileSizes),
            materializeEncodingInfo.innerDimsPos,
            materializeEncodingInfo.outerDimsPerm);

    rewriter
        .replaceOpWithNewOp<IREE::Flow::DispatchWorkgroupCountFromDagRootOp>(
            workgroupCountOp,
            getValueOrCreateConstantIndexOp(rewriter, workgroupCountOp.getLoc(),
                                            resultShape));
    return success();
  }

 private:
  IREE::LinalgExt::MaterializeEncodingInfo materializeEncodingInfo;
};

//===---------------------------------------------------------------------===//
// Patterns and methods for tile and distribute of Linalg ops to workgroups.
//===---------------------------------------------------------------------===//

namespace {
struct TileAndDistributeToWorkgroupsPass
    : public TileAndDistributeToWorkgroupsBase<
          TileAndDistributeToWorkgroupsPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry
        .insert<AffineDialect, IREE::Flow::FlowDialect, IREE::HAL::HALDialect,
                linalg::LinalgDialect, IREE::LinalgExt::IREELinalgExtDialect,
                scf::SCFDialect, tensor::TensorDialect>();
  }

  void runOnOperation() override;
};
}  // namespace

void TileAndDistributeToWorkgroupsPass::runOnOperation() {
  MLIRContext *context = &getContext();
  IREE::HAL::ExecutableVariantOp variantOp = getOperation();
  ModuleOp innerModule = variantOp.getInnerModule();
  llvm::StringMap<IREE::HAL::ExecutableExportOp> entryPoints =
      getAllEntryPoints(innerModule);

  for (func::FuncOp funcOp : innerModule.getOps<func::FuncOp>()) {
    auto exportOp = entryPoints.lookup(funcOp.getName());
    if (!exportOp) continue;

    SmallVector<Operation *> computeOps;
    SmallVector<LoopTilingAndDistributionInfo> tiledLoops;
    if (failed(getComputeOps(funcOp, computeOps, tiledLoops))) {
      funcOp.emitOpError("failed to get compute ops in dispatch");
      return signalPassFailure();
    }
    if (!tiledLoops.empty()) {
      // The entry point already has distribution to workgroups. Do nothing.
      continue;
    }
    SmallVector<int64_t> tileSizes, staticLoopRanges, interchange;
    SmallVector<unsigned> partitionableLoops;
    Operation *dispatchRootOp = nullptr;
    if (failed(getTileAndDistributeConfig(computeOps, dispatchRootOp, tileSizes,
                                          staticLoopRanges, interchange,
                                          partitionableLoops))) {
      funcOp.emitOpError("failed to get tile and distribute configuration");
      return signalPassFailure();
    }

    // Lower the workgroup count ops.
    {
      RewritePatternSet patterns(context);
      patterns.insert<LowerDispatchWorkgroupCountForDagRootOp>(
          context, tileSizes, staticLoopRanges, interchange,
          partitionableLoops);
      if (auto packRootOp =
              dyn_cast_or_null<IREE::LinalgExt::PackOp>(dispatchRootOp)) {
        FailureOr<IREE::LinalgExt::MaterializeEncodingInfo> encodingInfo =
            getMaterializationInfo(packRootOp);
        if (failed(encodingInfo)) {
          return signalPassFailure();
        }
        patterns.insert<LowerDispatchWorkgroupCountFromSetEncodingOp>(
            context, encodingInfo.value());
      }
      if (failed(applyPatternsAndFoldGreedily(exportOp, std::move(patterns)))) {
        exportOp.emitOpError("failed to lower number of workgroups");
        return signalPassFailure();
      }
    }

    // If there are no compute ops, nothing more to do.
    if (computeOps.empty()) continue;

    // Add a marker to the last operation in the list.
    auto marker = StringAttr::get(context, "__workgroup_tiling__");
    computeOps.back()->setAttr(
        IREE::LinalgExt::LinalgTransforms::kLinalgTransformMarker, marker);

    // Configure the linalg options.
    // Tile size selection function.
    auto tileSizeFn = [&](OpBuilder &builder,
                          Operation *op) -> SmallVector<Value, 4> {
      // Check if tile sizes are deduced from the configuration. If so use
      // those.
      return llvm::to_vector<4>(
          llvm::map_range(tileSizes, [&](int64_t ts) -> Value {
            return builder.create<arith::ConstantIndexOp>(op->getLoc(), ts);
          }));
    };

    auto linalgTilingOptions =
        linalg::LinalgTilingOptions()
            .setDistributionOptions(getIREELinalgLoopDistributionOptions())
            .setInterchange(llvm::to_vector<4>(
                llvm::map_range(interchange,
                                [](int64_t v) -> unsigned {
                                  return static_cast<unsigned>(v);
                                })))
            .setLoopType(linalg::LinalgTilingLoopType::Loops)
            .setTileSizeComputationFunction(tileSizeFn);

    {
      RewritePatternSet patterns(context);
      populateTileAndDistributeToWorkgroupsPatterns(
          patterns, linalgTilingOptions,
          IREE::LinalgExt::LinalgTransformationFilter(marker));
      if (failed(applyPatternsAndFoldGreedily(funcOp, std::move(patterns)))) {
        funcOp.emitOpError("Tile+Distribute failed");
        return signalPassFailure();
      }
    }

    // If tiling didn't happen because there are no tile sizes we are
    // potentially left with a marker that will confuse the following passes so
    // we remove the intermediate markers.
    funcOp->walk([&](Operation *op) {
      op->removeAttr(IREE::LinalgExt::LinalgTransforms::kLinalgTransformMarker);
    });

    LLVM_DEBUG({
      llvm::dbgs() << "--- After Tile + Distribute ---\n";
      funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
      llvm::dbgs() << "\n\n";
    });

    {
      // Apply linalg tiling optimization patterns, which includes folding
      // casting ops into tiled operations.
      RewritePatternSet patterns(context);
      linalg::populateLinalgTilingCanonicalizationPatterns(patterns);
      populateFoldAffineMinInDistributedLoopsPatterns(patterns);
      context->getOrLoadDialect<IREE::LinalgExt::IREELinalgExtDialect>()
          ->getCanonicalizationPatterns(patterns);
      if (failed(applyPatternsAndFoldGreedily(funcOp, std::move(patterns)))) {
        funcOp.emitOpError("tiling canonicalizations failed");
        return signalPassFailure();
      }
    }

    LLVM_DEBUG({
      llvm::dbgs() << "--- After Canonicalize ---\n";
      funcOp.print(llvm::dbgs(), OpPrintingFlags().useLocalScope());
      llvm::dbgs() << "\n\n";
    });

    // After rewriting destructive updates, there might be uses of compute
    // operations only in `tensor.dim` ops. Resolve these.
    RewritePatternSet resolveDimOps(context);
    memref::populateResolveRankedShapeTypeResultDimsPatterns(resolveDimOps);
    if (failed(
            applyPatternsAndFoldGreedily(funcOp, std::move(resolveDimOps)))) {
      funcOp.emitOpError("resolving ranked shaped results dims failed");
      return signalPassFailure();
    }
  }
}

std::unique_ptr<OperationPass<IREE::HAL::ExecutableVariantOp>>
createTileAndDistributeToWorkgroupsPass() {
  return std::make_unique<TileAndDistributeToWorkgroupsPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
