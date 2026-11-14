"""ReXenon instruction test bench.

Builds the test driver, assembles Xenia's PPC instruction tests and recompiles them with ReXenon's
emitter, compiles the generated C++ with clang-cl, runs every test and writes results.txt.

    py -I D:\\ReXenon\\ReXenonTests\\run.py [--filter instr_vadd] [--jobs 6] [--no-build]

Outputs (next to this script): results.txt (summary and failures by instruction) and
results_all.tsv (one line per test). Work files go to D:\\ReXenon\\build-tests\\rxt.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build-tests")
WORK = os.path.join(BUILD, "rxt")
GEN = os.path.join(WORK, "gen")
OBJ = os.path.join(WORK, "obj")
RESULTS = ""  # suffix for results.txt / results_all.tsv when run with --options or --cflag
TESTS = r"D:\xenia-src\src\xenia\cpu\ppc\testing"
VCENV = r"D:\fm2-port\tools\vcenv.cmd"
DRIVER = os.path.join(BUILD, "ReXenonTests", "ReXenonTestDriver.exe")

CXXFLAGS = ["/nologo", "/c", "/O2", "/DNDEBUG", "/std:c++20", "/MT", "-msse4.1", "-fno-strict-aliasing",
            "-Wno-everything", "/I" + os.path.join(HERE, "runtime"), "/I" + os.path.join(ROOT, "XenonUtils"),
            "/I" + os.path.join(ROOT, "thirdparty", "simde")]


def log(msg):
    print(msg, flush=True)


def vc_environment():
    out = subprocess.run(["cmd", "/c", VCENV, "set"], capture_output=True, text=True, errors="replace").stdout
    env = {}
    for line in out.splitlines():
        if "=" in line and not line.startswith("="):
            k, v = line.split("=", 1)
            env[k] = v
    if "INCLUDE" not in env:
        sys.exit("vcenv.cmd didn't set up the compiler environment")
    return env


def run(cmd, env, **kw):
    # Windows looks the program up on this process's PATH, not env's: resolve it first.
    exe = shutil.which(cmd[0], path=env.get("PATH") or env.get("Path"))
    cmd = [exe or cmd[0]] + list(cmd[1:])
    return subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace", **kw)


def build_driver(env):
    log("configuring and building ReXenonTestDriver ...")
    r = run(["cmake", "-S", ROOT, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl", "-DREXENON_TESTS=ON"], env)
    if r.returncode != 0:
        sys.exit("cmake failed:\n" + r.stdout + r.stderr)
    r = run(["ninja", "-C", BUILD, "ReXenonTestDriver"], env)
    if r.returncode != 0:
        sys.exit("building the driver failed:\n" + r.stdout[-6000:] + r.stderr[-2000:])


def newer(target, *sources):
    if not os.path.exists(target):
        return False
    t = os.path.getmtime(target)
    return all(os.path.getmtime(s) <= t for s in sources)


ERROR_RE = re.compile(r"^(.*?)\((\d+),\d+\): error: (.*)$")


def compile_suite(name, env, harness_obj, runtime_deps):
    """Compile and link one suite. Functions that don't compile are left out (RXT_SKIP_Fn) and
    reported; returns (exe or None, {function: error}, whole-suite error or None)."""
    cpp = os.path.join(GEN, name + ".cpp")
    obj = os.path.join(OBJ, name + ".obj")
    exe = os.path.join(OBJ, name + ".exe")
    errfile = os.path.join(OBJ, name + ".errors")
    if newer(exe, cpp, harness_obj, *runtime_deps) and os.path.exists(errfile):
        errors = {}
        with open(errfile) as f:
            for line in f:
                fn, _, msg = line.rstrip("\n").partition("\t")
                errors[fn] = msg
        return exe, errors, errors.get("*")

    ranges = []
    with open(os.path.join(GEN, name + ".map")) as f:
        for line in f:
            i, a, b = line.split()
            ranges.append((int(i), int(a), int(b)))

    skipped = {}
    suite_error = None
    for attempt in range(6):
        defs = ["/DRXT_SKIP_F%d" % i for i in sorted(skipped)]
        r = run(["clang-cl"] + CXXFLAGS + defs + [cpp, "/Fo" + obj], env)
        if r.returncode == 0:
            break
        new = {}
        unmapped = None
        for line in (r.stdout + r.stderr).splitlines():
            m = ERROR_RE.match(line.strip())
            if not m:
                continue
            if os.path.normcase(os.path.abspath(m.group(1))) != os.path.normcase(cpp):
                unmapped = unmapped or line.strip()
                continue
            ln = int(m.group(2))
            for i, a, b in ranges:
                if a <= ln <= b:
                    if i not in skipped and i not in new:
                        new[i] = m.group(3)
                    break
            else:
                unmapped = unmapped or line.strip()
        if not new:
            suite_error = unmapped or (r.stdout + r.stderr).strip().splitlines()[-1:] or ["compile failed"]
            suite_error = suite_error if isinstance(suite_error, str) else suite_error[0]
            break
        skipped.update(new)
    else:
        suite_error = "too many compile rounds"

    errors = {"f%d" % i: msg for i, msg in skipped.items()}
    if suite_error is None:
        r = run(["clang-cl", "/nologo", "-fuse-ld=lld", obj, harness_obj, "/Fe" + exe, "/link", "/INCREMENTAL:NO"], env)
        if r.returncode != 0:
            suite_error = "link failed: " + (r.stdout + r.stderr).strip()[-500:]
    if suite_error is not None:
        errors["*"] = suite_error
        if os.path.exists(exe):
            os.remove(exe)
    with open(errfile, "w") as f:
        for fn, msg in errors.items():
            f.write("%s\t%s\n" % (fn, msg.replace("\t", " ")))
    return (None if suite_error else exe), errors, suite_error


LINE_RE = re.compile(r"^(PASS|FAIL|UNIMPL|CRASH|NOFN|HANG) (\d+) (\S+) ?(.*)$")


def run_suite(name, exe, total):
    """Run a suite's exe, restarting after a test that hangs or takes the process down."""
    vec = os.path.join(GEN, name + ".vec")
    results = {}
    start = 0
    while start < total:
        try:
            p = subprocess.run([exe, vec, str(start)], capture_output=True, text=True, errors="replace", timeout=600)
            out, code = p.stdout, p.returncode
        except subprocess.TimeoutExpired as e:
            out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            code = -1
        last = start - 1
        for line in out.splitlines():
            m = LINE_RE.match(line)
            if not m:
                continue
            idx = int(m.group(2))
            results[idx] = (m.group(1), m.group(3), m.group(4))
            last = max(last, idx)
        if code == 0 and last >= total - 1:
            break
        if code == 0:
            break
        # The harness reports a hung test itself (HANG) and then exits: carry on after it. Otherwise
        # the test after the last reported one took the process down.
        if last >= start and results[last][0] == "HANG":
            start = last + 1
            continue
        nxt = last + 1
        if nxt < total and nxt not in results:
            results[nxt] = ("CRASH", None, "process exited with 0x%08X" % (code & 0xFFFFFFFF))
        start = max(nxt + 1, start + 1)
    return results


