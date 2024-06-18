#include "LoopExtraction.h"

#include <iostream>

#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Use.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include <algorithm>
#include <stack>
#include <optional>

#define DEBUG_TYPE "loop-extraction"
using namespace llvm;

std::set<Function *> LoopExtractionPass::GeneratedFunctions;
std::set<Function *> LoopExtractionPass::PreservedFunctions;

static llvm::cl::opt<bool> EnableExtraction("enable-extract-loop-bodies", llvm::cl::desc("Enable loop extraction"));

void LoopExtractionPass::verifyBody(Function *F){
  std::string errorMessage; 
  raw_string_ostream errorStream(errorMessage); 

  if(verifyFunction(*F, &errorStream)){
    LLVM_DEBUG(dbgs() << errorMessage << "\n");
    F->dump();
    report_fatal_error("ERROR VERIFYING LOOP BODY: " + F->getName() + "\n");
  } 
}

void LoopExtractionPass::addGenerated(Function *F){
  GeneratedFunctions.insert(F);
  Module *M = F->getParent();
  StringRef FName = F->getName();

  LLVMContext &Context = M->getContext();
  
  NamedMDNode *NMD = M->getOrInsertNamedMetadata("GeneratedFunctions");

  Metadata *MDStr = MDString::get(Context, FName); 
  MDNode *MDN = MDNode::get(Context, MDStr);
  NMD->addOperand(MDN);
}

void LoopExtractionPass::trySimplifyLoops(){
  for(Loop *L : *LI) {
    simplifyLoop(L, DT, LI, SE, nullptr, nullptr, false);
  }
}

void LoopExtractionPass::findPHINodesForLoop(const Loop *L, 
    PHINode *IndVar,
    std::vector<Value *> &ExternalUses){
  std::vector<BasicBlock *> Blocks = L->getBlocks();
  for(BasicBlock *BB: Blocks){
    for(auto &I : *BB){
      auto *PHI = dyn_cast<PHINode>(&I);
      if(!PHI) continue; 
      if(PHI == IndVar) continue;
      if(std::count(ExternalUses.begin(), ExternalUses.end(), &cast<Value>(I))) continue;

      bool inSubLoop = false;
      for(auto *SubLoop: L->getSubLoops()){
        inSubLoop = SubLoop->contains(PHI);
        LLVM_DEBUG(dbgs() << "PHI in sub-loop" << *PHI << "\n");
        if(inSubLoop) break; 
      }

      if(inSubLoop) continue;

      ExternalUses.push_back(PHI);
    }
  }
}

void LoopExtractionPass::findExternalUses(Function &F,
    std::vector<BasicBlock *> &LoopBlocks,
    Function *External,
    std::vector<Value *> &ExternalUses,
    bool ignoreArgs){
  
  for(auto *A = F.arg_begin(), *E = F.arg_end(); A != E; A++){
    if(ignoreArgs) break;
    for(Use &U: A->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      
      if(UserInst->getFunction() != External) continue;
      if(std::count(ExternalUses.begin(), ExternalUses.end(), &cast<Value>(*A))) continue;
      
      ExternalUses.push_back(&cast<Value>(*A));
    }
  }

  for(auto I = inst_begin(&F), E = inst_end(&F); I != E; I++){
    if(std::count(LoopBlocks.begin(), LoopBlocks.end(), I->getParent())) continue;
    for(Use &U: I->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      
      if(UserInst->getFunction() != External) continue;
      if(std::count(ExternalUses.begin(), ExternalUses.end(), &cast<Value>(*I))) continue;
      
      ExternalUses.push_back(&cast<Value>(*I));
    }
  }

  LLVM_DEBUG(dbgs() << "External Uses: " << ExternalUses.size() << "\n");
}

