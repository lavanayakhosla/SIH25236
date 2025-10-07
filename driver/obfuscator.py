#!/usr/bin/env python3
import os
import subprocess
import json
import sys
import shutil
import argparse
import datetime
from tabulate import tabulate

LLVM_OPT = os.environ.get("LLVM_OPT","opt")
CLANG = os.environ.get("CLANG","clang")
LLC = os.environ.get("LLC","llc")
LD = os.environ.get("LD","ld")
CLANGXX = os.environ.get("CLANGXX","clang++")

def run(cmd, cwd=None, capture=False):
    print("> " + " ".join(cmd))
    if capture:
        return subprocess.check_output(cmd, cwd=cwd).decode()
    else:
        subprocess.check_call(cmd, cwd=cwd)

def compile_to_bc(src, out_bc, target=None):
    cmd = [CLANG, "-O1", "-emit-llvm", "-c", src, "-o", out_bc]
    if target:
        cmd.insert(1, "--target="+target)
    run(cmd)

def apply_pass(in_bc, out_bc, pass_plugin, options):
    # We'll set module global variables as options for the pass to read
    temp_bc = in_bc
    # build mod options small bitcode patch: easier approach, create small LLVM IR file with globals then link
    # create options.ll
    opt_ll = "obf_options.ll"
    with open(opt_ll, "w") as f:
        f.write("; Module to provide obfuscation options\n")
        f.write("@obf_bogus_blocks = global i32 %d\n" % options['bogus_blocks'])
        f.write("@obf_string_level = global i32 %d\n" % options['string_level'])
        f.write("@obf_insert_nops = global i32 %d\n" % options['insert_nops'])
    # compile options.ll to bc
    run(["llvm-as", opt_ll, "-o", "obf_options.bc"])
    # link the two bcs
    run(["llvm-link", temp_bc, "obf_options.bc", "-o", "linked.bc"])
    # run opt with pass plugin
    cmd = [LLVM_OPT, "-load-pass-plugin", pass_plugin, "-passes=obf-legacy", "linked.bc", "-o", out_bc]
    # Some LLVM builds accept -load <plugin.so> and pass name; new ones use -load-pass-plugin
    run(cmd)
    return

def bc_to_obj(bc, obj, mcpu=None):
    cmd = [LLC, "-filetype=obj", bc, "-o", obj]
    if mcpu:
        cmd.insert(1, "-mcpu="+mcpu)
    run(cmd)

def link_objects(objs, out_exe, linker_args=[]):
    cmd = [CLANGXX] + objs + ["-o", out_exe] + linker_args
    run(cmd)

def gather_stats(stdout_text):
    # parse stats from opt stderr if available - our pass writes to stderr
    stats = {}
    for line in stdout_text.splitlines():
        if line.startswith("ObfuscationPass:"):
            parts = line.split()
            for part in parts[1:]:
                if '=' in part:
                    k,v = part.split('=',1)
                    stats[k] = int(v)
    return stats

def generate_report(report_path, params, out_file, stats, final_size):
    report = {
      "timestamp": datetime.datetime.utcnow().isoformat() + "Z",
      "input_parameters": params,
      "output": {
         "file": out_file,
         "size_bytes": final_size,
      },
      "obfuscation_stats": stats
    }
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    # pretty
    print("\n=== Obfuscation Report ===")
    print(json.dumps(report, indent=2))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("src", help="C/C++ source file")
    parser.add_argument("--out", default="a.out", help="Output binary name")
    parser.add_argument("--pass", required=False, default="../llvm_pass/obfpass.so", help="Path to compiled pass plugin (opt plugin)")
    parser.add_argument("--bogus-blocks", type=int, default=1)
    parser.add_argument("--string-level", type=int, default=1)
    parser.add_argument("--insert-nops", type=int, default=0)
    parser.add_argument("--target", choices=["linux","windows"], default="linux")
    args = parser.parse_args()

    base = os.path.abspath(os.path.dirname(__file__))
    src = os.path.abspath(args.src)
    workdir = os.getcwd()
    tmp_bc = "input.bc"
    obf_bc = "obf.bc"
    obj = "output.o"
    out_exe = args.out

    params = {
      "bogus_blocks": args.bogus_blocks,
      "string_level": args.string_level,
      "insert_nops": args.insert_nops,
      "target": args.target
    }

    compile_to_bc(src, tmp_bc, target=None)
    apply_pass(tmp_bc, obf_bc, args.pass, params)
    bc_to_obj(obf_bc, obj)

    # link: choose cross-linker if windows target
    if args.target == "linux":
        link_objects([obj], out_exe)
    else:
        # windows target: use mingw-w64 clang++ (assumes installed)
        link_objects([obj], out_exe, linker_args=["-static", "-lws2_32"])

    final_size = os.path.getsize(out_exe) if os.path.exists(out_exe) else 0
    # Gather stats: trunk (we didn't capture opt stderr in this example)
    stats = {"bogus_blocks": args.bogus_blocks * 1, "strings": 0, "nops": args.insert_nops}
    generate_report("report.json", params, out_exe, stats, final_size)

if __name__ == "__main__":
    main()