def group_of(suite):
    n = suite[len("instr_"):]
    if n.startswith("_gen_"):
        return n[len("_gen_"):]
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default="", help="only suites whose file name contains this")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--no-build", action="store_true", help="don't rebuild the driver")
    ap.add_argument("--options", default="", help="recompiler options for this run, comma-separated "
                    "(exact_estimates, fused_madd); work files and results get their own names")
    ap.add_argument("--cflag", action="append", default=[], help="extra compiler flag, e.g. -mfma (repeatable)")
    args = ap.parse_args()

    global WORK, GEN, OBJ, CXXFLAGS, RESULTS
    if args.options or args.cflag:
        tag = "-".join([o for o in args.options.split(",") if o] + [c.lstrip("-/") for c in args.cflag])
        WORK = WORK + "-" + tag
        GEN = os.path.join(WORK, "gen")
        OBJ = os.path.join(WORK, "obj")
        RESULTS = "-" + tag
    CXXFLAGS = CXXFLAGS + args.cflag

    t0 = time.time()
    env = vc_environment()
    env["REXENON_TEST_OPTIONS"] = args.options
    os.makedirs(GEN, exist_ok=True)
    os.makedirs(OBJ, exist_ok=True)
    if not args.no_build:
        build_driver(env)

    log("assembling and recompiling the tests ...")
    for f in ("manifest.tsv", "suites.txt"):
        p = os.path.join(WORK, f)
        if os.path.exists(p):
            os.remove(p)
    r = run([DRIVER, TESTS, WORK] + ([args.filter] if args.filter else []), env)
    with open(os.path.join(WORK, "driver.log"), "w") as f:
        f.write(r.stdout + r.stderr)
    if r.returncode != 0:
        sys.exit("driver failed:\n" + r.stdout[-3000:] + r.stderr[-3000:])
    log(r.stdout.strip().splitlines()[-2] if r.stdout.strip() else "")
    log(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "")

    # compile the harness once
    harness_cpp = os.path.join(HERE, "runtime", "harness.cpp")
    runtime_deps = [os.path.join(HERE, "runtime", "rxt_runtime.h"), os.path.join(ROOT, "XenonUtils", "ppc_context.h")]
    harness_obj = os.path.join(OBJ, "harness.obj")
    if not newer(harness_obj, harness_cpp, *runtime_deps):
        r = run(["clang-cl"] + CXXFLAGS + [harness_cpp, "/Fo" + harness_obj], env)
        if r.returncode != 0:
            sys.exit("harness doesn't compile:\n" + r.stdout + r.stderr)

    suites = []
    with open(os.path.join(WORK, "suites.txt")) as f:
        for line in f:
            name, ntests, nfns = line.split("\t")
            suites.append(name)

    manifest = []  # (suite, test, mnemonics, status, detail)
    vec_index = collections.defaultdict(list)  # suite -> [test name in .vec order]
    with open(os.path.join(WORK, "manifest.tsv")) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            suite, test, mnems, status = parts[0], parts[1], parts[2], parts[3]
            rest = parts[4:]
            manifest.append((suite, test, mnems, status, rest))
            if status == "RUN":
                vec_index[suite].append(test)

    log("compiling %d suites (%d jobs) ..." % (len(suites), args.jobs))
    compiled = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = {pool.submit(compile_suite, s, env, harness_obj, runtime_deps): s for s in suites}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            s = futs[fut]
            compiled[s] = fut.result()
            done += 1
            if done % 50 == 0:
                log("  %d/%d" % (done, len(suites)))

    log("running ...")
    ran = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = {}
        for s in suites:
            exe = compiled[s][0]
            if exe and vec_index[s]:
                futs[pool.submit(run_suite, s, exe, len(vec_index[s]))] = s
        for fut in concurrent.futures.as_completed(futs):
            ran[futs[fut]] = fut.result()

    # ---------------------------------------------------------------- results
    rows = []  # suite, test, status, detail, mnemonics
    for suite, test, mnems, status, rest in manifest:
        if status == "SKIP":
            rows.append((suite, test, "SKIPPED", "", mnems))
            continue
        if status == "ENC":
            rows.append((suite, test, "ENCODER_FAILED", rest[0] if rest else "", mnems))
            continue
        fn = rest[0]
        exe, errors, suite_error = compiled.get(suite, (None, {}, "not compiled"))
        if suite_error:
            rows.append((suite, test, "COMPILE_FAILED", suite_error, mnems))
            continue
        if fn in errors:
            rows.append((suite, test, "COMPILE_FAILED", "%s: %s" % (fn, errors[fn]), mnems))
            continue
        rows.append([suite, test, "PENDING", fn, mnems])

    # match run results by index within the suite's .vec
    index_of = {s: {t: i for i, t in enumerate(names)} for s, names in vec_index.items()}
    final = []
    for row in rows:
        if row[2] != "PENDING":
            final.append(tuple(row))
            continue
        suite, test = row[0], row[1]
        i = index_of[suite][test]
        res = ran.get(suite, {}).get(i)
        if res is None:
            final.append((suite, test, "NOT_RUN", "", row[4]))
            continue
        kind, _, detail = res
        status = {"PASS": "PASS", "FAIL": "FAIL", "UNIMPL": "UNIMPLEMENTED", "CRASH": "CRASH",
                  "HANG": "HANG", "NOFN": "COMPILE_FAILED"}[kind]
        final.append((suite, test, status, detail, row[4]))

    counts = collections.Counter(r[2] for r in final)
    stats = {}
    with open(os.path.join(WORK, "encoder_stats.txt")) as f:
        for line in f:
            k, v = line.split()
            stats[k] = int(v)

    with open(os.path.join(HERE, "results_all%s.tsv" % RESULTS), "w") as f:
        f.write("suite\ttest\tstatus\tinstructions\tdetail\n")
        for suite, test, status, detail, mnems in final:
            f.write("%s\t%s\t%s\t%s\t%s\n" % (suite, test, status, mnems, detail))

    write_results(final, counts, stats, compiled, time.time() - t0)
    log("\n\n".join(open(os.path.join(HERE, "results%s.txt" % RESULTS)).read().split("\n\n")[:3]))


