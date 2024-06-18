#ifndef LLVM_ANALYSIS_LOOPEXTRACTION_H
#define LLVM_ANALYSIS_LOOPEXTRACTION_H

#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <map>
#include <set>
#include <vector>

namespace llvm {
class LoopExtractionPass : public PassInfoMixin<LoopExtractionPass>{
public:
  static std::set<Function *> GeneratedFunctions;
  static std::set<Function *> PreservedFunctions;

protected:
  FunctionAnalysisManager *FAM;
  LoopInfo *LI;
  ScalarEvolution *SE;
  DominatorTree *DT;

public:
  static void verifyBody(Function *F);

  static void addGenerated(Function *F);

  static bool isGenerated(Function *F){
    return GeneratedFunctions.find(F) != GeneratedFunctions.end();
  }
  
  static bool isPreserved(Function *F){
    return PreservedFunctions.find(F) != PreservedFunctions.end(); 
  }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

protected:
  void trySimplifyLoops();
    
  void findPHINodesForLoop(const Loop *L, 
      PHINode *IndVar,
      std::vector<Value *> &ExternalUses);

  void findExternalUses(Function &F, 
      std::vector<BasicBlock *> &LoopBlocks,
      Function *External, 
      std::vector<Value *> &ExternalUses,
      bool ignoreArgs = false); 

  void findExternalUses(Function &F, 
      std::vector<BasicBlock *> &Blocks, 
      std::vector<Value *> &ExternalUses,
      bool ignoreArgs = false); 

  void eraseNestedBlocks(const Loop *L, 
      std::vector<BasicBlock *> &AllBlocks,
      std::map<BasicBlock *, BasicBlock *> &BMap);

  bool expandPHINodes(std::vector<Value *> &ExternalUses, 
      ValueToValueMapTy &VMap);

  BasicBlock* replaceWithArgs(Function* OriginalF, 
      Function* ClonedF, 
      std::vector<Value *> &ReplaceWithArgs,
      bool InductionReplaced = false);

  void replaceForeignUses(Function* OriginalF, 
      Function* ClonedF, ValueToValueMapTy &VMap, 
      std::map<BasicBlock *, BasicBlock *>  &BMap);

  Value* createStoresForArgs(Function* OriginalF, 
      std::vector<Value *> &ReplaceWithArgs,
      bool InductionIncluded = true);

  void enqueueTask(Function* OriginalF,  
      BasicBlock* Preheader, 
      BasicBlock* Header, 
      BasicBlock* Succ, 
      PHINode *IndVar, 
      Function *ParallelBody, 
      Value *SequentialBody, 
      Value *RestOfFunc, 
      Loop::LoopBounds &Bounds, 
      Value *StoreAddr,
      Value *NewScopeAddr);

  Value *createNestedScope(Function *GeneratedF, 
      Function *SeqExtractedBody, 
      Function *RestOfFunc, 
      std::vector<BasicBlock *> &LoopBlocks, 
      std::vector<Value *> &ReplaceWithArgs, 
      BasicBlock *Preheader,
      BasicBlock *Exit,
      BasicBlock *Succ);

  void cloneLoopAndRemap(Function &F, const Loop *L);
};
}
#endif
