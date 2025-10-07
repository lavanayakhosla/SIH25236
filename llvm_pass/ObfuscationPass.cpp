#include "ObfuscationPass.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"
#include <random>

using namespace llvm;

namespace {
class ObfuscationLegacyPass : public ModulePass {
public:
  static char ID;
  obf::ObfuscationOptions Options;
  unsigned stats_bogus_blocks = 0;
  unsigned stats_strings_obf = 0;
  unsigned stats_nops = 0;

  ObfuscationLegacyPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    // Read options from module metadata (simple approach)
    parseOptionsFromModule(M);

    for (Function &F : M) {
      if (F.isDeclaration()) continue;
      if (F.getName().startswith("llvm.")) continue;
      runOnFunction(F);
    }

    runStringObfuscation(M);

    // Print summary to stderr so driver can capture
    errs() << "ObfuscationPass: bogus_blocks=" << stats_bogus_blocks
           << " strings=" << stats_strings_obf
           << " nops=" << stats_nops << "\n";

    return true;
  }

  void parseOptionsFromModule(Module &M) {
    // Very small: If module contains a global named "obf.options" interpreted as int fields
    if (GlobalVariable *gv = M.getGlobalVariable("obf_bogus_blocks")) {
      if (ConstantInt *CI = dyn_cast<ConstantInt>(gv->getInitializer())) {
        Options.bogusBlocksPerFunction = (unsigned)CI->getZExtValue();
      }
    }
    if (GlobalVariable *gv = M.getGlobalVariable("obf_string_level")) {
      if (ConstantInt *CI = dyn_cast<ConstantInt>(gv->getInitializer())) {
        Options.stringEncryptLevel = (unsigned)CI->getZExtValue();
      }
    }
    if (GlobalVariable *gv = M.getGlobalVariable("obf_insert_nops")) {
      if (ConstantInt *CI = dyn_cast<ConstantInt>(gv->getInitializer())) {
        Options.insertNops = (unsigned)CI->getZExtValue();
      }
    }
  }

  void runOnFunction(Function &F) {
    // Insert bogus blocks
    for (unsigned i = 0; i < Options.bogusBlocksPerFunction; ++i)
      insertBogusBlock(F);
    // Insert cheap NOPs (fake instructions)
    if (Options.insertNops) insertNopSequences(F, Options.insertNops);
  }

  void insertBogusBlock(Function &F) {
    // Find a basic block to split (choose entry->first)
    BasicBlock &BB = F.getEntryBlock();
    Instruction *first = BB.getFirstNonPHI();
    if (!first) return;

    LLVMContext &C = F.getContext();
    IRBuilder<> B(first);

    // Create opaque predicate using current time (not constant-foldable) or use llvm.cpu.feature? For simplicity, use rand via global.
    // We'll create: if ( (ptrtoint (fnptr) & 0xFF) == 0xAB ) goto bogus else continue
    Constant *fnPtr = ConstantExpr::getBitCast(&F, Type::getInt8PtrTy(C));
    Value *intVal = B.CreatePtrToInt(fnPtr, Type::getInt64Ty(C));
    Value *masked = B.CreateAnd(intVal, ConstantInt::get(Type::getInt64Ty(C), 0xFF));
    Value *cmp = B.CreateICmpEQ(masked, ConstantInt::get(Type::getInt64Ty(C), 0xAB));

    BasicBlock *cont = BB.splitBasicBlock(first, F.getName() + "_cont");
    BasicBlock *bogus = BasicBlock::Create(C, F.getName() + "_bogus", &F, cont);

    // Replace the unconditional branch at end of BB (split) with conditional branch
    // Remove the original terminator from BB (it was moved to cont), and create conditional branch
    BB.getTerminator()->eraseFromParent();
    IRBuilder<> B2(&BB);
    B2.CreateCondBr(cmp, bogus, cont);

    // Fill bogus with weird instructions and return or jump to cont
    IRBuilder<> Bb(bogus);
    // produce some unreachable-looking operations
    Function *printfFn = nullptr;
    // simple arithmetic
    Value *a = Bb.CreateAlloca(Type::getInt32Ty(C));
    Bb.CreateStore(ConstantInt::get(Type::getInt32Ty(C), 0xDEADBEEF), a);
    Value *ld = Bb.CreateLoad(Type::getInt32Ty(C), a);
    Value *xorv = Bb.CreateXor(ld, ConstantInt::get(Type::getInt32Ty(C), 0xFEED));
    Bb.CreateStore(xorv, a);
    // branch to cont
    Bb.CreateBr(cont);

    ++stats_bogus_blocks;
  }

  void insertNopSequences(Function &F, unsigned count) {
    LLVMContext &C = F.getContext();
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        // Insert after instruction
        if (count == 0) break;
        IRBuilder<> B(&I);
        // create a faux operation: tmp = add i32 0, 0
        Value *tmp = B.CreateAdd(ConstantInt::get(Type::getInt32Ty(C), 0),
                                 ConstantInt::get(Type::getInt32Ty(C), 0));
        (void)tmp; // unused
        stats_nops++;
        --count;
      }
    }
  }

  void runStringObfuscation(Module &M) {
    LLVMContext &C = M.getContext();
    std::vector<GlobalVariable*> toReplace;
    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasInitializer()) continue;
      if (GV.getType()->getElementType()->isArrayTy()) {
        if (GV.getName().startswith("str.")) {
          toReplace.push_back(&GV);
        }
      }
    }

    for (GlobalVariable *GV : toReplace) {
      Constant *init = GV->getInitializer();
      if (ConstantDataArray *CDA = dyn_cast<ConstantDataArray>(init)) {
        if (!CDA->isCString()) continue;
        StringRef s = CDA->getAsCString();
        std::string enc;
        enc.reserve(s.size());
        uint8_t key = (uint8_t)(Options.stringEncryptLevel * 37 + 13);
        for (unsigned i = 0; i < s.size(); ++i) {
          enc.push_back((char)(s[i] ^ (key + (i & 0xFF))));
        }
        // Create new global with encrypted bytes
        ArrayType *arrTy = ArrayType::get(IntegerType::get(C, 8), enc.size()+1);
        Constant *newInit = ConstantDataArray::getString(C, StringRef(enc), true);
        GlobalVariable *gEnc = new GlobalVariable(M, arrTy, true, GlobalValue::PrivateLinkage, newInit, GV->getName() + ".enc");
        // Replace uses: create runtime decryptor function that returns char*
        FunctionType *decFT = FunctionType::get(Type::getInt8PtrTy(C), false);
        Function *decF = cast<Function>(M.getOrInsertFunction("__obf_decrypt", decFT).getCallee());

        // create a call site to decrypt in global constructors: easier approach - change uses in functions
        GV->replaceAllUsesWith(ConstantExpr::getBitCast(gEnc, GV->getType()));
        stats_strings_obf++;
      }
    }
  }

  // (optional) more helpers...
};
}

char ObfuscationLegacyPass::ID = 0;
static RegisterPass<ObfuscationLegacyPass> X("obf-legacy", "Simple Obfuscation Pass", false, false);
