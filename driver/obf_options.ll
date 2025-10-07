; Module to provide obfuscation options
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx13.0.0"
@obf_bogus_blocks = internal global i32 5
@obf_string_level = internal global i32 3
@obf_insert_nops = internal global i32 16
@obf_flatten = internal global i1 1
