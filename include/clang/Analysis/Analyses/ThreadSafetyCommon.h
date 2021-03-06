//===- ThreadSafetyCommon.h ------------------------------------*- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Parts of thread safety analysis that are not specific to thread safety
// itself have been factored into classes here, where they can be potentially
// used by other analyses.  Currently these include:
//
// * Generalize clang CFG visitors.
// * Conversion of the clang CFG to SSA form.
// * Translation of clang Exprs to TIL SExprs
//
// UNDER CONSTRUCTION.  USE AT YOUR OWN RISK.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_THREAD_SAFETY_COMMON_H
#define LLVM_CLANG_THREAD_SAFETY_COMMON_H

#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/Analyses/ThreadSafetyTIL.h"
#include "clang/Analysis/AnalysisContext.h"
#include "clang/Basic/OperatorKinds.h"

#include <vector>


namespace clang {
namespace threadSafety {

// This class defines the interface of a clang CFG Visitor.
// CFGWalker will invoke the following methods.
// Note that methods are not virtual; the visitor is templatized.
class CFGVisitor {
  // Enter the CFG for Decl D, and perform any initial setup operations.
  void enterCFG(CFG *Cfg, const NamedDecl *D, const CFGBlock *First) {}

  // Enter a CFGBlock.
  void enterCFGBlock(const CFGBlock *B) {}

  // Returns true if this visitor implements handlePredecessor
  bool visitPredecessors() { return true; }

  // Process a predecessor edge.
  void handlePredecessor(const CFGBlock *Pred) {}

  // Process a successor back edge to a previously visited block.
  void handlePredecessorBackEdge(const CFGBlock *Pred) {}

  // Called just before processing statements.
  void enterCFGBlockBody(const CFGBlock *B) {}

  // Process an ordinary statement.
  void handleStatement(const Stmt *S) {}

  // Process a destructor call
  void handleDestructorCall(const VarDecl *VD, const CXXDestructorDecl *DD) {}

  // Called after all statements have been handled.
  void exitCFGBlockBody(const CFGBlock *B) {}

  // Return true
  bool visitSuccessors() { return true; }

  // Process a successor edge.
  void handleSuccessor(const CFGBlock *Succ) {}

  // Process a successor back edge to a previously visited block.
  void handleSuccessorBackEdge(const CFGBlock *Succ) {}

  // Leave a CFGBlock.
  void exitCFGBlock(const CFGBlock *B) {}

  // Leave the CFG, and perform any final cleanup operations.
  void exitCFG(const CFGBlock *Last) {}
};


// Walks the clang CFG, and invokes methods on a given CFGVisitor.
class CFGWalker {
public:
  CFGWalker() :
    CFGraph(nullptr), FDecl(nullptr), ACtx(nullptr), SortedGraph(nullptr) {}

  // Initialize the CFGWalker.  This setup only needs to be done once, even
  // if there are multiple passes over the CFG.
  bool init(AnalysisDeclContext &AC) {
    ACtx = &AC;
    CFGraph = AC.getCFG();
    if (!CFGraph)
      return false;

    FDecl = dyn_cast_or_null<NamedDecl>(AC.getDecl());
    if (!FDecl) // ignore anonymous functions
      return false;

    SortedGraph = AC.getAnalysis<PostOrderCFGView>();
    if (!SortedGraph)
      return false;

    return true;
  }

