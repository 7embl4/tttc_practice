//===--- SemaOpenACC.cpp - Semantic Analysis for OpenACC constructs -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements semantic analysis for OpenACC constructs, and things
/// that are not clause specific.
///
//===----------------------------------------------------------------------===//

#include "clang/Sema/SemaOpenACC.h"
#include "clang/AST/DeclOpenACC.h"
#include "clang/AST/StmtOpenACC.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/OpenACCKinds.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Casting.h"

using namespace clang;

namespace {
bool diagnoseConstructAppertainment(SemaOpenACC &S, OpenACCDirectiveKind K,
                                    SourceLocation StartLoc, bool IsStmt) {
  switch (K) {
  default:
  case OpenACCDirectiveKind::Invalid:
    // Nothing to do here, both invalid and unimplemented don't really need to
    // do anything.
    break;
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::ParallelLoop:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::SerialLoop:
  case OpenACCDirectiveKind::Kernels:
  case OpenACCDirectiveKind::KernelsLoop:
  case OpenACCDirectiveKind::Loop:
  case OpenACCDirectiveKind::Data:
  case OpenACCDirectiveKind::EnterData:
  case OpenACCDirectiveKind::ExitData:
  case OpenACCDirectiveKind::HostData:
  case OpenACCDirectiveKind::Wait:
  case OpenACCDirectiveKind::Update:
  case OpenACCDirectiveKind::Init:
  case OpenACCDirectiveKind::Shutdown:
  case OpenACCDirectiveKind::Cache:
  case OpenACCDirectiveKind::Atomic:
    if (!IsStmt)
      return S.Diag(StartLoc, diag::err_acc_construct_appertainment) << K;
    break;
  }
  return false;
}

void CollectActiveReductionClauses(
    llvm::SmallVector<OpenACCReductionClause *> &ActiveClauses,
    ArrayRef<OpenACCClause *> CurClauses) {
  for (auto *CurClause : CurClauses) {
    if (auto *RedClause = dyn_cast<OpenACCReductionClause>(CurClause);
        RedClause && !RedClause->getVarList().empty())
      ActiveClauses.push_back(RedClause);
  }
}

// Depth needs to be preserved for all associated statements that aren't
// supposed to modify the compute/combined/loop construct information.
bool PreserveLoopRAIIDepthInAssociatedStmtRAII(OpenACCDirectiveKind DK) {
  switch (DK) {
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::ParallelLoop:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::SerialLoop:
  case OpenACCDirectiveKind::Kernels:
  case OpenACCDirectiveKind::KernelsLoop:
  case OpenACCDirectiveKind::Loop:
    return false;
  case OpenACCDirectiveKind::Data:
  case OpenACCDirectiveKind::HostData:
  case OpenACCDirectiveKind::Atomic:
    return true;
  case OpenACCDirectiveKind::Cache:
  case OpenACCDirectiveKind::Routine:
  case OpenACCDirectiveKind::Declare:
  case OpenACCDirectiveKind::EnterData:
  case OpenACCDirectiveKind::ExitData:
  case OpenACCDirectiveKind::Wait:
  case OpenACCDirectiveKind::Init:
  case OpenACCDirectiveKind::Shutdown:
  case OpenACCDirectiveKind::Set:
  case OpenACCDirectiveKind::Update:
    llvm_unreachable("Doesn't have an associated stmt");
  case OpenACCDirectiveKind::Invalid:
    llvm_unreachable("Unhandled directive kind?");
  }
  llvm_unreachable("Unhandled directive kind?");
}

} // namespace

SemaOpenACC::SemaOpenACC(Sema &S) : SemaBase(S) {}

SemaOpenACC::AssociatedStmtRAII::AssociatedStmtRAII(
    SemaOpenACC &S, OpenACCDirectiveKind DK, SourceLocation DirLoc,
    ArrayRef<const OpenACCClause *> UnInstClauses,
    ArrayRef<OpenACCClause *> Clauses)
    : SemaRef(S), OldActiveComputeConstructInfo(S.ActiveComputeConstructInfo),
      DirKind(DK), OldLoopGangClauseOnKernel(S.LoopGangClauseOnKernel),
      OldLoopWorkerClauseLoc(S.LoopWorkerClauseLoc),
      OldLoopVectorClauseLoc(S.LoopVectorClauseLoc),
      OldLoopWithoutSeqInfo(S.LoopWithoutSeqInfo),
      ActiveReductionClauses(S.ActiveReductionClauses),
      LoopRAII(SemaRef, PreserveLoopRAIIDepthInAssociatedStmtRAII(DirKind)) {

  // Compute constructs end up taking their 'loop'.
  if (DirKind == OpenACCDirectiveKind::Parallel ||
      DirKind == OpenACCDirectiveKind::Serial ||
      DirKind == OpenACCDirectiveKind::Kernels) {
    CollectActiveReductionClauses(S.ActiveReductionClauses, Clauses);
    SemaRef.ActiveComputeConstructInfo.Kind = DirKind;
    SemaRef.ActiveComputeConstructInfo.Clauses = Clauses;

    // OpenACC 3.3 2.9.2: When the parent compute construct is a kernels
    // construct, the gang clause behaves as follows. ... The region of a loop
    // with a gang clause may not contain another loop with a gang clause unless
    // within a nested compute region.
    //
    // Implement the 'unless within a nested compute region' part.
    SemaRef.LoopGangClauseOnKernel = {};
    SemaRef.LoopWorkerClauseLoc = {};
    SemaRef.LoopVectorClauseLoc = {};
    SemaRef.LoopWithoutSeqInfo = {};
  } else if (DirKind == OpenACCDirectiveKind::ParallelLoop ||
             DirKind == OpenACCDirectiveKind::SerialLoop ||
             DirKind == OpenACCDirectiveKind::KernelsLoop) {
    SemaRef.ActiveComputeConstructInfo.Kind = DirKind;
    SemaRef.ActiveComputeConstructInfo.Clauses = Clauses;

    CollectActiveReductionClauses(S.ActiveReductionClauses, Clauses);
    SetCollapseInfoBeforeAssociatedStmt(UnInstClauses, Clauses);
    SetTileInfoBeforeAssociatedStmt(UnInstClauses, Clauses);

    SemaRef.LoopGangClauseOnKernel = {};
    SemaRef.LoopWorkerClauseLoc = {};
    SemaRef.LoopVectorClauseLoc = {};

    // Set the active 'loop' location if there isn't a 'seq' on it, so we can
    // diagnose the for loops.
    SemaRef.LoopWithoutSeqInfo = {};
    if (Clauses.end() ==
        llvm::find_if(Clauses, llvm::IsaPred<OpenACCSeqClause>))
      SemaRef.LoopWithoutSeqInfo = {DirKind, DirLoc};

    // OpenACC 3.3 2.9.2: When the parent compute construct is a kernels
    // construct, the gang clause behaves as follows. ... The region of a loop
    // with a gang clause may not contain another loop with a gang clause unless
    // within a nested compute region.
    //
    // We don't bother doing this when this is a template instantiation, as
    // there is no reason to do these checks: the existance of a
    // gang/kernels/etc cannot be dependent.
    if (DirKind == OpenACCDirectiveKind::KernelsLoop && UnInstClauses.empty()) {
      // This handles the 'outer loop' part of this.
      auto *Itr = llvm::find_if(Clauses, llvm::IsaPred<OpenACCGangClause>);
      if (Itr != Clauses.end())
        SemaRef.LoopGangClauseOnKernel = {(*Itr)->getBeginLoc(), DirKind};
    }

    if (UnInstClauses.empty()) {
      auto *Itr = llvm::find_if(Clauses, llvm::IsaPred<OpenACCWorkerClause>);
      if (Itr != Clauses.end())
        SemaRef.LoopWorkerClauseLoc = (*Itr)->getBeginLoc();

      auto *Itr2 = llvm::find_if(Clauses, llvm::IsaPred<OpenACCVectorClause>);
      if (Itr2 != Clauses.end())
        SemaRef.LoopVectorClauseLoc = (*Itr2)->getBeginLoc();
    }
  } else if (DirKind == OpenACCDirectiveKind::Loop) {
    CollectActiveReductionClauses(S.ActiveReductionClauses, Clauses);
    SetCollapseInfoBeforeAssociatedStmt(UnInstClauses, Clauses);
    SetTileInfoBeforeAssociatedStmt(UnInstClauses, Clauses);

    // Set the active 'loop' location if there isn't a 'seq' on it, so we can
    // diagnose the for loops.
    SemaRef.LoopWithoutSeqInfo = {};
    if (Clauses.end() ==
        llvm::find_if(Clauses, llvm::IsaPred<OpenACCSeqClause>))
      SemaRef.LoopWithoutSeqInfo = {DirKind, DirLoc};

    // OpenACC 3.3 2.9.2: When the parent compute construct is a kernels
    // construct, the gang clause behaves as follows. ... The region of a loop
    // with a gang clause may not contain another loop with a gang clause unless
    // within a nested compute region.
    //
    // We don't bother doing this when this is a template instantiation, as
    // there is no reason to do these checks: the existance of a
    // gang/kernels/etc cannot be dependent.
    if (SemaRef.getActiveComputeConstructInfo().Kind ==
            OpenACCDirectiveKind::Kernels &&
        UnInstClauses.empty()) {
      // This handles the 'outer loop' part of this.
      auto *Itr = llvm::find_if(Clauses, llvm::IsaPred<OpenACCGangClause>);
      if (Itr != Clauses.end())
        SemaRef.LoopGangClauseOnKernel = {(*Itr)->getBeginLoc(),
                                          OpenACCDirectiveKind::Kernels};
    }

    if (UnInstClauses.empty()) {
      auto *Itr = llvm::find_if(Clauses, llvm::IsaPred<OpenACCWorkerClause>);
      if (Itr != Clauses.end())
        SemaRef.LoopWorkerClauseLoc = (*Itr)->getBeginLoc();

      auto *Itr2 = llvm::find_if(Clauses, llvm::IsaPred<OpenACCVectorClause>);
      if (Itr2 != Clauses.end())
        SemaRef.LoopVectorClauseLoc = (*Itr2)->getBeginLoc();
    }
  }
}

void SemaOpenACC::AssociatedStmtRAII::SetCollapseInfoBeforeAssociatedStmt(
    ArrayRef<const OpenACCClause *> UnInstClauses,
    ArrayRef<OpenACCClause *> Clauses) {

  // Reset this checking for loops that aren't covered in a RAII object.
  SemaRef.LoopInfo.CurLevelHasLoopAlready = false;
  SemaRef.CollapseInfo.CollapseDepthSatisfied = true;
  SemaRef.TileInfo.TileDepthSatisfied = true;

  // We make sure to take an optional list of uninstantiated clauses, so that
  // we can check to make sure we don't 'double diagnose' in the event that
  // the value of 'N' was not dependent in a template. We also ensure during
  // Sema that there is only 1 collapse on each construct, so we can count on
  // the fact that if both find a 'collapse', that they are the same one.
  auto *CollapseClauseItr =
      llvm::find_if(Clauses, llvm::IsaPred<OpenACCCollapseClause>);
  auto *UnInstCollapseClauseItr =
      llvm::find_if(UnInstClauses, llvm::IsaPred<OpenACCCollapseClause>);

  if (Clauses.end() == CollapseClauseItr)
    return;

  OpenACCCollapseClause *CollapseClause =
      cast<OpenACCCollapseClause>(*CollapseClauseItr);

  SemaRef.CollapseInfo.ActiveCollapse = CollapseClause;
  Expr *LoopCount = CollapseClause->getLoopCount();

  // If the loop count is still instantiation dependent, setting the depth
  // counter isn't necessary, so return here.
  if (!LoopCount || LoopCount->isInstantiationDependent())
    return;

  // Suppress diagnostics if we've done a 'transform' where the previous version
  // wasn't dependent, meaning we already diagnosed it.
  if (UnInstCollapseClauseItr != UnInstClauses.end() &&
      !cast<OpenACCCollapseClause>(*UnInstCollapseClauseItr)
           ->getLoopCount()
           ->isInstantiationDependent())
    return;

  SemaRef.CollapseInfo.CollapseDepthSatisfied = false;
  SemaRef.CollapseInfo.CurCollapseCount =
      cast<ConstantExpr>(LoopCount)->getResultAsAPSInt();
  SemaRef.CollapseInfo.DirectiveKind = DirKind;
}