void LoopExtractionPass::findExternalUses(Function &F, 
    std::vector<BasicBlock *> &Blocks, 
    std::vector<Value *> &ExternalUses,
    bool ignoreArgs){
  
  for(auto *A = F.arg_begin(), *E = F.arg_end(); A != E; A++){
    if(ignoreArgs) break;
    for(Use &U: A->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      
      if(!std::count(Blocks.begin(), Blocks.end(), UserInst->getParent())) continue;
      if(std::count(ExternalUses.begin(), ExternalUses.end(), &cast<Value>(*A))) continue;
      
      ExternalUses.push_back(&cast<Value>(*A));
    }
  }

  for(auto I = inst_begin(&F), E = inst_end(&F); I != E; I++){
    if(std::count(Blocks.begin(), Blocks.end(), I->getParent())) continue;

    for(Use &U: I->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      
      if(!std::count(Blocks.begin(), Blocks.end(), UserInst->getParent())) continue;
      if(std::count(ExternalUses.begin(), ExternalUses.end(), &cast<Value>(*I))) continue;
      
      ExternalUses.push_back(&cast<Value>(*I));
    }
  }

  LLVM_DEBUG(dbgs() << "External Uses: " << ExternalUses.size() << "\n");
}

void LoopExtractionPass::eraseNestedBlocks(const Loop *L,
    std::vector<BasicBlock *> &AllBlocks, 
    std::map<BasicBlock *, 
    BasicBlock *> &BMap){
  for(Loop *SubL : L->getSubLoops()){ 
    for(BasicBlock *BB : SubL->getBlocks()){
      if(BMap.find(BB) == BMap.end()) continue;

      auto it = std::find(AllBlocks.begin(), AllBlocks.end(), BMap[BB]);
      if(it != AllBlocks.end()) AllBlocks.erase(it);
    }
  }
}

bool LoopExtractionPass::expandPHINodes(std::vector<Value *> &ExternalUses,
    ValueToValueMapTy &VMap){
  for(Value * V : ExternalUses){
    if(auto *PHI = dyn_cast<PHINode>(V)){
      assert(VMap.find(PHI) != VMap.end() && "Entry for PHI in VMap could not be found");

      if(!SE->isSCEVable(PHI->getType())) return false;

      SCEVExpander Expander(*SE, PHI->getModule()->getDataLayout(), "scev"); 

      PHINode *ClonedPHI = cast<PHINode>(VMap[PHI]);
      const SCEV *PHISCEV = SE->getSCEV(ClonedPHI);
      assert(PHISCEV && "scev is null in replaceWithArgs");

      Value *Expanded = Expander.expandCodeFor(
          PHISCEV, 
          ClonedPHI->getType(), 
          &*ClonedPHI->getParent()->getFirstInsertionPt()
      );

      if(isa<PoisonValue>(Expanded)){ 
        LLVM_DEBUG(dbgs() << "Didn't expand PHINode: poison value \n");
        return false;
      }
      
      if(ClonedPHI == Expanded){
        LLVM_DEBUG(dbgs() << "Didn't expand PHINode:" << *ClonedPHI << "\n");
        return false;
      }

      LLVM_DEBUG(dbgs() << "Replaced" << *ClonedPHI << "with" << *Expanded << "\n");
      ClonedPHI->replaceAllUsesWith(Expanded);
      ClonedPHI->eraseFromParent();

    }
  }
  
  return true;
}

BasicBlock* LoopExtractionPass::replaceWithArgs(Function* OriginalF, 
    Function* ClonedF, 
    std::vector<Value *> &ReplaceWithArgs,
    bool InductionReplaced){
  
  auto *A = ClonedF->arg_begin();
  if(InductionReplaced) A++;

  int PtrIndex = 0;
  std::vector<Use *> Uses;
  
  IRBuilder<> Builder(ClonedF->getContext());
  BasicBlock *Entry = &ClonedF->getEntryBlock(); 
  BasicBlock *LoadBlock = BasicBlock::Create(ClonedF->getContext(), "", ClonedF, Entry);
  Builder.SetInsertPoint(LoadBlock);

  Type *Int64Ty = IntegerType::getInt64Ty(ClonedF->getContext());
  Type *PtrTy = PointerType::getUnqual(OriginalF->getContext()); 
 
  std::vector<Value *> Idx;

  for(Value *ToReplace : ReplaceWithArgs){
    Type *ArgTy = ToReplace->getType();

    for(Use &U : ToReplace->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      const Function *UserFunc = UserInst->getFunction();
      if(ClonedF == UserFunc){
        Uses.push_back(&U);
      }
    }

    Value *Arg = &*A;
    Value *GEP, *Load;
    
    if(InductionReplaced){
      Idx.insert(Idx.end(), {ConstantInt::get(Int64Ty, PtrIndex, false)});
      GEP = Builder.CreateGEP(PtrTy, Arg, Idx);
      if(ArgTy != PtrTy){
        Load = Builder.CreateLoad(PtrTy, GEP, true); 
        Load = Builder.CreateLoad(ArgTy, Load, true); 
      } else {
        Load = Builder.CreateLoad(ArgTy, GEP, true); 
      }
      Idx.clear();
      PtrIndex++;
    }

    for(auto *Use : Uses){
      Use->set(InductionReplaced ? Load : Arg);
    }

    if(!InductionReplaced){
      InductionReplaced = true;
      A++;
    }

    Uses.clear();
  }

  Builder.CreateBr(Entry);
  return LoadBlock;
}

