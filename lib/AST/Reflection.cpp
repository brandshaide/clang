//===--- Reflection.cpp - Classes for representing reflection ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements the Reflection class.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/Reflection.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/LocInfoType.h"
using namespace clang;

/// Returns an APValue-packaged truth value.
static APValue makeBool(ASTContext &C, bool B) {
  return APValue(C.MakeIntValue(B, C.BoolTy));
}

/// Sets result to the truth value of B and returns true.
static bool SuccessBool(const Reflection &R, APValue &Result, bool B) {
  Result = makeBool(R.getContext(), B);
  return true;
}

static bool SuccessTrue(const Reflection &R, APValue &Result) {
  return SuccessBool(R, Result, true);
}

static bool SuccessFalse(const Reflection &R, APValue &Result) {
  return SuccessBool(R, Result, false);
}

template<typename F>
static bool CustomError(const Reflection &R, F BuildDiagnostic) {
  if (SmallVectorImpl<PartialDiagnosticAt> *Diag = R.getDiag()) {
    // FIXME: We could probably do a better job with the location.
    SourceLocation Loc = R.getQuery()->getExprLoc();
    Diag->push_back(std::make_pair(Loc, BuildDiagnostic()));
  }
  return false;
}

/// Returns the type reflected by R. R must be a type reflection.
///
/// Note that this does not get the canonical type.
static QualType getQualType(QualType QT) {
  // See through "location types".
  if (const LocInfoType *LIT = dyn_cast<LocInfoType>(QT))
    return LIT->getType();

  return QT;
}

static QualType getQualType(const Reflection &R) {
  return getQualType(R.getAsType());
}

static QualType getQualType(const APValue &R) {
  return getQualType(R.getReflectedType());
}

// Returns false, possibly saving the diagnostic.
static bool Error(const Reflection &R) {
  return CustomError(R, [&]() {
    PartialDiagnostic PD(diag::note_reflection_not_defined,
                         R.getContext().getDiagAllocator());

    switch (R.getKind()) {
    case RK_type:
      PD << 1;
      PD << getQualType(R);
      break;

    default:
      PD << 0;
      break;
    }

    return PD;
  });
}

static bool ErrorUnimplemented(const Reflection &R) {
  return CustomError(R, [&]() {
    return PartialDiagnostic(diag::note_reflection_query_unimplemented,
                             R.getContext().getDiagAllocator());
  });
}

/// Returns the TypeDecl for a reflected Type, if any.
static const TypeDecl *getAsTypeDecl(const Reflection &R) {
  if (R.isType()) {
    QualType T = getQualType(R);

    if (const TagDecl *TD = T->getAsTagDecl())
      return TD;

    // FIXME: Handle alias types.
  }
  return nullptr;
}

/// Returns the entity designate by the expression E.
///
/// FIXME: If E is a call expression, return the resolved function.
static const ValueDecl *getEntityDecl(const Expr *E) {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();
  return nullptr;
}

/// If R designates some kind of declaration, either directly, as a type,
/// or via a reflection, return that declaration.
static const Decl *getReachableDecl(const Reflection &R) {
  if (const TypeDecl *TD = getAsTypeDecl(R))
    return TD;
  if (R.isDeclaration())
    return R.getAsDeclaration();
  if (R.isExpression())
    return getEntityDecl(R.getAsExpression());
  return nullptr;
}

namespace {

/// A helper class to manage conditions involving types.
struct MaybeType {
  MaybeType(QualType T) : Ty(T) { }

  explicit operator bool() const { return !Ty.isNull(); }

  operator QualType() const {
    assert(!Ty.isNull());
    return Ty;
  }

  const Type* operator->() const { return Ty.getTypePtr(); }

  QualType operator*() const { return Ty; }

  QualType Ty;
};

} // end anonymous namespace


/// Returns the canonical type reflected by R, if R is a type reflection.
///
/// This is used for queries concerned with type entities
/// rather than e.g., aliases.
static QualType getCanonicalType(const Reflection &R) {
  if (R.isType()) {
    return R.getContext().getCanonicalType(getQualType(R));
  }

  return QualType();
}

static const Expr *getExpr(const Reflection &R) {
  if (R.isExpression())
    return R.getAsExpression();
  return nullptr;
}

/// Returns true if R is an invalid reflection.
static bool isInvalid(const Reflection &R, APValue &Result) {
  return SuccessBool(R, Result, R.isInvalid());
}

/// Sets Result to true if R reflects an entity.
static bool isEntity(const Reflection &R, APValue &Result) {
  if (R.isType())
    // Types are entities.
    return SuccessTrue(R, Result);

  if (R.isDeclaration()) {
    const Decl *D = R.getAsDeclaration();

    if (isa<ValueDecl>(D))
      // Values, objects, references, functions, enumerators, class members,
      // and bit-fields are entities.
      return SuccessTrue(R, Result);

    if (isa<TemplateDecl>(D))
      // Templates are entities (but not template template parameters).
      return SuccessBool(R, Result, !isa<TemplateTemplateParmDecl>(D));

    if (isa<NamespaceDecl>(D))
      // Namespaces are entities.
      return SuccessTrue(R, Result);

    // FIXME: How is a pack an entity?
  }
  return SuccessFalse(R, Result);
}

/// Returns true if R is unnamed.
static bool isUnnamed(const Reflection &R, APValue &Result) {
  if (const Decl *D = R.getAsDeclaration()) {
    if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
      return SuccessBool(R, Result, ND->getIdentifier() == nullptr);
  }
  return Error(R);
}

/// Returns true if R designates a variable.
static bool isVariable(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<VarDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a function.
static bool isFunction(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<FunctionDecl>(D));
  return SuccessFalse(R, Result);
}

static const CXXRecordDecl *getReachableRecordDecl(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    return dyn_cast<CXXRecordDecl>(D);
  return nullptr;
}