void SemaOpenACC::AssociatedStmtRAII::SetTileInfoBeforeAssociatedStmt(
    ArrayRef<const OpenACCClause *> UnInstClauses,
    ArrayRef<OpenACCClause *> Clauses) {
  // We don't diagnose if this is during instantiation, since the only thing we
  // care about is the number of arguments, which we can figure out without
  // instantiation, so we don't want to double-diagnose.
  if (UnInstClauses.size() > 0)
    return;
  auto *TileClauseItr =
      llvm::find_if(Clauses, llvm::IsaPred<OpenACCTileClause>);

  if (Clauses.end() == TileClauseItr)
    return;

  OpenACCTileClause *TileClause = cast<OpenACCTileClause>(*TileClauseItr);
  SemaRef.TileInfo.ActiveTile = TileClause;
  SemaRef.TileInfo.TileDepthSatisfied = false;
  SemaRef.TileInfo.CurTileCount =
      static_cast<unsigned>(TileClause->getSizeExprs().size());
  SemaRef.TileInfo.DirectiveKind = DirKind;
}

SemaOpenACC::AssociatedStmtRAII::~AssociatedStmtRAII() {
  if (DirKind == OpenACCDirectiveKind::Parallel ||
      DirKind == OpenACCDirectiveKind::Serial ||
      DirKind == OpenACCDirectiveKind::Kernels ||
      DirKind == OpenACCDirectiveKind::Loop ||
      DirKind == OpenACCDirectiveKind::ParallelLoop ||
      DirKind == OpenACCDirectiveKind::SerialLoop ||
      DirKind == OpenACCDirectiveKind::KernelsLoop) {
    SemaRef.ActiveComputeConstructInfo = OldActiveComputeConstructInfo;
    SemaRef.LoopGangClauseOnKernel = OldLoopGangClauseOnKernel;
    SemaRef.LoopWorkerClauseLoc = OldLoopWorkerClauseLoc;
    SemaRef.LoopVectorClauseLoc = OldLoopVectorClauseLoc;
    SemaRef.LoopWithoutSeqInfo = OldLoopWithoutSeqInfo;
    SemaRef.ActiveReductionClauses.swap(ActiveReductionClauses);
  } else if (DirKind == OpenACCDirectiveKind::Data ||
             DirKind == OpenACCDirectiveKind::HostData) {
    // Intentionally doesn't reset the Loop, Compute Construct, or reduction
    // effects.
  }
}

void SemaOpenACC::ActOnConstruct(OpenACCDirectiveKind K,
                                 SourceLocation DirLoc) {
  // Start an evaluation context to parse the clause arguments on.
  SemaRef.PushExpressionEvaluationContext(
      Sema::ExpressionEvaluationContext::PotentiallyEvaluated);

  // There is nothing do do here as all we have at this point is the name of the
  // construct itself.
}

ExprResult SemaOpenACC::ActOnIntExpr(OpenACCDirectiveKind DK,
                                     OpenACCClauseKind CK, SourceLocation Loc,
                                     Expr *IntExpr) {

  assert(((DK != OpenACCDirectiveKind::Invalid &&
           CK == OpenACCClauseKind::Invalid) ||
          (DK == OpenACCDirectiveKind::Invalid &&
           CK != OpenACCClauseKind::Invalid) ||
          (DK == OpenACCDirectiveKind::Invalid &&
           CK == OpenACCClauseKind::Invalid)) &&
         "Only one of directive or clause kind should be provided");

  class IntExprConverter : public Sema::ICEConvertDiagnoser {
    OpenACCDirectiveKind DirectiveKind;
    OpenACCClauseKind ClauseKind;
    Expr *IntExpr;

    // gets the index into the diagnostics so we can use this for clauses,
    // directives, and sub array.s
    unsigned getDiagKind() const {
      if (ClauseKind != OpenACCClauseKind::Invalid)
        return 0;
      if (DirectiveKind != OpenACCDirectiveKind::Invalid)
        return 1;
      return 2;
    }

  public:
    IntExprConverter(OpenACCDirectiveKind DK, OpenACCClauseKind CK,
                     Expr *IntExpr)
        : ICEConvertDiagnoser(/*AllowScopedEnumerations=*/false,
                              /*Suppress=*/false,
                              /*SuppressConversion=*/true),
          DirectiveKind(DK), ClauseKind(CK), IntExpr(IntExpr) {}

    bool match(QualType T) override {
      // OpenACC spec just calls this 'integer expression' as having an
      // 'integer type', so fall back on C99's 'integer type'.
      return T->isIntegerType();
    }
    SemaBase::SemaDiagnosticBuilder diagnoseNotInt(Sema &S, SourceLocation Loc,
                                                   QualType T) override {
      return S.Diag(Loc, diag::err_acc_int_expr_requires_integer)
             << getDiagKind() << ClauseKind << DirectiveKind << T;
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseIncomplete(Sema &S, SourceLocation Loc, QualType T) override {
      return S.Diag(Loc, diag::err_acc_int_expr_incomplete_class_type)
             << T << IntExpr->getSourceRange();
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseExplicitConv(Sema &S, SourceLocation Loc, QualType T,
                         QualType ConvTy) override {
      return S.Diag(Loc, diag::err_acc_int_expr_explicit_conversion)
             << T << ConvTy;
    }

    SemaBase::SemaDiagnosticBuilder noteExplicitConv(Sema &S,
                                                     CXXConversionDecl *Conv,
                                                     QualType ConvTy) override {
      return S.Diag(Conv->getLocation(), diag::note_acc_int_expr_conversion)
             << ConvTy->isEnumeralType() << ConvTy;
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseAmbiguous(Sema &S, SourceLocation Loc, QualType T) override {
      return S.Diag(Loc, diag::err_acc_int_expr_multiple_conversions) << T;
    }

    SemaBase::SemaDiagnosticBuilder
    noteAmbiguous(Sema &S, CXXConversionDecl *Conv, QualType ConvTy) override {
      return S.Diag(Conv->getLocation(), diag::note_acc_int_expr_conversion)
             << ConvTy->isEnumeralType() << ConvTy;
    }

    SemaBase::SemaDiagnosticBuilder
    diagnoseConversion(Sema &S, SourceLocation Loc, QualType T,
                       QualType ConvTy) override {
      llvm_unreachable("conversion functions are permitted");
    }
  } IntExprDiagnoser(DK, CK, IntExpr);

  if (!IntExpr)
    return ExprError();

  ExprResult IntExprResult = SemaRef.PerformContextualImplicitConversion(
      Loc, IntExpr, IntExprDiagnoser);
  if (IntExprResult.isInvalid())
    return ExprError();

  IntExpr = IntExprResult.get();
  if (!IntExpr->isTypeDependent() && !IntExpr->getType()->isIntegerType())
    return ExprError();

  // TODO OpenACC: Do we want to perform usual unary conversions here? When
  // doing codegen we might find that is necessary, but skip it for now.
  return IntExpr;
}

bool SemaOpenACC::CheckVarIsPointerType(OpenACCClauseKind ClauseKind,
                                        Expr *VarExpr) {
  // We already know that VarExpr is a proper reference to a variable, so we
  // should be able to just take the type of the expression to get the type of
  // the referenced variable.

  // We've already seen an error, don't diagnose anything else.
  if (!VarExpr || VarExpr->containsErrors())
    return false;

  if (isa<ArraySectionExpr>(VarExpr->IgnoreParenImpCasts()) ||
      VarExpr->hasPlaceholderType(BuiltinType::ArraySection)) {
    Diag(VarExpr->getExprLoc(), diag::err_array_section_use) << /*OpenACC=*/0;
    Diag(VarExpr->getExprLoc(), diag::note_acc_expected_pointer_var);
    return true;
  }

  QualType Ty = VarExpr->getType();
  Ty = Ty.getNonReferenceType().getUnqualifiedType();

  // Nothing we can do if this is a dependent type.
  if (Ty->isDependentType())
    return false;

  if (!Ty->isPointerType())
    return Diag(VarExpr->getExprLoc(), diag::err_acc_var_not_pointer_type)
           << ClauseKind << Ty;
  return false;
}

ExprResult SemaOpenACC::ActOnCacheVar(Expr *VarExpr) {
  Expr *CurVarExpr = VarExpr->IgnoreParenImpCasts();
  if (!isa<ArraySectionExpr, ArraySubscriptExpr>(CurVarExpr)) {
    Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref_cache);
    return ExprError();
  }

  // It isn't clear what 'simple array element or simple subarray' means, so we
  // will just allow arbitrary depth.
  while (isa<ArraySectionExpr, ArraySubscriptExpr>(CurVarExpr)) {
    if (auto *SubScrpt = dyn_cast<ArraySubscriptExpr>(CurVarExpr))
      CurVarExpr = SubScrpt->getBase()->IgnoreParenImpCasts();
    else
      CurVarExpr =
          cast<ArraySectionExpr>(CurVarExpr)->getBase()->IgnoreParenImpCasts();
  }

  // References to a VarDecl are fine.
  if (const auto *DRE = dyn_cast<DeclRefExpr>(CurVarExpr)) {
    if (isa<VarDecl, NonTypeTemplateParmDecl>(
            DRE->getFoundDecl()->getCanonicalDecl()))
      return VarExpr;
  }

  if (const auto *ME = dyn_cast<MemberExpr>(CurVarExpr)) {
    if (isa<FieldDecl>(ME->getMemberDecl()->getCanonicalDecl())) {
      return VarExpr;
    }
  }

  // Nothing really we can do here, as these are dependent.  So just return they
  // are valid.
  if (isa<DependentScopeDeclRefExpr, CXXDependentScopeMemberExpr>(CurVarExpr))
    return VarExpr;

  // There isn't really anything we can do in the case of a recovery expr, so
  // skip the diagnostic rather than produce a confusing diagnostic.
  if (isa<RecoveryExpr>(CurVarExpr))
    return ExprError();

  Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref_cache);
  return ExprError();
}
ExprResult SemaOpenACC::ActOnVar(OpenACCDirectiveKind DK, OpenACCClauseKind CK,
                                 Expr *VarExpr) {
  // This has unique enough restrictions that we should split it to a separate
  // function.
  if (DK == OpenACCDirectiveKind::Cache)
    return ActOnCacheVar(VarExpr);

  Expr *CurVarExpr = VarExpr->IgnoreParenImpCasts();

  // 'use_device' doesn't allow array subscript or array sections.
  // OpenACC3.3 2.8:
  // A 'var' in a 'use_device' clause must be the name of a variable or array.
  // OpenACC3.3 2.13:
  // A 'var' in a 'declare' directive must be a variable or array name.
  if ((CK == OpenACCClauseKind::UseDevice ||
       DK == OpenACCDirectiveKind::Declare) &&
      isa<ArraySectionExpr, ArraySubscriptExpr>(CurVarExpr)) {
    Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref_use_device_declare)
        << (DK == OpenACCDirectiveKind::Declare);
    return ExprError();
  }

  // Sub-arrays/subscript-exprs are fine as long as the base is a
  // VarExpr/MemberExpr. So strip all of those off.
  while (isa<ArraySectionExpr, ArraySubscriptExpr>(CurVarExpr)) {
    if (auto *SubScrpt = dyn_cast<ArraySubscriptExpr>(CurVarExpr))
      CurVarExpr = SubScrpt->getBase()->IgnoreParenImpCasts();
    else
      CurVarExpr =
          cast<ArraySectionExpr>(CurVarExpr)->getBase()->IgnoreParenImpCasts();
  }

  // References to a VarDecl are fine.
  if (const auto *DRE = dyn_cast<DeclRefExpr>(CurVarExpr)) {
    if (isa<VarDecl, NonTypeTemplateParmDecl>(
            DRE->getFoundDecl()->getCanonicalDecl()))
      return VarExpr;
  }

  // If CK is a Reduction, this special cases for OpenACC3.3 2.5.15: "A var in a
  // reduction clause must be a scalar variable name, an aggregate variable
  // name, an array element, or a subarray.
  // If CK is a 'use_device', this also isn't valid, as it isn't the name of a
  // variable or array, if not done as a member expr.
  // A MemberExpr that references a Field is valid for other clauses.
  if (const auto *ME = dyn_cast<MemberExpr>(CurVarExpr)) {
    if (isa<FieldDecl>(ME->getMemberDecl()->getCanonicalDecl())) {
      if (DK == OpenACCDirectiveKind::Declare ||
          CK == OpenACCClauseKind::Reduction ||
          CK == OpenACCClauseKind::UseDevice) {

        // We can allow 'member expr' if the 'this' is implicit in the case of
        // declare, reduction, and use_device.
        const auto *This = dyn_cast<CXXThisExpr>(ME->getBase());
        if (This && This->isImplicit())
          return VarExpr;
      } else {
        return VarExpr;
      }
    }
  }

  // Referring to 'this' is ok for the most part, but for 'use_device'/'declare'
  // doesn't fall into 'variable or array name'
  if (CK != OpenACCClauseKind::UseDevice &&
      DK != OpenACCDirectiveKind::Declare && isa<CXXThisExpr>(CurVarExpr))
    return VarExpr;

  // Nothing really we can do here, as these are dependent.  So just return they
  // are valid.
  if (isa<DependentScopeDeclRefExpr>(CurVarExpr) ||
      (CK != OpenACCClauseKind::Reduction &&
       isa<CXXDependentScopeMemberExpr>(CurVarExpr)))
    return VarExpr;

  // There isn't really anything we can do in the case of a recovery expr, so
  // skip the diagnostic rather than produce a confusing diagnostic.
  if (isa<RecoveryExpr>(CurVarExpr))
    return ExprError();

  if (DK == OpenACCDirectiveKind::Declare)
    Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref_use_device_declare)
        << /*declare*/ 1;
  else if (CK == OpenACCClauseKind::UseDevice)
    Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref_use_device_declare)
        << /*use_device*/ 0;
  else
    Diag(VarExpr->getExprLoc(), diag::err_acc_not_a_var_ref)
        << (CK != OpenACCClauseKind::Reduction);
  return ExprError();
}

