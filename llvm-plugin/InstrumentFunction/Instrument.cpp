#include "Instrument.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "llvm/Support/CommandLine.h"
#include <iostream>

#define DEBUG_TYPE "instrument-function"
using namespace llvm;

std::map<Function *, Function *> InstrumentFunctionPass::FunctionMap;

void InstrumentFunctionPass::collectCalledFunctions(Function *F){
  for(auto I = inst_begin(F), E = inst_end(F); I != E; I++){
    if(auto *Call = dyn_cast<CallInst>(&*I)){
      Function *Callee = Call->getCalledFunction(); 
      if(Generated.find(Callee) != Generated.end()) continue;

      bool defined = Callee->begin() != Callee->end(); 

      if(!defined){
        LLVM_DEBUG(dbgs() << Callee->getName() << " not defined, skipping\n");
        continue;
      }

      assert(Callee && "Getting called function returned nullptr");
      auto it = FunctionMap.find(Callee);
      if(it != FunctionMap.end()){
        Call->setCalledFunction((*it).second);
        continue;
      }
 
      ValueToValueMapTy VMap;
      Function *New = CloneFunction(Callee, VMap);
      assert(New && "Cloning function returned nullptr");

      FunctionMap[Callee] = New;
      Call->setCalledFunction(New);

      InstrumentStack.push(New);
    }
  }
}

void InstrumentFunctionPass::instrumentFunction(Function *F){
  LLVM_DEBUG(dbgs() << "instrumenting " << F->getName() << "\n");
  addVersioningAndConflictDetection(F);
  Instrumented.insert(F);
  collectCalledFunctions(F); 
}

void InstrumentFunctionPass::addVersioningAndConflictDetection(Function *F){
  Module *M = F->getParent();
  DataLayout Layout = M->getDataLayout();

  IRBuilder<> Builder(F->getContext());

  Type *PtrTy = PointerType::getUnqual(M->getContext());
  Type *I64Ty = IntegerType::getInt64Ty(M->getContext());

  Function *GetShadowPtr = M->getFunction("__check_write_conflict");
  Function *CheckLoadConflict = M->getFunction("__check_load_conflict");
  Function *Malloc = M->getFunction("__malloc");

  bool GeneratedF = Generated.find(F) != Generated.end();
  if(F == GetShadowPtr || F == CheckLoadConflict || F == Malloc){
    return;
  }

  if(!GetShadowPtr){
    std::vector<Type *> ArgTy = {PtrTy, I64Ty};
 
    FunctionType *FuncType = FunctionType::get(
        PointerType::getUnqual(M->getContext()), 
        ArgTy, false);

    GetShadowPtr = Function::Create(FuncType,
        GlobalValue::ExternalLinkage, 
        "__check_write_conflict", M);
  }
  
  if(!CheckLoadConflict){
    std::vector<Type *> ArgTy = {PtrTy};
 
    FunctionType *FuncType = FunctionType::get(
        PointerType::getUnqual(M->getContext()), 
        ArgTy, 
        false);

    CheckLoadConflict = Function::Create(
        FuncType, 
        GlobalValue::ExternalLinkage, 
        "__check_load_conflict", M);
  }
  
  std::vector<Value *> Args;
  
  auto *A = F->arg_begin(); 
  A++;
  
  Value *Ptr = A++;
  for(auto I = inst_begin(F), E = inst_end(F); I != E; I++){
    if(auto *Store = dyn_cast<StoreInst>(&*I)){
      if(GeneratedF){
        Value *StorePtr = Store->getPointerOperand();
        if(auto *Call = dyn_cast<CallInst>(StorePtr)){
          Function *Callee = Call->getCalledFunction();
          if(Callee == Malloc) continue;
        }
      }
      Builder.SetInsertPoint(Store);
      Value* ValueOp = Store->getValueOperand();
      
      Args.insert(Args.end(), {
          Store->getPointerOperand(), 
          ConstantInt::get(I64Ty, Layout.getTypeAllocSize(ValueOp->getType()))
      });

      Builder.CreateCall(GetShadowPtr, Args);
    } else if(auto *Load = dyn_cast<LoadInst>(&*I)) {
      if(GeneratedF){
        if(Load->getPointerOperand() == Ptr) continue;
        else if(auto *Deref = dyn_cast<LoadInst>(Load->getPointerOperand())){
          if(Deref->getPointerOperand() == Ptr) continue;
        } else if(auto *Deref = dyn_cast<GetElementPtrInst>(Load->getPointerOperand())){
          if(Deref->getPointerOperand() == Ptr) continue;
        }
      }
      Builder.SetInsertPoint(Load);

      Args.insert(Args.end(), {
          Load->getPointerOperand()
      });

      Builder.CreateCall(CheckLoadConflict, Args);
    }
    Args.clear();
  } 
}

PreservedAnalyses InstrumentFunctionPass::run(Module &M, ModuleAnalysisManager &AM){
  NamedMDNode *NMD = M.getNamedMetadata("GeneratedFunctions");
  if(!NMD) return PreservedAnalyses::all();

  for(MDNode *Op : NMD->operands()){
    auto *FName = cast<MDString>(Op->getOperand(0));
    Function *F = M.getFunction(FName->getString());
    if(!F) {
      continue;
    }
    Generated.insert(F);
  }
  
  LLVM_DEBUG(dbgs() << "Number of generated functions: " << Generated.size() << "\n");
  for(Function *F : Generated){
    instrumentFunction(F); 
  }

  while(!InstrumentStack.empty()){
    Function *F = InstrumentStack.top();
    InstrumentStack.pop();
    if(Instrumented.find(F) == Instrumented.end()) instrumentFunction(F);
  }

  return PreservedAnalyses::none();
}

llvm::PassPluginLibraryInfo getInstrumentFunctionPluginInfo(){
return {LLVM_PLUGIN_API_VERSION, "InstrumentFunction", LLVM_VERSION_STRING,
        [](PassBuilder &PB) { 
          PB.registerFullLinkTimeOptimizationLastEPCallback(
              [](llvm::ModulePassManager &PM, OptimizationLevel Level){
                PM.addPass(InstrumentFunctionPass());
              });
          PB.registerPipelineParsingCallback(
              [](StringRef Name, llvm::ModulePassManager &PM,
                 ArrayRef<llvm::PassBuilder::PipelineElement>) {
                if (Name == "instrument-function") {
                  PM.addPass(InstrumentFunctionPass());
                  return true;
                }
                return false;
              });
        }};
}

#ifndef LLVM_INSTRUMENTFUNCTION_LINK_INTO_TOOLS
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo()  {
  return getInstrumentFunctionPluginInfo();
}
#endif
