//===--- Reflection.h - Classes for representing reflection -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Defines facilities for representing reflected entities.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_REFLECTION_H
#define LLVM_CLANG_AST_REFLECTION_H

#include "clang/AST/APValue.h"
#include "clang/AST/Type.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/ASTContext.h"

namespace clang {

class CXXBaseSpecifier;
class CXXReflectionTraitExpr;
class Decl;
class Expr;
class NamespaceDecl;
class NestedNameSpecifier;
class Type;
class UnresolvedLookupExpr;

using ReflectedNamespace =
  llvm::PointerUnion<NamespaceDecl *, TranslationUnitDecl *>;

/// Represents a qualified namespace-name.
class QualifiedNamespaceName {
  /// The namespace designated by the operand.
  ReflectedNamespace NS;

  // The qualifying nested name specifier for the namespace.
  NestedNameSpecifier *NNS;
public:
  QualifiedNamespaceName(
      ReflectedNamespace NS, NestedNameSpecifier *NNS)
    : NS(NS), NNS(NNS) {
    // Namespace must always be set.
    assert(!NS.isNull());
  }

  /// Returns the designated namespace.
  ReflectedNamespace getNamespace() const { return NS; }

  /// Returns the nested-name-specifier, if any.
  NestedNameSpecifier *getQualifier() const { return NNS; }
};

/// Represents a namespace-name within a reflection operand.
class NamespaceName {
  // This is either an a reflected namespace or a qualified namespace name.
  using StorageType =
    llvm::PointerUnion3<
        NamespaceDecl *, TranslationUnitDecl *, QualifiedNamespaceName *>;

  StorageType Storage;

  explicit NamespaceName(void *Ptr)
    : Storage(StorageType::getFromOpaqueValue(Ptr)) {
    // Storage must always be set.
    assert(!Storage.isNull());
  }
public:
  enum NameKind {
    /// An unqualified namespace-name.
    Namespace,

    /// A qualified namespace-name.
    QualifiedNamespace
  };

  explicit NamespaceName(ReflectedNamespace NS)
    : Storage(StorageType::getFromOpaqueValue(NS.getOpaqueValue())) {
    // Ensure the translation from ReflectedNamespace to StorageType
    // is working correctly since these are two different pointer unions.
    //
    // The transition should be one of the following:
    // - a Namespace to a Namespace
    // - a TranslationUnit to a TranslationUnit.
    assert(Storage.is<TranslationUnitDecl *>() || Storage.is<NamespaceDecl *>());
    assert(NS.is<NamespaceDecl *>() == Storage.is<NamespaceDecl *>());
    assert(
         NS.is<TranslationUnitDecl *>() == Storage.is<TranslationUnitDecl *>());
  }

  explicit NamespaceName(QualifiedNamespaceName *Q) : Storage(Q) {
    // Storage must always be set.
    assert(!Storage.isNull());
  }

  /// Returns the kind of name stored.
  NameKind getKind() const {
    if (Storage.is<QualifiedNamespaceName *>())
      return QualifiedNamespace;

    return Namespace;
  }

  /// Returns true if this is qualified.
  bool isQualified() const { return getKind() == QualifiedNamespace; }

  /// Returns the designated namespace, if any.
  NestedNameSpecifier *getQualifier() const {
    if (isQualified())
      return Storage.get<QualifiedNamespaceName *>()->getQualifier();

    return nullptr;
  }

  /// Returns the designated namespace.
  ReflectedNamespace getNamespace() const {
    if (NamespaceDecl *NS = Storage.dyn_cast<NamespaceDecl *>())
      return { NS };

    if (TranslationUnitDecl *TU = Storage.dyn_cast<TranslationUnitDecl *>())
      return { TU };

    return Storage.get<QualifiedNamespaceName *>()->getNamespace();
  }

  /// Returns the designated namespace as a Decl.
  Decl *getNamespaceAsDecl() const {
    ReflectedNamespace ReflNs = getNamespace();

    if (NamespaceDecl *NS = ReflNs.dyn_cast<NamespaceDecl *>())
      return NS;

    return ReflNs.get<TranslationUnitDecl *>();
  }