  // Traverse the CFG, calling methods on V as appropriate.
  template <class Visitor>
  void walk(Visitor &V) {
    PostOrderCFGView::CFGBlockSet VisitedBlocks(CFGraph);

    V.enterCFG(CFGraph, FDecl, &CFGraph->getEntry());

    for (const auto *CurrBlock : *SortedGraph) {
      VisitedBlocks.insert(CurrBlock);

      V.enterCFGBlock(CurrBlock);

      // Process predecessors
      if (V.visitPredecessors()) {
        // Process successors
        for (CFGBlock::const_pred_iterator SI = CurrBlock->pred_begin(),
                                           SE = CurrBlock->pred_end();
             SI != SE; ++SI) {
          if (*SI == nullptr)
            continue;

          if (!VisitedBlocks.alreadySet(*SI)) {
            V.handlePredecessorBackEdge(*SI);
            continue;
          }
          V.handlePredecessor(*SI);
        }
      }

      V.enterCFGBlockBody(CurrBlock);

      // Process statements
      for (const auto &BI : *CurrBlock) {
        switch (BI.getKind()) {
        case CFGElement::Statement: {
          V.handleStatement(BI.castAs<CFGStmt>().getStmt());
          break;
        }
        case CFGElement::AutomaticObjectDtor: {
          CFGAutomaticObjDtor AD = BI.castAs<CFGAutomaticObjDtor>();
          CXXDestructorDecl *DD = const_cast<CXXDestructorDecl*>(
              AD.getDestructorDecl(ACtx->getASTContext()));
          VarDecl *VD = const_cast<VarDecl*>(AD.getVarDecl());
          V.handleDestructorCall(VD, DD);
          break;
        }
        default:
          break;
        }
      }

      V.exitCFGBlockBody(CurrBlock);

      // Process successors
      if (V.visitSuccessors()) {
        // Process successors
        for (CFGBlock::const_succ_iterator SI = CurrBlock->succ_begin(),
                                           SE = CurrBlock->succ_end();
             SI != SE; ++SI) {
          if (*SI == nullptr)
            continue;

          if (VisitedBlocks.alreadySet(*SI)) {
            V.handleSuccessorBackEdge(*SI);
            continue;
          }
          V.handleSuccessor(*SI);
        }
      }

      V.exitCFGBlock(CurrBlock);
    }
    V.exitCFG(&CFGraph->getExit());
  }

public:  // TODO: make these private.
  CFG *CFGraph;
  const NamedDecl *FDecl;
  AnalysisDeclContext *ACtx;
  PostOrderCFGView *SortedGraph;
};


// Translate clang::Expr to til::SExpr.
class SExprBuilder {
public:
  typedef llvm::DenseMap<const Stmt*, til::Variable*> StatementMap;

  /// \brief Encapsulates the lexical context of a function call.  The lexical
  /// context includes the arguments to the call, including the implicit object
  /// argument.  When an attribute containing a mutex expression is attached to
  /// a method, the expression may refer to formal parameters of the method.
  /// Actual arguments must be substituted for formal parameters to derive
  /// the appropriate mutex expression in the lexical context where the function
  /// is called.  PrevCtx holds the context in which the arguments themselves
  /// should be evaluated; multiple calling contexts can be chained together
  /// by the lock_returned attribute.
  struct CallingContext {
    const NamedDecl *AttrDecl;  // The decl to which the attr is attached.
    const Expr *SelfArg;        // Implicit object argument -- e.g. 'this'
    unsigned NumArgs;           // Number of funArgs
    const Expr *const *FunArgs; // Function arguments
    CallingContext *Prev;       // The previous context; or 0 if none.
    bool SelfArrow;             // is Self referred to with -> or .?

    CallingContext(const NamedDecl *D = nullptr, const Expr *S = nullptr,
                   unsigned N = 0, const Expr *const *A = nullptr,
                   CallingContext *P = nullptr)
        : AttrDecl(D), SelfArg(S), NumArgs(N), FunArgs(A), Prev(P),
          SelfArrow(false)
    {}
  };

  SExprBuilder(til::MemRegionRef A)
    : Arena(A), SelfVar(nullptr), Scfg(nullptr), CallCtx(nullptr),
    CurrentBB(nullptr), CurrentBlockID(0), CurrentVarID(0),
    CurrentArgIndex(0) {
    // FIXME: we don't always have a self-variable.
    SelfVar = new (Arena)til::Variable(til::Variable::VK_SFun);
  }

  // Translate a clang statement or expression to a TIL expression.
  // Also performs substitution of variables; Ctx provides the context.
  // Dispatches on the type of S.
  til::SExpr *translate(const Stmt *S, CallingContext *Ctx);
  til::SCFG  *buildCFG(CFGWalker &Walker);

  til::SExpr *lookupStmt(const Stmt *S);
  const til::SCFG *getCFG() const { return Scfg; }
  til::SCFG *getCFF() { return Scfg; }

private:
  til::SExpr *translateDeclRefExpr(const DeclRefExpr *DRE,
                                   CallingContext *Ctx) ;
  til::SExpr *translateCXXThisExpr(const CXXThisExpr *TE, CallingContext *Ctx);
  til::SExpr *translateMemberExpr(const MemberExpr *ME, CallingContext *Ctx);
  til::SExpr *translateCallExpr(const CallExpr *CE, CallingContext *Ctx);
  til::SExpr *translateCXXMemberCallExpr(const CXXMemberCallExpr *ME,
                                         CallingContext *Ctx);
  til::SExpr *translateCXXOperatorCallExpr(const CXXOperatorCallExpr *OCE,
                                           CallingContext *Ctx);
  til::SExpr *translateUnaryOperator(const UnaryOperator *UO,
                                     CallingContext *Ctx);
  til::SExpr *translateBinaryOperator(const BinaryOperator *BO,
                                      CallingContext *Ctx);
  til::SExpr *translateCastExpr(const CastExpr *CE, CallingContext *Ctx);
  til::SExpr *translateArraySubscriptExpr(const ArraySubscriptExpr *E,
                                          CallingContext *Ctx);
  til::SExpr *translateConditionalOperator(const ConditionalOperator *C,
                                           CallingContext *Ctx);
  til::SExpr *translateBinaryConditionalOperator(
      const BinaryConditionalOperator *C, CallingContext *Ctx);

