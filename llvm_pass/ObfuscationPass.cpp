#include "ObfuscationPass.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Type.h"
#include <random>
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {
class ObfuscationLegacyPass : public ModulePass {
public:
  static char ID;
  obf::ObfuscationOptions Options;
  unsigned stats_bogus_blocks = 0;
  unsigned stats_strings_obf = 0;
  unsigned stats_nops = 0;
  unsigned stats_fake_loops = 0;

  ObfuscationLegacyPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    // Read options from module metadata (simple approach)
    parseOptionsFromModule(M);

    for (Function &F : M) {
      if (F.isDeclaration()) continue;
      if (F.getName().starts_with("llvm.")) continue;
      runOnFunction(F);
    }

    runStringObfuscation(M);

    // Print summary to stderr so driver can capture
    errs() << "ObfuscationPass: bogus_blocks=" << stats_bogus_blocks
           << " strings=" << stats_strings_obf
           << " nops=" << stats_nops
           << " fake_loops=" << stats_fake_loops << "\n";

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
    if (GlobalVariable *gv = M.getGlobalVariable("obf_flatten")) {
      if (ConstantInt *CI = dyn_cast<ConstantInt>(gv->getInitializer())) {
        Options.enableFlatten = CI->isOne();
      }
    }
  }

  void runOnFunction(Function &F) {
    // Insert bogus blocks
    for (unsigned i = 0; i < Options.bogusBlocksPerFunction; ++i)
      insertBogusBlock(F);
    // Insert cheap NOPs (fake instructions)
    if (Options.insertNops) insertNopSequences(F, Options.insertNops);
    if (Options.enableFlatten) {
      // Lightweight fake loop as a minimal flattening surrogate
      // Note: kept conservative to avoid IR verifier issues across LLVM 20
      insertFakeLoopOnce(F);
      ++stats_fake_loops;
    }
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
    Constant *fnPtr = ConstantExpr::getBitCast(&F, Type::getInt8Ty(C)->getPointerTo());
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

  // Wrap entry block instructions into a loop that executes exactly once
  // to introduce additional control flow without altering semantics.
  void insertFakeLoopOnce(Function &F) {
    if (F.isDeclaration()) return;
    BasicBlock &entry = F.getEntryBlock();
    Instruction *first = entry.getFirstNonPHI();
    if (!first) return;

    LLVMContext &C = F.getContext();
    IRBuilder<> B(&entry);

    // Create blocks: pre (existing entry up to branch), loop, body, exit
    BasicBlock *loopHdr = BasicBlock::Create(C, F.getName() + ".obf.loop", &F);
    BasicBlock *body = BasicBlock::Create(C, F.getName() + ".obf.body", &F);
    BasicBlock *exitB = BasicBlock::Create(C, F.getName() + ".obf.exit", &F);

    // Split entry so that 'first' becomes start of continuation
    BasicBlock *cont = entry.splitBasicBlock(first, F.getName() + ".obf.cont");

    // Rebuild terminator of original 'entry' to branch to loop header
    entry.getTerminator()->eraseFromParent();
    IRBuilder<> Be(&entry);
    Be.CreateBr(loopHdr);

    // Loop header with a predicate that is true exactly once
    IRBuilder<> Bh(loopHdr);
    // Create an induction variable in an alloca to avoid SSA PHIs for simplicity
    AllocaInst *iv = new AllocaInst(Type::getInt32Ty(C), 0, "obf_iv", &*cont->getFirstInsertionPt());
    Bh.CreateStore(ConstantInt::get(Type::getInt32Ty(C), 0), iv);
    Value *ivLoad = Bh.CreateLoad(Type::getInt32Ty(C), iv);
    Value *cond = Bh.CreateICmpEQ(ivLoad, ConstantInt::get(Type::getInt32Ty(C), 0));
    Bh.CreateCondBr(cond, body, exitB);

    // Move original 'cont' block instructions into 'body' safely
    IRBuilder<> Bb(body);
    while (!cont->empty()) {
      Instruction &I = cont->front();
      if (Instruction *Term = body->getTerminator()) {
        I.moveBefore(Term);
      } else {
        I.moveBefore(*body, body->end());
      }
    }
    // At end of body, set iv=1 and jump back to header
    IRBuilder<> Bend(&body->back());
    // Ensure last instruction is a terminator; if not, append before terminator creation
    if (!body->getTerminator()) {
      Bend.SetInsertPoint(body);
    } else {
      Bend.SetInsertPoint(body->getTerminator());
    }
    Bend.CreateStore(ConstantInt::get(Type::getInt32Ty(C), 1), iv);
    Bend.CreateBr(loopHdr);

    // Exit block branches to the (now empty) cont block successor (which was moved)
    IRBuilder<> BeX(exitB);
    BeX.CreateBr(cont);
  }

  void runStringObfuscation(Module &M) {
    LLVMContext &C = M.getContext();
    std::vector<GlobalVariable*> toReplace;
    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasInitializer()) continue;
      if (GV.getValueType()->isArrayTy()) {
        if (GV.getName().starts_with("str.")) {
          toReplace.push_back(&GV);
        }
      }
    }

    // Prepare an init function to decrypt strings at startup
    Function *initF = nullptr;
    IRBuilder<> *initBuilder = nullptr;

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
        GlobalVariable *gEnc = new GlobalVariable(M, arrTy, /*isConstant*/false, GlobalValue::PrivateLinkage, newInit, GV->getName() + ".enc");
        // Replace original GV with pointer to encrypted global
        GV->replaceAllUsesWith(ConstantExpr::getBitCast(gEnc, GV->getType()));
        stats_strings_obf++;

        // Lazily create/init builder and function
        if (!initF) {
          FunctionType *FT = FunctionType::get(Type::getVoidTy(C), false);
          initF = Function::Create(FT, GlobalValue::InternalLinkage, "__obf_init", M);
          BasicBlock *ib = BasicBlock::Create(C, "entry", initF);
          initBuilder = new IRBuilder<>(ib);
        }
        // Emit a simple loop to XOR-decrypt in-place at startup
        // i = 0
        AllocaInst *idx = initBuilder->CreateAlloca(Type::getInt32Ty(C));
        initBuilder->CreateStore(ConstantInt::get(Type::getInt32Ty(C), 0), idx);
        BasicBlock *loop = BasicBlock::Create(C, "dec.loop", initF);
        BasicBlock *after = BasicBlock::Create(C, "dec.after", initF);
        initBuilder->CreateBr(loop);
        IRBuilder<> lb(loop);
        Value *iv = lb.CreateLoad(Type::getInt32Ty(C), idx);
        Value *cond = lb.CreateICmpULT(iv, ConstantInt::get(Type::getInt32Ty(C), (unsigned)s.size()));
        BasicBlock *body = BasicBlock::Create(C, "dec.body", initF);
        lb.CreateCondBr(cond, body, after);
        IRBuilder<> bb(body);
        // ptr = &gEnc[iv]
        Value *zero = ConstantInt::get(Type::getInt32Ty(C), 0);
        Value *iv64 = bb.CreateZExt(iv, Type::getInt64Ty(C));
        Value *gep = bb.CreateInBoundsGEP(gEnc->getValueType(), gEnc, {zero, bb.CreateTruncOrBitCast(iv64, Type::getInt32Ty(C))});
        Value *ch = bb.CreateLoad(Type::getInt8Ty(C), gep);
        Value *k = ConstantInt::get(Type::getInt8Ty(C), key);
        // key + (i & 0xFF)
        Value *ioff = bb.CreateTrunc(iv, Type::getInt8Ty(C));
        Value *k2 = bb.CreateAdd(k, ioff);
        Value *dec = bb.CreateXor(ch, k2);
        bb.CreateStore(dec, gep);
        // i++
        Value *inc = bb.CreateAdd(iv, ConstantInt::get(Type::getInt32Ty(C), 1));
        bb.CreateStore(inc, idx);
        bb.CreateBr(loop);
        IRBuilder<> ab(after);
        // continue; will be connected by next emission or finish below
      }
    }

    // If we created an init function, finish it and register in global_ctors
    if (initF) {
      IRBuilder<> endB(&initF->back());
      if (!initF->back().getTerminator())
        endB.CreateRetVoid();
      // Add to llvm.global_ctors with priority 65535
      Type *i32Ty = Type::getInt32Ty(C);
      Type *voidPtrTy = PointerType::getUnqual(Type::getInt8Ty(C));
      StructType *CtorTy = StructType::get(i32Ty, initF->getType()->getPointerTo(), voidPtrTy);
      Constant *Ctor = ConstantStruct::get(CtorTy, ConstantInt::get(i32Ty, 65535), initF, ConstantPointerNull::get(cast<PointerType>(voidPtrTy)));
      ArrayType *AT = ArrayType::get(CtorTy, 1);
      GlobalVariable *GVctors = M.getGlobalVariable("llvm.global_ctors");
      if (!GVctors) {
        GVctors = new GlobalVariable(M, AT, false, GlobalValue::AppendingLinkage, nullptr, "llvm.global_ctors");
      }
      GVctors->setInitializer(ConstantArray::get(AT, {Ctor}));
    }
  }

  // (optional) more helpers...
};
}

char ObfuscationLegacyPass::ID = 0;
static RegisterPass<ObfuscationLegacyPass> X("obf-legacy", "Simple Obfuscation Pass", false, false);

namespace {
class ObfuscationModernPass : public PassInfoMixin<ObfuscationModernPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    ObfuscationLegacyPass L;
    L.runOnModule(M);
    return PreservedAnalyses::none();
  }
};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "obfpass", "0.1",
      [](PassBuilder &PB) {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "obf-legacy") {
                MPM.addPass(ObfuscationModernPass());
                return true;
              }
              return false;
            });
      }};
}
