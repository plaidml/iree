// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtDialect.h"
#include "iree-dialects/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree-dialects/Dialect/LinalgTransform/LinalgTransformOps.h"
#include "iree-dialects/Dialect/LinalgTransform/Passes.h"
#include "iree-dialects/Dialect/LinalgTransform/TransformInterpreterUtils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Arith/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Bufferization/Transforms/FuncBufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/Linalg/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/PDL/IR/PDL.h"
#include "mlir/Dialect/PDLInterp/IR/PDLInterp.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/SCF/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Dialect/Transform/IR/TransformInterfaces.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/Dialect/Vector/Transforms/BufferizableOpInterfaceImpl.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Support/FileUtilities.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/SourceMgr.h"

#define DEBUG_TYPE "transform-dialect-interpreter"
#define DBGS() (llvm::dbgs() << "[" DEBUG_TYPE << "]: ")

using namespace mlir;

LogicalResult mlir::transform::parseTransformModuleFromFile(
    MLIRContext *context, llvm::StringRef transformFileName,
    OwningOpRef<ModuleOp> &transformModule) {
  if (transformFileName.empty()) {
    llvm::errs() << "no transform file name specified, assuming the transform "
                    "module is embedded in the IR next to the top-level\n";
    return success();
  }
  // Parse transformFileName content into a ModuleOp.
  std::string errorMessage;
  auto memoryBuffer = mlir::openInputFile(transformFileName, &errorMessage);
  if (!memoryBuffer) {
    llvm::errs() << "failed to parse transform file: " << transformFileName
                 << "\n";
    return failure();
  }
  // Tell sourceMgr about this buffer, the parser will pick it up.
  llvm::SourceMgr sourceMgr;
  sourceMgr.AddNewSourceBuffer(std::move(memoryBuffer), llvm::SMLoc());
  transformModule =
      OwningOpRef<ModuleOp>(parseSourceFile<ModuleOp>(sourceMgr, context));
  return success();
}

LogicalResult mlir::transform::applyTransformsInRegion(Region &transformRegion,
                                                       Operation *target) {
  SmallVector<transform::TransformOpInterface> transforms;
  if (failed(
          transform::extractTopLevelTransformOps(transformRegion, transforms)))
    return failure();

  for (transform::TransformOpInterface transform : transforms) {
    // TransformState::applyTransform requires that the parent region is a
    // proper ancestor of the transform op to perform SSA liveness assertions.
    // In multithreaded state however, we cannot clone into `transformRegion` so
    // we build a new single-block region and clone the transform op into it.
    Region r;
    OpBuilder b(target->getContext());
    b.createBlock(&r);
    TransformOptions options;
#ifndef NDEBUG
    options = options.enableExpensiveChecks();
#endif
    auto xform = cast<transform::TransformOpInterface>(b.clone(*transform));
    auto g = llvm::make_scope_exit([&]() { xform->erase(); });
    if (failed(transform::applyTransforms(target, xform, options)))
      return failure();
  }
  return success();
}

LogicalResult mlir::transform::extractTopLevelTransformOps(
    Region &r, SmallVectorImpl<TransformOpInterface> &res) {
  assert(r.getBlocks().size() == 1 &&
         "Expected single-block region to extract transform ops from");
  r.walk<WalkOrder::PreOrder>([&](transform::TransformOpInterface transform) {
    if (transform->hasTrait<transform::PossibleTopLevelTransformOpTrait>()) {
      assert(llvm::none_of(res, [&](transform::TransformOpInterface seen) {
        return seen->isAncestor(transform);
      }));
      res.push_back(transform);
      return WalkResult::skip();
    }
    return WalkResult::advance();
  });
  return success();
}

namespace {

/// Pass declaration.
/// Interpreter pass that applies transform dialect ops for codegen.
/// This needs to be its own pass because the registration mechanism and ops
/// available are different than for other interpreters.
class TransformDialectInterpreter
    : public PassWrapper<TransformDialectInterpreter, Pass> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(TransformDialectInterpreter)

  void getDependentDialects(DialectRegistry &registry) const override {
    // TODO: this is only necessary to make registry subset happy when running
    // the lowering to LLVM. The lowering should be changed to stop using the
    // nested pass manager and this will go away.

    // clang-format off
    registry.insert<mlir::iree_compiler::IREE::LinalgExt::IREELinalgExtDialect,
                    arith::ArithDialect,
                    AffineDialect,
                    bufferization::BufferizationDialect,
                    func::FuncDialect,
                    linalg::LinalgDialect,
                    linalg::transform::LinalgTransformDialect,
                    LLVM::LLVMDialect,
                    pdl::PDLDialect,
                    pdl_interp::PDLInterpDialect,
                    scf::SCFDialect,
                    tensor::TensorDialect,
                    vector::VectorDialect
        // clang-format on
        >();

    // TODO: these should be registered by the extension instead, but there is
    // no support for it in core currently.
    arith::registerBufferizableOpInterfaceExternalModels(registry);
    linalg::registerBufferizableOpInterfaceExternalModels(registry);
    scf::registerBufferizableOpInterfaceExternalModels(registry);
    bufferization::func_ext::registerBufferizableOpInterfaceExternalModels(
        registry);
    tensor::registerBufferizableOpInterfaceExternalModels(registry);
    vector::registerBufferizableOpInterfaceExternalModels(registry);
  }

