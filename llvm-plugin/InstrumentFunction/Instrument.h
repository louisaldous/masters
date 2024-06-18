#ifndef LLVM_PLUGIN_INSTRUMENTFUNCTION_H
#define LLVM_PLUGIN_INSTRUMENTFUNCTION_H

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include <set>
#include <stack>
#include <map>

namespace llvm{
class Function;
class Module;

class InstrumentFunctionPass : public PassInfoMixin<InstrumentFunctionPass>{

protected:
  static std::map<Function *, Function *> FunctionMap;

  std::stack<Function *> InstrumentStack;
  std::set<Function *> Generated;
  std::set<Function *> Instrumented;

public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

protected:
  void collectCalledFunctions(Function *F);
  void instrumentFunction(Function *F);
  void addVersioningAndConflictDetection(Function *F);
};
}
#endif