void LoopExtractionPass::replaceForeignUses(Function *F, 
    Function *ClonedF, 
    ValueToValueMapTy &VMap, 
    std::map<BasicBlock *, BasicBlock *> &BMap){
  
  std::vector<Use *> Uses;
  
  for(auto it : BMap){
    BasicBlock *BB = it.first;
    BasicBlock *ClonedBB = it.second;

    for(Use &U : BB->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      const Function *UserFunc = UserInst->getFunction();
      if(ClonedF == UserFunc){
        Uses.push_back(&U);
      }
    }

    for(auto *Use : Uses){
      Use->set(ClonedBB);
    }

    Uses.clear();
  }

  for(auto I = inst_begin(F), E = inst_end(F); I != E; I++){
    Value *ClonedValue = &cast<Value>(*I);
    Value *Replacement = VMap.count(ClonedValue) ? VMap[ClonedValue] : nullptr;
    if(!Replacement) continue;

    // Replace old BasicBlock references for PHINodes
    if(auto *SubIndVar = dyn_cast<PHINode>(Replacement)){
      assert(SubIndVar->getNumIncomingValues() == 2 && "SubIndVar does not have exactly two incoming values");
        BasicBlock *PHIBlock0 = SubIndVar->getIncomingBlock(0);
        BasicBlock *PHIBlock1 = SubIndVar->getIncomingBlock(1);
        //FIXME we should check if preds 
        BasicBlock *Replacement0 = 
          BMap.find(PHIBlock0) == BMap.end() ? &ClonedF->getEntryBlock() : BMap[PHIBlock0];

        BasicBlock *Replacement1 =   
          BMap.find(PHIBlock1) == BMap.end() ? &ClonedF->getEntryBlock() : BMap[PHIBlock1];

        SubIndVar->setIncomingBlock(0, Replacement0);
        SubIndVar->setIncomingBlock(1, Replacement1); 
      
    }

    for(Use &U : I->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      const Function *UserFunc = UserInst->getFunction();
      if(ClonedF == UserFunc){
        Uses.push_back(&U);
      }
    }

    for(auto *Use : Uses){
      Use->set(Replacement);
    } 

    Uses.clear();
  } 
}

