//===- SparseTensorStorageLayout.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header file defines utilities for lowering and accessing sparse tensor
// types.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_DIALECT_SPARSETENSOR_TRANSFORMS_SPARSETENSORBUILDER_H_
#define MLIR_DIALECT_SPARSETENSOR_TRANSFORMS_SPARSETENSORBUILDER_H_

#include "mlir/Conversion/LLVMCommon/StructBuilder.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"
#include "mlir/Dialect/SparseTensor/Transforms/Passes.h"
#include "mlir/Transforms/DialectConversion.h"

namespace mlir {
namespace sparse_tensor {

//===----------------------------------------------------------------------===//
// SparseTensorDescriptor and helpers, manage the sparse tensor memory layout
// scheme.
//
// Sparse tensor storage scheme for rank-dimensional tensor is organized
// as a single compound type with the following fields. Note that every
// memref with ? size actually behaves as a "vector", i.e. the stored
// size is the capacity and the used size resides in the memSizes array.
//
// struct {
//   ; per-dimension d:
//   ;  if dense:
//        <nothing>
//   ;  if compresed:
//        memref<? x ptr>  pointers-d  ; pointers for sparse dim d
//        memref<? x idx>  indices-d   ; indices for sparse dim d
//   ;  if singleton:
//        memref<? x idx>  indices-d   ; indices for singleton dim d
//   memref<? x eltType> values        ; values
//
//   ; sparse tensor metadata
//   struct {
//     array<rank x int> dimSizes    ; sizes for each dimension
//     array<n x int> memSizes;      ; sizes for each data memref
//   }
// };
//
//===----------------------------------------------------------------------===//

enum class SparseTensorFieldKind : uint32_t {
  StorageSpec = 0,
  PtrMemRef = 1,
  IdxMemRef = 2,
  ValMemRef = 3
};

static_assert(static_cast<uint32_t>(SparseTensorFieldKind::PtrMemRef) ==
              static_cast<uint32_t>(StorageSpecifierKind::PtrMemSize));
static_assert(static_cast<uint32_t>(SparseTensorFieldKind::IdxMemRef) ==
              static_cast<uint32_t>(StorageSpecifierKind::IdxMemSize));
static_assert(static_cast<uint32_t>(SparseTensorFieldKind::ValMemRef) ==
              static_cast<uint32_t>(StorageSpecifierKind::ValMemSize));

/// For each field that will be allocated for the given sparse tensor encoding,
/// calls the callback with the corresponding field index, field kind, dimension
/// (for sparse tensor level memrefs) and dimlevelType.
/// The field index always starts with zero and increments by one between two
/// callback invocations.
/// Ideally, all other methods should rely on this function to query a sparse
/// tensor fields instead of relying on ad-hoc index computation.
void foreachFieldInSparseTensor(
    SparseTensorEncodingAttr,
    llvm::function_ref<bool(unsigned /*fieldIdx*/,
                            SparseTensorFieldKind /*fieldKind*/,
                            unsigned /*dim (if applicable)*/,
                            DimLevelType /*DLT (if applicable)*/)>,
    bool isBuffer = false);

/// Same as above, except that it also builds the Type for the corresponding
/// field.
void foreachFieldAndTypeInSparseTensor(
    RankedTensorType,
    llvm::function_ref<bool(Type /*fieldType*/, unsigned /*fieldIdx*/,
                            SparseTensorFieldKind /*fieldKind*/,
                            unsigned /*dim (if applicable)*/,
                            DimLevelType /*DLT (if applicable)*/)>);

/// Gets the total number of fields for the given sparse tensor encoding.
unsigned getNumFieldsFromEncoding(SparseTensorEncodingAttr enc, bool isBuffer);

/// Gets the total number of data fields (index arrays, pointer arrays, and a
/// value array) for the given sparse tensor encoding.
unsigned getNumDataFieldsFromEncoding(SparseTensorEncodingAttr enc);

inline StorageSpecifierKind toSpecifierKind(SparseTensorFieldKind kind) {
  assert(kind != SparseTensorFieldKind::StorageSpec);
  return static_cast<StorageSpecifierKind>(kind);
}

inline SparseTensorFieldKind toFieldKind(StorageSpecifierKind kind) {
  assert(kind != StorageSpecifierKind::DimSize);
  return static_cast<SparseTensorFieldKind>(kind);
}

/// Provides methods to access fields of a sparse tensor with the given
/// encoding. When isBuffer is true, the fields are the actual buffers of the
/// sparse tensor storage. In particular, when a linear buffer is used to
/// store the COO data as an array-of-structures, the fields include the
/// linear buffer (isBuffer=true) or includes the subviews of the buffer for the
/// indices (isBuffer=false).
template <bool isBuffer>
class StorageLayout {
public:
  explicit StorageLayout(SparseTensorEncodingAttr enc) : enc(enc) {}