  /// Returns this as an opaque pointer.
  void *getAsVoidPointer() const {
    return Storage.getOpaqueValue();
  }

  /// Returns this as an opaque pointer.
  static NamespaceName getFromVoidPointer(void *P) {
    return NamespaceName(P);
  }
};

/// Represents an invalid reflection.
struct InvalidReflection {
  // The evaluated error message expr.
  const Expr *ErrorMessage;
};

/// Represents an operand to the reflection operator.
class ReflectionOperand {
public:
  enum ReflectionOpKind {
    Type,        // Begin parseable kinds
    Template,
    Namespace,
    Expression,  // End parseable kinds

    Invalid,
    Declaration,
    BaseSpecifier
  };

private:
  ReflectionOpKind Kind;

  /// Points to the representation of the operand. For type operands, this is
  /// the opaque pointer of a QualType. For template-name operands, this is
  /// the opaque pointer for a TemplateName. For namespace operands, this is
  /// a pointer to NamespaceName. For expressions, this is the expression
  /// pointer.
  void *Data;

public:
  ReflectionOperand()
    : Kind(Invalid)
  { }

  ReflectionOperand(InvalidReflection *IR)
    : Kind(Invalid), Data(IR)
  { }

  ReflectionOperand(QualType T)
    : Kind(Type), Data(T.getAsOpaquePtr()) { }

  ReflectionOperand(TemplateName T)
    : Kind(Template), Data(T.getAsVoidPointer()) { }

  ReflectionOperand(NamespaceName T)
    : Kind(Namespace), Data(T.getAsVoidPointer()) { }

  ReflectionOperand(Expr *E)
    : Kind(Expression), Data(E) { }

  ReflectionOperand(Decl *D)
    : Kind(Declaration), Data(D) { }

  ReflectionOperand(CXXBaseSpecifier *B)
    : Kind(BaseSpecifier), Data(B) { }

  /// Returns the kind of reflection.
  ReflectionOpKind getKind() const { return Kind; }

  /// Returns true if the reflection is invalid.
  bool isInvalid() const { return Kind == Invalid; }

  /// Returns the opaque reflection pointer.
  const void *getOpaqueReflectionValue() const {
    return Data;
  }

  /// Returns the invalid reflection information.
  ///
  /// This can and will be null in most cases.
  InvalidReflection *getAsInvalidReflection() const {
    assert(getKind() == Invalid && "not invalid");
    return reinterpret_cast<InvalidReflection *>(Data);
  }

  /// Returns this as a type operand.
  QualType getAsType() const {
    assert(getKind() == Type && "not a type");
    return QualType::getFromOpaquePtr(Data);
  }

  TemplateName getAsTemplate() const {
    assert(getKind() == Template && "not a template");
    return TemplateName::getFromVoidPointer(Data);
  }

  NamespaceName getAsNamespace() const {
    assert(getKind() == Namespace && "not a namespace");
    return NamespaceName::getFromVoidPointer(Data);
  }

  Expr *getAsExpression() const {
    assert(getKind() == Expression && "not an expression");
    return reinterpret_cast<Expr *>(Data);
  }

  Decl *getAsDeclaration() const {
    assert(getKind() == Declaration && "not a declaration");
    return reinterpret_cast<Decl *>(Data);
  }

  CXXBaseSpecifier *getAsBaseSpecifier() const {
    assert(getKind() == BaseSpecifier && "not a base specifier");
    return reinterpret_cast<CXXBaseSpecifier *>(Data);
  }
};

enum ReflectionQuery {
  RQ_unknown,

  RQ_is_invalid,
  RQ_is_entity,
  RQ_is_unnamed,

  // Declarations
  RQ_is_variable,
  RQ_is_function,
  RQ_is_class,
  RQ_is_union,
  RQ_is_unscoped_enum,
  RQ_is_scoped_enum,
  RQ_is_enumerator,
  RQ_is_bitfield,
  RQ_is_static_data_member,
  RQ_is_nonstatic_data_member,
  RQ_is_static_member_function,
  RQ_is_nonstatic_member_function,
  RQ_is_copy_assignment_operator,
  RQ_is_move_assignment_operator,
  RQ_is_constructor,
  RQ_is_default_constructor,
  RQ_is_copy_constructor,
  RQ_is_move_constructor,
  RQ_is_destructor,