Value* LoopExtractionPass::createStoresForArgs(Function* OriginalF, std::vector<Value *> &ReplaceWithArgs, bool InductionIncluded){
  int PtrIndex = 0;
  Type *Int64Ty = IntegerType::getInt64Ty(OriginalF->getContext());
  Type *PtrTy = PointerType::getUnqual(OriginalF->getContext()); 
  BasicBlock *Entry = &(OriginalF->getEntryBlock());
 
  Module *M = OriginalF->getParent();
  DataLayout Layout = M->getDataLayout();
  
  Function *Malloc = M->getFunction("__malloc");

  if(!Malloc){
    std::vector<Type *> ArgTy = {Int64Ty, Int64Ty};
 
    FunctionType *FuncType = FunctionType::get(
        PointerType::getUnqual(M->getContext()), 
        ArgTy, false);

    Malloc = Function::Create(FuncType,
        GlobalValue::ExternalLinkage, 
        "__malloc", M);
  }
  
  IRBuilder<> Builder(OriginalF->getContext());
  Builder.SetInsertPoint(Entry, Entry->getFirstInsertionPt());
  
  if((ReplaceWithArgs.size() - InductionIncluded) == 0){
    LLVM_DEBUG(dbgs() << "NO MALLOC NEEDED\n");
    return ConstantPointerNull::get(cast<PointerType>(PtrTy));
  }

  std::vector<Value *> Args = {
    ConstantInt::get(Int64Ty, Layout.getTypeAllocSize(PtrTy)),
    ConstantInt::get(Int64Ty, ReplaceWithArgs.size() - InductionIncluded)
  };

  CallInst* Alloca = Builder.CreateCall(Malloc, Args);

  Args.clear();

  std::vector<Value *> Idx;
  for(Value *V : ReplaceWithArgs){
    if(isa<PHINode>(V)) continue;
    
    //Idx.insert(Idx.end(), {ConstantInt::get(Int64Ty, 0), ConstantInt::get(Int64Ty, PtrIndex)});
    Idx.insert(Idx.end(), {ConstantInt::get(Int64Ty, PtrIndex)});
    if(auto *I = dyn_cast<Instruction>(V)){
      Builder.SetInsertPoint(I->getNextNode());
    } else {
      Builder.SetInsertPoint(Alloca->getNextNode());
    }
  
    Value *GEP = Builder.CreateGEP(PtrTy, Alloca, Idx); 

    Type *VTy = V->getType(); 
    
    if(VTy != PtrTy){
      Args.insert(Args.end(), {
        ConstantInt::get(Int64Ty, Layout.getTypeAllocSize(VTy)),
        ConstantInt::get(Int64Ty, 1)       
      });

      CallInst *Addr = Builder.CreateCall(Malloc, Args);
      Value *ValueStore = Builder.CreateStore(V, Addr);
      Value *AddrStore = Builder.CreateStore(Addr, GEP);
      LLVM_DEBUG(dbgs() << "CREATED MALLOC STORE FOR " << *V  << *Addr << *ValueStore << *AddrStore << "\n");

      Args.clear();
    } else {
      Builder.CreateStore(V, GEP);
      LLVM_DEBUG(dbgs() << "CREATED STORE FOR " << *V << "\n");
    }

    Idx.clear();
    PtrIndex++;
  }
  
  return Alloca;
}

void LoopExtractionPass::enqueueTask(Function *OriginalF, 
    BasicBlock *Preheader, 
    BasicBlock *Header, 
    BasicBlock *Succ, 
    PHINode *IndVar, 
    Function *ParallelBody, 
    Value *SequentialBody, 
    Value *RestOfFunc, 
    Loop::LoopBounds &Bounds, 
    Value *StoreAddr,
    Value *NewScopeAddr){

  Module *M = OriginalF->getParent();
  Function *EnqueueTask = M->getFunction("__enqueue_task");
  
  IRBuilder<> Builder(OriginalF->getContext());
  
  Type *PtrTy = PointerType::getUnqual(M->getContext());
  Type *I64Ty = IntegerType::getInt64Ty(M->getContext());

  if(!EnqueueTask){
    std::vector<Type *> EnqueueArgTy = {PtrTy, PtrTy, PtrTy, PtrTy, PtrTy, I64Ty, I64Ty, I64Ty};
 
    FunctionType *FuncType = FunctionType::get(Type::getInt1Ty(M->getContext()), EnqueueArgTy, false);
    EnqueueTask = Function::Create(FuncType, GlobalValue::ExternalLinkage, "__enqueue_task", M);
  }

  assert(EnqueueTask && "EnqueueTask is null");
 
  Instruction *Terminator = Preheader->getTerminator();
  Builder.SetInsertPoint(Terminator);
  
  Value *Initial = &(Bounds.getInitialIVValue());
  Value *Step = Bounds.getStepValue();
  Value *Final = &(Bounds.getFinalIVValue());

  if(auto *Int = dyn_cast<ConstantInt>(Bounds.getStepValue())){
    if(Int->getBitWidth() < 64){
      Initial = Builder.CreateIntCast(Initial, I64Ty, true);
      Step = Builder.CreateIntCast(Step, I64Ty, true);
      Final = Builder.CreateIntCast(Final, I64Ty, true);
    } else if(Int->getBitWidth() > 64){
      report_fatal_error("Induction variable is integer of >64 bits!");
    }
  } else {
    report_fatal_error("Induction variable is not ConstantInt!");
  }

  std::vector<Value *> Args = {
    ParallelBody,
    SequentialBody,
    RestOfFunc,
    StoreAddr,
    NewScopeAddr,
    Initial,
    Step,
    Final
  };

  Instruction* Return = Builder.CreateCall(EnqueueTask, Args);

  if(!LoopExtractionPass::isGenerated(OriginalF)){
    if(!isa<BranchInst>(Terminator)) report_fatal_error("Terminator is not BranchInst");
    Builder.CreateCondBr(Return, Succ, Header); 
  } else {
    Builder.CreateRetVoid();
  }
  
  Terminator->eraseFromParent();
}