  ///
  /// Getters: get the field index for required field.
  ///

  unsigned getMemRefFieldIndex(SparseTensorFieldKind kind,
                               std::optional<unsigned> dim) const {
    return getFieldIndexAndStride(kind, dim).first;
  }

  unsigned getMemRefFieldIndex(StorageSpecifierKind kind,
                               std::optional<unsigned> dim) const {
    return getMemRefFieldIndex(toFieldKind(kind), dim);
  }

  static unsigned getNumFieldsFromEncoding(SparseTensorEncodingAttr enc) {
    return sparse_tensor::getNumFieldsFromEncoding(enc, isBuffer);
  }

  static void foreachFieldInSparseTensor(
      const SparseTensorEncodingAttr enc,
      llvm::function_ref<bool(unsigned, SparseTensorFieldKind, unsigned,
                              DimLevelType)>
          callback) {
    return sparse_tensor::foreachFieldInSparseTensor(enc, callback, isBuffer);
  }

  std::pair<unsigned, unsigned>
  getFieldIndexAndStride(SparseTensorFieldKind kind,
                         std::optional<unsigned> dim) const {
    unsigned fieldIdx = -1u;
    unsigned stride = 1;
    if (isBuffer && kind == SparseTensorFieldKind::IdxMemRef) {
      assert(dim.has_value());
      unsigned cooStart = getCOOStart(enc);
      unsigned rank = enc.getDimLevelType().size();
      if (dim.value() >= cooStart && dim.value() < rank) {
        dim = cooStart;
        stride = rank - cooStart;
      }
    }
    foreachFieldInSparseTensor(
        enc,
        [dim, kind, &fieldIdx](unsigned fIdx, SparseTensorFieldKind fKind,
                               unsigned fDim, DimLevelType dlt) -> bool {
          if ((dim && fDim == dim.value() && kind == fKind) ||
              (kind == fKind && fKind == SparseTensorFieldKind::ValMemRef)) {
            fieldIdx = fIdx;
            // Returns false to break the iteration.
            return false;
          }
          return true;
        });
    assert(fieldIdx != -1u);
    return std::pair<unsigned, unsigned>(fieldIdx, stride);
  }

private:
  SparseTensorEncodingAttr enc;
};

class SparseTensorSpecifier {
public:
  explicit SparseTensorSpecifier(Value specifier) : specifier(specifier) {}

  // Undef value for dimension sizes, all zero value for memory sizes.
  static Value getInitValue(OpBuilder &builder, Location loc,
                            RankedTensorType rtp);

  /*implicit*/ operator Value() { return specifier; }

  Value getSpecifierField(OpBuilder &builder, Location loc,
                          StorageSpecifierKind kind, Optional<unsigned> dim);

  void setSpecifierField(OpBuilder &builder, Location loc, Value v,
                         StorageSpecifierKind kind, Optional<unsigned> dim);

  Type getFieldType(StorageSpecifierKind kind, Optional<unsigned> dim) {
    return specifier.getType().getFieldType(kind, dim);
  }

private:
  TypedValue<StorageSpecifierType> specifier;
};

/// A helper class around an array of values that corresponding to a sparse
/// tensor, provides a set of meaningful APIs to query and update a particular
/// field in a consistent way.
/// Users should not make assumption on how a sparse tensor is laid out but
/// instead relies on this class to access the right value for the right field.
template <bool mut>
class SparseTensorDescriptorImpl {
protected:
  // Uses ValueRange for immuatable descriptors; uses SmallVectorImpl<Value> &
  // for mutable descriptors.
  // Using SmallVector for mutable descriptor allows users to reuse it as a tmp
  // buffers to append value for some special cases, though users should be
  // responsible to restore the buffer to legal states after their use. It is
  // probably not a clean way, but it is the most efficient way to avoid copying
  // the fields into another SmallVector. If a more clear way is wanted, we
  // should change it to MutableArrayRef instead.
  using ValueArrayRef = typename std::conditional<mut, SmallVectorImpl<Value> &,
                                                  ValueRange>::type;