  // Types
  RQ_is_type,
  RQ_is_function_type,
  RQ_is_class_type,
  RQ_is_union_type,
  RQ_is_enum_type,
  RQ_is_scoped_enum_type,
  RQ_is_void_type,
  RQ_is_null_pointer_type,
  RQ_is_integral_type,
  RQ_is_floating_point_type,
  RQ_is_array_type,
  RQ_is_pointer_type,
  RQ_is_lvalue_reference_type,
  RQ_is_rvalue_reference_type,
  RQ_is_member_object_pointer_type,
  RQ_is_member_function_pointer_type,
  RQ_is_closure_type,

  // Namespaces and aliases
  RQ_is_namespace,
  RQ_is_namespace_alias,
  RQ_is_type_alias,

  // Templates and specializations
  RQ_is_template,
  RQ_is_class_template,
  RQ_is_alias_template,
  RQ_is_function_template,
  RQ_is_variable_template,
  RQ_is_static_member_function_template,
  RQ_is_nonstatic_member_function_template,
  RQ_is_constructor_template,
  RQ_is_destructor_template,
  RQ_is_concept,
  RQ_is_specialization,
  RQ_is_partial_specialization,
  RQ_is_explicit_specialization,
  RQ_is_implicit_instantiation,
  RQ_is_explicit_instantiation,

  // Base class specifiers
  RQ_is_direct_base,
  RQ_is_virtual_base,

  // Parameters
  RQ_is_function_parameter,
  RQ_is_template_parameter,
  RQ_is_type_template_parameter,
  RQ_is_nontype_template_parameter,
  RQ_is_template_template_parameter,

  // Expressions
  RQ_is_expression,
  RQ_is_lvalue,
  RQ_is_xvalue,
  RQ_is_rvalue,
  RQ_is_value,

  // Scope
  RQ_is_local,
  RQ_is_class_member,

  // Access queries
  RQ_has_default_access,

  // Traits
  RQ_get_decl_traits,
  RQ_get_linkage_traits,
  RQ_get_access_traits,
  RQ_get_type_traits,

  // Associated reflections
  RQ_get_entity,
  RQ_get_parent,
  RQ_get_type,
  RQ_get_return_type,
  RQ_get_this_ref_type,
  RQ_get_definition,

  // Traversal
  RQ_get_begin,
  RQ_get_next,

  // Name
  RQ_get_name,
  RQ_get_display_name,

  RQ_get_attribute,
  RQ_has_attribute,

  // Labels for kinds of queries. These need to be updated when new
  // queries are added.

  // Predicates -- these return bool.
  RQ_first_predicate = RQ_is_invalid,
  RQ_last_predicate = RQ_has_default_access,
  // Traits -- these return unsigned.
  RQ_first_trait = RQ_get_decl_traits,
  RQ_last_trait = RQ_get_type_traits,
  // Associated reflections -- these return meta::info.
  RQ_first_assoc = RQ_get_entity,
  RQ_last_assoc = RQ_get_next,
  // Names -- these return const char*
  RQ_first_name = RQ_get_name,
  RQ_last_name = RQ_get_display_name,
};

/// True if Q is a predicate.
inline bool isPredicateQuery(ReflectionQuery Q) {
  return RQ_first_predicate <= Q && Q <= RQ_last_predicate;
}

/// True if Q returns trait information.
inline bool isTraitQuery(ReflectionQuery Q) {
  return RQ_first_trait <= Q && Q <= RQ_last_trait;
}

/// True if Q returns an associated reflection.
inline bool isAssociatedReflectionQuery(ReflectionQuery Q) {
  return RQ_first_assoc <= Q && Q <= RQ_last_assoc;
}

/// True if Q returns a name.
inline bool isNameQuery(ReflectionQuery Q) {
  return RQ_first_name <= Q && Q <= RQ_last_name;
}


/// The reflection class provides context for evaluating queries.
///
/// FIXME: This might not need diagnostics; we could simply return invalid
/// reflections, which would make the class much, much easier to implement.
class Reflection {
  /// The AST context is needed for global information.
  ASTContext *Ctx;