Value *LoopExtractionPass::createNestedScope(Function *GeneratedF, 
    Function *SequentialBody, 
    Function *RestOfFunc, 
    std::vector<BasicBlock *> &LoopBlocks, 
    std::vector<Value *> &ReplaceWithArgs, 
    BasicBlock *Preheader,
    BasicBlock *Exit,
    BasicBlock *Succ){

  std::map<BasicBlock *, BasicBlock *> BMap;
  std::map<BasicBlock *, BasicBlock *> NewToOldBMap;

  std::vector<BasicBlock *> ToErase;

  ValueToValueMapTy ExtractedBodyVMap;
  ValueToValueMapTy RestOfFuncVMap;

  BasicBlock *ClonedExit;

  std::stack<BasicBlock *> BlockStack;

  std::set<BasicBlock *> Preds;
  std::set<BasicBlock *> Succs;
  Preds.insert(Preheader);
  BlockStack.push(Preheader);

  while(!BlockStack.empty()){
    BasicBlock *Top = BlockStack.top();
    BlockStack.pop();

    for(BasicBlock *BB : predecessors(Top)){
      if(Preds.count(BB)) {
        continue;
      } 

      Preds.insert(BB);
      BlockStack.push(BB);
    }
  }

  Succs.insert(Succ);
  BlockStack.push(Succ);

  while(!BlockStack.empty()){
    BasicBlock *Top = BlockStack.top();
    BlockStack.pop();

    Instruction *Terminator = Top->getTerminator();

    if(auto *Branch = dyn_cast<BranchInst>(Terminator)){
      for(auto It : Branch->successors()){
        if(Succs.count(It)) {
          continue;
        } 

        Succs.insert(It);
        BlockStack.push(It);
      }
    }
  }

  BasicBlock *ClonedSucc = CloneBasicBlock(Succ, RestOfFuncVMap);
  BMap[Succ] = ClonedSucc;
  ClonedSucc->insertInto(RestOfFunc);

  for(BasicBlock &it : *GeneratedF){
    BasicBlock *BB = &it;

    bool Pred = Preds.find(BB) != Preds.end(); 
    bool Successor = Succs.find(BB) != Succs.end(); 
    bool InLoop = std::count(LoopBlocks.begin(), LoopBlocks.end(), BB); 
    assert(!(Pred && Successor) && "BasicBlock both pred and succ");
  
    if(Pred) continue;

    if(InLoop){
      BasicBlock *ClonedBlock = CloneBasicBlock(BB, ExtractedBodyVMap);
      ClonedBlock->insertInto(SequentialBody);
     
      if(BB == Exit){
        ClonedExit = ClonedBlock;
      }

      BMap[BB] = ClonedBlock;
      NewToOldBMap[ClonedBlock] = BB;

      ToErase.push_back(BB);

    } else if(Successor && BB != Succ) {
      BasicBlock *ClonedBlock = CloneBasicBlock(BB, RestOfFuncVMap);
      BMap[BB] = ClonedBlock;
      NewToOldBMap[ClonedBlock] = BB;
      ClonedBlock->insertInto(RestOfFunc);
    }
    
  }

  assert(ClonedExit && "ClonedExit not found!");

  // Clean-up sequential loop
  stripDebugInfo(*SequentialBody);
  replaceWithArgs(GeneratedF, SequentialBody, ReplaceWithArgs);
  replaceForeignUses(GeneratedF, SequentialBody, ExtractedBodyVMap, BMap); 

  for(auto I = inst_begin(SequentialBody), E = inst_end(SequentialBody); I != E; I++){
    if(auto *Branch = dyn_cast<BranchInst>(&*I)){
      IRBuilder Builder(SequentialBody->getContext());
      if(Branch->getParent() == ClonedExit) {
        uint32_t BranchSucc = Branch->getSuccessor(0) == BMap[Succ] ? 0 : 1;

        BasicBlock *RetBlock = BasicBlock::Create(SequentialBody->getContext(), "", SequentialBody);
        Builder.SetInsertPoint(RetBlock);
        Builder.CreateRetVoid();

        Branch->setSuccessor(BranchSucc, RetBlock);
        continue;
      } 

      BasicBlock *TrueBB = Branch->getSuccessor(0);
      if(TrueBB->getParent() != SequentialBody){
        BasicBlock *TrueBBReplacement = nullptr;
        if(BMap.find(TrueBB) == BMap.end()){
          TrueBBReplacement = BasicBlock::Create(SequentialBody->getContext(), "", SequentialBody);
          Builder.SetInsertPoint(TrueBBReplacement);
          Builder.CreateRetVoid();
        } else {
          TrueBBReplacement = BMap[TrueBB];
        }
        Branch->setSuccessor(0, TrueBBReplacement);
      }

      if(Branch->getNumSuccessors() == 2){
        BasicBlock *FalseBB = Branch->getSuccessor(1);    

        if(FalseBB->getParent() != SequentialBody){
          BasicBlock *FalseBBReplacement = BMap.find(FalseBB) != BMap.end() ? BMap[FalseBB] : nullptr;
          assert(FalseBBReplacement && "No suitable block found for Branch, should be loop entry");

          Branch->setSuccessor(1, FalseBBReplacement);
        }
      }
    }
  }

  addGenerated(SequentialBody);
  LoopExtractionPass::PreservedFunctions.insert(SequentialBody);

  // Clean-up remainder of top-level loop body
  stripDebugInfo(*RestOfFunc);
  std::vector<BasicBlock *> Blocks;
  for(auto *BB : Succs){
    Blocks.push_back(BB);
  }

  std::vector<Value *> Replace;
  findExternalUses(*GeneratedF, Blocks, Replace, true);

  Value *NewScope = createStoresForArgs(GeneratedF, Replace, false);
  replaceWithArgs(GeneratedF, RestOfFunc, Replace, true);
  replaceForeignUses(GeneratedF, RestOfFunc, RestOfFuncVMap, BMap); 
  
  Value *ReplacementArgIt = RestOfFunc->arg_begin();
  std::vector<Use *> Uses;

  for(auto *A = GeneratedF->arg_begin(), *E = GeneratedF->arg_end(); A != E; A++){
    Value *ArgV = &cast<Value>(*A);
    Value *Replacement = &cast<Value>(*ReplacementArgIt);
    if(!Replacement) continue;
    
    for(Use &U : ArgV->uses()){
      Instruction *UserInst = cast<Instruction>(U.getUser());
      const Function *UserFunc = UserInst->getFunction();

      if(RestOfFunc == UserFunc){
        Uses.push_back(&U);
      }
    }

    for(auto *Use : Uses){
      Use->set(Replacement);
    } 

    Uses.clear(); 
    ReplacementArgIt++;
  }

  addGenerated(RestOfFunc);
 
  IRBuilder Builder(Preheader->getContext());
  ReplaceInstWithInst(Preheader->getTerminator(), Builder.CreateRetVoid());

  // Finally, remove extracted blocks from original function 
  for(BasicBlock *BB : ToErase){ 
    for(auto &I : *BB){
      I.dropAllReferences();   
    } 
  }

  for(BasicBlock *BB : ToErase){    
    // Uses could still remain as we don't delete successor blocks
    // Delete the successor blocks if they have no pred
    for(auto &I : *BB){
      for(Use &U : I.uses()){
        Instruction *UserInst = cast<Instruction>(U.getUser());
        assert(pred_empty(UserInst->getParent()) && "preds not empty");
        UserInst->dropAllReferences(); 
      }
    }
    BB->eraseFromParent();
  }
  
  verifyBody(SequentialBody);
  verifyBody(RestOfFunc);

  return NewScope;
}

