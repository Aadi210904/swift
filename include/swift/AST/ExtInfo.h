//===--- ExtInfo.h - Extended information for function types ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Defines the ASTExtInfo and SILExtInfo classes, which are used to store
// the calling convention and related information for function types in the AST
// and SIL respectively. These types are lightweight and immutable; they are
// constructed using builder-pattern style APIs to enforce invariants.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_EXTINFO_H
#define SWIFT_EXTINFO_H

#include "swift/AST/AutoDiff.h"
#include "swift/AST/ClangModuleLoader.h"

#include "llvm/ADT/Optional.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

namespace clang {
class Type;
} // namespace clang

namespace swift {
class AnyFunctionType;
class ASTExtInfo;
class ASTExtInfoBuilder;
class FunctionType;
class SILExtInfo;
class SILExtInfoBuilder;
class SILFunctionType;
} // namespace swift

namespace swift {

// MARK: - ClangTypeInfo
/// Wrapper class for storing a clang::Type in an (AST|SIL)ExtInfo.
class ClangTypeInfo {
  friend AnyFunctionType;
  friend FunctionType;
  friend SILFunctionType;
  friend ASTExtInfoBuilder;
  friend SILExtInfoBuilder;

  // We preserve a full clang::Type *, not a clang::FunctionType * as:
  // 1. We need to keep sugar in case we need to present an error to the user
  //    (for AnyFunctionType).
  // 2. The actual type being stored is [ignoring sugar] either a
  //    clang::PointerType, a clang::BlockPointerType, or a
  //    clang::ReferenceType which points to a clang::FunctionType.
  //
  // When used as a part of SILFunctionType, the type is canonical.
  const clang::Type *type;

  constexpr ClangTypeInfo() : type(nullptr) {}
  constexpr ClangTypeInfo(const clang::Type *type) : type(type) {}

  ClangTypeInfo getCanonical() const;

public:
  constexpr const clang::Type *getType() const { return type; }

  constexpr bool empty() const { return !type; }

  /// Use the ClangModuleLoader to print the Clang type as a string.
  void printType(ClangModuleLoader *cml, llvm::raw_ostream &os) const;

  void dump(llvm::raw_ostream &os) const;
};

// MARK: - FunctionTypeRepresentation
/// The representation form of a function.
enum class FunctionTypeRepresentation : uint8_t {
  /// A "thick" function that carries a context pointer to reference captured
  /// state. The default native function representation.
  Swift = 0,

  /// A thick function that is represented as an Objective-C block.
  Block,

  /// A "thin" function that needs no context.
  Thin,

  /// A C function pointer (or reference), which is thin and also uses the C
  /// calling convention.
  CFunctionPointer,

  /// The value of the greatest AST function representation.
  Last = CFunctionPointer,
};

// MARK: - SILFunctionTypeRepresentation

/// The representation form of a SIL function.
///
/// This is a superset of FunctionTypeRepresentation. The common representations
/// must share an enum value.
///
/// TODO: The overlap of SILFunctionTypeRepresentation and
/// FunctionTypeRepresentation is a total hack necessitated by the way SIL
/// TypeLowering is currently written. We ought to refactor TypeLowering so that
/// it is not necessary to distinguish these cases.
enum class SILFunctionTypeRepresentation : uint8_t {
  /// A freestanding thick function.
  Thick = uint8_t(FunctionTypeRepresentation::Swift),

  /// A thick function that is represented as an Objective-C block.
  Block = uint8_t(FunctionTypeRepresentation::Block),

  /// A freestanding thin function that needs no context.
  Thin = uint8_t(FunctionTypeRepresentation::Thin),

  /// A C function pointer, which is thin and also uses the C calling
  /// convention.
  CFunctionPointer = uint8_t(FunctionTypeRepresentation::CFunctionPointer),

  /// The value of the greatest AST function representation.
  LastAST = CFunctionPointer,

  /// The value of the least SIL-only function representation.
  FirstSIL = 8,