  StringRef getArgument() const override {
    return "transform-dialect-interpreter";
  }

  StringRef getDescription() const override {
    return "apply transform dialect operations one by one";
  }

  bool canScheduleOn(RegisteredOperationName name) const override {
    return true;
  }

  TransformDialectInterpreter(StringRef transformFileName = StringRef()) {
    this->transformFileName = transformFileName.str();
  }
  TransformDialectInterpreter(const TransformDialectInterpreter &pass) {
    this->transformFileName = pass.transformFileName;
    // TODO: if we really don't like shared_ptr, we could also clone the
    // transformModule here.
    sharedTransformModule = pass.sharedTransformModule;
  }

  LogicalResult initialize(MLIRContext *context) override {
    OwningOpRef<ModuleOp> module;
    if (failed(transform::parseTransformModuleFromFile(
            context, transformFileName, module)))
      return failure();
    sharedTransformModule =
        std::make_shared<OwningOpRef<ModuleOp>>(std::move(module));
    return success();
  }

  void runOnOperation() override {
    Operation *target = getOperation();
    bool parsedTransform = (sharedTransformModule && *sharedTransformModule);
    assert(parsedTransform || (target->getNumRegions() == 1 &&
                               target->getRegion(0).getBlocks().size() == 1) &&
                                  "Cannot extract transform from op");
    Region &transformRegion = parsedTransform
                                  ? (*sharedTransformModule)->getRegion()
                                  : target->getRegion(0);
    if (failed(transform::applyTransformsInRegion(transformRegion, target)))
      return signalPassFailure();
  }

protected:
  Pass::Option<std::string> transformFileName{
      *this, "transform-file-name",
      ::llvm::cl::desc(
          "File name containing a transform dialect specification to apply.")};

private:
  // The parsed transform module to be used for scheduling.
  // TODO: Figure a better way to build a transform module and transport it in
  // the proper places in the IR as it is transformed by IREE so that it is
  // available with better ownership semantics.
  // Note: we wrap the OwningOpRef to get the desired destruction mechanism.
  // Note: shared_ptr is not great but we know the sharedTransformModule is
  // readonly.
  // Alternatives comprise:
  //   1. no shared_ptr but copying the module with every pass clone that the
  //      OpPassManager decides to perform.
  //   2. lifting ownership of the parsed transform module higher up in the
  //      IREE stack. This may be only shift the problem as we have passes
  //      building pass managers in IREE.
  //   3. build better support to embed the transformation module in the
  //      input IR and transport it to the place of use in IREE. This is deemed
  //      too intrusive atm.
  //   4. (future) config/resources mechanism that is being proposed in core?
  std::shared_ptr<OwningOpRef<ModuleOp>> sharedTransformModule;
};

struct DropSchedulePass : public PassWrapper<DropSchedulePass, Pass> {
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DropSchedulePass)

  StringRef getArgument() const final {
    return "transform-dialect-drop-schedule";
  }

  StringRef getDescription() const final {
    return "Drop the schedule from the operation";
  }

  bool canScheduleOn(RegisteredOperationName opName) const override {
    return true;
  }

  void runOnOperation() override {
    getOperation()->walk<WalkOrder::PreOrder>([&](Operation *nestedOp) {
      if (isa<iree_compiler::IREE::LinalgExt::DoNotDCEOperandsOp>(nestedOp))
        nestedOp->erase();
      if (isa<::mlir::transform::TransformOpInterface>(nestedOp)) {
        nestedOp->erase();
        return WalkResult::skip();
      }
      return WalkResult::advance();
    });
  }
};
} // namespace

/// Create a Transform dialect interpreter pass.
std::unique_ptr<Pass>
mlir::createTransformDialectInterpreterPass(llvm::StringRef transformFileName) {
  return std::make_unique<TransformDialectInterpreter>(transformFileName);
}

/// Create a Linalg pass to drop the schedule from the module.
std::unique_ptr<Pass> mlir::createDropSchedulePass() {
  return std::make_unique<DropSchedulePass>();
}

/// Registration hook for the Linalg drop schedule from module pass.
void mlir::linalg::transform::registerDropSchedulePass() {
  PassRegistration<DropSchedulePass>();
}

/// Registration hook for the Transform dialect interpreter pass.
void mlir::linalg::transform::registerTransformDialectInterpreterPass() {
  PassRegistration<TransformDialectInterpreter>();
}