void LoopExtractionPass::cloneLoopAndRemap(Function &F, const Loop *L){
  LLVM_DEBUG(dbgs() << "Extracting loop body found in " << F.getName() << "\n");
  Module *M = F.getParent();

  BasicBlock *Preheader = L->getLoopPreheader();
  assert(Preheader && "Canonical loop must have preheader");

  BasicBlock *Header = L->getHeader(); 
  assert(Header && "Canonical loop must have a header");

  BasicBlock *Exit = L->getExitingBlock();
  if(!Exit){  
    std::cout << "SKIPPING: MORE THAN 1 EXIT\n";
    return;
  }

  assert(Exit && "Canonical loop has more than one exit");
  
  BasicBlock *Succ = L->getExitBlock();
  assert(Succ && "Canonical loop has more than one successor");

  BasicBlock *ClonedExit = nullptr;

  PHINode *IndVar = L->getInductionVariable(*SE);
  //assert(IndVar && "Canonical loop must have induction variable");
  if(!IndVar) {
    std::cout << "SKIPPING: NO INDVAR\n";
    return;
  }

  std::optional<Loop::LoopBounds> BoundsOpt = L->getBounds(*SE);
  //assert(BoundsOpt && "No loop bounds found for loop");
  if(!BoundsOpt) {
    std::cout << "SKIPPING: NO BOUNDS\n";
    return;
  }

  if(!isa<ConstantInt>(BoundsOpt->getStepValue())){
    std::cout << "SKIPPING: INDVAR NOT CONSTANTINT";
    return;
  }

  std::vector<BasicBlock *> LoopBlocks = L->getBlocks();

  std::map<BasicBlock *, BasicBlock *> BMap;

  // Find function arg types
  std::vector<Value *> ReplaceWithArgs;
  
  // Create new function
  PointerType *PtrTy = PointerType::getUnqual(M->getContext()); 
  std::vector<Type *> FuncTypes = {IndVar->getType(), PtrTy};

  FunctionType *FuncType = FunctionType::get(Type::getVoidTy(M->getContext()), FuncTypes, false);
  Function *ExtractedBody = Function::Create(FuncType, GlobalValue::ExternalLinkage, F.getName() + "ParallelLoopBody", M);    
 
  // Initialise Function pointers in case we are looking at a nested loop
  Value *SequentialBody = ConstantPointerNull::get(PtrTy);
  Value *RestOfFunc = ConstantPointerNull::get(PtrTy);
  Value *NewScope = ConstantPointerNull::get(PtrTy);

  if(isGenerated(&F)){
    SequentialBody = Function::Create(FuncType, GlobalValue::ExternalLinkage, F.getName() + "SequentialLoopBody", M);    
    RestOfFunc = Function::Create(F.getFunctionType(), GlobalValue::ExternalLinkage, F.getName() + "ContinuedLoopBody", M);
  }

  // Clone and insert loop into new function
  ValueToValueMapTy VMap;
  std::vector<BasicBlock *> TopLevelBlocks;
  
  for(BasicBlock *BB : LoopBlocks){
    BasicBlock *ClonedBlock = CloneBasicBlock(BB, VMap);
    BMap[BB] = ClonedBlock;

    if(BB == Exit){
      ClonedExit = ClonedBlock;
    }

    TopLevelBlocks.push_back(ClonedBlock);
    ClonedBlock->insertInto(ExtractedBody);
  }

  assert(ClonedExit && "Failed to find exit for loop after cloning"); 
 
  eraseNestedBlocks(L, TopLevelBlocks, BMap);

  findPHINodesForLoop(L, IndVar, ReplaceWithArgs);
  if(!expandPHINodes(ReplaceWithArgs, VMap)) {
    ExtractedBody->eraseFromParent();
    if(isGenerated(&F)){
      cast<Function>(RestOfFunc)->eraseFromParent();
      cast<Function>(SequentialBody)->eraseFromParent();
    }
    return;
  }

  ReplaceWithArgs = {IndVar};
  findExternalUses(F, LoopBlocks, ExtractedBody, ReplaceWithArgs); 

  stripDebugInfo(*ExtractedBody);  

  // Replace our outdated uses with valid ones
  replaceWithArgs(&F, ExtractedBody, ReplaceWithArgs);
  replaceForeignUses(&F, ExtractedBody, VMap, BMap); 

  // Find our terminator instruction to replace with ret void
  IRBuilder<> Builder(ExtractedBody->getContext());
  
  BranchInst *B = cast<BranchInst>(ClonedExit->getTerminator()); 
  ReplaceInstWithInst(B, Builder.CreateRetVoid()); 

  // Remove IndVar as we provide that as an arg
  cast<Instruction>(VMap[IndVar])->eraseFromParent();  
 
  // Create stores for pointers to external values
  Value* StoreAddr = createStoresForArgs(&F, ReplaceWithArgs);

  if(LoopExtractionPass::isGenerated(&F)){
    NewScope = createNestedScope(&F, 
        cast<Function>(SequentialBody), 
        cast<Function>(RestOfFunc), 
        LoopBlocks, 
        ReplaceWithArgs, 
        Preheader,
        Exit,
        Succ);
  }

  // Clean-up blocks with no preds
  std::vector<BasicBlock *> ToErase;
  for(BasicBlock &BB : F){
    if(!pred_empty(&BB) || &BB == &F.getEntryBlock()) continue;
    
    ToErase.push_back(&BB); 
    for(Instruction &I : BB){
      I.dropAllReferences();
    }
  }

  for(BasicBlock *BB: ToErase){
    BB->eraseFromParent();
  }

  // Tell API to enqueue a new job
  enqueueTask(&F, 
      Preheader, 
      Header, 
      Succ, 
      IndVar, 
      ExtractedBody, 
      SequentialBody, 
      RestOfFunc, 
      *BoundsOpt, 
      StoreAddr,
      NewScope);

  verifyBody(ExtractedBody);

  addGenerated(ExtractedBody);
}

