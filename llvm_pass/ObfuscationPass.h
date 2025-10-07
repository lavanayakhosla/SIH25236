#ifndef OBFUSCATION_PASS_H
#define OBFUSCATION_PASS_H

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"

using namespace llvm;

namespace obf {
struct ObfuscationOptions {
  unsigned bogusBlocksPerFunction = 1;
  unsigned stringEncryptLevel = 1;
  unsigned insertNops = 0;
  bool enableFlatten = false;
};
}

#endif