ExprResult SemaOpenACC::ActOnArraySectionExpr(Expr *Base, SourceLocation LBLoc,
                                              Expr *LowerBound,
                                              SourceLocation ColonLoc,
                                              Expr *Length,
                                              SourceLocation RBLoc) {
  ASTContext &Context = getASTContext();

  // Handle placeholders.
  if (Base->hasPlaceholderType() &&
      !Base->hasPlaceholderType(BuiltinType::ArraySection)) {
    ExprResult Result = SemaRef.CheckPlaceholderExpr(Base);
    if (Result.isInvalid())
      return ExprError();
    Base = Result.get();
  }
  if (LowerBound && LowerBound->getType()->isNonOverloadPlaceholderType()) {
    ExprResult Result = SemaRef.CheckPlaceholderExpr(LowerBound);
    if (Result.isInvalid())
      return ExprError();
    Result = SemaRef.DefaultLvalueConversion(Result.get());
    if (Result.isInvalid())
      return ExprError();
    LowerBound = Result.get();
  }
  if (Length && Length->getType()->isNonOverloadPlaceholderType()) {
    ExprResult Result = SemaRef.CheckPlaceholderExpr(Length);
    if (Result.isInvalid())
      return ExprError();
    Result = SemaRef.DefaultLvalueConversion(Result.get());
    if (Result.isInvalid())
      return ExprError();
    Length = Result.get();
  }

  // Check the 'base' value, it must be an array or pointer type, and not to/of
  // a function type.
  QualType OriginalBaseTy = ArraySectionExpr::getBaseOriginalType(Base);
  QualType ResultTy;
  if (!Base->isTypeDependent()) {
    if (OriginalBaseTy->isAnyPointerType()) {
      ResultTy = OriginalBaseTy->getPointeeType();
    } else if (OriginalBaseTy->isArrayType()) {
      ResultTy = OriginalBaseTy->getAsArrayTypeUnsafe()->getElementType();
    } else {
      return ExprError(
          Diag(Base->getExprLoc(), diag::err_acc_typecheck_subarray_value)
          << Base->getSourceRange());
    }

    if (ResultTy->isFunctionType()) {
      Diag(Base->getExprLoc(), diag::err_acc_subarray_function_type)
          << ResultTy << Base->getSourceRange();
      return ExprError();
    }

    if (SemaRef.RequireCompleteType(Base->getExprLoc(), ResultTy,
                                    diag::err_acc_subarray_incomplete_type,
                                    Base))
      return ExprError();

    if (!Base->hasPlaceholderType(BuiltinType::ArraySection)) {
      ExprResult Result = SemaRef.DefaultFunctionArrayLvalueConversion(Base);
      if (Result.isInvalid())
        return ExprError();
      Base = Result.get();
    }
  }

  auto GetRecovery = [&](Expr *E, QualType Ty) {
    ExprResult Recovery =
        SemaRef.CreateRecoveryExpr(E->getBeginLoc(), E->getEndLoc(), E, Ty);
    return Recovery.isUsable() ? Recovery.get() : nullptr;
  };

  // Ensure both of the expressions are int-exprs.
  if (LowerBound && !LowerBound->isTypeDependent()) {
    ExprResult LBRes =
        ActOnIntExpr(OpenACCDirectiveKind::Invalid, OpenACCClauseKind::Invalid,
                     LowerBound->getExprLoc(), LowerBound);

    if (LBRes.isUsable())
      LBRes = SemaRef.DefaultLvalueConversion(LBRes.get());
    LowerBound =
        LBRes.isUsable() ? LBRes.get() : GetRecovery(LowerBound, Context.IntTy);
  }

  if (Length && !Length->isTypeDependent()) {
    ExprResult LenRes =
        ActOnIntExpr(OpenACCDirectiveKind::Invalid, OpenACCClauseKind::Invalid,
                     Length->getExprLoc(), Length);

    if (LenRes.isUsable())
      LenRes = SemaRef.DefaultLvalueConversion(LenRes.get());
    Length =
        LenRes.isUsable() ? LenRes.get() : GetRecovery(Length, Context.IntTy);
  }

  // Length is required if the base type is not an array of known bounds.
  if (!Length && (OriginalBaseTy.isNull() ||
                  (!OriginalBaseTy->isDependentType() &&
                   !OriginalBaseTy->isConstantArrayType() &&
                   !OriginalBaseTy->isDependentSizedArrayType()))) {
    bool IsArray = !OriginalBaseTy.isNull() && OriginalBaseTy->isArrayType();
    Diag(ColonLoc, diag::err_acc_subarray_no_length) << IsArray;
    // Fill in a dummy 'length' so that when we instantiate this we don't
    // double-diagnose here.
    ExprResult Recovery = SemaRef.CreateRecoveryExpr(
        ColonLoc, SourceLocation(), ArrayRef<Expr *>(), Context.IntTy);
    Length = Recovery.isUsable() ? Recovery.get() : nullptr;
  }

  // Check the values of each of the arguments, they cannot be negative(we
  // assume), and if the array bound is known, must be within range. As we do
  // so, do our best to continue with evaluation, we can set the
  // value/expression to nullptr/nullopt if they are invalid, and treat them as
  // not present for the rest of evaluation.

  // We don't have to check for dependence, because the dependent size is
  // represented as a different AST node.
  std::optional<llvm::APSInt> BaseSize;
  if (!OriginalBaseTy.isNull() && OriginalBaseTy->isConstantArrayType()) {
    const auto *ArrayTy = Context.getAsConstantArrayType(OriginalBaseTy);
    BaseSize = ArrayTy->getSize();
  }

  auto GetBoundValue = [&](Expr *E) -> std::optional<llvm::APSInt> {
    if (!E || E->isInstantiationDependent())
      return std::nullopt;

    Expr::EvalResult Res;
    if (!E->EvaluateAsInt(Res, Context))
      return std::nullopt;
    return Res.Val.getInt();
  };

  std::optional<llvm::APSInt> LowerBoundValue = GetBoundValue(LowerBound);
  std::optional<llvm::APSInt> LengthValue = GetBoundValue(Length);

  // Check lower bound for negative or out of range.
  if (LowerBoundValue.has_value()) {
    if (LowerBoundValue->isNegative()) {
      Diag(LowerBound->getExprLoc(), diag::err_acc_subarray_negative)
          << /*LowerBound=*/0 << toString(*LowerBoundValue, /*Radix=*/10);
      LowerBoundValue.reset();
      LowerBound = GetRecovery(LowerBound, LowerBound->getType());
    } else if (BaseSize.has_value() &&
               llvm::APSInt::compareValues(*LowerBoundValue, *BaseSize) >= 0) {
      // Lower bound (start index) must be less than the size of the array.
      Diag(LowerBound->getExprLoc(), diag::err_acc_subarray_out_of_range)
          << /*LowerBound=*/0 << toString(*LowerBoundValue, /*Radix=*/10)
          << toString(*BaseSize, /*Radix=*/10);
      LowerBoundValue.reset();
      LowerBound = GetRecovery(LowerBound, LowerBound->getType());
    }
  }

  // Check length for negative or out of range.
  if (LengthValue.has_value()) {
    if (LengthValue->isNegative()) {
      Diag(Length->getExprLoc(), diag::err_acc_subarray_negative)
          << /*Length=*/1 << toString(*LengthValue, /*Radix=*/10);
      LengthValue.reset();
      Length = GetRecovery(Length, Length->getType());
    } else if (BaseSize.has_value() &&
               llvm::APSInt::compareValues(*LengthValue, *BaseSize) > 0) {
      // Length must be lessthan or EQUAL to the size of the array.
      Diag(Length->getExprLoc(), diag::err_acc_subarray_out_of_range)
          << /*Length=*/1 << toString(*LengthValue, /*Radix=*/10)
          << toString(*BaseSize, /*Radix=*/10);
      LengthValue.reset();
      Length = GetRecovery(Length, Length->getType());
    }
  }

  // Adding two APSInts requires matching sign, so extract that here.
  auto AddAPSInt = [](llvm::APSInt LHS, llvm::APSInt RHS) -> llvm::APSInt {
    if (LHS.isSigned() == RHS.isSigned())
      return LHS + RHS;

    unsigned Width = std::max(LHS.getBitWidth(), RHS.getBitWidth()) + 1;
    return llvm::APSInt(LHS.sext(Width) + RHS.sext(Width), /*Signed=*/true);
  };

  // If we know all 3 values, we can diagnose that the total value would be out
  // of range.
  if (BaseSize.has_value() && LowerBoundValue.has_value() &&
      LengthValue.has_value() &&
      llvm::APSInt::compareValues(AddAPSInt(*LowerBoundValue, *LengthValue),
                                  *BaseSize) > 0) {
    Diag(Base->getExprLoc(),
         diag::err_acc_subarray_base_plus_length_out_of_range)
        << toString(*LowerBoundValue, /*Radix=*/10)
        << toString(*LengthValue, /*Radix=*/10)
        << toString(*BaseSize, /*Radix=*/10);

    LowerBoundValue.reset();
    LowerBound = GetRecovery(LowerBound, LowerBound->getType());
    LengthValue.reset();
    Length = GetRecovery(Length, Length->getType());
  }

  // If any part of the expression is dependent, return a dependent sub-array.
  QualType ArrayExprTy = Context.ArraySectionTy;
  if (Base->isTypeDependent() ||
      (LowerBound && LowerBound->isInstantiationDependent()) ||
      (Length && Length->isInstantiationDependent()))
    ArrayExprTy = Context.DependentTy;

  return new (Context)
      ArraySectionExpr(Base, LowerBound, Length, ArrayExprTy, VK_LValue,
                       OK_Ordinary, ColonLoc, RBLoc);
}