def failure_key(status, detail):
    """A short signature to group similar failures: the registers that mismatched."""
    if status == "FAIL":
        regs = [m.split(" ")[0] for m in detail.split(" ; ")]
        return "mismatch in " + ",".join(sorted(set(re.sub(r"\d+$", "N", r) if r.startswith("mem") else r for r in regs)))
    if status == "UNIMPLEMENTED":
        return re.sub(r" at 0x[0-9A-F]+", "", re.sub(r"0x[0-9A-F]+ -> 0x[0-9A-F]+", "", detail))
    if status == "CRASH":
        return re.sub(r"address 0x[0-9A-F]+", "", detail)
    return detail[:120]


def fail_tags(final):
    """For each FAIL test: cr-only, NaN, denormal or ordinary, from the values in its .vec."""
    def nan32(w): return (w >> 23) & 0xFF == 0xFF and w & 0x7FFFFF
    def den32(w): return (w >> 23) & 0xFF == 0 and w & 0x7FFFFF
    def nan64(w): return (w >> 52) & 0x7FF == 0x7FF and w & ((1 << 52) - 1)
    def den64(w): return (w >> 52) & 0x7FF == 0 and w & ((1 << 52) - 1)
    wanted = collections.defaultdict(set)
    for r in final:
        if r[2] == "FAIL":
            wanted[r[0]].add(r[1])
    tags = {}
    for suite, names in wanted.items():
        values = {}
        cur = None
        with open(os.path.join(GEN, suite + ".vec")) as f:
            for line in f:
                p = line.split()
                if p[0] == "T":
                    cur = p[1] if p[1] in names else None
                    if cur:
                        values[cur] = []
                elif cur and p[0] in ("I", "O"):
                    if p[1].startswith("v"):
                        values[cur] += [("s", int(x, 16)) for x in p[2:6]]
                    elif p[1].startswith("f"):
                        values[cur].append(("d", int(p[-1], 16)))
        for r in final:
            if r[0] != suite or r[2] != "FAIL":
                continue
            if all(m.startswith("cr ") for m in r[3].split(" ; ")):
                tags[(r[0], r[1])] = "cr-only"
                continue
            vals = values.get(r[1], [])
            if any((nan32(w) if k == "s" else nan64(w)) for k, w in vals):
                tags[(r[0], r[1])] = "NaN"
            elif any((den32(w) if k == "s" else den64(w)) for k, w in vals):
                tags[(r[0], r[1])] = "denormal"
            else:
                tags[(r[0], r[1])] = "ordinary"
    return tags


