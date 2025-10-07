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

def run(cmd, cwd=None, capture=False, capture_stderr=False):
    print("> " + " ".join(cmd))
    if capture or capture_stderr:
        completed = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if completed.returncode != 0:
            raise subprocess.CalledProcessError(completed.returncode, cmd, completed.stdout)
        if capture_stderr:
            return completed.stdout, completed.stderr
        return completed.stdout
    else:
        subprocess.check_call(cmd, cwd=cwd)

def compile_to_bc(src, out_bc, target=None):
    cmd = [CLANG, "-O1", "-emit-llvm", "-c", src, "-o", out_bc]
    if target:
        cmd.insert(1, "--target="+target)
    run(cmd)

def _extract_headers_from_bc(bc_path):
    try:
        out = run(["llvm-dis", bc_path, "-o", "-"], capture=True)
    except Exception:
        return None, None
    triple = None
    datalayout = None
    for line in out.splitlines():
        if line.startswith("target datalayout"):
            datalayout = line
        elif line.startswith("target triple"):
            triple = line
        if triple and datalayout:
            break
    return datalayout, triple

def apply_pass(in_bc, out_bc, pass_plugin, options, cycles=1):
    # We'll set module global variables as options for the pass to read
    temp_bc = in_bc
    # build mod options small bitcode patch: easier approach, create small LLVM IR file with globals then link
    # create options.ll with same target layout/triple as input
    opt_ll = "obf_options.ll"
    dl, tt = _extract_headers_from_bc(in_bc)
    with open(opt_ll, "w") as f:
        f.write("; Module to provide obfuscation options\n")
        if dl:
            f.write(dl + "\n")
        if tt:
            f.write(tt + "\n")
        # mark globals internal to avoid multiply-defined on relinking
        f.write("@obf_bogus_blocks = internal global i32 %d\n" % options['bogus_blocks'])
        f.write("@obf_string_level = internal global i32 %d\n" % options['string_level'])
        f.write("@obf_insert_nops = internal global i32 %d\n" % options['insert_nops'])
        f.write("@obf_flatten = internal global i1 %d\n" % (1 if options.get('flatten') else 0))
    # compile options.ll to bc
    run(["llvm-as", opt_ll, "-o", "obf_options.bc"])
    # link the two bcs once
    run(["llvm-link", temp_bc, "obf_options.bc", "-o", "linked.bc"])
    stderr_accum = ""
    # Try single-invocation repeat; on failure, fall back to multiple invocations
    if cycles and cycles > 1:
        try:
            passes_spec = f"repeat<{cycles}>(obf-legacy)"
            cmd = [LLVM_OPT, "-load-pass-plugin", pass_plugin, f"-passes={passes_spec}", "linked.bc", "-o", out_bc]
            _, stderr_text = run(cmd, capture_stderr=True)
            return stderr_text
        except subprocess.CalledProcessError:
            pass
    # Fallback: run opt multiple times without relinking options
    src_bc = "linked.bc"
    tmp_out = out_bc
    for i in range(max(1, cycles)):
        cmd = [LLVM_OPT, "-load-pass-plugin", pass_plugin, "-passes=obf-legacy", src_bc, "-o", tmp_out]
        _, stderr_text = run(cmd, capture_stderr=True)
        stderr_accum += (stderr_text or "")
        src_bc = tmp_out
    return stderr_accum

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

def generate_report(report_path, params, out_file, stats, final_size, cycles, methods_applied, tool_versions):
    report = {
      "timestamp": datetime.datetime.utcnow().isoformat() + "Z",
      "input_parameters": params,
      "output": {
         "file": out_file,
         "size_bytes": final_size,
      },
      "obfuscation_stats": stats,
      "obfuscation_cycles": cycles,
      "methods_applied": methods_applied,
      "tools": tool_versions,
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
    parser.add_argument("--pass", dest="plugin", required=False, default="../build/llvm_pass/obfpass.so", help="Path to compiled pass plugin (opt plugin)")
    parser.add_argument("--bogus-blocks", type=int, default=1)
    parser.add_argument("--string-level", type=int, default=1)
    parser.add_argument("--insert-nops", type=int, default=0)
    parser.add_argument("--target", choices=["linux","windows"], default="linux")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--profile", choices=["light","medium","aggressive"], default=None)
    parser.add_argument("--flatten", action="store_true", help="Enable basic control-flow flattening")
    args = parser.parse_args()

    base = os.path.abspath(os.path.dirname(__file__))
    src = os.path.abspath(args.src)
    workdir = os.getcwd()
    tmp_bc = "input.bc"
    obf_bc = "obf.bc"
    obj = "output.o"
    out_exe = args.out

    # apply profile if given
    bogus = args.bogus_blocks
    slevel = args.string_level
    nops = args.insert_nops
    if args.profile == "light":
        bogus, slevel, nops = 1, 1, 0
    elif args.profile == "medium":
        bogus, slevel, nops = max(bogus, 2), max(slevel, 2), max(nops, 4)
    elif args.profile == "aggressive":
        bogus, slevel, nops = max(bogus, 5), max(slevel, 3), max(nops, 16)

    params = {
      "bogus_blocks": bogus,
      "string_level": slevel,
      "insert_nops": nops,
      "target": args.target,
      "flatten": bool(args.flatten)
    }

    compile_to_bc(src, tmp_bc, target=None)
    cumulative_stats = {"bogus_blocks": 0, "strings": 0, "nops": 0}
    # apply pass with cycles in a single opt invocation to avoid relinking
    stderr_text = apply_pass(tmp_bc, obf_bc, args.plugin, params, cycles=max(1, args.cycles))
    stats = gather_stats(stderr_text or "")
    for k in cumulative_stats:
        cumulative_stats[k] = cumulative_stats.get(k, 0) + stats.get(k, 0)
    bc_to_obj(obf_bc, obj)

    # link: choose cross-linker if windows target
    if args.target == "linux":
        link_objects([obj], out_exe)
    else:
        # windows target: use mingw-w64 clang++ (assumes installed)
        link_objects([obj], out_exe, linker_args=["-static", "-lws2_32"])

    final_size = os.path.getsize(out_exe) if os.path.exists(out_exe) else 0
    methods = []
    if bogus > 0:
        methods.append("control_flow_bogus")
    if slevel > 0:
        methods.append("string_obfuscation")
    if nops > 0:
        methods.append("nop_insertion")
    if params.get("flatten"):
        methods.append("control_flow_flatten")
    tool_versions = {
        "clang": (run([CLANG, "--version"], capture=True).splitlines()[0] if shutil.which(CLANG) else None),
        "opt": (run([LLVM_OPT, "--version"], capture=True).splitlines()[0] if shutil.which(LLVM_OPT) else None),
    }
    generate_report("report.json", params, out_exe, cumulative_stats, final_size, max(1, args.cycles), methods, tool_versions)

if __name__ == "__main__":
    main()