void SemaOpenACC::ActOnWhileStmt(SourceLocation WhileLoc) {
  if (!getLangOpts().OpenACC)
    return;

  if (!LoopInfo.TopLevelLoopSeen)
    return;

  if (CollapseInfo.CurCollapseCount && *CollapseInfo.CurCollapseCount > 0) {
    Diag(WhileLoc, diag::err_acc_invalid_in_loop)
        << /*while loop*/ 1 << CollapseInfo.DirectiveKind
        << OpenACCClauseKind::Collapse;
    assert(CollapseInfo.ActiveCollapse && "Collapse count without object?");
    Diag(CollapseInfo.ActiveCollapse->getBeginLoc(),
         diag::note_acc_active_clause_here)
        << OpenACCClauseKind::Collapse;

    // Remove the value so that we don't get cascading errors in the body. The
    // caller RAII object will restore this.
    CollapseInfo.CurCollapseCount = std::nullopt;
  }

  if (TileInfo.CurTileCount && *TileInfo.CurTileCount > 0) {
    Diag(WhileLoc, diag::err_acc_invalid_in_loop)
        << /*while loop*/ 1 << TileInfo.DirectiveKind
        << OpenACCClauseKind::Tile;
    assert(TileInfo.ActiveTile && "tile count without object?");
    Diag(TileInfo.ActiveTile->getBeginLoc(), diag::note_acc_active_clause_here)
        << OpenACCClauseKind::Tile;

    // Remove the value so that we don't get cascading errors in the body. The
    // caller RAII object will restore this.
    TileInfo.CurTileCount = std::nullopt;
  }
}

void SemaOpenACC::ActOnDoStmt(SourceLocation DoLoc) {
  if (!getLangOpts().OpenACC)
    return;

  if (!LoopInfo.TopLevelLoopSeen)
    return;

  if (CollapseInfo.CurCollapseCount && *CollapseInfo.CurCollapseCount > 0) {
    Diag(DoLoc, diag::err_acc_invalid_in_loop)
        << /*do loop*/ 2 << CollapseInfo.DirectiveKind
        << OpenACCClauseKind::Collapse;
    assert(CollapseInfo.ActiveCollapse && "Collapse count without object?");
    Diag(CollapseInfo.ActiveCollapse->getBeginLoc(),
         diag::note_acc_active_clause_here)
        << OpenACCClauseKind::Collapse;

    // Remove the value so that we don't get cascading errors in the body. The
    // caller RAII object will restore this.
    CollapseInfo.CurCollapseCount = std::nullopt;
  }

  if (TileInfo.CurTileCount && *TileInfo.CurTileCount > 0) {
    Diag(DoLoc, diag::err_acc_invalid_in_loop)
        << /*do loop*/ 2 << TileInfo.DirectiveKind << OpenACCClauseKind::Tile;
    assert(TileInfo.ActiveTile && "tile count without object?");
    Diag(TileInfo.ActiveTile->getBeginLoc(), diag::note_acc_active_clause_here)
        << OpenACCClauseKind::Tile;

    // Remove the value so that we don't get cascading errors in the body. The
    // caller RAII object will restore this.
    TileInfo.CurTileCount = std::nullopt;
  }
}

void SemaOpenACC::ForStmtBeginHelper(SourceLocation ForLoc,
                                     ForStmtBeginChecker &C) {
  assert(getLangOpts().OpenACC && "Check enabled when not OpenACC?");

  // Enable the while/do-while checking.
  LoopInfo.TopLevelLoopSeen = true;

  if (CollapseInfo.CurCollapseCount && *CollapseInfo.CurCollapseCount > 0) {
    C.check();

    // OpenACC 3.3 2.9.1:
    // Each associated loop, except the innermost, must contain exactly one loop
    // or loop nest.
    // This checks for more than 1 loop at the current level, the
    // 'depth'-satisifed checking manages the 'not zero' case.
    if (LoopInfo.CurLevelHasLoopAlready) {
      Diag(ForLoc, diag::err_acc_clause_multiple_loops)
          << CollapseInfo.DirectiveKind << OpenACCClauseKind::Collapse;
      assert(CollapseInfo.ActiveCollapse && "No collapse object?");
      Diag(CollapseInfo.ActiveCollapse->getBeginLoc(),
           diag::note_acc_active_clause_here)
          << OpenACCClauseKind::Collapse;
    } else {
      --(*CollapseInfo.CurCollapseCount);

      // Once we've hit zero here, we know we have deep enough 'for' loops to
      // get to the bottom.
      if (*CollapseInfo.CurCollapseCount == 0)
        CollapseInfo.CollapseDepthSatisfied = true;
    }
  }

  if (TileInfo.CurTileCount && *TileInfo.CurTileCount > 0) {
    C.check();

    if (LoopInfo.CurLevelHasLoopAlready) {
      Diag(ForLoc, diag::err_acc_clause_multiple_loops)
          << TileInfo.DirectiveKind << OpenACCClauseKind::Tile;
      assert(TileInfo.ActiveTile && "No tile object?");
      Diag(TileInfo.ActiveTile->getBeginLoc(),
           diag::note_acc_active_clause_here)
          << OpenACCClauseKind::Tile;
    } else {
      TileInfo.CurTileCount = *TileInfo.CurTileCount - 1;
      // Once we've hit zero here, we know we have deep enough 'for' loops to
      // get to the bottom.
      if (*TileInfo.CurTileCount == 0)
        TileInfo.TileDepthSatisfied = true;
    }
  }

  // Set this to 'false' for the body of this loop, so that the next level
  // checks independently.
  LoopInfo.CurLevelHasLoopAlready = false;
}

namespace {
bool isValidLoopVariableType(QualType LoopVarTy) {
  // Just skip if it is dependent, it could be any of the below.
  if (LoopVarTy->isDependentType())
    return true;

  // The loop variable must be of integer,
  if (LoopVarTy->isIntegerType())
    return true;

  // C/C++ pointer,
  if (LoopVarTy->isPointerType())
    return true;

  // or C++ random-access iterator type.
  if (const auto *RD = LoopVarTy->getAsCXXRecordDecl()) {
    // Note: Only do CXXRecordDecl because RecordDecl can't be a random access
    // iterator type!

    // We could either do a lot of work to see if this matches
    // random-access-iterator, but it seems that just checking that the
    // 'iterator_category' typedef is more than sufficient. If programmers are
    // willing to lie about this, we can let them.

    for (const auto *TD :
         llvm::make_filter_range(RD->decls(), llvm::IsaPred<TypedefNameDecl>)) {
      const auto *TDND = cast<TypedefNameDecl>(TD)->getCanonicalDecl();

      if (TDND->getName() != "iterator_category")
        continue;

      // If there is no type for this decl, return false.
      if (TDND->getUnderlyingType().isNull())
        return false;

      const CXXRecordDecl *ItrCategoryDecl =
          TDND->getUnderlyingType()->getAsCXXRecordDecl();

      // If the category isn't a record decl, it isn't the tag type.
      if (!ItrCategoryDecl)
        return false;

      auto IsRandomAccessIteratorTag = [](const CXXRecordDecl *RD) {
        if (RD->getName() != "random_access_iterator_tag")
          return false;
        // Checks just for std::random_access_iterator_tag.
        return RD->getEnclosingNamespaceContext()->isStdNamespace();
      };

      if (IsRandomAccessIteratorTag(ItrCategoryDecl))
        return true;

      // We can also support types inherited from the
      // random_access_iterator_tag.
      for (CXXBaseSpecifier BS : ItrCategoryDecl->bases()) {

        if (IsRandomAccessIteratorTag(BS.getType()->getAsCXXRecordDecl()))
          return true;
      }

      return false;
    }
  }

  return false;
}

} // namespace

void SemaOpenACC::ForStmtBeginChecker::check() {
  if (SemaRef.LoopWithoutSeqInfo.Kind == OpenACCDirectiveKind::Invalid)
    return;

  if (AlreadyChecked)
    return;
  AlreadyChecked = true;

  // OpenACC3.3 2.1:
  // A loop associated with a loop construct that does not have a seq clause
  // must be written to meet all the following conditions:
  // - The loop variable must be of integer, C/C++ pointer, or C++ random-access
  // iterator type.
  // - The loop variable must monotonically increase or decrease in the
  // direction of its termination condition.
  // - The loop trip count must be computable in constant time when entering the
  // loop construct.
  //
  // For a C++ range-based for loop, the loop variable
  // identified by the above conditions is the internal iterator, such as a
  // pointer, that the compiler generates to iterate the range.  it is not the
  // variable declared by the for loop.

  if (IsRangeFor) {
    // If the range-for is being instantiated and didn't change, don't
    // re-diagnose.
    if (!RangeFor.has_value())
      return;
    // For a range-for, we can assume everything is 'corect' other than the type
    // of the iterator, so check that.
    const DeclStmt *RangeStmt = (*RangeFor)->getBeginStmt();

    // In some dependent contexts, the autogenerated range statement doesn't get
    // included until instantiation, so skip for now.
    if (!RangeStmt)
      return;

    const ValueDecl *InitVar = cast<ValueDecl>(RangeStmt->getSingleDecl());
    QualType VarType = InitVar->getType().getNonReferenceType();
    if (!isValidLoopVariableType(VarType)) {
      SemaRef.Diag(InitVar->getBeginLoc(), diag::err_acc_loop_variable_type)
          << SemaRef.LoopWithoutSeqInfo.Kind << VarType;
      SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc,
                   diag::note_acc_construct_here)
          << SemaRef.LoopWithoutSeqInfo.Kind;
    }
    return;
  }

  // Else we are in normal 'ForStmt', so we can diagnose everything.
  // We only have to check cond/inc if they have changed, but 'init' needs to
  // just suppress its diagnostics if it hasn't changed.
  const ValueDecl *InitVar = checkInit();
  if (Cond.has_value())
    checkCond();
  if (Inc.has_value())
    checkInc(InitVar);
}
const ValueDecl *SemaOpenACC::ForStmtBeginChecker::checkInit() {
  if (!Init) {
    if (InitChanged) {
      SemaRef.Diag(ForLoc, diag::err_acc_loop_variable)
          << SemaRef.LoopWithoutSeqInfo.Kind;
      SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc,
                   diag::note_acc_construct_here)
          << SemaRef.LoopWithoutSeqInfo.Kind;
    }
    return nullptr;
  }

  auto DiagLoopVar = [&]() {
    if (InitChanged) {
      SemaRef.Diag(Init->getBeginLoc(), diag::err_acc_loop_variable)
          << SemaRef.LoopWithoutSeqInfo.Kind;
      SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc,
                   diag::note_acc_construct_here)
          << SemaRef.LoopWithoutSeqInfo.Kind;
    }
    return nullptr;
  };

  if (const auto *ExprTemp = dyn_cast<ExprWithCleanups>(Init))
    Init = ExprTemp->getSubExpr();
  if (const auto *E = dyn_cast<Expr>(Init))
    Init = E->IgnoreParenImpCasts();

  const ValueDecl *InitVar = nullptr;

  if (const auto *BO = dyn_cast<BinaryOperator>(Init)) {
    // Allow assignment operator here.

    if (!BO->isAssignmentOp())
      return DiagLoopVar();

    const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();

    if (const auto *DRE = dyn_cast<DeclRefExpr>(LHS))
      InitVar = DRE->getDecl();
  } else if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
    // Allow T t = <whatever>
    if (!DS->isSingleDecl())
      return DiagLoopVar();

    InitVar = dyn_cast<ValueDecl>(DS->getSingleDecl());

    // Ensure we have an initializer, unless this is a record/dependent type.

    if (InitVar) {
      if (!isa<VarDecl>(InitVar))
        return DiagLoopVar();

      if (!InitVar->getType()->isRecordType() &&
          !InitVar->getType()->isDependentType() &&
          !cast<VarDecl>(InitVar)->hasInit())
        return DiagLoopVar();
    }
  } else if (auto *CE = dyn_cast<CXXOperatorCallExpr>(Init)) {
    // Allow assignment operator call.
    if (CE->getOperator() != OO_Equal)
      return DiagLoopVar();

    const Expr *LHS = CE->getArg(0)->IgnoreParenImpCasts();

    if (auto *DRE = dyn_cast<DeclRefExpr>(LHS)) {
      InitVar = DRE->getDecl();
    } else if (auto *ME = dyn_cast<MemberExpr>(LHS)) {
      if (isa<CXXThisExpr>(ME->getBase()->IgnoreParenImpCasts()))
        InitVar = ME->getMemberDecl();
    }
  }

  if (!InitVar)
    return DiagLoopVar();

  InitVar = cast<ValueDecl>(InitVar->getCanonicalDecl());
  QualType VarType = InitVar->getType().getNonReferenceType();

  // Since we have one, all we need to do is ensure it is the right type.
  if (!isValidLoopVariableType(VarType)) {
    if (InitChanged) {
      SemaRef.Diag(InitVar->getBeginLoc(), diag::err_acc_loop_variable_type)
          << SemaRef.LoopWithoutSeqInfo.Kind << VarType;
      SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc,
                   diag::note_acc_construct_here)
          << SemaRef.LoopWithoutSeqInfo.Kind;
    }
    return nullptr;
  }

  return InitVar;
}
void SemaOpenACC::ForStmtBeginChecker::checkCond() {
  if (!*Cond) {
    SemaRef.Diag(ForLoc, diag::err_acc_loop_terminating_condition)
        << SemaRef.LoopWithoutSeqInfo.Kind;
    SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc, diag::note_acc_construct_here)
        << SemaRef.LoopWithoutSeqInfo.Kind;
  }
  // Nothing else to do here.  we could probably do some additional work to look
  // into the termination condition, but that error-prone.  For now, we don't
  // implement anything other than 'there is a termination condition', and if
  // codegen/MLIR comes up with some necessary restrictions, we can implement
  // them here.
}