/// Returns true if R designates a class.
static bool isClass(const Reflection &R, APValue &Result) {
  if (const CXXRecordDecl *D = getReachableRecordDecl(R))
    return SuccessBool(R, Result, D->isClass() || D->isStruct());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a union.
static bool isUnion(const Reflection &R, APValue &Result) {
  if (const CXXRecordDecl *D = getReachableRecordDecl(R))
    return SuccessBool(R, Result, D->isUnion());
  return SuccessFalse(R, Result);
}

static const EnumDecl *getReachableEnumDecl(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    return dyn_cast<EnumDecl>(D);
  return nullptr;
}

/// Returns true if R designates an unscoped enum.
static bool isUnscopedEnum(const Reflection &R, APValue &Result) {
  if (const EnumDecl *D = getReachableEnumDecl(R))
    return SuccessBool(R, Result, !D->isScoped());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a scoped enum.
static bool isScopedEnum(const Reflection &R, APValue &Result) {
  if (const EnumDecl *D = getReachableEnumDecl(R))
    return SuccessBool(R, Result, D->isScoped());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an enumerator.
static bool isEnumerator(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<EnumConstantDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns the reflected data member.
static const FieldDecl *getAsDataMember(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    return dyn_cast<FieldDecl>(D);
  return nullptr;
}

/// Returns true if R designates a nonstatic data member.
static bool isBitField(const Reflection &R, APValue &Result) {
  if (const FieldDecl *D = getAsDataMember(R))
    return SuccessBool(R, Result, D->isBitField());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a static member variable.
static bool isStaticDataMember(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    if (const VarDecl *Var = dyn_cast<VarDecl>(D))
      return SuccessBool(R, Result, Var->isStaticDataMember());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a nonstatic data member.
static bool isNonstaticDataMember(const Reflection &R, APValue &Result) {
  if (const FieldDecl *D = getAsDataMember(R))
    // FIXME: Is a bitfield a non-static data member?
    return SuccessTrue(R, Result);
  return SuccessFalse(R, Result);
}

/// Returns the reflected member function.
static const CXXMethodDecl *getAsMemberFunction(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    return dyn_cast<CXXMethodDecl>(D);
  return nullptr;
}

/// Returns true if R designates a static member function
static bool isStaticMemberFunction(const Reflection &R, APValue &Result) {
  if (const CXXMethodDecl *M = getAsMemberFunction(R))
    return SuccessBool(R, Result, M->isStatic());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a nonstatic member function
static bool isNonstaticMemberFunction(const Reflection &R, APValue &Result) {
  if (const CXXMethodDecl *M = getAsMemberFunction(R))
    return SuccessBool(R, Result, M->isInstance());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a copy assignment operator
static bool isCopyAssignmentOperator(const Reflection &R, APValue &Result) {
  if (const CXXMethodDecl *M = getAsMemberFunction(R))
    return SuccessBool(R, Result, M->isCopyAssignmentOperator());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a move assignment operator
static bool isMoveAssignmentOperator(const Reflection &R, APValue &Result) {
  if (const CXXMethodDecl *M = getAsMemberFunction(R))
    return SuccessBool(R, Result, M->isMoveAssignmentOperator());
  return SuccessFalse(R, Result);
}

static const CXXConstructorDecl *getReachableConstructor(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    return dyn_cast<CXXConstructorDecl>(D);
  return nullptr;
}

/// Returns true if R designates a constructor.
static bool isConstructor(const Reflection &R,APValue &Result) {
  if (getReachableConstructor(R))
    return SuccessTrue(R, Result);
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a default constructor.
static bool isDefaultConstructor(const Reflection &R,APValue &Result) {
  if (const CXXConstructorDecl *CD = getReachableConstructor(R))
    return SuccessBool(R, Result, CD->isDefaultConstructor());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a copy constructor.
static bool isCopyConstructor(const Reflection &R,APValue &Result) {
  if (const CXXConstructorDecl *CD = getReachableConstructor(R))
    return SuccessBool(R, Result, CD->isCopyConstructor());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a copy constructor.
static bool isMoveConstructor(const Reflection &R,APValue &Result) {
  if (const CXXConstructorDecl *CD = getReachableConstructor(R))
    return SuccessBool(R, Result, CD->isMoveConstructor());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an enumerator.
static bool isDestructor(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<CXXDestructorDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a type.
static bool isType(const Reflection &R, APValue &Result) {
  return SuccessBool(R, Result, R.isType());
}

/// Returns true if R designates a function type.
static bool isFunctionType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R)) {
    return SuccessBool(R, Result, T->isFunctionType());
  }
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a class type.
static bool isClassType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R)) {
    return SuccessBool(R, Result, T->isRecordType());
  }
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a union type.
static bool isUnionType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isUnionType());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an enum type.
static bool isEnumType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isEnumeralType());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a scoped enum type.
static bool isScopedEnumType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isScopedEnumeralType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has void type.
static bool isVoidType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isVoidType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has nullptr type.
static bool isNullPtrType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isNullPtrType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has integral type.
static bool isIntegralType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isIntegralOrEnumerationType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has floating point type.
static bool isFloatingPointType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isFloatingType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has array type.
static bool isArrayType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isArrayType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has pointer type.
static bool isPointerType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isPointerType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has lvalue reference type.
static bool isLValueReferenceType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isLValueReferenceType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has rvalue reference type.
static bool isRValueReferenceType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isRValueReferenceType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has member object pointer type.
static bool isMemberObjectPointerType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isMemberDataPointerType());
  return SuccessFalse(R, Result);
}

/// Returns true if R has member function pointer type.
static bool isMemberFunctionPointerType(const Reflection &R, APValue &Result) {
  if (MaybeType T = getCanonicalType(R))
    return SuccessBool(R, Result, T->isMemberFunctionPointerType());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a closure type.
static bool isClosureType(const Reflection &R, APValue &Result) {
  return ErrorUnimplemented(R);
}

/// Returns true if R designates a namespace.
static bool isNamespace(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R)) {
    bool IsNamespace = isa<NamespaceDecl>(D) || isa<TranslationUnitDecl>(D);
    return SuccessBool(R, Result, IsNamespace);
  }
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a namespace alias.
static bool isNamespaceAlias(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<NamespaceAliasDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an type alias.
static bool isTypeAlias(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<TypedefNameDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a template.
static bool isTemplate(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, D->isTemplateDecl());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a class template.
static bool isClassTemplate(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<ClassTemplateDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an alias template.
static bool isAliasTemplate(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<TypeAliasTemplateDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a function template.
static bool isFunctionTemplate(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<FunctionTemplateDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a variable template.
static bool isVariableTemplate(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isa<VarTemplateDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns the reflected template member function.
static const CXXMethodDecl *getAsTemplateMemberFunction(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    if (const FunctionTemplateDecl *FTD = dyn_cast<FunctionTemplateDecl>(D))
      return dyn_cast<CXXMethodDecl>(D->getAsFunction());
  return nullptr;
}

/// Returns true if R designates a static member function template.
static bool isStaticMemberFunctionTemplate(const Reflection &R,
                                           APValue &Result) {
  if (const CXXMethodDecl *D = getAsTemplateMemberFunction(R))
    return SuccessBool(R, Result, D->isStatic());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a nonstatic member function template.
static bool isNonstaticMemberFunctionTemplate(const Reflection &R,
                                              APValue &Result) {
  if (const CXXMethodDecl *D = getAsTemplateMemberFunction(R))
    return SuccessBool(R, Result, D->isInstance());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a constructor template.
static bool isConstructorTemplate(const Reflection &R, APValue &Result) {
  if (const CXXMethodDecl *D = getAsTemplateMemberFunction(R))
    return SuccessBool(R, Result, isa<CXXConstructorDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a destructor template.
static bool isDestructorTemplate(const Reflection &R, APValue &Result) {
  if (const CXXMethodDecl *D = getAsTemplateMemberFunction(R))
    return SuccessBool(R, Result, isa<CXXDestructorDecl>(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a concept.
static bool isConcept(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    if (const TemplateDecl *TD = dyn_cast<TemplateDecl>(D))
      return SuccessBool(R, Result, TD->isConcept());
  return SuccessFalse(R, Result);
}

static bool isPartialTemplateSpecialization(const Decl *D) {
  if (isa<ClassTemplatePartialSpecializationDecl>(D))
    return true;

  if (isa<VarTemplatePartialSpecializationDecl>(D))
    return true;

  return false;
}

static bool isTemplateSpecialization(const Decl *D) {
  if (isa<ClassTemplateSpecializationDecl>(D))
    return true;

  if (isa<ClassScopeFunctionSpecializationDecl>(D))
    return true;

  if (isa<VarTemplateSpecializationDecl>(D))
    return true;

  return isPartialTemplateSpecialization(D);
}

/// Returns true if R designates a specialized template.
static bool isSpecialization(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isTemplateSpecialization(D));
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a partially specialized template.
static bool isPartialSpecialization(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, isPartialTemplateSpecialization(D));
  return SuccessFalse(R, Result);
}

// TODO: This currently uses TSK_Undeclared as a catch all
// for any issues, should this be a different state?
static TemplateSpecializationKind
getTemplateSpecializationKind(const Reflection &R) {
  const Decl *D = getReachableDecl(R);

  if (!D)
    return TSK_Undeclared;

  if (const CXXRecordDecl *RD = dyn_cast<CXXRecordDecl>(D))
    return RD->getTemplateSpecializationKind();

  if (const VarDecl *VD = dyn_cast<VarDecl>(D))
    return VD->getTemplateSpecializationKind();

  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
    return FD->getTemplateSpecializationKind();

  if (const EnumDecl *ED = dyn_cast<EnumDecl>(D))
    return ED->getTemplateSpecializationKind();

  return TSK_Undeclared;
}

/// Returns true if R designates a explicitly specialized template.
static bool isExplicitSpecialization(const Reflection &R, APValue &Result) {
  if (TemplateSpecializationKind TSK = getTemplateSpecializationKind(R))
    return SuccessBool(R, Result, TSK == TSK_ExplicitSpecialization);
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an implicitly instantiated template.
static bool isImplicitInstantiation(const Reflection &R, APValue &Result) {
  if (TemplateSpecializationKind TSK = getTemplateSpecializationKind(R))
    return SuccessBool(R, Result, TSK == TSK_ImplicitInstantiation);
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an explicitly instantiated template.
static bool isExplicitInstantiation(const Reflection &R, APValue &Result) {
  if (TemplateSpecializationKind TSK = getTemplateSpecializationKind(R))
    return SuccessBool(R, Result, TSK == TSK_ExplicitInstantiationDeclaration
                               || TSK == TSK_ExplicitInstantiationDefinition);
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a direct base.
static bool isDirectBase(const Reflection &R, APValue &Result) {
  return ErrorUnimplemented(R);
}

/// Returns true if R designates a virtual base.
static bool isVirtualBase(const Reflection &R, APValue &Result) {
  return ErrorUnimplemented(R);
}

/// Returns true if R designates a function parameter.
static bool isFunctionParameter(const Reflection &R, APValue &Result) {
  return ErrorUnimplemented(R);
}

/// Returns true if R designates a template parameter.
static bool isTemplateParameter(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, D->isTemplateParameter());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a type template parameter.
static bool isTypeTemplateParameter(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, D->getKind() == Decl::TemplateTypeParm);
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a nontype template parameter.
static bool isNontypeTemplateParameter(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, D->getKind() == Decl::NonTypeTemplateParm);
  return SuccessFalse(R, Result);
}

/// Return true if R designates a template template parameter.
static bool isTemplateTemplateParameter(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessBool(R, Result, D->getKind() == Decl::TemplateTemplateParm);
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an expression.
static bool isExpression(const Reflection &R, APValue &Result) {
  return SuccessBool(R, Result, R.isExpression());
}

/// Returns true if R designates an LValue expression.
static bool isLValue(const Reflection &R, APValue &Result) {
  if (const Expr *E = getExpr(R))
    return SuccessBool(R, Result, E->isLValue());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an XValue expression.
static bool isXValue(const Reflection &R, APValue &Result) {
  if (const Expr *E = getExpr(R))
    return SuccessBool(R, Result, E->isXValue());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates an RValue expression.
static bool isRValue(const Reflection &R, APValue &Result) {
  if (const Expr *E = getExpr(R))
    return SuccessBool(R, Result, E->isRValue());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a value.
static bool isValue(const Reflection &R, APValue &Result) {
  if (const Expr *E = getExpr(R)) {
    if (isa<IntegerLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<FixedPointLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<FloatingLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<CharacterLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<ImaginaryLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<StringLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<CompoundLiteralExpr>(E))
      return SuccessTrue(R, Result);
    if (isa<UserDefinedLiteral>(E))
      return SuccessTrue(R, Result);
    if (isa<CXXBoolLiteralExpr>(E))
      return SuccessTrue(R, Result);
    if (isa<CXXNullPtrLiteralExpr>(E))
      return SuccessTrue(R, Result);
  }
  return SuccessFalse(R, Result);
}

static const DeclContext *getReachableRedeclContext(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    if (const DeclContext *DC = D->getLexicalDeclContext())
      return DC->getRedeclContext();
  return nullptr;
}

/// Returns true if R designates a local entity.
static bool isLocal(const Reflection &R, APValue &Result) {
  if (const DeclContext *DC = getReachableRedeclContext(R))
    return SuccessBool(R, Result, DC->isFunctionOrMethod());
  return SuccessFalse(R, Result);
}

/// Returns true if R designates a class member.
static bool isClassMember(const Reflection &R, APValue &Result) {
  if (const DeclContext *DC = getReachableRedeclContext(R))
    return SuccessBool(R, Result, DC->isRecord());
  return SuccessFalse(R, Result);
}

/// Returns true if R has default access.
static bool hasDefaultAccess(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R)) {
    if (const RecordDecl *RD = dyn_cast<RecordDecl>(D->getDeclContext())) {
      for (const Decl *CurDecl : dyn_cast<DeclContext>(RD)->decls()) {
        if (isa<AccessSpecDecl>(CurDecl))
          return SuccessFalse(R, Result);
        if (CurDecl == D)
          return SuccessTrue(R, Result);
      }
    }
  }
  return SuccessFalse(R, Result);
}

bool Reflection::EvaluatePredicate(ReflectionQuery Q, APValue &Result) {
  assert(isPredicateQuery(Q) && "invalid query");
  switch (Q) {
  case RQ_is_invalid:
    return ::isInvalid(*this, Result);
  case RQ_is_entity:
    return isEntity(*this, Result);
  case RQ_is_unnamed:
    return isUnnamed(*this, Result);

  case RQ_is_variable:
    return isVariable(*this, Result);
  case RQ_is_function:
    return isFunction(*this, Result);
  case RQ_is_class:
    return isClass(*this, Result);
  case RQ_is_union:
    return isUnion(*this, Result);
  case RQ_is_unscoped_enum:
    return isUnscopedEnum(*this, Result);
  case RQ_is_scoped_enum:
    return isScopedEnum(*this, Result);
  case RQ_is_enumerator:
    return isEnumerator(*this, Result);
  case RQ_is_bitfield:
    return isBitField(*this, Result);
  case RQ_is_static_data_member:
    return isStaticDataMember(*this, Result);
  case RQ_is_nonstatic_data_member:
    return isNonstaticDataMember(*this, Result);
  case RQ_is_static_member_function:
    return isStaticMemberFunction(*this, Result);
  case RQ_is_nonstatic_member_function:
    return isNonstaticMemberFunction(*this, Result);
  case RQ_is_copy_assignment_operator:
    return isCopyAssignmentOperator(*this, Result);
  case RQ_is_move_assignment_operator:
    return isMoveAssignmentOperator(*this, Result);
  case RQ_is_constructor:
    return isConstructor(*this, Result);
  case RQ_is_default_constructor:
    return isDefaultConstructor(*this, Result);
  case RQ_is_copy_constructor:
    return isCopyConstructor(*this, Result);
  case RQ_is_move_constructor:
    return isMoveConstructor(*this, Result);
  case RQ_is_destructor:
    return isDestructor(*this, Result);

  case RQ_is_type:
    return ::isType(*this, Result);
  case RQ_is_function_type:
    return isFunctionType(*this, Result);
  case RQ_is_class_type:
      return isClassType(*this, Result);
  case RQ_is_union_type:
    return isUnionType(*this, Result);
  case RQ_is_enum_type:
    return isEnumType(*this, Result);
  case RQ_is_scoped_enum_type:
    return isScopedEnumType(*this, Result);
  case RQ_is_void_type:
    return isVoidType(*this, Result);
  case RQ_is_null_pointer_type:
      return isNullPtrType(*this, Result);
  case RQ_is_integral_type:
    return isIntegralType(*this, Result);
  case RQ_is_floating_point_type:
    return isFloatingPointType(*this, Result);
  case RQ_is_array_type:
    return isArrayType(*this, Result);
  case RQ_is_pointer_type:
    return isPointerType(*this, Result);
  case RQ_is_lvalue_reference_type:
    return isLValueReferenceType(*this, Result);
  case RQ_is_rvalue_reference_type:
    return isRValueReferenceType(*this, Result);
  case RQ_is_member_object_pointer_type:
    return isMemberObjectPointerType(*this, Result);
  case RQ_is_member_function_pointer_type:
    return isMemberFunctionPointerType(*this, Result);
  case RQ_is_closure_type:
    return isClosureType(*this, Result);

  case RQ_is_namespace:
    return isNamespace(*this, Result);
  case RQ_is_namespace_alias:
    return isNamespaceAlias(*this, Result);
  case RQ_is_type_alias:
    return isTypeAlias(*this, Result);

  case RQ_is_template:
    return isTemplate(*this, Result);
  case RQ_is_class_template:
    return isClassTemplate(*this, Result);
  case RQ_is_alias_template:
    return isAliasTemplate(*this, Result);
  case RQ_is_function_template:
    return isFunctionTemplate(*this, Result);
  case RQ_is_variable_template:
    return isVariableTemplate(*this, Result);
  case RQ_is_static_member_function_template:
    return isStaticMemberFunctionTemplate(*this, Result);
  case RQ_is_nonstatic_member_function_template:
    return isNonstaticMemberFunctionTemplate(*this, Result);
  case RQ_is_constructor_template:
    return isConstructorTemplate(*this, Result);
  case RQ_is_destructor_template:
    return isDestructorTemplate(*this, Result);
  case RQ_is_concept:
    return isConcept(*this, Result);
  case RQ_is_specialization:
    return isSpecialization(*this, Result);
  case RQ_is_partial_specialization:
    return isPartialSpecialization(*this, Result);
  case RQ_is_explicit_specialization:
    return isExplicitSpecialization(*this, Result);
  case RQ_is_implicit_instantiation:
    return isImplicitInstantiation(*this, Result);
  case RQ_is_explicit_instantiation:
    return isExplicitInstantiation(*this, Result);

  case RQ_is_direct_base:
    return isDirectBase(*this, Result);
  case RQ_is_virtual_base:
    return isVirtualBase(*this, Result);

  case RQ_is_function_parameter:
    return isFunctionParameter(*this, Result);
  case RQ_is_template_parameter:
    return isTemplateParameter(*this, Result);
  case RQ_is_type_template_parameter:
    return isTypeTemplateParameter(*this, Result);
  case RQ_is_nontype_template_parameter:
    return isNontypeTemplateParameter(*this, Result);
  case RQ_is_template_template_parameter:
    return isTemplateTemplateParameter(*this, Result);

  case RQ_is_expression:
    return ::isExpression(*this, Result);
  case RQ_is_lvalue:
    return isLValue(*this, Result);
  case RQ_is_xvalue:
    return isXValue(*this, Result);
  case RQ_is_rvalue:
    return isRValue(*this, Result);
  case RQ_is_value:
    return isValue(*this, Result);

  case RQ_is_local:
    return isLocal(*this, Result);
  case RQ_is_class_member:
    return isClassMember(*this, Result);

  case RQ_has_default_access:
    return hasDefaultAccess(*this, Result);

  default:
    break;
  }
  llvm_unreachable("invalid predicate selector");
}
\
/// Convert a bit-field structure into a uint32.
template <typename Traits>
static std::uint32_t TraitsToUnsignedInt(Traits S) {
  static_assert(sizeof(std::uint32_t) == sizeof(Traits), "size mismatch");
  unsigned ret = 0;
  std::memcpy(&ret, &S, sizeof(S));
  return ret;
}

template <typename Traits>
static APValue makeTraits(ASTContext &C, Traits S) {
  return APValue(C.MakeIntValue(TraitsToUnsignedInt(S), C.UnsignedIntTy));
}

template <typename Traits>
static bool SuccessTraits(const Reflection &R, Traits S, APValue &Result) {
  Result = makeTraits(R.getContext(), S);
  return true;
}

enum LinkageTrait : unsigned { LinkNone, LinkInternal, LinkExternal };

/// Remap linkage specifiers into a 2-bit value.
static LinkageTrait getLinkage(const NamedDecl *D) {
  switch (D->getFormalLinkage()) {
  case NoLinkage:
    return LinkNone;
  case InternalLinkage:
    return LinkInternal;
  case ExternalLinkage:
    return LinkExternal;
  default:
    break;
  }
  llvm_unreachable("Invalid linkage specification");
}

enum AccessTrait : unsigned {
  AccessNone,
  AccessPublic,
  AccessPrivate,
  AccessProtected
};

/// Returns the access specifiers for \p D.
static AccessTrait getAccess(const Decl *D) {
  switch (D->getAccess()) {
  case AS_public:
    return AccessPublic;
  case AS_private:
    return AccessPrivate;
  case AS_protected:
    return AccessProtected;
  case AS_none:
    return AccessNone;
  }
  llvm_unreachable("Invalid access specifier");
}

/// This gives the storage duration of declared objects, not the storage
/// specifier, which incorporates aspects of duration and linkage.
enum StorageTrait : unsigned {
  AutomaticStorage,
  StaticStorage,
  ThreadStorage,
  DynamicStorage
};

/// Returns the storage duration of \p D.
static StorageTrait getStorage(const VarDecl *D) {
  switch (D->getStorageDuration()) {
  case SD_FullExpression:
  case SD_Automatic:
    return AutomaticStorage;
  case SD_Thread:
    return ThreadStorage;
  case SD_Static:
    return StaticStorage;
  case SD_Dynamic:
    return DynamicStorage;
  }
  llvm_unreachable("Invalid storage duration");
}

#pragma pack(push, 1)
/// Traits for named objects.
///
/// Note that a variable can be declared \c extern and not be defined.
struct VariableTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  StorageTrait Storage : 2;
  unsigned Constexpr : 1;
  unsigned Defined : 1;
  unsigned Inline : 1; ///< Valid only when defined.
  unsigned Rest : 23;
};

static VariableTraits getVariableTraits(const VarDecl *D) {
  VariableTraits T = VariableTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Storage = getStorage(D);
  T.Constexpr = D->isConstexpr();
  T.Defined = D->getDefinition() != nullptr;
  T.Inline = D->isInline();
  return T;
}

/// Traits for named sub-objects of a class (or union?).
struct FieldTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  unsigned Mutable : 1;
  unsigned Rest : 27;
};

/// Get the traits for a non-static member of a class or union.
static FieldTraits getFieldTraits(const FieldDecl *D) {
  FieldTraits T = FieldTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Mutable = D->isMutable();
  return T;
}

/// Computed traits of normal, extern local, and static class functions.
///
// TODO: Add calling conventions to function traits.
struct FunctionTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  unsigned Constexpr : 1;
  unsigned Nothrow : 1; ///< Called \c noexcept in C++.
  unsigned Defined : 1;
  unsigned Inline : 1;  ///< Valid only when defined.
  unsigned Deleted : 1; ///< Valid only when defined.
  unsigned Rest : 23;
};

static bool getNothrow(const FunctionDecl *D) {
  if (const FunctionProtoType *Ty = D->getType()->getAs<FunctionProtoType>())
    return Ty->isNothrow();
  return false;
}

static FunctionTraits getFunctionTraits(const FunctionDecl *D) {
  FunctionTraits T = FunctionTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Constexpr = D->isConstexpr();
  T.Nothrow = getNothrow(D);
  T.Defined = D->getDefinition() != nullptr;
  T.Inline = D->isInlined();
  T.Deleted = D->isDeleted();
  return T;
}

enum MethodKind : unsigned {
  Method,
  Constructor,
  Destructor,
  Conversion
};

/// Traits for normal member functions.
struct MethodTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  MethodKind Kind : 2;
  unsigned Constexpr : 1;
  unsigned Explicit : 1;
  unsigned Virtual : 1;
  unsigned Pure : 1;
  unsigned Final : 1;
  unsigned Override : 1;
  unsigned Nothrow : 1; ///< Called \c noexcept in C++.
  unsigned Defined : 1;
  unsigned Inline : 1;
  unsigned Deleted : 1;
  unsigned Defaulted : 1;
  unsigned Trivial : 1;
  unsigned DefaultCtor : 1;
  unsigned CopyCtor : 1;
  unsigned MoveCtor : 1;
  unsigned CopyAssign : 1;
  unsigned MoveAssign : 1;
  unsigned Rest : 9;
};

static MethodTraits getMethodTraits(const CXXConstructorDecl *D) {
  MethodTraits T = MethodTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Kind = Constructor;
  T.Constexpr = D->isConstexpr();
  T.Nothrow = getNothrow(D);
  T.Defined = D->getDefinition() != nullptr;
  T.Inline = D->isInlined();
  T.Deleted = D->isDeleted();
  T.Defaulted = D->isDefaulted();
  T.Trivial = D->isTrivial();
  T.DefaultCtor = D->isDefaultConstructor();
  T.CopyCtor = D->isCopyConstructor();
  T.MoveCtor = D->isMoveConstructor();
  return T;
}

static MethodTraits getMethodTraits(const CXXDestructorDecl *D) {
  MethodTraits T = MethodTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Kind = Destructor;
  T.Virtual = D->isVirtual();
  T.Pure = D->isPure();
  T.Final = D->hasAttr<FinalAttr>();
  T.Override = D->hasAttr<OverrideAttr>();
  T.Nothrow = getNothrow(D);
  T.Defined = D->getDefinition() != nullptr;
  T.Inline = D->isInlined();
  T.Deleted = D->isDeleted();
  T.Defaulted = D->isDefaulted();
  T.Trivial = D->isTrivial();
  return T;
}

static MethodTraits getMethodTraits(const CXXConversionDecl *D) {
  MethodTraits T = MethodTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Kind = Conversion;
  T.Constexpr = D->isConstexpr();
  T.Explicit = D->isExplicit();
  T.Virtual = D->isVirtual();
  T.Pure = D->isPure();
  T.Final = D->hasAttr<FinalAttr>();
  T.Override = D->hasAttr<OverrideAttr>();
  T.Nothrow = getNothrow(D);
  T.Defined = D->getDefinition() != nullptr;
  T.Inline = D->isInlined();
  T.Deleted = D->isDeleted();
  return T;
}

static MethodTraits getMethodTraits(const CXXMethodDecl *D) {
  MethodTraits T = MethodTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Kind = Method;
  T.Constexpr = D->isConstexpr();
  T.Virtual = D->isVirtual();
  T.Pure = D->isPure();
  T.Final = D->hasAttr<FinalAttr>();
  T.Override = D->hasAttr<OverrideAttr>();
  T.Nothrow = getNothrow(D);
  T.Defined = D->getDefinition() != nullptr;
  T.Inline = D->isInlined();
  T.Deleted = D->isDeleted();
  T.CopyAssign = D->isCopyAssignmentOperator();
  T.MoveAssign = D->isMoveAssignmentOperator();
  return T;
}

struct ValueTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  unsigned Rest : 28;
};

static ValueTraits getValueTraits(const EnumConstantDecl *D) {
  ValueTraits T = ValueTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  return T;
}

struct NamespaceTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  unsigned Inline : 1;
  unsigned Rest : 27;
};

static NamespaceTraits getNamespaceTraits(const NamespaceDecl *D) {
  NamespaceTraits T = NamespaceTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Inline = D->isInline();
  return T;
}

static bool makeDeclTraits(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R)) {
    if (const VarDecl *Var = dyn_cast<VarDecl>(D))
      return SuccessTraits(R, getVariableTraits(Var), Result);
    else if (const FieldDecl *Field = dyn_cast<FieldDecl>(D))
      return SuccessTraits(R, getFieldTraits(Field), Result);
    else if (const CXXConstructorDecl *Ctor = dyn_cast<CXXConstructorDecl>(D))
      return SuccessTraits(R, getMethodTraits(Ctor), Result);
    else if (const CXXDestructorDecl *Dtor = dyn_cast<CXXDestructorDecl>(D))
      return SuccessTraits(R, getMethodTraits(Dtor), Result);
    else if (const CXXConversionDecl *Conv = dyn_cast<CXXConversionDecl>(D))
      return SuccessTraits(R, getMethodTraits(Conv), Result);
    else if (const CXXMethodDecl *Meth = dyn_cast<CXXMethodDecl>(D))
      return SuccessTraits(R, getMethodTraits(Meth), Result);
    else if (const FunctionDecl *Fn = dyn_cast<FunctionDecl>(D))
      return SuccessTraits(R, getFunctionTraits(Fn), Result);
    else if (const EnumConstantDecl *Enum = dyn_cast<EnumConstantDecl>(D))
      return SuccessTraits(R, getValueTraits(Enum), Result);
    else if (const NamespaceDecl *Ns = dyn_cast<NamespaceDecl>(D))
      return SuccessTraits(R, getNamespaceTraits(Ns), Result);
  }

  return Error(R);
}

struct LinkageTraits {
  LinkageTrait Kind : 2;
  unsigned Rest : 30;
};

static LinkageTraits getLinkageTraits(const NamedDecl *D) {
  LinkageTraits T = LinkageTraits();
  T.Kind = getLinkage(D);
  return T;
};

static bool makeLinkageTraits(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
      return SuccessTraits(R, getLinkageTraits(ND), Result);

  return Error(R);
}

struct AccessTraits {
  unsigned Padding : 2; ///< Padded for library implementation ease.
  AccessTrait Kind : 2;
  unsigned Rest : 28;
};

static AccessTraits getAccessTraits(const Decl *D) {
  AccessTraits T = AccessTraits();
  T.Kind = getAccess(D);
  return T;
};

static bool makeAccessTraits(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return SuccessTraits(R, getAccessTraits(D), Result);

  return Error(R);
}

enum ClassKindTrait : unsigned { StructKind, ClassKind, UnionKind };

// TODO: Accumulate all known type traits for classes.
struct ClassTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  ClassKindTrait Kind : 2;
  unsigned Complete : 1;
  unsigned Polymoprhic : 1;
  unsigned Abstract : 1;
  unsigned Final : 1;
  unsigned Empty : 1;
  unsigned Rest : 21;
};

static ClassKindTrait getClassKind(const CXXRecordDecl *D) {
  switch(D->getTagKind()) {
  case TTK_Struct:
    return StructKind;
  case TTK_Class:
    return ClassKind;
  case TTK_Union:
    return UnionKind;
  default:
    llvm_unreachable("unsupported kind");
  }
}

static ClassTraits getClassTraits(const CXXRecordDecl *D) {
  ClassTraits T = ClassTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Kind = getClassKind(D);
  T.Complete = D->getDefinition() != nullptr;
  if (T.Complete) {
    T.Polymoprhic = D->isPolymorphic();
    T.Abstract = D->isAbstract();
    T.Final = D->hasAttr<FinalAttr>();
    T.Empty = D->isEmpty();
  }
  return T;
}

struct EnumTraits {
  LinkageTrait Linkage : 2;
  AccessTrait Access : 2;
  unsigned Scoped : 1;
  unsigned Complete : 1;
  unsigned Rest : 26;
};

static EnumTraits getEnumTraits(const EnumDecl *D) {
  EnumTraits T = EnumTraits();
  T.Linkage = getLinkage(D);
  T.Access = getAccess(D);
  T.Scoped = D->isScoped();
  T.Complete = D->isComplete();
  return T;
}

static bool makeTypeTraits(const Reflection &R, APValue &Result) {
  if (const MaybeType T = getCanonicalType(R)) {
    if (const TagDecl *TD = T->getAsTagDecl()) {
      if (const CXXRecordDecl *Class = dyn_cast<CXXRecordDecl>(TD))
        return SuccessTraits(R, getClassTraits(Class), Result);
      else if (const EnumDecl *Enum = dyn_cast<EnumDecl>(TD))
        return SuccessTraits(R, getEnumTraits(Enum), Result);
      else
        llvm_unreachable("unsupported type");
    }
  }

  return Error(R);
}

bool Reflection::GetTraits(ReflectionQuery Q, APValue &Result) {
  assert(isTraitQuery(Q) && "invalid query");
  switch (Q) {
  // Traits
  case RQ_get_decl_traits:
    return makeDeclTraits(*this, Result);
  case RQ_get_linkage_traits:
    return makeLinkageTraits(*this, Result);
  case RQ_get_access_traits:
    return makeAccessTraits(*this, Result);
  case RQ_get_type_traits:
    return makeTypeTraits(*this, Result);

  default:
    break;
  }
  llvm_unreachable("invalid traits selector");
}

#pragma pack(pop)

/// Set Result to an invalid reflection.
static bool makeReflection(APValue &Result) {
  Result = APValue(RK_invalid, nullptr);
  return true;
}

/// Set Result to a reflection of D.
static bool makeReflection(const Decl *D, APValue &Result) {
  if (!D)
    return makeReflection(Result);
  Result = APValue(RK_declaration, D);
  return true;
}

/// Set Result to a reflection of D.
static bool makeReflection(const DeclContext *DC, APValue &Result) {
  if (!DC)
    return makeReflection(Result);
  Result = APValue(RK_declaration, Decl::castFromDeclContext(DC));
  return true;
}

/// Set Result to a reflection of T.
static bool makeReflection(QualType T, APValue &Result) {
  if (T.isNull())
    return makeReflection(Result);
  Result = APValue(RK_type, T.getAsOpaquePtr());
  return true;
}

/// Set Result to a reflection of T.
static bool makeReflection(const Type *T, APValue &Result) {
  assert(T);
  return makeReflection(QualType(T, 0), Result);
}

/// Set Result to a reflection of E.
static bool makeReflection(const Expr *E, APValue &Result) {
  Result = APValue(RK_declaration, E);
  return true;
}

/// Set Result to a reflection of B.
static bool makeReflection(const CXXBaseSpecifier *B, APValue &Result) {
  Result = APValue(RK_declaration, B);
  return true;
}

static bool getEntity(const Reflection &R, APValue &Result) {
  if (R.isType()) {
    /// The entity is the canonical type.
    QualType T = R.getContext().getCanonicalType(R.getAsType());
    return makeReflection(T, Result);
  }
  if (R.isDeclaration()) {
    /// The entity is the canonical declaration.
    const Decl *D = R.getAsDeclaration()->getCanonicalDecl();
    return makeReflection(D, Result);
  }
  if (R.isExpression()) {
    /// The entity is the reachable declaration.
    if (const Decl *D = getReachableDecl(R))
      return makeReflection(D, Result);

    // FIXME: Give a better error message.
    return Error(R);
  }
  if (R.isBase()) {
    // The entity is the canonical type named by the specifier.
    const CXXBaseSpecifier *Base = R.getAsBase();
    QualType T = R.getContext().getCanonicalType(Base->getType());
    return makeReflection(T, Result);
  }
  return Error(R);
}

static bool getParent(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return makeReflection(D->getDeclContext(), Result);
  return Error(R);
}

static bool getType(const Reflection &R, APValue &Result) {
  if (const Expr *E = getExpr(R))
    return makeReflection(E->getType(), Result);
  if (const Decl *D = getReachableDecl(R)) {
    if (const TypeDecl *TD = dyn_cast<TypeDecl>(D))
      return makeReflection(TD->getTypeForDecl(), Result);
    if (const ValueDecl *VD = dyn_cast<ValueDecl>(D))
      return makeReflection(VD->getType(), Result);
  }

  // FIXME: Emit an appropriate error diagnostic.
  return Error(R);
}

static bool getReturnType(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R)) {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D))
      return makeReflection(FD->getReturnType(), Result);
  }
  return Error(R);
}

static bool getThisRefType(const Reflection &R, APValue &Result) {
  return ErrorUnimplemented(R);
}

static bool getDefinition(const Reflection &R, APValue &Result) {
  if (const TypeDecl *TD = getAsTypeDecl(R)) {
    if (isa<TagDecl>(TD))
      return makeReflection(TD, Result);
  }
  return Error(R);
}

/// True if D is reflectable. Some declarations are not reflected (e.g.,
/// access specifiers).
static bool isReflectableDecl(const Decl *D) {
  assert(D && "null declaration");
  if (isa<AccessSpecDecl>(D))
    return false;
  if (const CXXRecordDecl *Class = dyn_cast<CXXRecordDecl>(D))
    if (Class->isInjectedClassName())
      return false;
  return true;
}

/// Filter non-reflectable members.
static const Decl *findNextMember(const Decl *D) {
  while (D && !isReflectableDecl(D))
    D = D->getNextDeclInContext();
  return D;
}

/// Returns the first reflectable member.
static const Decl *getFirstMember(const DeclContext *DC) {
  return findNextMember(*DC->decls_begin());
}

/// Returns the next reflectable member
static const Decl *getNextMember(const Decl *D) {
  return findNextMember(D->getNextDeclInContext());
}

/// Returns the reachable declaration context for R, if any.
static const DeclContext *getReachableDeclContext(const Reflection &R) {
  if (const Decl *D = getReachableDecl(R))
    if (const DeclContext *DC = dyn_cast<DeclContext>(D))
      return DC;
  return nullptr;
}

/// Returns the first member
static bool getBegin(const Reflection &R, APValue &Result) {
  if (const DeclContext *DC = getReachableDeclContext(R))
    return makeReflection(getFirstMember(DC), Result);
  return Error(R);
}

static bool getNext(const Reflection &R, APValue &Result) {
  if (const Decl *D = getReachableDecl(R))
    return makeReflection(getNextMember(D), Result);
  return Error(R);
}

bool Reflection::GetAssociatedReflection(ReflectionQuery Q, APValue &Result) {
  assert(isAssociatedReflectionQuery(Q) && "invalid query");
  switch (Q) {
  // Associated reflections
  case RQ_get_entity:
    return getEntity(*this, Result);
  case RQ_get_parent:
    return getParent(*this, Result);
  case RQ_get_type:
    return getType(*this, Result);
  case RQ_get_return_type:
    return getReturnType(*this, Result);
  case RQ_get_this_ref_type:
    return getThisRefType(*this, Result);
  case RQ_get_definition:
    return getDefinition(*this, Result);

  // Traversal
  case RQ_get_begin:
    return getBegin(*this, Result);
  case RQ_get_next:
    return getNext(*this, Result);

  default:
    break;
  }
  llvm_unreachable("invalid reflection selector");
}

// Creates a c-string of type const char *.
//
// This is morally equivalent to creating a global string.
// During codegen, that's exactly how this is interpreted.
static Expr *
MakeConstCharPointer(ASTContext& Ctx, StringRef Str, SourceLocation Loc) {
  QualType StrLitTy = Ctx.getConstantArrayType(Ctx.CharTy.withConst(),
                                            llvm::APInt(32, Str.size() + 1),
                                            ArrayType::Normal, 0);

  // Create a string literal of type const char [L] where L
  // is the number of characters in the StringRef.
  StringLiteral *StrLit = StringLiteral::Create(Ctx, Str, StringLiteral::Ascii,
                                                false, StrLitTy, Loc);

  // Create an implicit cast expr so that we convert our const char [L]
  // into an actual const char * for proper evaluation.
  QualType StrTy = Ctx.getPointerType(Ctx.getConstType(Ctx.CharTy));
  return ImplicitCastExpr::Create(Ctx, StrTy, CK_ArrayToPointerDecay, StrLit,
                                  /*BasePath=*/nullptr, VK_RValue);
}

bool getName(const Reflection R, APValue &Result) {
  ASTContext &Ctx = R.getContext();

  if (R.isType()) {
    QualType T = R.getAsType();

    // See through loc infos.
    if (const LocInfoType *LIT = dyn_cast<LocInfoType>(T))
      T = LIT->getType();

    // Render the string of the type.
    PrintingPolicy PP = Ctx.getPrintingPolicy();
    PP.SuppressTagKeyword = true;
    Expr *Str = MakeConstCharPointer(Ctx, T.getAsString(PP), SourceLocation());

    // Generate the result value.
    Expr::EvalResult Eval;
    if (!Str->EvaluateAsConstantExpr(Eval, Expr::EvaluateForCodeGen, Ctx))
      return false;
    Result = Eval.Val;
    return true;
  }

  if (const NamedDecl *ND = dyn_cast<NamedDecl>(getReachableDecl(R))) {
    if (IdentifierInfo *II = ND->getIdentifier()) {
      // Get the identifier of the declaration.
      Expr *Str = MakeConstCharPointer(Ctx, II->getName(),
                                       SourceLocation());

      // Generate the result value.
      Expr::EvalResult Eval;
      if (!Str->EvaluateAsConstantExpr(Eval, Expr::EvaluateForCodeGen, Ctx))
        return false;
      Result = Eval.Val;
      return true;
    }
  }

  return Error(R);
}

bool Reflection::GetName(ReflectionQuery Q, APValue &Result) {
  assert(isNameQuery(Q) && "invalid query");

  if (isInvalid()) {
    return Error(*this);
  }

  switch (Q) {
  // Names
  case RQ_get_name:
  case RQ_get_display_name:
    return getName(*this, Result);

  default:
    break;
  }

  llvm_unreachable("invalid name selector");
}


static bool Match(QualType a, QualType b) {
  return a->getCanonicalTypeUnqualified() == b->getCanonicalTypeUnqualified();
}

bool Reflection::GetUserDefinedAttribute(ReflectionQuery Q, APValue AttributeType, APValue &Result) {
  assert(Q == ReflectionQuery::RQ_get_attribute && "invalid query");
  ASTContext &Ctx = getContext();
  QualType ReflectedAttribute = AttributeType.getReflectedType();
  if(ReflectedAttribute.isNull())
    return false;

  if (const Decl *D = getReachableDecl(*this)) {
    for (const auto *UD : D->specific_attrs<UserDefinedAttr>()) {
      auto Expr = UD->getExpression();
      auto Type = Expr->getType();
      if(Match(Type, ReflectedAttribute)) {
        Result = UD->getValue();
        return true;
      }
    }
  }
  return false;
}

bool Reflection::HasUserDefinedAttribute(ReflectionQuery Q, APValue AttributeType, APValue &Result) {
  assert(Q == ReflectionQuery::RQ_has_attribute && "invalid query");
  ASTContext &Ctx = getContext();
  QualType ReflectedAttribute = AttributeType.getReflectedType();
  if(ReflectedAttribute.isNull())
    return false;

  if (const Decl *D = getReachableDecl(*this)) {
    for (const auto *UD : D->specific_attrs<UserDefinedAttr>()) {
      auto Expr = UD->getExpression();
      auto Type = Expr->getType();
      if(Match(Type, ReflectedAttribute)) {
        SuccessTrue(*this, Result);
        return true;
      }
    }
  }
  SuccessFalse(*this, Result);
  return true;
}

/// Returns true if canonical types are equal.
static bool EqualTypes(ASTContext &Ctx, QualType A, QualType B) {
  CanQualType CanA = Ctx.getCanonicalType(A);
  CanQualType CanB = Ctx.getCanonicalType(B);
  return CanA == CanB;
}

/// Returns true if the declared entities are the same.
static bool EqualDecls(const Decl *A, const Decl *B) {
  const Decl *CanA = A->getCanonicalDecl();
  const Decl *CanB = B->getCanonicalDecl();
  return CanA == CanB;
}

bool Reflection::Equal(ASTContext &Ctx, APValue const& A, APValue const& B) {
  assert(A.isReflection() && B.isReflection());

  if (A.getReflectionKind() != B.getReflectionKind())
    return false;

  switch (A.getReflectionKind()) {
  case RK_invalid:
    return true;
  case RK_type:
    return EqualTypes(Ctx,
                      getQualType(A),
                      getQualType(B));
  case RK_declaration:
    return EqualDecls(A.getReflectedDeclaration(),
                      B.getReflectedDeclaration());
  default:
    return false;
  }
}