  til::SExpr *translateDeclStmt(const DeclStmt *S, CallingContext *Ctx);

  // Used for looking the index of a name.
  typedef llvm::DenseMap<const ValueDecl *, unsigned> NameIndexMap;

  // Used for looking up the current SSA variable for a name, by index.
  typedef CopyOnWriteVector<std::pair<const ValueDecl *, til::SExpr *>>
    NameVarMap;

  struct BlockInfo {
    NameVarMap ExitMap;
    bool HasBackEdges;
    unsigned SuccessorsToProcess;
    BlockInfo() : HasBackEdges(false), SuccessorsToProcess(0) {}
    BlockInfo(BlockInfo &&RHS)
        : ExitMap(std::move(RHS.ExitMap)), HasBackEdges(RHS.HasBackEdges),
          SuccessorsToProcess(RHS.SuccessorsToProcess) {}

    BlockInfo &operator=(BlockInfo &&RHS) {
      if (this != &RHS) {
        ExitMap = std::move(RHS.ExitMap);
        HasBackEdges = RHS.HasBackEdges;
        SuccessorsToProcess = RHS.SuccessorsToProcess;
      }
      return *this;
    }
  private:
    BlockInfo(const BlockInfo &) LLVM_DELETED_FUNCTION;
    void operator=(const BlockInfo &) LLVM_DELETED_FUNCTION;
  };

  // We implement the CFGVisitor API
  friend class CFGWalker;

  void enterCFG(CFG *Cfg, const NamedDecl *D, const CFGBlock *First);
  void enterCFGBlock(const CFGBlock *B);
  bool visitPredecessors() { return true; }
  void handlePredecessor(const CFGBlock *Pred);
  void handlePredecessorBackEdge(const CFGBlock *Pred);
  void enterCFGBlockBody(const CFGBlock *B);
  void handleStatement(const Stmt *S);
  void handleDestructorCall(const VarDecl *VD, const CXXDestructorDecl *DD);
  void exitCFGBlockBody(const CFGBlock *B);
  bool visitSuccessors() { return true; }
  void handleSuccessor(const CFGBlock *Succ);
  void handleSuccessorBackEdge(const CFGBlock *Succ);
  void exitCFGBlock(const CFGBlock *B);
  void exitCFG(const CFGBlock *Last);

  void insertStmt(const Stmt *S, til::Variable *V);
  til::SExpr *addStatement(til::SExpr *E, const Stmt *S, const ValueDecl *VD=0);
  til::SExpr *lookupVarDecl(const ValueDecl *VD);
  til::SExpr *addVarDecl(const ValueDecl *VD, til::SExpr *E);
  til::SExpr *updateVarDecl(const ValueDecl *VD, til::SExpr *E);

  void mergeEntryMap(NameVarMap Map);

  til::MemRegionRef Arena;
  til::Variable *SelfVar;       // Variable to use for 'this'.  May be null.
  til::SCFG *Scfg;

  StatementMap SMap;                       // Map from Stmt to TIL Variables
  NameIndexMap IdxMap;                     // Indices of clang local vars.
  std::vector<til::BasicBlock *> BlockMap; // Map from clang to til BBs.
  std::vector<BlockInfo> BBInfo;           // Extra information per BB.
                                           // Indexed by clang BlockID.
  SExprBuilder::CallingContext *CallCtx;   // Root calling context

  NameVarMap CurrentNameMap;
  til::BasicBlock *CurrentBB;
  BlockInfo *CurrentBlockInfo;
  unsigned CurrentBlockID;
  unsigned CurrentVarID;
  unsigned CurrentArgIndex;
};


// Dump an SCFG to llvm::errs().
void printSCFG(CFGWalker &Walker);


} // end namespace threadSafety

} // end namespace clang

#endif  // LLVM_CLANG_THREAD_SAFETY_COMMON_H