void SemaOpenACC::ForStmtBeginChecker::checkInc(const ValueDecl *Init) {

  if (!*Inc) {
    SemaRef.Diag(ForLoc, diag::err_acc_loop_not_monotonic)
        << SemaRef.LoopWithoutSeqInfo.Kind;
    SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc, diag::note_acc_construct_here)
        << SemaRef.LoopWithoutSeqInfo.Kind;
    return;
  }
  auto DiagIncVar = [this] {
    SemaRef.Diag((*Inc)->getBeginLoc(), diag::err_acc_loop_not_monotonic)
        << SemaRef.LoopWithoutSeqInfo.Kind;
    SemaRef.Diag(SemaRef.LoopWithoutSeqInfo.Loc, diag::note_acc_construct_here)
        << SemaRef.LoopWithoutSeqInfo.Kind;
    return;
  };

  if (const auto *ExprTemp = dyn_cast<ExprWithCleanups>(*Inc))
    Inc = ExprTemp->getSubExpr();
  if (const auto *E = dyn_cast<Expr>(*Inc))
    Inc = E->IgnoreParenImpCasts();

  auto getDeclFromExpr = [](const Expr *E) -> const ValueDecl * {
    E = E->IgnoreParenImpCasts();
    if (const auto *FE = dyn_cast<FullExpr>(E))
      E = FE->getSubExpr();

    E = E->IgnoreParenImpCasts();

    if (!E)
      return nullptr;
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      return dyn_cast<ValueDecl>(DRE->getDecl());

    if (const auto *ME = dyn_cast<MemberExpr>(E))
      if (isa<CXXThisExpr>(ME->getBase()->IgnoreParenImpCasts()))
        return ME->getMemberDecl();

    return nullptr;
  };

  const ValueDecl *IncVar = nullptr;

  // Here we enforce the monotonically increase/decrease:
  if (const auto *UO = dyn_cast<UnaryOperator>(*Inc)) {
    // Allow increment/decrement ops.
    if (!UO->isIncrementDecrementOp())
      return DiagIncVar();
    IncVar = getDeclFromExpr(UO->getSubExpr());
  } else if (const auto *BO = dyn_cast<BinaryOperator>(*Inc)) {
    switch (BO->getOpcode()) {
    default:
      return DiagIncVar();
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_Assign:
      // += -= *= /= should all be fine here, this should be all of the
      // 'monotonical' compound-assign ops.
      // Assignment we just give up on, we could do better, and ensure that it
      // is a binary/operator expr doing more work, but that seems like a lot
      // of work for an error prone check.
      break;
    }
    IncVar = getDeclFromExpr(BO->getLHS());
  } else if (const auto *CE = dyn_cast<CXXOperatorCallExpr>(*Inc)) {
    switch (CE->getOperator()) {
    default:
      return DiagIncVar();
    case OO_PlusPlus:
    case OO_MinusMinus:
    case OO_PlusEqual:
    case OO_MinusEqual:
    case OO_StarEqual:
    case OO_SlashEqual:
    case OO_Equal:
      // += -= *= /= should all be fine here, this should be all of the
      // 'monotonical' compound-assign ops.
      // Assignment we just give up on, we could do better, and ensure that it
      // is a binary/operator expr doing more work, but that seems like a lot
      // of work for an error prone check.
      break;
    }

    IncVar = getDeclFromExpr(CE->getArg(0));

  } else if (const auto *ME = dyn_cast<CXXMemberCallExpr>(*Inc)) {
    IncVar = getDeclFromExpr(ME->getImplicitObjectArgument());
    // We can't really do much for member expressions, other than hope they are
    // doing the right thing, so give up here.
  }

  if (!IncVar)
    return DiagIncVar();

  // InitVar shouldn't be null unless there was an error, so don't diagnose if
  // that is the case. Else we should ensure that it refers to the  loop
  // value.
  if (Init && IncVar->getCanonicalDecl() != Init->getCanonicalDecl())
    return DiagIncVar();

  return;
}

void SemaOpenACC::ActOnForStmtBegin(SourceLocation ForLoc, const Stmt *OldFirst,
                                    const Stmt *First, const Stmt *OldSecond,
                                    const Stmt *Second, const Stmt *OldThird,
                                    const Stmt *Third) {
  if (!getLangOpts().OpenACC)
    return;

  std::optional<const Stmt *> S;
  if (OldSecond == Second)
    S = std::nullopt;
  else
    S = Second;
  std::optional<const Stmt *> T;
  if (OldThird == Third)
    S = std::nullopt;
  else
    S = Third;

  bool InitChanged = false;
  if (OldFirst != First) {
    InitChanged = true;

    // VarDecls are always rebuild because they are dependent, so we can do a
    // little work to suppress some of the double checking based on whether the
    // type is instantiation dependent.
    QualType OldVDTy;
    QualType NewVDTy;
    if (const auto *DS = dyn_cast<DeclStmt>(OldFirst))
      if (const VarDecl *VD = dyn_cast_if_present<VarDecl>(
              DS->isSingleDecl() ? DS->getSingleDecl() : nullptr))
        OldVDTy = VD->getType();
    if (const auto *DS = dyn_cast<DeclStmt>(First))
      if (const VarDecl *VD = dyn_cast_if_present<VarDecl>(
              DS->isSingleDecl() ? DS->getSingleDecl() : nullptr))
        NewVDTy = VD->getType();

    if (!OldVDTy.isNull() && !NewVDTy.isNull())
      InitChanged = OldVDTy->isInstantiationDependentType() !=
                    NewVDTy->isInstantiationDependentType();
  }

  ForStmtBeginChecker FSBC{*this, ForLoc, First, InitChanged, S, T};
  if (!LoopInfo.TopLevelLoopSeen) {
    FSBC.check();
  }

  ForStmtBeginHelper(ForLoc, FSBC);
}

void SemaOpenACC::ActOnForStmtBegin(SourceLocation ForLoc, const Stmt *First,
                                    const Stmt *Second, const Stmt *Third) {
  if (!getLangOpts().OpenACC)
    return;

  ForStmtBeginChecker FSBC{*this,  ForLoc, First, /*InitChanged=*/true,
                           Second, Third};
  if (!LoopInfo.TopLevelLoopSeen) {
    FSBC.check();
  }

  ForStmtBeginHelper(ForLoc, FSBC);
}

void SemaOpenACC::ActOnRangeForStmtBegin(SourceLocation ForLoc,
                                         const Stmt *OldRangeFor,
                                         const Stmt *RangeFor) {
  if (!getLangOpts().OpenACC)
    return;

  std::optional<const CXXForRangeStmt *> RF;

  if (OldRangeFor == RangeFor)
    RF = std::nullopt;
  else
    RF = cast<CXXForRangeStmt>(RangeFor);

  ForStmtBeginChecker FSBC{*this, ForLoc, RF};
  if (!LoopInfo.TopLevelLoopSeen) {
    FSBC.check();
  }
  ForStmtBeginHelper(ForLoc, FSBC);
}

void SemaOpenACC::ActOnRangeForStmtBegin(SourceLocation ForLoc,
                                         const Stmt *RangeFor) {
  if (!getLangOpts().OpenACC)
    return;

  ForStmtBeginChecker FSBC{*this, ForLoc, cast<CXXForRangeStmt>(RangeFor)};
  if (!LoopInfo.TopLevelLoopSeen) {
    FSBC.check();
  }
  ForStmtBeginHelper(ForLoc, FSBC);
}

namespace {
SourceLocation FindInterveningCodeInLoop(const Stmt *CurStmt) {
  // We should diagnose on anything except `CompoundStmt`, `NullStmt`,
  // `ForStmt`, `CXXForRangeStmt`, since those are legal, and `WhileStmt` and
  // `DoStmt`, as those are caught as a violation elsewhere.
  // For `CompoundStmt` we need to search inside of it.
  if (!CurStmt ||
      isa<ForStmt, NullStmt, ForStmt, CXXForRangeStmt, WhileStmt, DoStmt>(
          CurStmt))
    return SourceLocation{};

  // Any other construct is an error anyway, so it has already been diagnosed.
  if (isa<OpenACCConstructStmt>(CurStmt))
    return SourceLocation{};

  // Search inside the compound statement, this allows for arbitrary nesting
  // of compound statements, as long as there isn't any code inside.
  if (const auto *CS = dyn_cast<CompoundStmt>(CurStmt)) {
    for (const auto *ChildStmt : CS->children()) {
      SourceLocation ChildStmtLoc = FindInterveningCodeInLoop(ChildStmt);
      if (ChildStmtLoc.isValid())
        return ChildStmtLoc;
    }
    // Empty/not invalid compound statements are legal.
    return SourceLocation{};
  }
  return CurStmt->getBeginLoc();
}
} // namespace