PreservedAnalyses LoopExtractionPass::run(Function &F, FunctionAnalysisManager &AM) {
  if(!EnableExtraction) return PreservedAnalyses::all();

  LI = &AM.getResult<LoopAnalysis>(F);

  if(isPreserved(&F)){
    return PreservedAnalyses::all();
  }

  // If no loops then no work to do, so return 
  if(LI->empty()){
    LLVM_DEBUG(dbgs() << "No loops in function " << F.getName() << "\n");
    return PreservedAnalyses::all();
  }

  SE = &AM.getResult<ScalarEvolutionAnalysis>(F); 
  DT = &AM.getResult<DominatorTreeAnalysis>(F);

  trySimplifyLoops();

  for(const auto L: *LI){
    if(!L->isLoopSimplifyForm()){ 
      LLVM_DEBUG(dbgs() << "Non-simplified loop in " << F.getName() << ", skipping\n");
      continue;
    }

    if(!L->isOutermost()){
      LLVM_DEBUG(dbgs() << "Not outermost loop, skipping\n");
      continue;
    }

    cloneLoopAndRemap(F, L);
  }

  return PreservedAnalyses::none();
}

llvm::PassPluginLibraryInfo getLoopExtractionPluginInfo(){
  return {LLVM_PLUGIN_API_VERSION, "LoopExtraction", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerVectorizerStartEPCallback(
                [](llvm::FunctionPassManager &PM, OptimizationLevel Level){
                  PM.addPass(LoopExtractionPass());
                }); 
            PB.registerPipelineParsingCallback(
                [](StringRef Name, llvm::FunctionPassManager &PM,
                   ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "loop-extraction") {
                    PM.addPass(LoopExtractionPass());
                    return true;
                  }
                  return false;
                });
          }};
}

#ifndef LLVM_LOOPEXTRACTION_LINK_INTO_TOOLS
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo()  {
  return getLoopExtractionPluginInfo();
}
#endif
