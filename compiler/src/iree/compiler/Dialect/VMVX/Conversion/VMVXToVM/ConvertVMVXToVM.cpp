// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/VMVX/Conversion/VMVXToVM/ConvertVMVXToVM.h"

#include "iree/compiler/Dialect/Util/IR/UtilTypes.h"
#include "iree/compiler/Dialect/VM/Conversion/ConversionTarget.h"
#include "iree/compiler/Dialect/VM/Conversion/ImportUtils.h"
#include "iree/compiler/Dialect/VM/Conversion/TypeConverter.h"
#include "iree/compiler/Dialect/VM/IR/VMOps.h"
#include "iree/compiler/Dialect/VMVX/IR/VMVXOps.h"
#include "iree/compiler/Dialect/VMVX/IR/VMVXTypes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace iree_compiler {
namespace {

// Erases an op. This should only be used for ops that are legalized away
// as part of lowering (i.e. tagging or metadata ops that are unrepresentable
// in the VM dialect).
class EraseNonVMOp : public ConversionPattern {
 public:
  EraseNonVMOp(StringRef rootName, MLIRContext *ctx)
      : ConversionPattern(rootName, 0, ctx) {}

  LogicalResult matchAndRewrite(
      Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  }
};

// VMVX -> VM import conversion base for generic ops.
// Handles signatures with integers, VM types, or simple buffers.
// TODO: This is a big mess and doesn't support the generality we need. Redo
// it.
template <typename T>
class VMVXImportOpConversion : public OpConversionPattern<T> {
 public:
  VMVXImportOpConversion(MLIRContext *context, SymbolTable &importSymbols,
                         TypeConverter &typeConverter)
      : OpConversionPattern<T>(context),
        importSymbols(importSymbols),
        typeConverter(typeConverter) {}

  LogicalResult matchAndRewrite(
      T op, typename T::Adaptor adaptor,
      ConversionPatternRewriter &rewriter) const override {
    std::string importFqName = getImportFqName(op);
    auto importOp =
        importSymbols.template lookup<IREE::VM::ImportOp>(importFqName);
    if (!importOp) {
      op.emitError() << "failed to resolve VM function import for "
                     << importFqName;
      return failure();
    }
    auto results = emitCall(op, adaptor, importOp, rewriter);
    if (!results.has_value()) return failure();
    rewriter.replaceOp(op, results.value());
    return success();
  }

 protected:
  virtual std::string getImportFqName(T op) const = 0;
  virtual Optional<SmallVector<Value>> emitCall(
      T op, typename T::Adaptor adaptor, IREE::VM::ImportOp importOp,
      ConversionPatternRewriter &rewriter) const {
    return rewriteToCall(op, adaptor, importOp, typeConverter, rewriter);
  }

  std::string getSizedTypeStr(Type elementType) const {
    int bitWidth = elementType.getIntOrFloatBitWidth();
    // Widen i1 -> i8 to match the VM type conversion.
    if (bitWidth == 1) {
      bitWidth = 8;
    }
    return "x" + std::to_string(bitWidth);
  }

  std::string getTypedTypeStr(Type type, bool forceUnsigned = false) const {
    Type elementType = type;
    auto shapedType = type.dyn_cast<ShapedType>();
    if (shapedType) {
      elementType = shapedType.getElementType();
    }

    std::string typePrefix = "x";
    if (elementType.isa<FloatType>()) {
      typePrefix = "f";
    } else if (elementType.isSignlessInteger()) {
      typePrefix = forceUnsigned ? "u" : "i";
    }

    int bitWidth = elementType.getIntOrFloatBitWidth();
    // Widen i1 -> i8 to match the VM type conversion.
    if (bitWidth == 1) {
      bitWidth = 8;
    }
    return typePrefix + std::to_string(bitWidth);
  }