void SemaOpenACC::ActOnForStmtEnd(SourceLocation ForLoc, StmtResult Body) {
  if (!getLangOpts().OpenACC)
    return;

  // Set this to 'true' so if we find another one at this level we can diagnose.
  LoopInfo.CurLevelHasLoopAlready = true;

  if (!Body.isUsable())
    return;

  bool IsActiveCollapse = CollapseInfo.CurCollapseCount &&
                          *CollapseInfo.CurCollapseCount > 0 &&
                          !CollapseInfo.ActiveCollapse->hasForce();
  bool IsActiveTile = TileInfo.CurTileCount && *TileInfo.CurTileCount > 0;

  if (IsActiveCollapse || IsActiveTile) {
    SourceLocation OtherStmtLoc = FindInterveningCodeInLoop(Body.get());

    if (OtherStmtLoc.isValid() && IsActiveCollapse) {
      Diag(OtherStmtLoc, diag::err_acc_intervening_code)
          << OpenACCClauseKind::Collapse << CollapseInfo.DirectiveKind;
      Diag(CollapseInfo.ActiveCollapse->getBeginLoc(),
           diag::note_acc_active_clause_here)
          << OpenACCClauseKind::Collapse;
    }

    if (OtherStmtLoc.isValid() && IsActiveTile) {
      Diag(OtherStmtLoc, diag::err_acc_intervening_code)
          << OpenACCClauseKind::Tile << TileInfo.DirectiveKind;
      Diag(TileInfo.ActiveTile->getBeginLoc(),
           diag::note_acc_active_clause_here)
          << OpenACCClauseKind::Tile;
    }
  }
}

namespace {
// Get a list of clause Kinds for diagnosing a list, joined by a commas and an
// 'or'.
std::string GetListOfClauses(llvm::ArrayRef<OpenACCClauseKind> Clauses) {
  assert(!Clauses.empty() && "empty clause list not supported");

  std::string Output;
  llvm::raw_string_ostream OS{Output};

  if (Clauses.size() == 1) {
    OS << '\'' << Clauses[0] << '\'';
    return Output;
  }

  llvm::ArrayRef<OpenACCClauseKind> AllButLast{Clauses.begin(),
                                               Clauses.end() - 1};

  llvm::interleave(
      AllButLast, [&](OpenACCClauseKind K) { OS << '\'' << K << '\''; },
      [&] { OS << ", "; });

  OS << " or \'" << Clauses.back() << '\'';
  return Output;
}

// Helper that should mirror ActOnRoutineName to get the FunctionDecl out for
// magic-static checking.
FunctionDecl *getFunctionFromRoutineName(Expr *RoutineName) {
  if (!RoutineName)
    return nullptr;
  RoutineName = RoutineName->IgnoreParenImpCasts();
  if (isa<RecoveryExpr>(RoutineName)) {
    // There is nothing we can do here, this isn't a function we can count on.
    return nullptr;
  } else if (isa<DependentScopeDeclRefExpr, CXXDependentScopeMemberExpr>(
                 RoutineName)) {
    // The lookup is dependent, so we'll have to figure this out later.
    return nullptr;
  } else if (auto *DRE = dyn_cast<DeclRefExpr>(RoutineName)) {
    ValueDecl *VD = DRE->getDecl();

    if (auto *FD = dyn_cast<FunctionDecl>(VD))
      return FD;

    // Allow lambdas.
    if (auto *VarD = dyn_cast<VarDecl>(VD)) {
      QualType VarDTy = VarD->getType();
      if (!VarDTy.isNull()) {
        if (auto *RD = VarDTy->getAsCXXRecordDecl()) {
          if (RD->isGenericLambda())
            return nullptr;
          if (RD->isLambda())
            return RD->getLambdaCallOperator();
        } else if (VarDTy->isDependentType()) {
          // We don't really know what this is going to be.
          return nullptr;
        }
      }
      return nullptr;
    } else if (isa<OverloadExpr>(RoutineName)) {
      return nullptr;
    }
  }
  return nullptr;
}
} // namespace

ExprResult SemaOpenACC::ActOnRoutineName(Expr *RoutineName) {
  assert(RoutineName && "Routine name cannot be null here");
  RoutineName = RoutineName->IgnoreParenImpCasts();

  if (isa<RecoveryExpr>(RoutineName)) {
    // This has already been diagnosed, so we can skip it.
    return ExprError();
  } else if (isa<DependentScopeDeclRefExpr, CXXDependentScopeMemberExpr>(
                 RoutineName)) {
    // These are dependent and we can't really check them, so delay until
    // instantiation.
    return RoutineName;
  } else if (const auto *DRE = dyn_cast<DeclRefExpr>(RoutineName)) {
    const ValueDecl *VD = DRE->getDecl();

    if (isa<FunctionDecl>(VD))
      return RoutineName;

    // Allow lambdas.
    if (const auto *VarD = dyn_cast<VarDecl>(VD)) {
      QualType VarDTy = VarD->getType();
      if (!VarDTy.isNull()) {
        if (const auto *RD = VarDTy->getAsCXXRecordDecl()) {
          if (RD->isGenericLambda()) {
            Diag(RoutineName->getBeginLoc(), diag::err_acc_routine_overload_set)
                << RoutineName;
            return ExprError();
          }
          if (RD->isLambda())
            return RoutineName;
        } else if (VarDTy->isDependentType()) {
          // If this is a dependent variable, it might be a lambda. So we just
          // accept this and catch it next time.
          return RoutineName;
        }
      }
    }

    Diag(RoutineName->getBeginLoc(), diag::err_acc_routine_not_func)
        << RoutineName;
    return ExprError();
  } else if (isa<OverloadExpr>(RoutineName)) {
    // This happens in function templates, even when the template arguments are
    // fully specified. We could possibly do some sort of matching to make sure
    // that this is looked up/deduced, but GCC does not do this, so there
    // doesn't seem to be a good reason for us to do it either.
    Diag(RoutineName->getBeginLoc(), diag::err_acc_routine_overload_set)
        << RoutineName;
    return ExprError();
  }

  Diag(RoutineName->getBeginLoc(), diag::err_acc_routine_not_func)
      << RoutineName;
  return ExprError();
}
void SemaOpenACC::ActOnVariableDeclarator(VarDecl *VD) {
  if (!VD->isStaticLocal() || !getLangOpts().OpenACC)
    return;

  // This cast should be safe, since a static-local can only happen in a
  // function declaration.
  auto *ContextDecl = cast<FunctionDecl>(getCurContext());

  // OpenACC 3.3 2.15:
  // In C and C++, function static variables are not supported in functions to
  // which a routine directive applies.
  for (const auto *A : ContextDecl->attrs()) {
    if (isa<OpenACCRoutineDeclAttr, OpenACCRoutineAnnotAttr>(A)) {
      Diag(VD->getBeginLoc(), diag::err_acc_magic_static_in_routine);
      Diag(A->getLocation(), diag::note_acc_construct_here)
          << OpenACCDirectiveKind::Routine;
      return;
    }
  }

  MagicStaticLocs.insert({ContextDecl->getCanonicalDecl(), VD->getBeginLoc()});
}
void SemaOpenACC::CheckLastRoutineDeclNameConflict(const NamedDecl *ND) {
  // OpenACC 3.3 A.3.4
  // When a procedure with that name is in scope and it is not the same
  // procedure as the immediately following procedure declaration or
  // definition, the resolution of the name can be confusing.  Implementations
  // should then issue a compile-time warning diagnostic even though the
  // application is conforming.

  // If we haven't created one, also can't diagnose.
  if (!LastRoutineDecl)
    return;

  // If the currently created function doesn't have a name, we can't diagnose on
  // a match.
  if (!ND->getDeclName().isIdentifier())
    return;

  // If the two are in different decl contexts, it doesn't make sense to
  // diagnose.
  if (LastRoutineDecl->getDeclContext() != ND->getLexicalDeclContext())
    return;

  // If we don't have a referenced thing yet, we can't diagnose.
  FunctionDecl *RoutineTarget =
      getFunctionFromRoutineName(LastRoutineDecl->getFunctionReference());
  if (!RoutineTarget)
    return;

  // If the Routine target doesn't have a name, we can't diagnose.
  if (!RoutineTarget->getDeclName().isIdentifier())
    return;

  // Of course don't diagnose if the names don't match.
  if (ND->getName() != RoutineTarget->getName())
    return;

  long NDLine = SemaRef.SourceMgr.getSpellingLineNumber(ND->getBeginLoc());
  long LastLine =
      SemaRef.SourceMgr.getSpellingLineNumber(LastRoutineDecl->getBeginLoc());

  // Do some line-number math to make sure they are within a line of eachother.
  // Comments or newlines can be inserted to clarify intent.
  if (NDLine - LastLine > 1)
    return;

  // Don't warn if it actually DOES apply to this function via redecls.
  if (ND->getCanonicalDecl() == RoutineTarget->getCanonicalDecl())
    return;

  Diag(LastRoutineDecl->getFunctionReference()->getBeginLoc(),
       diag::warn_acc_confusing_routine_name);
  Diag(RoutineTarget->getBeginLoc(), diag::note_previous_decl) << ND;
}

void SemaOpenACC::ActOnVariableInit(VarDecl *VD, QualType InitType) {
  if (!VD || !getLangOpts().OpenACC || InitType.isNull())
    return;

  // To avoid double-diagnostic, just diagnose this during instantiation.  We'll
  // get 1 warning per instantiation, but this permits us to be more sensible
  // for cases where the lookup is confusing.
  if (VD->getLexicalDeclContext()->isDependentContext())
    return;

  const auto *RD = InitType->getAsCXXRecordDecl();
  // If this isn't a lambda, no sense in diagnosing.
  if (!RD || !RD->isLambda())
    return;

  CheckLastRoutineDeclNameConflict(VD);
}

void SemaOpenACC::ActOnFunctionDeclarator(FunctionDecl *FD) {
  if (!FD || !getLangOpts().OpenACC)
    return;
  CheckLastRoutineDeclNameConflict(FD);
}