  SparseTensorDescriptorImpl(Type tp)
      : rType(tp.cast<RankedTensorType>()), fields() {}

  SparseTensorDescriptorImpl(Type tp, ValueArrayRef fields)
      : rType(tp.cast<RankedTensorType>()), fields(fields) {
    sanityCheck();
  }

  void sanityCheck() {
    assert(getSparseTensorEncoding(rType) &&
           StorageLayout<mut>::getNumFieldsFromEncoding(
               getSparseTensorEncoding(rType)) == fields.size());
    // We should make sure the class is trivially copyable (and should be small
    // enough) such that we can pass it by value.
    static_assert(
        std::is_trivially_copyable_v<SparseTensorDescriptorImpl<mut>>);
  }

public:
  unsigned getMemRefFieldIndex(SparseTensorFieldKind kind,
                               Optional<unsigned> dim) const {
    // Delegates to storage layout.
    StorageLayout<mut> layout(getSparseTensorEncoding(rType));
    return layout.getMemRefFieldIndex(kind, dim);
  }

  unsigned getPtrMemRefIndex(unsigned ptrDim) const {
    return getMemRefFieldIndex(SparseTensorFieldKind::PtrMemRef, ptrDim);
  }

  unsigned getIdxMemRefIndex(unsigned idxDim) const {
    return getMemRefFieldIndex(SparseTensorFieldKind::IdxMemRef, idxDim);
  }

  unsigned getValMemRefIndex() const {
    return getMemRefFieldIndex(SparseTensorFieldKind::ValMemRef, std::nullopt);
  }

  unsigned getNumFields() const { return fields.size(); }

  ///
  /// Getters: get the value for required field.
  ///

  Value getSpecifierField(OpBuilder &builder, Location loc,
                          StorageSpecifierKind kind,
                          Optional<unsigned> dim) const {
    SparseTensorSpecifier md(fields.back());
    return md.getSpecifierField(builder, loc, kind, dim);
  }

  Value getDimSize(OpBuilder &builder, Location loc, unsigned dim) const {
    return getSpecifierField(builder, loc, StorageSpecifierKind::DimSize, dim);
  }

  Value getPtrMemRef(unsigned ptrDim) const {
    return getMemRefField(SparseTensorFieldKind::PtrMemRef, ptrDim);
  }

  Value getIdxMemRef(unsigned idxDim) const {
    return getMemRefField(SparseTensorFieldKind::IdxMemRef, idxDim);
  }

  Value getValMemRef() const {
    return getMemRefField(SparseTensorFieldKind::ValMemRef, std::nullopt);
  }

  Value getMemRefField(SparseTensorFieldKind kind,
                       Optional<unsigned> dim) const {
    return fields[getMemRefFieldIndex(kind, dim)];
  }

  Value getMemRefField(unsigned fidx) const {
    assert(fidx < fields.size() - 1);
    return fields[fidx];
  }

  Value getField(unsigned fidx) const {
    assert(fidx < fields.size());
    return fields[fidx];
  }

  ValueRange getMemRefFields() const {
    ValueRange ret = fields;
    // Drop the last metadata fields.
    return ret.slice(0, fields.size() - 1);
  }

  Type getMemRefElementType(SparseTensorFieldKind kind,
                            Optional<unsigned> dim) const {
    return getMemRefField(kind, dim)
        .getType()
        .template cast<MemRefType>()
        .getElementType();
  }

  RankedTensorType getTensorType() const { return rType; }
  ValueArrayRef getFields() const { return fields; }

protected:
  RankedTensorType rType;
  ValueArrayRef fields;
};

class MutSparseTensorDescriptor : public SparseTensorDescriptorImpl<true> {
public:
  MutSparseTensorDescriptor(Type tp, ValueArrayRef buffers)
      : SparseTensorDescriptorImpl<true>(tp, buffers) {}

  ///
  /// Getters: get the value for required field.
  ///