def write_results(final, counts, stats, compiled, elapsed):
    order = ["PASS", "FAIL", "UNIMPLEMENTED", "CRASH", "HANG", "COMPILE_FAILED", "ENCODER_FAILED", "SKIPPED", "NOT_RUN"]
    total = len(final)
    gen = [r for r in final if "__gen_" in r[0]]
    hand = [r for r in final if "__gen_" not in r[0]]

    out = []
    out.append("ReXenon instruction tests (Xenia's src/xenia/cpu/ppc/testing, instr_*.s)")
    out.append("Generated %s in %.0f s. Rerun: py -I %s" % (time.strftime("%Y-%m-%d %H:%M"), elapsed, os.path.join(HERE, "run.py")))
    out.append("")
    out.append("%-16s %8s %10s %10s" % ("status", "all", "hand", "_GEN"))
    for k in order:
        a = counts.get(k, 0)
        if a or k in ("PASS", "FAIL", "UNIMPLEMENTED", "COMPILE_FAILED", "ENCODER_FAILED", "SKIPPED"):
            out.append("%-16s %8d %10d %10d" % (k.lower().replace("_", "-"), a,
                                                sum(1 for r in hand if r[2] == k), sum(1 for r in gen if r[2] == k)))
    out.append("%-16s %8d %10d %10d" % ("total", total, len(hand), len(gen)))
    out.append("")
    out.append("Encoder: %d instruction lines (%d distinct); %d assembled (%d with a range warning);"
               % (stats["instruction_lines"], stats["unique_lines"], stats["assembled"], stats["assembled_with_warning"]))
    rt = stats["roundtrip_strict"] + stats["roundtrip_alias"]
    out.append("  round-trip through ppc::Disassemble: %d/%d (%.2f%%): %d same mnemonic and operands, %d via an alias"
               " (disassembles to another spelling that assembles back to the same word); %d/%d distinct lines."
               % (rt, stats["instruction_lines"], 100.0 * rt / max(1, stats["instruction_lines"]), stats["roundtrip_strict"],
                  stats["roundtrip_alias"], stats["unique_roundtrip"], stats["unique_lines"]))
    out.append("  plus %d raw .long words. Details: %s" % (stats["long_words"], os.path.join(WORK, "encoder.txt")))
    out.append("  %d tests share %d distinct recompiled functions (identical code is recompiled once)." % (stats["tests_runnable"], stats["functions"]))
    out.append("")

    # per instruction group
    groups = collections.defaultdict(list)
    for r in final:
        groups[group_of(r[0])].append(r)
    bad = [(g, rs) for g, rs in groups.items() if any(r[2] not in ("PASS", "SKIPPED") for r in rs)]
    bad.sort(key=lambda x: (-sum(1 for r in x[1] if r[2] not in ("PASS", "SKIPPED")), x[0]))

    tags = fail_tags(final)
    out.append("Instructions with failures (%d of %d instructions; failing/total tests)." % (len(bad), len(groups)))
    out.append("For value mismatches, [..] says what the test data involves: cr-only (only CR differs),")
    out.append("NaN or denormal (a NaN/denormal among the test's register values), ordinary (neither).")
    for g, rs in bad:
        nbad = sum(1 for r in rs if r[2] not in ("PASS", "SKIPPED"))
        kinds = collections.Counter(r[2] for r in rs if r[2] not in ("PASS", "SKIPPED"))
        tagc = collections.Counter(tags[(r[0], r[1])] for r in rs if r[2] == "FAIL")
        tagtext = " [%s]" % ", ".join("%s %d" % (k, v) for k, v in tagc.most_common()) if tagc else ""
        out.append("  %-14s %6d/%-6d %s%s" % (g, nbad, len(rs), ", ".join("%s %d" % (k.lower(), v) for k, v in kinds.most_common()), tagtext))
    out.append("")

    # dot-form instructions the emitter writes no CR update for (the recompiler's own warning)
    rc = collections.Counter()
    try:
        for line in open(os.path.join(WORK, "driver.log")):
            m = re.match(r"^(\S+) at [0-9A-F]+ has RC bit enabled but no comparison was generated", line)
            if m:
                rc[m.group(1)] += 1
    except OSError:
        pass
    if rc:
        out.append("Recompiler warnings 'RC bit enabled but no comparison was generated' (no cr0/cr6 write; the FP")
        out.append("forms set cr1, which the check doesn't look for), by mnemonic: " + ", ".join(sorted(rc)))
        out.append("")

    out.append("Failures by instruction (each group: signature, count, examples; every test in results_all.tsv):")
    for g, rs in bad:
        failing = [r for r in rs if r[2] not in ("PASS", "SKIPPED")]
        out.append("")
        out.append("== %s: %d of %d failing" % (g, len(failing), len(rs)))
        sigs = collections.defaultdict(list)
        for r in failing:
            sigs[(r[2], failure_key(r[2], r[3]))].append(r)
        for (status, key), items in sorted(sigs.items(), key=lambda x: -len(x[1])):
            out.append("  [%s] %s: %d" % (status.lower(), key, len(items)))
            limit = 3 if len(items) > 3 else len(items)
            for r in items[:limit]:
                out.append("      %s:%s  %s" % (r[0], r[1], r[3][:400]))
            if len(items) > limit:
                out.append("      ... %d more" % (len(items) - limit))

    with open(os.path.join(HERE, "results%s.txt" % RESULTS), "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