bool SemaOpenACC::ActOnStartStmtDirective(
    OpenACCDirectiveKind K, SourceLocation StartLoc,
    ArrayRef<const OpenACCClause *> Clauses) {

  // Declaration directives an appear in a statement location, so call into that
  // function here.
  if (K == OpenACCDirectiveKind::Declare || K == OpenACCDirectiveKind::Routine)
    return ActOnStartDeclDirective(K, StartLoc, Clauses);

  SemaRef.DiscardCleanupsInEvaluationContext();
  SemaRef.PopExpressionEvaluationContext();

  // OpenACC 3.3 2.9.1:
  // Intervening code must not contain other OpenACC directives or calls to API
  // routines.
  //
  // ALL constructs are ill-formed if there is an active 'collapse'
  if (CollapseInfo.CurCollapseCount && *CollapseInfo.CurCollapseCount > 0) {
    Diag(StartLoc, diag::err_acc_invalid_in_loop)
        << /*OpenACC Construct*/ 0 << CollapseInfo.DirectiveKind
        << OpenACCClauseKind::Collapse << K;
    assert(CollapseInfo.ActiveCollapse && "Collapse count without object?");
    Diag(CollapseInfo.ActiveCollapse->getBeginLoc(),
         diag::note_acc_active_clause_here)
        << OpenACCClauseKind::Collapse;
  }
  if (TileInfo.CurTileCount && *TileInfo.CurTileCount > 0) {
    Diag(StartLoc, diag::err_acc_invalid_in_loop)
        << /*OpenACC Construct*/ 0 << TileInfo.DirectiveKind
        << OpenACCClauseKind::Tile << K;
    assert(TileInfo.ActiveTile && "Tile count without object?");
    Diag(TileInfo.ActiveTile->getBeginLoc(), diag::note_acc_active_clause_here)
        << OpenACCClauseKind::Tile;
  }

  // OpenACC3.3 2.6.5: At least one copy, copyin, copyout, create, no_create,
  // present, deviceptr, attach, or default clause must appear on a 'data'
  // construct.
  if (K == OpenACCDirectiveKind::Data &&
      llvm::find_if(Clauses,
                    llvm::IsaPred<OpenACCCopyClause, OpenACCCopyInClause,
                                  OpenACCCopyOutClause, OpenACCCreateClause,
                                  OpenACCNoCreateClause, OpenACCPresentClause,
                                  OpenACCDevicePtrClause, OpenACCAttachClause,
                                  OpenACCDefaultClause>) == Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K
           << GetListOfClauses(
                  {OpenACCClauseKind::Copy, OpenACCClauseKind::CopyIn,
                   OpenACCClauseKind::CopyOut, OpenACCClauseKind::Create,
                   OpenACCClauseKind::NoCreate, OpenACCClauseKind::Present,
                   OpenACCClauseKind::DevicePtr, OpenACCClauseKind::Attach,
                   OpenACCClauseKind::Default});

  // OpenACC3.3 2.6.6: At least one copyin, create, or attach clause must appear
  // on an enter data directive.
  if (K == OpenACCDirectiveKind::EnterData &&
      llvm::find_if(Clauses,
                    llvm::IsaPred<OpenACCCopyInClause, OpenACCCreateClause,
                                  OpenACCAttachClause>) == Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K
           << GetListOfClauses({
                  OpenACCClauseKind::CopyIn,
                  OpenACCClauseKind::Create,
                  OpenACCClauseKind::Attach,
              });
  // OpenACC3.3 2.6.6: At least one copyout, delete, or detach clause must
  // appear on an exit data directive.
  if (K == OpenACCDirectiveKind::ExitData &&
      llvm::find_if(Clauses,
                    llvm::IsaPred<OpenACCCopyOutClause, OpenACCDeleteClause,
                                  OpenACCDetachClause>) == Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K
           << GetListOfClauses({
                  OpenACCClauseKind::CopyOut,
                  OpenACCClauseKind::Delete,
                  OpenACCClauseKind::Detach,
              });

  // OpenACC3.3 2.8: At least 'one use_device' clause must appear.
  if (K == OpenACCDirectiveKind::HostData &&
      llvm::find_if(Clauses, llvm::IsaPred<OpenACCUseDeviceClause>) ==
          Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K << GetListOfClauses({OpenACCClauseKind::UseDevice});

  // OpenACC3.3 2.14.3: At least one default_async, device_num, or device_type
  // clause must appear.
  if (K == OpenACCDirectiveKind::Set &&
      llvm::find_if(
          Clauses,
          llvm::IsaPred<OpenACCDefaultAsyncClause, OpenACCDeviceNumClause,
                        OpenACCDeviceTypeClause, OpenACCIfClause>) ==
          Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K
           << GetListOfClauses({OpenACCClauseKind::DefaultAsync,
                                OpenACCClauseKind::DeviceNum,
                                OpenACCClauseKind::DeviceType,
                                OpenACCClauseKind::If});

  // OpenACC3.3 2.14.4: At least one self, host, or device clause must appear on
  // an update directive.
  if (K == OpenACCDirectiveKind::Update &&
      llvm::find_if(Clauses, llvm::IsaPred<OpenACCSelfClause, OpenACCHostClause,
                                           OpenACCDeviceClause>) ==
          Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K
           << GetListOfClauses({OpenACCClauseKind::Self,
                                OpenACCClauseKind::Host,
                                OpenACCClauseKind::Device});

  return diagnoseConstructAppertainment(*this, K, StartLoc, /*IsStmt=*/true);
}

StmtResult SemaOpenACC::ActOnEndStmtDirective(
    OpenACCDirectiveKind K, SourceLocation StartLoc, SourceLocation DirLoc,
    SourceLocation LParenLoc, SourceLocation MiscLoc, ArrayRef<Expr *> Exprs,
    OpenACCAtomicKind AtomicKind, SourceLocation RParenLoc,
    SourceLocation EndLoc, ArrayRef<OpenACCClause *> Clauses,
    StmtResult AssocStmt) {
  switch (K) {
  case OpenACCDirectiveKind::Invalid:
    return StmtError();
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::Kernels: {
    return OpenACCComputeConstruct::Create(
        getASTContext(), K, StartLoc, DirLoc, EndLoc, Clauses,
        AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  case OpenACCDirectiveKind::ParallelLoop:
  case OpenACCDirectiveKind::SerialLoop:
  case OpenACCDirectiveKind::KernelsLoop: {
    return OpenACCCombinedConstruct::Create(
        getASTContext(), K, StartLoc, DirLoc, EndLoc, Clauses,
        AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  case OpenACCDirectiveKind::Loop: {
    return OpenACCLoopConstruct::Create(
        getASTContext(), ActiveComputeConstructInfo.Kind, StartLoc, DirLoc,
        EndLoc, Clauses, AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  case OpenACCDirectiveKind::Data: {
    return OpenACCDataConstruct::Create(
        getASTContext(), StartLoc, DirLoc, EndLoc, Clauses,
        AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  case OpenACCDirectiveKind::EnterData: {
    return OpenACCEnterDataConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                             EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::ExitData: {
    return OpenACCExitDataConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                            EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::HostData: {
    return OpenACCHostDataConstruct::Create(
        getASTContext(), StartLoc, DirLoc, EndLoc, Clauses,
        AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  case OpenACCDirectiveKind::Wait: {
    return OpenACCWaitConstruct::Create(
        getASTContext(), StartLoc, DirLoc, LParenLoc, Exprs.front(), MiscLoc,
        Exprs.drop_front(), RParenLoc, EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::Init: {
    return OpenACCInitConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                        EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::Shutdown: {
    return OpenACCShutdownConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                            EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::Set: {
    return OpenACCSetConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                       EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::Update: {
    return OpenACCUpdateConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                          EndLoc, Clauses);
  }
  case OpenACCDirectiveKind::Atomic: {
    return OpenACCAtomicConstruct::Create(
        getASTContext(), StartLoc, DirLoc, AtomicKind, EndLoc, Clauses,
        AssocStmt.isUsable() ? AssocStmt.get() : nullptr);
  }
  case OpenACCDirectiveKind::Cache: {
    assert(Clauses.empty() && "Cache doesn't allow clauses");
    return OpenACCCacheConstruct::Create(getASTContext(), StartLoc, DirLoc,
                                         LParenLoc, MiscLoc, Exprs, RParenLoc,
                                         EndLoc);
  }
  case OpenACCDirectiveKind::Routine:
    llvm_unreachable("routine shouldn't handled here");
  case OpenACCDirectiveKind::Declare: {
    // Declare and routine arei declaration directives, but can be used here as
    // long as we wrap it in a DeclStmt.  So make sure we do that here.
    DeclGroupRef DR = ActOnEndDeclDirective(K, StartLoc, DirLoc, LParenLoc,
                                            RParenLoc, EndLoc, Clauses);

    return SemaRef.ActOnDeclStmt(DeclGroupPtrTy::make(DR), StartLoc, EndLoc);
  }
  }
  llvm_unreachable("Unhandled case in directive handling?");
}

StmtResult SemaOpenACC::ActOnAssociatedStmt(
    SourceLocation DirectiveLoc, OpenACCDirectiveKind K,
    OpenACCAtomicKind AtKind, ArrayRef<const OpenACCClause *> Clauses,
    StmtResult AssocStmt) {
  switch (K) {
  default:
    llvm_unreachable("Unimplemented associated statement application");
  case OpenACCDirectiveKind::EnterData:
  case OpenACCDirectiveKind::ExitData:
  case OpenACCDirectiveKind::Wait:
  case OpenACCDirectiveKind::Init:
  case OpenACCDirectiveKind::Shutdown:
  case OpenACCDirectiveKind::Set:
  case OpenACCDirectiveKind::Cache:
    llvm_unreachable(
        "these don't have associated statements, so shouldn't get here");
  case OpenACCDirectiveKind::Atomic:
    return CheckAtomicAssociatedStmt(DirectiveLoc, AtKind, AssocStmt);
  case OpenACCDirectiveKind::Parallel:
  case OpenACCDirectiveKind::Serial:
  case OpenACCDirectiveKind::Kernels:
  case OpenACCDirectiveKind::Data:
  case OpenACCDirectiveKind::HostData:
    // There really isn't any checking here that could happen. As long as we
    // have a statement to associate, this should be fine.
    // OpenACC 3.3 Section 6:
    // Structured Block: in C or C++, an executable statement, possibly
    // compound, with a single entry at the top and a single exit at the
    // bottom.
    // FIXME: Should we reject DeclStmt's here? The standard isn't clear, and
    // an interpretation of it is to allow this and treat the initializer as
    // the 'structured block'.
    return AssocStmt;
  case OpenACCDirectiveKind::Loop:
  case OpenACCDirectiveKind::ParallelLoop:
  case OpenACCDirectiveKind::SerialLoop:
  case OpenACCDirectiveKind::KernelsLoop:
    if (!AssocStmt.isUsable())
      return StmtError();

    if (!isa<CXXForRangeStmt, ForStmt>(AssocStmt.get())) {
      Diag(AssocStmt.get()->getBeginLoc(), diag::err_acc_loop_not_for_loop)
          << K;
      Diag(DirectiveLoc, diag::note_acc_construct_here) << K;
      return StmtError();
    }

    if (!CollapseInfo.CollapseDepthSatisfied || !TileInfo.TileDepthSatisfied) {
      if (!CollapseInfo.CollapseDepthSatisfied) {
        Diag(DirectiveLoc, diag::err_acc_insufficient_loops)
            << OpenACCClauseKind::Collapse;
        assert(CollapseInfo.ActiveCollapse && "Collapse count without object?");
        Diag(CollapseInfo.ActiveCollapse->getBeginLoc(),
             diag::note_acc_active_clause_here)
            << OpenACCClauseKind::Collapse;
      }

      if (!TileInfo.TileDepthSatisfied) {
        Diag(DirectiveLoc, diag::err_acc_insufficient_loops)
            << OpenACCClauseKind::Tile;
        assert(TileInfo.ActiveTile && "Collapse count without object?");
        Diag(TileInfo.ActiveTile->getBeginLoc(),
             diag::note_acc_active_clause_here)
            << OpenACCClauseKind::Tile;
      }
      return StmtError();
    }

    return AssocStmt.get();
  }
  llvm_unreachable("Invalid associated statement application");
}

bool SemaOpenACC::ActOnStartDeclDirective(
    OpenACCDirectiveKind K, SourceLocation StartLoc,
    ArrayRef<const OpenACCClause *> Clauses) {
  // OpenCC3.3 2.1 (line 889)
  // A program must not depend on the order of evaluation of expressions in
  // clause arguments or on any side effects of the evaluations.
  SemaRef.DiscardCleanupsInEvaluationContext();
  SemaRef.PopExpressionEvaluationContext();

  if (K == OpenACCDirectiveKind::Routine &&
      llvm::find_if(Clauses,
                    llvm::IsaPred<OpenACCGangClause, OpenACCWorkerClause,
                                  OpenACCVectorClause, OpenACCSeqClause>) ==
          Clauses.end())
    return Diag(StartLoc, diag::err_acc_construct_one_clause_of)
           << K
           << GetListOfClauses({
                  OpenACCClauseKind::Gang,
                  OpenACCClauseKind::Worker,
                  OpenACCClauseKind::Vector,
                  OpenACCClauseKind::Seq,
              });

  return diagnoseConstructAppertainment(*this, K, StartLoc, /*IsStmt=*/false);
}

DeclGroupRef SemaOpenACC::ActOnEndDeclDirective(
    OpenACCDirectiveKind K, SourceLocation StartLoc, SourceLocation DirLoc,
    SourceLocation LParenLoc, SourceLocation RParenLoc, SourceLocation EndLoc,
    ArrayRef<OpenACCClause *> Clauses) {
  switch (K) {
  default:
  case OpenACCDirectiveKind::Invalid:
    return DeclGroupRef{};
  case OpenACCDirectiveKind::Declare: {
    // OpenACC3.3 2.13: At least one clause must appear on a declare directive.
    if (Clauses.empty()) {
      Diag(EndLoc, diag::err_acc_declare_required_clauses);
      // No reason to add this to the AST, as we would just end up trying to
      // instantiate this, which would double-diagnose here, which we wouldn't
      // want to do.
      return DeclGroupRef{};
    }

    auto *DeclareDecl = OpenACCDeclareDecl::Create(
        getASTContext(), getCurContext(), StartLoc, DirLoc, EndLoc, Clauses);
    DeclareDecl->setAccess(AS_public);
    getCurContext()->addDecl(DeclareDecl);
    return DeclGroupRef{DeclareDecl};
  }
  case OpenACCDirectiveKind::Routine:
    llvm_unreachable("routine shouldn't be handled here");
  }
  llvm_unreachable("unhandled case in directive handling?");
}

namespace {
// Given the decl on the next line, figure out if it is one that is acceptable
// to `routine`, or looks like the sort of decl we should be diagnosing against.
FunctionDecl *LegalizeNextParsedDecl(Decl *D) {
  if (!D)
    return nullptr;

  // Functions are per-fact acceptable as-is.
  if (auto *FD = dyn_cast<FunctionDecl>(D))
    return FD;

  // Function templates are functions, so attach to the templated decl.
  if (auto *FTD = dyn_cast<FunctionTemplateDecl>(D))
    return FTD->getTemplatedDecl();

  if (auto *FD = dyn_cast<FieldDecl>(D)) {
    auto *RD =
        FD->getType().isNull() ? nullptr : FD->getType()->getAsCXXRecordDecl();

    if (RD && RD->isGenericLambda())
      return RD->getDependentLambdaCallOperator()->getTemplatedDecl();
    if (RD && RD->isLambda())
      return RD->getLambdaCallOperator();
  }
  // VarDecl we can look at the init instead of the type of the variable, this
  // makes us more tolerant of the 'auto' deduced type.
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    Expr *Init = VD->getInit();
    if (!Init || Init->getType().isNull())
      return nullptr;

    const auto *RD = Init->getType()->getAsCXXRecordDecl();
    if (RD && RD->isGenericLambda())
      return RD->getDependentLambdaCallOperator()->getTemplatedDecl();
    if (RD && RD->isLambda())
      return RD->getLambdaCallOperator();

    // FIXME: We could try harder in the case where this is a dependent thing
    // that ends up being a lambda (that is, the init is an unresolved lookup
    // expr), but we can't attach to the call/lookup expr. If we instead try to
    // attach to the VarDecl, when we go to instantiate it, attributes are
    // instantiated before the init, so we can't actually see the type at any
    // point where it would be relevant/able to be checked. We could perhaps do
    // some sort of 'after-init' instantiation/checking here, but that doesn't
    // seem valuable for a situation that other compilers don't handle.
  }
  return nullptr;
}

void CreateRoutineDeclAttr(SemaOpenACC &SemaRef, SourceLocation DirLoc,
                           ArrayRef<const OpenACCClause *> Clauses,
                           ValueDecl *AddTo) {
  OpenACCRoutineDeclAttr *A =
      OpenACCRoutineDeclAttr::Create(SemaRef.getASTContext(), DirLoc);
  A->Clauses.assign(Clauses.begin(), Clauses.end());
  AddTo->addAttr(A);
}
} // namespace

// Variant that adds attributes, because this is the unnamed case.
void SemaOpenACC::CheckRoutineDecl(SourceLocation DirLoc,
                                   ArrayRef<const OpenACCClause *> Clauses,
                                   Decl *NextParsedDecl) {

  FunctionDecl *NextParsedFDecl = LegalizeNextParsedDecl(NextParsedDecl);

  if (!NextParsedFDecl) {
    // If we don't have a valid 'next thing', just diagnose.
    SemaRef.Diag(DirLoc, diag::err_acc_decl_for_routine);
    return;
  }

  // OpenACC 3.3 2.15:
  // In C and C++, function static variables are not supported in functions to
  // which a routine directive applies.
  if (auto Itr = MagicStaticLocs.find(NextParsedFDecl->getCanonicalDecl());
      Itr != MagicStaticLocs.end()) {
    Diag(Itr->second, diag::err_acc_magic_static_in_routine);
    Diag(DirLoc, diag::note_acc_construct_here)
        << OpenACCDirectiveKind::Routine;

    return;
  }

  auto BindItr = llvm::find_if(Clauses, llvm::IsaPred<OpenACCBindClause>);
  for (auto *A : NextParsedFDecl->attrs()) {
    // OpenACC 3.3 2.15:
    // If a procedure has a bind clause on both the declaration and definition
    // than they both must bind to the same name.
    if (auto *RA = dyn_cast<OpenACCRoutineDeclAttr>(A)) {
      auto OtherBindItr =
          llvm::find_if(RA->Clauses, llvm::IsaPred<OpenACCBindClause>);
      if (OtherBindItr != RA->Clauses.end() &&
          (*cast<OpenACCBindClause>(*BindItr)) !=
              (*cast<OpenACCBindClause>(*OtherBindItr))) {
        Diag((*BindItr)->getBeginLoc(), diag::err_acc_duplicate_unnamed_bind);
        Diag((*OtherBindItr)->getEndLoc(), diag::note_acc_previous_clause_here);
        return;
      }
    }

    // OpenACC 3.3 2.15:
    // A bind clause may not bind to a routine name that has a visible bind
    // clause.
    // We take the combo of these two 2.15 restrictions to mean that the
    // 'declaration'/'definition' quote is an exception to this. So we're going
    // to disallow mixing of the two types entirely.
    if (auto *RA = dyn_cast<OpenACCRoutineAnnotAttr>(A);
        RA && RA->getRange().getEnd().isValid()) {
      Diag((*BindItr)->getBeginLoc(), diag::err_acc_duplicate_bind);
      Diag(RA->getRange().getEnd(), diag::note_acc_previous_clause_here);
      return;
    }
  }

  CreateRoutineDeclAttr(*this, DirLoc, Clauses, NextParsedFDecl);
}

// Variant that adds a decl, because this is the named case.
OpenACCRoutineDecl *SemaOpenACC::CheckRoutineDecl(
    SourceLocation StartLoc, SourceLocation DirLoc, SourceLocation LParenLoc,
    Expr *FuncRef, SourceLocation RParenLoc,
    ArrayRef<const OpenACCClause *> Clauses, SourceLocation EndLoc) {
  assert(LParenLoc.isValid());

  if (FunctionDecl *FD = getFunctionFromRoutineName(FuncRef)) {
    // OpenACC 3.3 2.15:
    // In C and C++, function static variables are not supported in functions to
    // which a routine directive applies.
    if (auto Itr = MagicStaticLocs.find(FD->getCanonicalDecl());
        Itr != MagicStaticLocs.end()) {
      Diag(Itr->second, diag::err_acc_magic_static_in_routine);
      Diag(DirLoc, diag::note_acc_construct_here)
          << OpenACCDirectiveKind::Routine;

      return nullptr;
    }

    // OpenACC 3.3 2.15:
    // A bind clause may not bind to a routine name that has a visible bind
    // clause.
    auto BindItr = llvm::find_if(Clauses, llvm::IsaPred<OpenACCBindClause>);
    SourceLocation BindLoc;
    if (BindItr != Clauses.end()) {
      BindLoc = (*BindItr)->getBeginLoc();
      // Since this is adding a 'named' routine, we aren't allowed to combine
      // with ANY other visible bind clause. Error if we see either.

      for (auto *A : FD->attrs()) {
        if (auto *RA = dyn_cast<OpenACCRoutineDeclAttr>(A)) {
          auto OtherBindItr =
              llvm::find_if(RA->Clauses, llvm::IsaPred<OpenACCBindClause>);
          if (OtherBindItr != RA->Clauses.end()) {
            Diag((*BindItr)->getBeginLoc(), diag::err_acc_duplicate_bind);
            Diag((*OtherBindItr)->getEndLoc(),
                 diag::note_acc_previous_clause_here);
            return nullptr;
          }
        }

        if (auto *RA = dyn_cast<OpenACCRoutineAnnotAttr>(A);
            RA && RA->getRange().getEnd().isValid()) {
          Diag((*BindItr)->getBeginLoc(), diag::err_acc_duplicate_bind);
          Diag(RA->getRange().getEnd(), diag::note_acc_previous_clause_here);
          return nullptr;
        }
      }
    }

    // Set the end-range to the 'bind' clause here, so we can look it up
    // later.
    auto *RAA = OpenACCRoutineAnnotAttr::CreateImplicit(getASTContext(),
                                                        {DirLoc, BindLoc});
    FD->addAttr(RAA);
    // In case we are referencing not the 'latest' version, make sure we add
    // the attribute to all declarations.
    while (FD != FD->getMostRecentDecl()) {
      FD = FD->getMostRecentDecl();
      FD->addAttr(RAA);
    }
  }

  LastRoutineDecl = OpenACCRoutineDecl::Create(
      getASTContext(), getCurContext(), StartLoc, DirLoc, LParenLoc, FuncRef,
      RParenLoc, EndLoc, Clauses);
  LastRoutineDecl->setAccess(AS_public);
  getCurContext()->addDecl(LastRoutineDecl);

  return LastRoutineDecl;
}

DeclGroupRef SemaOpenACC::ActOnEndRoutineDeclDirective(
    SourceLocation StartLoc, SourceLocation DirLoc, SourceLocation LParenLoc,
    Expr *ReferencedFunc, SourceLocation RParenLoc,
    ArrayRef<const OpenACCClause *> Clauses, SourceLocation EndLoc,
    DeclGroupPtrTy NextDecl) {
  assert((!ReferencedFunc || !NextDecl) &&
         "Only one of these should be filled");

  if (LParenLoc.isInvalid()) {
    Decl *NextLineDecl = nullptr;
    if (NextDecl && NextDecl.get().isSingleDecl())
      NextLineDecl = NextDecl.get().getSingleDecl();

    CheckRoutineDecl(DirLoc, Clauses, NextLineDecl);

    return NextDecl.get();
  }

  return DeclGroupRef{CheckRoutineDecl(
      StartLoc, DirLoc, LParenLoc, ReferencedFunc, RParenLoc, Clauses, EndLoc)};
}

StmtResult SemaOpenACC::ActOnEndRoutineStmtDirective(
    SourceLocation StartLoc, SourceLocation DirLoc, SourceLocation LParenLoc,
    Expr *ReferencedFunc, SourceLocation RParenLoc,
    ArrayRef<const OpenACCClause *> Clauses, SourceLocation EndLoc,
    Stmt *NextStmt) {
  assert((!ReferencedFunc || !NextStmt) &&
         "Only one of these should be filled");

  if (LParenLoc.isInvalid()) {
    Decl *NextLineDecl = nullptr;
    if (NextStmt)
      if (DeclStmt *DS = dyn_cast<DeclStmt>(NextStmt); DS && DS->isSingleDecl())
        NextLineDecl = DS->getSingleDecl();

    CheckRoutineDecl(DirLoc, Clauses, NextLineDecl);
    return NextStmt;
  }

  DeclGroupRef DR{CheckRoutineDecl(StartLoc, DirLoc, LParenLoc, ReferencedFunc,
                                   RParenLoc, Clauses, EndLoc)};
  return SemaRef.ActOnDeclStmt(DeclGroupPtrTy::make(DR), StartLoc, EndLoc);
}

OpenACCRoutineDeclAttr *
SemaOpenACC::mergeRoutineDeclAttr(const OpenACCRoutineDeclAttr &Old) {
  OpenACCRoutineDeclAttr *New =
      OpenACCRoutineDeclAttr::Create(getASTContext(), Old.getLocation());
  // We should jsut be able to copy these, there isn't really any
  // merging/inheriting we have to do, so no worry about doing a deep copy.
  New->Clauses = Old.Clauses;
  return New;
}
ExprResult
SemaOpenACC::BuildOpenACCAsteriskSizeExpr(SourceLocation AsteriskLoc) {
  return OpenACCAsteriskSizeExpr::Create(getASTContext(), AsteriskLoc);
}

ExprResult
SemaOpenACC::ActOnOpenACCAsteriskSizeExpr(SourceLocation AsteriskLoc) {
  return BuildOpenACCAsteriskSizeExpr(AsteriskLoc);
}