  /// The reflected entity or construct.
  APValue Ref;

  /// The expression defining the query.
  const CXXReflectionTraitExpr *Query;

  /// Points to a vector of diagnostics, to be populated during query
  /// evaluation.
  SmallVectorImpl<PartialDiagnosticAt> *Diag;

public:
  Reflection()
    : Ctx(nullptr), Ref(APValue(RK_invalid, nullptr)), Query(), Diag() {
  }

  /// Construct a reflection that will be used only to observe the
  /// reflected value.
  Reflection(ASTContext &C, const APValue &R)
    : Ctx(&C), Ref(R), Query(), Diag() {
    assert(Ref.isReflection() && "not a reflection");
  }

  /// Construct a reflection that will be used to evaluate a query.
  Reflection(ASTContext &C, const APValue &R, const CXXReflectionTraitExpr *E,
             SmallVectorImpl<PartialDiagnosticAt> *D = nullptr)
    : Ctx(&C), Ref(R), Query(E), Diag(D) {
    assert(Ref.isReflection() && "not a reflection");
  }

  /// Returns the ASTContext for this reflection.
  ASTContext &getContext() const {
    return *Ctx;
  }

  /// Returns the related query for this reflection, if present.
  const CXXReflectionTraitExpr *getQuery() const {
    return Query;
  }

  /// Returns the vector holding diagnostics for query evaluation.
  SmallVectorImpl<PartialDiagnosticAt> *getDiag() const {
    return Diag;
  }

  /// Returns the reflection kind.
  ReflectionKind getKind() const {
    return Ref.getReflectionKind();
  }

  /// True if this is the invalid reflection.
  bool isInvalid() const {
    return Ref.isInvalidReflection();
  }

  /// True if this reflects a type.
  bool isType() const {
    return getKind() == RK_type;
  }

  /// True if this reflects a declaration.
  bool isDeclaration() const {
    return getKind() == RK_declaration;
  }

  /// True if this reflects an expression.
  bool isExpression() const {
    return getKind() == RK_expression;
  }

  /// True if this reflects a base class specifier.
  bool isBase() const {
    return getKind() == RK_base_specifier;
  }

  /// Returns this as an invalid reflection.
  const InvalidReflection *getAsInvalidReflection() const {
    return Ref.getInvalidReflectionInfo();
  }

  /// Returns this as a type.
  QualType getAsType() const {
    return Ref.getReflectedType();
  }

  /// Returns this as a declaration.
  const Decl *getAsDeclaration() const {
    return Ref.getReflectedDeclaration();
  }

  /// Returns this as an expression.
  const Expr *getAsExpression() const {
    return Ref.getReflectedExpression();
  }

  /// Returns this as a base class specifier.
  const CXXBaseSpecifier *getAsBase() const {
    return Ref.getReflectedBaseSpecifier();
  }

  /// Evaluates the predicate designated by Q.
  bool EvaluatePredicate(ReflectionQuery Q, APValue &Result);

  /// Returns the traits designated by Q.
  bool GetTraits(ReflectionQuery Q, APValue &Result);

  /// Returns the reflected construct designated by Q.
  bool GetAssociatedReflection(ReflectionQuery Q, APValue &Result);

  /// Returns the entity name designated by Q.
  bool GetName(ReflectionQuery, APValue &Result);

  //Return the user defined attribute
  bool GetUserDefinedAttribute(ReflectionQuery, APValue AttributeType, APValue &Result);

  //Return whether a particular entity has a user defined attribute
  bool HasUserDefinedAttribute(ReflectionQuery, APValue AttributeType, APValue &Result);

  /// True if A and B reflect the same entity.
  static bool Equal(ASTContext &Ctx, APValue const& X, APValue const& Y);
};
} // namespace clang

#endif