 private:
  SymbolTable &importSymbols;
  TypeConverter &typeConverter;
};

class BinaryOpConversion : public VMVXImportOpConversion<IREE::VMVX::BinaryOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::BinaryOp op) const override {
    int rank = op.getLhsStrides().size();
    std::string name("vmvx.");
    name.append(op.getOpcode().begin(), op.getOpcode().end());
    name.append(".");
    name.append(std::to_string(rank));
    name.append("d.");
    name.append(getTypedTypeStr(op.getElementType()));
    return name;
  }
};

// Converts the vmvx.copy op to an appropriate typed import.
class CopyOpConversion : public VMVXImportOpConversion<IREE::VMVX::CopyOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::CopyOp op) const override {
    int rank = op.getInStrides().size();
    std::string name("vmvx.copy.");
    name.append(std::to_string(rank));
    name.append("d.");
    name.append(getSizedTypeStr(op.getElementType()));
    return name;
  }
};

// Converts the vmvx.fill2d op to an appropriate typed import.
class Fill2DOpConversion : public VMVXImportOpConversion<IREE::VMVX::Fill2DOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::Fill2DOp op) const override {
    std::string name("vmvx.fill.2d.");
    name.append(getSizedTypeStr(op.getScalar().getType()));
    return name;
  }
};

// Converts the vmvx.matmul op to an appropriate typed import.
class MatmulOpConversion : public VMVXImportOpConversion<IREE::VMVX::MatmulOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::MatmulOp op) const override {
    std::string name("vmvx.matmul.");
    name.append(getTypedTypeStr(op.getLhsType()));
    name.append(getTypedTypeStr(op.getRhsType()));
    name.append(getTypedTypeStr(op.getOutType()));
    return name;
  }
};

// Converts the vmvx.mmt4d op to an appropriate typed import.
class Mmt4dOpConversion : public VMVXImportOpConversion<IREE::VMVX::Mmt4dOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::Mmt4dOp op) const override {
    std::string name("vmvx.mmt4d.");
    name.append(getTypedTypeStr(op.getLhsType()));
    name.append(getTypedTypeStr(op.getRhsType()));
    name.append(getTypedTypeStr(op.getOutType()));
    return name;
  }
};

// Converts the vmvx.pack op to an appropriate typed import.
class PackOpConversion : public VMVXImportOpConversion<IREE::VMVX::PackOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::PackOp op) const override {
    std::string name("vmvx.pack.");
    name.append(getTypedTypeStr(op.getInType()));
    name.append(getTypedTypeStr(op.getOutType()));
    return name;
  }
};

// Converts the vmvx.unpack op to an appropriate typed import.
class UnpackOpConversion : public VMVXImportOpConversion<IREE::VMVX::UnpackOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::UnpackOp op) const override {
    std::string name("vmvx.unpack.");
    name.append(getTypedTypeStr(op.getInType()));
    name.append(getTypedTypeStr(op.getOutType()));
    return name;
  }
};

class UnaryOpConversion : public VMVXImportOpConversion<IREE::VMVX::UnaryOp> {
 public:
  using VMVXImportOpConversion::VMVXImportOpConversion;

  std::string getImportFqName(IREE::VMVX::UnaryOp op) const override {
    int rank = op.getInStrides().size();
    std::string name("vmvx.");
    name.append(op.getOpcode().begin(), op.getOpcode().end());
    name.append(".");
    name.append(std::to_string(rank));
    name.append("d.");
    name.append(getTypedTypeStr(op.getElementType()));
    return name;
  }
};

}  // namespace

void populateVMVXToVMPatterns(MLIRContext *context,
                              ConversionTarget &conversionTarget,
                              TypeConverter &typeConverter,
                              SymbolTable &importSymbols,
                              RewritePatternSet &patterns) {
  patterns.insert<BinaryOpConversion, CopyOpConversion, Fill2DOpConversion,
                  MatmulOpConversion, Mmt4dOpConversion, UnaryOpConversion,
                  PackOpConversion, UnpackOpConversion>(context, importSymbols,
                                                        typeConverter);
}

}  // namespace iree_compiler
}  // namespace mlir