  Value getPtrMemSize(OpBuilder &builder, Location loc, unsigned dim) const {
    return getSpecifierField(builder, loc, StorageSpecifierKind::PtrMemSize,
                             dim);
  }

  Value getIdxMemSize(OpBuilder &builder, Location loc, unsigned dim) const {
    return getSpecifierField(builder, loc, StorageSpecifierKind::IdxMemSize,
                             dim);
  }

  Value getValMemSize(OpBuilder &builder, Location loc) const {
    return getSpecifierField(builder, loc, StorageSpecifierKind::ValMemSize,
                             std::nullopt);
  }

  ///
  /// Setters: update the value for required field (only enabled for
  /// MutSparseTensorDescriptor).
  ///

  template <typename T = Value>
  void setMemRefField(SparseTensorFieldKind kind, Optional<unsigned> dim, T v) {
    fields[getMemRefFieldIndex(kind, dim)] = v;
  }

  template <typename T = Value>
  void setMemRefField(unsigned fidx, T v) {
    assert(fidx < fields.size() - 1);
    fields[fidx] = v;
  }

  template <typename T = Value>
  void setField(unsigned fidx, T v) {
    assert(fidx < fields.size());
    fields[fidx] = v;
  }

  template <typename T = Value>
  void setSpecifierField(OpBuilder &builder, Location loc,
                         StorageSpecifierKind kind, Optional<unsigned> dim,
                         T v) {
    SparseTensorSpecifier md(fields.back());
    md.setSpecifierField(builder, loc, v, kind, dim);
    fields.back() = md;
  }

  template <typename T = Value>
  void setDimSize(OpBuilder &builder, Location loc, unsigned dim, T v) {
    setSpecifierField(builder, loc, StorageSpecifierKind::DimSize, dim, v);
  }

  std::pair<unsigned, unsigned>
  getIdxMemRefIndexAndStride(unsigned idxDim) const {
    StorageLayout<true> layout(getSparseTensorEncoding(rType));
    return layout.getFieldIndexAndStride(SparseTensorFieldKind::IdxMemRef,
                                         idxDim);
  }

  Value getAOSMemRef() const {
    auto enc = getSparseTensorEncoding(rType);
    unsigned cooStart = getCOOStart(enc);
    assert(cooStart < enc.getDimLevelType().size());
    return getIdxMemRef(cooStart);
  }
};

class SparseTensorDescriptor : public SparseTensorDescriptorImpl<false> {
public:
  SparseTensorDescriptor(OpBuilder &builder, Location loc, Type tp,
                         ValueArrayRef buffers);

private:
  // Store the fields passed to SparseTensorDescriptorImpl when the tensor has
  // a COO region.
  SmallVector<Value> expandedFields;
};

/// Returns the "tuple" value of the adapted tensor.
inline UnrealizedConversionCastOp getTuple(Value tensor) {
  return llvm::cast<UnrealizedConversionCastOp>(tensor.getDefiningOp());
}

/// Packs the given values as a "tuple" value.
inline Value genTuple(OpBuilder &builder, Location loc, Type tp,
                      ValueRange values) {
  return builder.create<UnrealizedConversionCastOp>(loc, TypeRange(tp), values)
      .getResult(0);
}

inline Value genTuple(OpBuilder &builder, Location loc,
                      MutSparseTensorDescriptor desc) {
  return genTuple(builder, loc, desc.getTensorType(), desc.getFields());
}

inline SparseTensorDescriptor
getDescriptorFromTensorTuple(OpBuilder &builder, Location loc, Value tensor) {
  auto tuple = getTuple(tensor);
  return SparseTensorDescriptor(builder, loc, tuple.getResultTypes()[0],
                                tuple.getInputs());
}

inline MutSparseTensorDescriptor
getMutDescriptorFromTensorTuple(Value tensor, SmallVectorImpl<Value> &fields) {
  auto tuple = getTuple(tensor);
  fields.assign(tuple.getInputs().begin(), tuple.getInputs().end());
  return MutSparseTensorDescriptor(tuple.getResultTypes()[0], fields);
}

} // namespace sparse_tensor
} // namespace mlir

#endif // MLIR_DIALECT_SPARSETENSOR_TRANSFORMS_SPARSETENSORBUILDER_H_