  /// A Swift instance method.
  Method = FirstSIL,

  /// An Objective-C method.
  ObjCMethod,

  /// A Swift protocol witness.
  WitnessMethod,

  /// A closure invocation function that has not been bound to a context.
  Closure,
};

/// Can this calling convention result in a function being called indirectly
/// through the runtime.
constexpr bool canBeCalledIndirectly(SILFunctionTypeRepresentation rep) {
  switch (rep) {
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Block:
  case SILFunctionTypeRepresentation::Closure:
    return false;
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
    return true;
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

// MARK: - ASTExtInfoBuilder
/// A builder type for creating an \c ASTExtInfo.
///
/// The main API public includes the \c withXYZ and \p build() methods.
class ASTExtInfoBuilder {
  friend AnyFunctionType;
  friend ASTExtInfo;

  // If bits are added or removed, then TypeBase::AnyFunctionTypeBits
  // and NumMaskBits must be updated, and they must match.
  //
  //   |representation|noEscape|async|throws|differentiability|
  //   |    0 .. 3    |    4   |  5  |   6  |      7 .. 8     |
  //
  enum : unsigned {
    RepresentationMask = 0xF << 0,
    NoEscapeMask = 1 << 4,
    AsyncMask = 1 << 5,
    ThrowsMask = 1 << 6,
    DifferentiabilityMaskOffset = 7,
    DifferentiabilityMask = 0x3 << DifferentiabilityMaskOffset,
    NumMaskBits = 9
  };

  unsigned bits; // Naturally sized for speed.

  ClangTypeInfo clangTypeInfo;

  using Representation = FunctionTypeRepresentation;

  static void assertIsFunctionType(const clang::Type *);

  ASTExtInfoBuilder(unsigned bits, ClangTypeInfo clangTypeInfo)
      : bits(bits), clangTypeInfo(clangTypeInfo) {
    // TODO: [clang-function-type-serialization] Once we start serializing
    // the Clang type, we should also assert that the pointer is non-null.
    auto Rep = Representation(bits & RepresentationMask);
    if ((Rep == Representation::CFunctionPointer) && clangTypeInfo.type)
      assertIsFunctionType(clangTypeInfo.type);
  }

public:
  // Constructor with all defaults.
  ASTExtInfoBuilder()
      : ASTExtInfoBuilder(Representation::Swift, false, false,
                          DifferentiabilityKind::NonDifferentiable, nullptr) {}

  // Constructor for polymorphic type.
  ASTExtInfoBuilder(Representation rep, bool throws)
      : ASTExtInfoBuilder(rep, false, throws,
                          DifferentiabilityKind::NonDifferentiable, nullptr) {}

  // Constructor with no defaults.
  ASTExtInfoBuilder(Representation rep, bool isNoEscape, bool throws,
                    DifferentiabilityKind diffKind, const clang::Type *type)
      : ASTExtInfoBuilder(
            ((unsigned)rep) | (isNoEscape ? NoEscapeMask : 0) |
                (throws ? ThrowsMask : 0) |
                (((unsigned)diffKind << DifferentiabilityMaskOffset) &
                 DifferentiabilityMask),
            ClangTypeInfo(type)) {}

  void checkInvariants() const;

  /// Check if \c this is well-formed and create an ExtInfo.
  ASTExtInfo build() const;

  constexpr Representation getRepresentation() const {
    unsigned rawRep = bits & RepresentationMask;
    assert(rawRep <= unsigned(Representation::Last) &&
           "unexpected SIL representation");
    return Representation(rawRep);
  }

  constexpr bool isNoEscape() const { return bits & NoEscapeMask; }

  constexpr bool async() const { return bits & AsyncMask; }

  constexpr bool throws() const { return bits & ThrowsMask; }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return DifferentiabilityKind((bits & DifferentiabilityMask) >>
                                 DifferentiabilityMaskOffset);
  }

  constexpr bool isDifferentiable() const {
    return getDifferentiabilityKind() >
           DifferentiabilityKind::NonDifferentiable;
  }

  /// Get the underlying ClangTypeInfo value if it is not the default value.
  Optional<ClangTypeInfo> getClangTypeInfo() const {
    return clangTypeInfo.empty() ? Optional<ClangTypeInfo>() : clangTypeInfo;
  }

  constexpr SILFunctionTypeRepresentation getSILRepresentation() const {
    unsigned rawRep = bits & RepresentationMask;
    return SILFunctionTypeRepresentation(rawRep);
  }

  constexpr bool hasSelfParam() const {
    switch (getSILRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Block:
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::CFunctionPointer:
    case SILFunctionTypeRepresentation::Closure:
      return false;
    case SILFunctionTypeRepresentation::ObjCMethod:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::WitnessMethod:
      return true;
    }
    llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
  }

  /// True if the function representation carries context.
  constexpr bool hasContext() const {
    switch (getSILRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Block:
      return true;
    case SILFunctionTypeRepresentation::Thin:
    case SILFunctionTypeRepresentation::Method:
    case SILFunctionTypeRepresentation::ObjCMethod:
    case SILFunctionTypeRepresentation::WitnessMethod:
    case SILFunctionTypeRepresentation::CFunctionPointer:
    case SILFunctionTypeRepresentation::Closure:
      return false;
    }
    llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
  }

  // Note that we don't have setters. That is by design, use
  // the following with methods instead of mutating these objects.
  LLVM_NODISCARD
  ASTExtInfoBuilder withRepresentation(Representation rep) const {
    return ASTExtInfoBuilder((bits & ~RepresentationMask) | (unsigned)rep,
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withNoEscape(bool noEscape = true) const {
    return ASTExtInfoBuilder(noEscape ? (bits | NoEscapeMask)
                                      : (bits & ~NoEscapeMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withAsync(bool async = true) const {
    return ASTExtInfoBuilder(async ? (bits | AsyncMask)
                                   : (bits & ~AsyncMask),
                             clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withThrows(bool throws = true) const {
    return ASTExtInfoBuilder(
        throws ? (bits | ThrowsMask) : (bits & ~ThrowsMask), clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder
  withDifferentiabilityKind(DifferentiabilityKind differentiability) const {
    return ASTExtInfoBuilder(
        (bits & ~DifferentiabilityMask) |
            ((unsigned)differentiability << DifferentiabilityMaskOffset),
        clangTypeInfo);
  }
  LLVM_NODISCARD
  ASTExtInfoBuilder withClangFunctionType(const clang::Type *type) const {
    return ASTExtInfoBuilder(bits, ClangTypeInfo(type));
  }

  /// Put a SIL representation in the ExtInfo.
  ///
  /// SIL type lowering transiently generates AST function types with SIL
  /// representations. However, they shouldn't persist in the AST, and
  /// don't need to be parsed, printed, or serialized.
  LLVM_NODISCARD
  ASTExtInfoBuilder
  withSILRepresentation(SILFunctionTypeRepresentation rep) const {
    return ASTExtInfoBuilder((bits & ~RepresentationMask) | (unsigned)rep,
                             clangTypeInfo);
  }

  std::pair<unsigned, const void *> getFuncAttrKey() const {
    return std::make_pair(bits, clangTypeInfo.getType());
  }
}; // end ASTExtInfoBuilder

// MARK: - ASTExtInfo
/// Calling convention and related information for AnyFunctionType + subclasses.
///
/// New instances can be made from existing instances via \c ASTExtInfoBuilder,
/// typically using a code pattern like:
/// \code
/// extInfo.intoBuilder().withX(x).withY(y).build()
/// \endcode
class ASTExtInfo {
  friend ASTExtInfoBuilder;
  friend AnyFunctionType;

  ASTExtInfoBuilder builder;

  ASTExtInfo(ASTExtInfoBuilder builder) : builder(builder) {}
  ASTExtInfo(unsigned bits, ClangTypeInfo clangTypeInfo)
      : builder(bits, clangTypeInfo){};

public:
  ASTExtInfo() : builder(){};

  /// Create a builder with the same state as \c this.
  ASTExtInfoBuilder intoBuilder() const { return builder; }

private:
  constexpr unsigned getBits() const { return builder.bits; }

public:
  constexpr FunctionTypeRepresentation getRepresentation() const {
    return builder.getRepresentation();
  }

  constexpr SILFunctionTypeRepresentation getSILRepresentation() const {
    return builder.getSILRepresentation();
  }

  constexpr bool isNoEscape() const { return builder.isNoEscape(); }

  constexpr bool async() const { return builder.async(); }

  constexpr bool throws() const { return builder.throws(); }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return builder.getDifferentiabilityKind();
  }

  constexpr bool isDifferentiable() const { return builder.isDifferentiable(); }

  Optional<ClangTypeInfo> getClangTypeInfo() const {
    return builder.getClangTypeInfo();
  }

  constexpr bool hasSelfParam() const { return builder.hasSelfParam(); }

  constexpr bool hasContext() const { return builder.hasContext(); }

  /// Helper method for changing the representation.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withRepresentation for chaining.
  LLVM_NODISCARD
  ASTExtInfo withRepresentation(ASTExtInfoBuilder::Representation rep) const {
    return builder.withRepresentation(rep).build();
  }

  /// Helper method for changing only the noEscape field.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withNoEscape for chaining.
  LLVM_NODISCARD
  ASTExtInfo withNoEscape(bool noEscape = true) const {
    return builder.withNoEscape(noEscape).build();
  }

  /// Helper method for changing only the throws field.
  ///
  /// Prefer using \c ASTExtInfoBuilder::withThrows for chaining.
  LLVM_NODISCARD
  ASTExtInfo withThrows(bool throws = true) const {
    return builder.withThrows(throws).build();
  }

  bool operator==(ASTExtInfo other) const {
    return builder.bits == other.builder.bits;
  }
  bool operator!=(ASTExtInfo other) const {
    return builder.bits != other.builder.bits;
  }

  constexpr std::pair<unsigned, const void *> getFuncAttrKey() const {
    return builder.getFuncAttrKey();
  }
}; // end ASTExtInfo

// MARK: - SILFunctionLanguage

/// A language-level calling convention.
enum class SILFunctionLanguage : uint8_t {
  /// A variation of the Swift calling convention.
  Swift = 0,

  /// A variation of the C calling convention.
  C,
};

/// Map a SIL function representation to the base language calling convention
/// it uses.
constexpr
SILFunctionLanguage getSILFunctionLanguage(SILFunctionTypeRepresentation rep) {
  switch (rep) {
  case SILFunctionTypeRepresentation::ObjCMethod:
  case SILFunctionTypeRepresentation::CFunctionPointer:
  case SILFunctionTypeRepresentation::Block:
    return SILFunctionLanguage::C;
  case SILFunctionTypeRepresentation::Thick:
  case SILFunctionTypeRepresentation::Thin:
  case SILFunctionTypeRepresentation::Method:
  case SILFunctionTypeRepresentation::WitnessMethod:
  case SILFunctionTypeRepresentation::Closure:
    return SILFunctionLanguage::Swift;
  }

  llvm_unreachable("Unhandled SILFunctionTypeRepresentation in switch.");
}

// MARK: - SILExtInfoBuilder
/// A builder type for creating an \c SILExtInfo.
///
/// The main API public includes the \c withXYZ and \p build() methods.
class SILExtInfoBuilder {
  friend SILExtInfo;
  friend SILFunctionType;

  // If bits are added or removed, then TypeBase::SILFunctionTypeBits
  // and NumMaskBits must be updated, and they must match.

  //   |representation|pseudogeneric| noescape |differentiability|
  //   |    0 .. 3    |      4      |     5    |      6 .. 7     |
  //
  enum : unsigned {
    RepresentationMask = 0xF << 0,
    PseudogenericMask = 1 << 4,
    NoEscapeMask = 1 << 5,
    DifferentiabilityMaskOffset = 6,
    DifferentiabilityMask = 0x3 << DifferentiabilityMaskOffset,
    NumMaskBits = 8
  };

  unsigned bits; // Naturally sized for speed.

  ClangTypeInfo clangTypeInfo;

  using Language = SILFunctionLanguage;
  using Representation = SILFunctionTypeRepresentation;

  SILExtInfoBuilder(unsigned bits, ClangTypeInfo clangTypeInfo)
      : bits(bits), clangTypeInfo(clangTypeInfo) {}

public:
  // Constructor with all defaults.
  SILExtInfoBuilder() : bits(0), clangTypeInfo(ClangTypeInfo(nullptr)) {}

  // Constructor for polymorphic type.
  SILExtInfoBuilder(Representation rep, bool isPseudogeneric, bool isNoEscape,
                    DifferentiabilityKind diffKind, const clang::Type *type)
      : SILExtInfoBuilder(
            ((unsigned)rep) | (isPseudogeneric ? PseudogenericMask : 0) |
                (isNoEscape ? NoEscapeMask : 0) |
                (((unsigned)diffKind << DifferentiabilityMaskOffset) &
                 DifferentiabilityMask),
            ClangTypeInfo(type)) {}

  void checkInvariants() const;

  /// Check if \c this is well-formed and create an ExtInfo.
  SILExtInfo build() const;

  /// What is the abstract representation of this function value?
  constexpr Representation getRepresentation() const {
    return Representation(bits & RepresentationMask);
  }

  constexpr Language getLanguage() const {
    return getSILFunctionLanguage(getRepresentation());
  }

  /// Is this function pseudo-generic?  A pseudo-generic function
  /// is not permitted to dynamically depend on its type arguments.
  constexpr bool isPseudogeneric() const { return bits & PseudogenericMask; }

  // Is this function guaranteed to be no-escape by the type system?
  constexpr bool isNoEscape() const { return bits & NoEscapeMask; }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return DifferentiabilityKind((bits & DifferentiabilityMask) >>
                                 DifferentiabilityMaskOffset);
  }

  constexpr bool isDifferentiable() const {
    return getDifferentiabilityKind() !=
           DifferentiabilityKind::NonDifferentiable;
  }

  /// Get the underlying ClangTypeInfo value if it is not the default value.
  Optional<ClangTypeInfo> getClangTypeInfo() const {
    return clangTypeInfo.empty() ? Optional<ClangTypeInfo>() : clangTypeInfo;
  }

  constexpr bool hasSelfParam() const {
    switch (getRepresentation()) {
    case Representation::Thick:
    case Representation::Block:
    case Representation::Thin:
    case Representation::CFunctionPointer:
    case Representation::Closure:
      return false;
    case Representation::ObjCMethod:
    case Representation::Method:
    case Representation::WitnessMethod:
      return true;
    }
    llvm_unreachable("Unhandled Representation in switch.");
  }

  /// True if the function representation carries context.
  constexpr bool hasContext() const {
    switch (getRepresentation()) {
    case Representation::Thick:
    case Representation::Block:
      return true;
    case Representation::Thin:
    case Representation::CFunctionPointer:
    case Representation::ObjCMethod:
    case Representation::Method:
    case Representation::WitnessMethod:
    case Representation::Closure:
      return false;
    }
    llvm_unreachable("Unhandled Representation in switch.");
  }

  // Note that we don't have setters. That is by design, use
  // the following with methods instead of mutating these objects.
  SILExtInfoBuilder withRepresentation(Representation rep) const {
    return SILExtInfoBuilder((bits & ~RepresentationMask) | (unsigned)rep,
                             clangTypeInfo);
  }
  SILExtInfoBuilder withIsPseudogeneric(bool isPseudogeneric = true) const {
    return SILExtInfoBuilder(isPseudogeneric ? (bits | PseudogenericMask)
                                             : (bits & ~PseudogenericMask),
                             clangTypeInfo);
  }
  SILExtInfoBuilder withNoEscape(bool noEscape = true) const {
    return SILExtInfoBuilder(noEscape ? (bits | NoEscapeMask)
                                      : (bits & ~NoEscapeMask),
                             clangTypeInfo);
  }
  SILExtInfoBuilder
  withDifferentiabilityKind(DifferentiabilityKind differentiability) const {
    return SILExtInfoBuilder(
        (bits & ~DifferentiabilityMask) |
            ((unsigned)differentiability << DifferentiabilityMaskOffset),
        clangTypeInfo);
  }

  std::pair<unsigned, const void *> getFuncAttrKey() const {
    return std::make_pair(bits, clangTypeInfo.getType());
  }
}; // end SILExtInfoBuilder

// MARK: - SILExtInfo
/// Calling convention information for SILFunctionType.
///
/// New instances can be made from existing instances via \c SILExtInfoBuilder,
/// typically using a code pattern like:
/// \code
/// extInfo.intoBuilder().withX(x).withY(y).build()
/// \endcode
class SILExtInfo {
  friend SILExtInfoBuilder;
  friend SILFunctionType;

  SILExtInfoBuilder builder;

  SILExtInfo(SILExtInfoBuilder builder) : builder(builder) {}
  SILExtInfo(unsigned bits, ClangTypeInfo clangTypeInfo)
      : builder(bits, clangTypeInfo){};

public:
  SILExtInfo() : builder(){};

  static SILExtInfo getThin() {
    return SILExtInfoBuilder(SILExtInfoBuilder::Representation::Thin, false,
                             false, DifferentiabilityKind::NonDifferentiable,
                             nullptr)
        .build();
  }

  /// Create a builder with the same state as \c this.
  SILExtInfoBuilder intoBuilder() const { return builder; }

private:
  constexpr unsigned getBits() const { return builder.bits; }

public:
  constexpr SILFunctionTypeRepresentation getRepresentation() const {
    return builder.getRepresentation();
  }

  constexpr SILFunctionLanguage getLanguage() const {
    return builder.getLanguage();
  }

  constexpr bool isPseudogeneric() const { return builder.isPseudogeneric(); }

  constexpr bool isNoEscape() const { return builder.isNoEscape(); }

  constexpr DifferentiabilityKind getDifferentiabilityKind() const {
    return builder.getDifferentiabilityKind();
  }

  constexpr bool isDifferentiable() const { return builder.isDifferentiable(); }

  Optional<ClangTypeInfo> getClangTypeInfo() const {
    return builder.getClangTypeInfo();
  }

  constexpr bool hasSelfParam() const { return builder.hasSelfParam(); }

  constexpr bool hasContext() const { return builder.hasContext(); }

  /// Helper method for changing the Representation.
  ///
  /// Prefer using \c SILExtInfoBuilder::withRepresentation for chaining.
  SILExtInfo withRepresentation(SILExtInfoBuilder::Representation rep) const {
    return builder.withRepresentation(rep).build();
  }

  /// Helper method for changing only the NoEscape field.
  ///
  /// Prefer using \c SILExtInfoBuilder::withNoEscape for chaining.
  SILExtInfo withNoEscape(bool noEscape = true) const {
    return builder.withNoEscape(noEscape).build();
  }

  bool operator==(SILExtInfo other) const {
    return builder.bits == other.builder.bits;
  }
  bool operator!=(SILExtInfo other) const {
    return builder.bits != other.builder.bits;
  }


  constexpr std::pair<unsigned, const void *> getFuncAttrKey() const {
    return builder.getFuncAttrKey();
  }
};

} // end namespace swift

#endif // SWIFT_EXTINFO_H
