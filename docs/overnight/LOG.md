# ReXenon overnight log (2026-11-14)

This log covers the night's work on the roadmap in `docs/research-2026-11-14.md`. Nothing here is committed. The working tree holds all the changes.

How the pieces are kept separate:
- After each step, the working tree is saved as a git tree object. Its id is in `tree-NN.txt`.
- `git diff <treeA> <treeB>` shows exactly what one step changed.
- `git diff HEAD <tree>` shows everything up to and including that step.

How each step was checked:
- Generation: both games are regenerated into `build/gen/{fm2,pgr4}/<run>`, using configs whose paths are rewritten (`build/gen/*/config`). The output is compared function by function with `build/gen/fndiff.py`.
- Compiling: every changed file is compiled with the port's own flags, using `build/gen/cc.ps1`.
- Not checked: no game was run and the FX machine was not used, because there was nobody awake to ask.

## Morning summary (read this first)

**State:** 19 steps of roadmap work, all uncommitted in `D:\ReXenon`'s working tree. Each step's tree is saved in `tree-NN.txt`. `git diff HEAD $(cat docs/overnight/tree-19.txt)` is the whole night. No game was run, the FX wasn't used, `D:\PGR4-ReGPU` and `D:\fm2-port` weren't touched (only read), and nothing was committed or pushed.

**Checks run on everything:**
- Both games are regenerated after every step and compared function by function, and the changed files compile with each port's flags.
- The instruction bench (Xenia's 170,448 tests) passes 142,286, up from 110,259 before step 9. The rest are NaN/FPSCR details and the opt-in estimate/fma cases. Final per-test results are identical to step 12.
- Independent review: one review of steps 1–14 (four reviewers, each with a refuter), then a separate check of each later step. 12 defects confirmed and fixed (8 in the first review, 4 in the checks); 7 refuted. The worst confirmed defect is a bug that is in the shipped FM2 output today (below).

**What changed, by roadmap item:**
- *Fail loudly / correctness:*
  - Generic branch and trap decoding (7).
  - Every scalar instruction the bench exercises implemented (9), and 47 vector instructions added or made exact (10).
  - vpkd3d128 for every type and pack, matching Xenia (11, 15).
  - Conditional calls set LR on both paths (15).
  - Functions that run off their end become tail calls (13; this fixes PGR4's memset).
- *Zero-config analysis:*
  - Exception data skipped (1).
  - Splits at stranded branch targets (2).
  - Switch tables found automatically and validated (8, 15).
  - Switch-aware function sizing and fragment merging, with entry points protected (13, 15).
  - Data-pointer seeding (14).
  - The analysis can be written out and frozen, and the round trip is identical for both games (18, 19).
  - Without their hand lists, both games generate with 0 problems.
- *Speed (all need an FX measurement):*
  - Empty-function call elision (3, opt-in).
  - FP-mode switch skipped for mode-neutral ops (4; −20% static switches).
  - Settable indirect-call table (5; .text −2.3% on a sample file).
  - Real barrier and timebase macros (6).
  - Console-exact estimates and fused madd (12, opt-in).
- *Build:* `chunk_by_address`: a function change rebuilds about 10 chunk files instead of all 200 (16).
- *Diagnostics:* `PPC_SWITCH_MISS`, `PPC_CHECK_INDIRECT_CALLS` (17).

**Bugs found in today's shipped output:**
1. **FM2 has 8 × `vpkd3d128 v0,v0,5,2,2` that pack garbage** (vD == vB aliasing in the old FLOAT16_4 code), in 4 functions: sub_823B76F8, sub_823B7768, sub_823E8DF8 and sub_823E8EA0. The half floats they write are wrong. A fifth, sub_82395F70, changes only for values above 65504 or below 2^-14 (the corrected half limits). Fixed in ReXenon (step 15). Worth a look in game: if they feed geometry, something may look *different* (correct) after the switch.
2. **PGR4's memset loses the last 1–3 bytes for lengths of 2 or 3 mod 4.** This is the bug FM2 fixed with a hand entry (Bink, Arcade crash). PGR4 doesn't hook memset. ReXenon fixes it (step 13). **Tell the PGR4 session.**

**Before FM2 switches to ReXenon output (port changes, not done):**
1. Define `PPC_MFTB()` as `Fm2Timebase()` in `src/ppc_hooks.h` (step 6). Otherwise it falls back to the raw TSC.
2. Hook `sub_82773580`, the second SwitchToFiber copy that is now a real function, like the first one (step 2).
3. Run a game test on the FX of the behaviour changes: vmax/vmin NaN and signed zero (10), D3DCOLOR NaN (11), vpkd3d128 FLOAT16 (15), and the barrier/timebase macros.

**Optional for FM2 (each needs an FX A/B):**
- `function_table_address = 0x843C0000`, plus committing that memory (5).
- `elide_empty_functions = true` (3).
- `chunk_by_address = 0x8000` (16).
- Dropping the 28-entry `functions` list, or freezing the emitted one (13, 18).
- `exact_estimates` / `fused_madd` with `-mfma` (12).

**Housekeeping:**
- Disk D: is at about 2 GB free.
- My generator outputs in `D:\ReXenon\build\gen` (about 1 GB) can be deleted.
- About 300 MB of loose git objects came from a bad snapshot at step 14. `git prune` removes them, but it also deletes the tree-NN snapshots, so only run it once those aren't needed.
- Review inputs are in `docs/overnight/review*-input.patch`.
- `docs/` (this log and the research report) is new and untracked.

## Step 1: skip exception data automatically (roadmap item 4, part a)

When a function's `.pdata` entry has ExceptionFlag set, the 8 bytes in front of it are exception data (a handler pointer and a scope table), not code. The linear scan now skips them, so the `invalid_instructions` entries for them are no longer needed.

- FM2: the output is byte-identical even with its three hand-written exception-data entries removed.
- PGR4: problems drop from 161 to 3 with no config change. One more phantom function is removed than hand entries would remove: sub_828F6A34, which was exception data decoded as code.

## Step 2: split functions at stranded branch targets (roadmap item 4, part b; C1)

A branch can land in the middle of another function. When the instruction just before the target never falls through, the target is really the start of its own function, so ReXenon now splits there. Instructions that never fall through are `b`, `blr`, `bctr`, and a `bl` to KeBugCheck or KeBugCheckEx.

The split only happens when no branch in the first piece enters the second and none in the second goes back. That way no register state flows between the two pieces.

Also added: `SymbolTable::findContaining`. The existing `find` only matches exact starts. Problem messages now name the function that contains the target.

| | FM2 | PGR4 |
|---|---|---|
| Problems before | 1 | 3 |
| Problems after | 0 | 0 |
| Generator exit code | 0 | 0 |
| Split | `sub_82773570` at `82773580` (after KeBugCheck) | 3 funclets, each after `blr` |
| Functions changed | 2 changed, 1 added | 6 changed, 3 added |
| Changed files compile | yes | yes |

**FM2 needs action:** `sub_82771F08` now tail-calls the real `sub_82773580`. That function is a second copy of SwitchToFiber, a guest register swap that doesn't switch host stacks. The FM2 port must hook it in the same way as `sub_82754EF0` (in `src/hooks/fibers.cpp`) before it uses ReXenon. A task card for that was proposed earlier tonight.

## Step 3: leave out calls to functions that are only `blr` (roadmap item 7a)

This is opt-in: set `elide_empty_functions = true` in [main]. `keep_functions = [...]` lists any the port hooks. Calls to a function whose whole body is `blr` are left out. The call site still sets LR, so all guest state stays identical. The list of skipped functions is written to `ppc_elided_functions.txt`. A call that was skipped doesn't reset the tracked FP mode either.

- **Default off:** FM2 output is identical with the option off.
- **FM2 with it on:** 266 empty functions found. Only `sub_82404EC0` is called directly, from 16,325 call sites. The other 265 are only reached indirectly.
- **Hooks:** none of FM2's hooks is on an empty function. This was checked against the TU-translated hook sources in `build/tu/remapped`.
- **Speed:** not measured (no FX tonight).

## Step 4: don't switch the FP mode for mode-neutral ops (roadmap item 6)

These ops no longer call `printSetFlushMode`, because neither flush-to-zero nor denormals-are-zero can change their result: lfd, lfdx, stfd, stfdx, stfiwx (bit copies), fmr, fabs, fnabs, fneg (sign bit), fcfid (exact int → double; the result is never denormal), and fctidz/fctiwz (truncating a denormal gives 0 either way).

- **fctid stays as it was:** with round-toward-+inf, a positive denormal gives 1 without DAZ and 0 with it.
- **lfs/stfs stay as they were:** the float/double conversion depends on DAZ.

| | FM2 | PGR4 |
|---|---|---|
| Static flush-mode calls | 21,354 → 16,733 (−22%) | 34,843 → 27,736 (−20%) |
| Functions changed | 2,333 | 2,825 |

**Audit:** I checked every emitter that touches FP values without setting the mode itself, since one could have relied on a neutral op before it to set FPU mode. There are four: vcmpequw, vcmpgtuw, vrlimi128 and vupkd3d128. All four are bit operations, so none depended on it.

- Sample object files are about 0.6% smaller.
- The changed files compile in both games.

## Step 5: the indirect-call table address can be set (roadmap item 3)

Two parts:
- **Runtime header:** `PPC_LOOKUP_FUNC` now uses `PPC_FUNC_TABLE_BASE`. It defaults to `PPC_IMAGE_BASE + PPC_IMAGE_SIZE`, exactly as before.
- **Generator:** a new config key, `function_table_address`, emits `#define PPC_FUNC_TABLE_BASE` into `ppc_config.h`. When the table is too far away for a 32-bit displacement, the generator prints a note with the address range that fixes it. If the table would run past 4 GB, that is reported as a problem.

What it does for FM2:
- **Default output:** unchanged, ppc_config.h included.
- **Range the generator suggests for FM2:** 0x843C0000–0xFF0C0000. The table needs 0xF39E98 bytes (about 15.6 MB).
- **Measured on file 10:** 696 indirect calls. `movabs` drops from 166 to 23 and `.text` shrinks from 154,126 to 150,644 bytes (−2.3%). That matches the research figure.

**FM2 needs action to use it:**
1. Set `function_table_address = 0x843C0000` in `fm2_tu4.toml`.
2. Make sure FM2's guest memory commits 0x843C0000–0x852FA000 and that nothing else allocates there.
3. Test on the FX.

The three port uses of the macro (`loader.cpp:286`, `thread.cpp:276`, `wheel_ff.cpp:289`) follow automatically.

## Step 6: timebase and barrier macros (C10; roadmap item 5, step 1)

**Timebase.** `mftb` now emits `PPC_MFTB()`. It defaults to `__rdtsc()`, the old behaviour. Two related fixes:
- `mftb rD,269` (TBU, the classic form) and `mftbu` now return the high word. Before, they returned the whole counter.
- `mftbl` is handled too.

**Barriers.** `sync`, `lwsync`, `eieio` and `isync` emit `PPC_SYNC()`, `PPC_LWSYNC()`, `PPC_EIEIO()` and `PPC_ISYNC()`. Before, they emitted nothing.
- By default these are compiler barriers only (`atomic_signal_fence`), so there is no hardware cost.
- With `hardware_fences = true`, sync becomes a seq_cst fence (mfence) and lwsync/eieio become acq_rel fences (nothing on x64, dmb on arm64). This is the groundwork for ever dropping `volatile`.

Counts and checks:
- FM2 has 11 sync, 2 lwsync, 13 eieio and 16 mftb sites; 26 functions changed. PGR4 has 29 functions changed.
- The files with these sites compile in both games.

**FM2 needs action:** before using ReXenon, the port must define `PPC_MFTB()` as `Fm2Timebase()` in `src/ppc_hooks.h`. Without that it falls back to the raw TSC, the bug `tools/patch_mftb.py` exists to fix. Once it's defined, `patch_mftb.py` can be retired.

## Step 7: generic branch and trap decoding (C8, C9)

**Branches.** A `bc`, `bclr` or `bcctr` with no case of its own is now decoded from its BO/BI/LK/AA fields. The disassembler gives every combination its own ID (beqctrl, bdnzflr, bsol and so on), which is about 200 aliases. Before this, all of them fell through to "unimplemented".

The decoder covers:
- CTR decrement, plus ==0 and !=0 tests;
- any CR bit;
- conditional calls (bcl, bclrl, bcctrl);
- out-of-function targets (a tail call, or a call with link);
- `bcl 20,31,$+4`, which reads the PC into LR;
- the invalid bcctr-with-decrement, reported as a problem.

The `bdnzf` case, which assumed the `eq` bit, is replaced by the decoder. FM2's only bdnzf form is `eq`, and its output is the same.

**Traps.** tw, twi, td and tdi and all their aliases are decoded from TO (signed and unsigned, 32- and 64-bit) and emit `if (cond) PPC_TRAP(addr)`. `PPC_TRAP` does nothing by default, which is today's behaviour. A port can define it to log the game's asserts. The old trap no-op cases are removed.

- FM2: 7,843 trap sites (6,878 unconditional `twi 31`).
- PGR4: 1,198.

Checks:
- The only changed lines are the trap conditions and the macro definition.
- Sample files compile in both games.

## Step 8: switch tables found automatically (tree-08)

XenonAnalyse's pattern scanner now lives in `XenonAnalyse/switch_tables.cpp` as `FindSwitchTables(image, type)`, part of LibXenonAnalyse. XenonAnalyse calls it, and its output is byte-identical to before for both games (7,088 and 19,304 lines).

The recompiler has a new opt-in option, `auto_switch_tables = true`. It runs the same scan at load time and adds every table found. Entries in `switch_table_file_path` win, so the file only has to list the tables the patterns miss.

The old "Found a switch jump table ... with no switch table entry" print is now a problem, so the generator fails on a missed table instead of scrolling past it. With the current hand files neither game triggers it.

Results:
- **FM2:** auto alone finds 320 tables, and exactly one problem remains: the bctr at 8281B540 in sub_8281B3B0. Its table is reached through a `b` with no bounds check nearby. With auto plus a one-entry file (base 0x8281B51C), the output is identical to the 7,088-line hand file.
- **PGR4:** auto alone finds 794 tables, gives 0 problems, and the output is identical with no switch-table file at all.

**Disk:** D: hit 0 bytes free during this step. I deleted my old generator outputs (about 4 GB, all regenerable), which left 3.5 GB free. The other ~229 GB isn't from this work.

### Correction to step 8 (found during step 9)

The step-8 PGR4 claim "identical with no switch-table file" was wrong. The h1 baseline came from the step-7 binary and was written as D: ran out of space. In it, sub_824ED058's bctr at 824ED288 was already an indirect call, even with the hand table present, so auto-only output "matched" it.

A fresh baseline (j1) uses the hand table there. Auto-only misses it, so PGR4 also needs a one-entry file (base 0x824ED274).

I've redone the step-8 checks:
- FM2 with auto plus one entry (0x8281B51C) is identical to a fresh hand-config baseline (k1).
- PGR4 with auto plus one entry (0x824ED274) is identical to j1.

The missing-table check now catches both cases (see step 9). The old check only looked at the single word before the bctr.

## Step 9: scalar instructions, record forms, bench (tree-09)

**Instruction test bench (background agent).** It lives in `ReXenonTests/` and is gated on `-DREXENON_TESTS=ON`; the top-level CMakeLists gains one `add_subdirectory` line. It runs Xenia's 170,448 instruction tests through the real recompiler.

- Rerun with `py -I D:\ReXenon\ReXenonTests\run.py` (about 30 s) from PowerShell or cmd; Git Bash fails.
- `results.txt` groups failures and tags each one as NaN, denormal, cr-only or ordinary.
- `results_all.tsv` has one line per test.
- The bench's own report is summarised at the end of this log.

**New scalar emitters.** All Xenon scalar instructions the bench exercises are now implemented:
- **Arithmetic and rotates:** addc, addme, subfme, subfze, mulhd, rldic, rotld, rldcl, rldcr, rlwnm.
- **CR operations:** every cr* logical op and alias (decoded from the word), mcrf, mcrxr, mtcrf and mtocrf (partial masks).
- **Compares:** cmp, cmpi, cmpl and cmpli with the L bit.
- **Loads and stores:**
  - all D- and X-form update loads and stores (lhau, lhzu, lfsu, lfdu, sthu, stfsu, stfdu, and lbzux through stfdux);
  - lmw/stmw, lswi/stswi (unrolled);
  - ldbrx/stdbrx.
- **Floating point:** fctiw (current rounding), fnmadd, fcmpo (as fcmpu).
- **FPSCR:** mtfsb0/1 and mtfsfi (RN only), mcrfs.
- **Special registers:**
  - mfvscr returns NJ and mtvscr is a no-op;
  - mfxer, mfctr;
  - mfspr/mtspr for XER, LR, CTR, VRSAVE and TB;
  - mtmsr is handled as mtmsrd.
- **Cache and vector loads/stores:** icbi/dcbi (no-ops), lvebx, stvebx, and the lvsl128/lvsr128 and *xl/*xl128 vector load/store forms.
- **Still unimplemented:** lswx/stswx, because XER's byte count isn't modelled.

**O forms.** A pre-pass sets XER[OV] and the sticky XER[SO], then runs the base op. This covers addo, addco, addeo, addmeo, addzeo, subfo, subfco, subfeo, subfmeo, subfzeo, nego, mullwo, mulldo, divwo, divwuo, divdo and divduo. Overflow is Xenon's 32-bit-mode overflow, as Xenia's tests expect; mulldo uses 64-bit overflow.

**Record forms.** After any integer op whose name ends in '.', if its case didn't set cr0 itself, the compare is now emitted for it. Before, mulld., divd., mulhw., nand., orc., sld. and others silently left cr0 alone; the bench showed it. FP record forms clear cr1, since FPSCR exception bits aren't modelled. A vector record form that doesn't set cr6 is a problem.

**Missing switch tables.** The bctr check now looks back up to 8 instructions for lis+addi (the table) and mtctr, reads the table's first word, and flags the bctr only if that word points inside the function. Function-pointer tail calls are therefore not flagged. On auto-only configs it flags exactly FM2 8281B540 and PGR4 824ED288. An earlier version without the table read gave 6 false positives.

**Other fixes:**
- "Can't decode" problems print the instruction word byte-swapped correctly.
- `Image::Find` has no bounds check, so the table read uses its own section lookup. My first version segfaulted on it.

**Bench, before and after:**

| status | before | after |
|---|---|---|
| pass | 110,259 | 112,561 |
| unimplemented | 32,901 | 28,884 |

No test went from pass to anything else. The 1,726 new failures are all in newly implemented ops and are NaN-sign or cr1 cases: fnmadd 1,715, fctiw 6, mtfsb 5. The only scalar test still unimplemented is lvx_permute, whose test data is wrong.

**Games:** FM2 and PGR4 output is identical to fresh baselines; neither game uses any of the new instructions.

## Step 10: vector instructions (tree-10)

**New emitters, 47 in all:**
- **One SSE4.1 intrinsic each:**
  - compares: vcmpgtsb/sh/sw, vcmpequh;
  - integer max/min: vmax and vmin in the sb/sh/ub/uh/uw forms, plus vminsw;
  - saturating and modulo add/sub: vadduhs, vsubuhs, vsubsbs, vsubshs, vsububm;
  - vavguh;
  - signed packs: vpkshss(128), vpkswus(128).
- **Per-element loops:** vavguw, vavgsw, vsubuws, vaddcuw, vsubcuw, vrlw(128), vrlb, vrlh, vslh, vsrh, vsrah, vsrab.
- **Unsigned and pixel packs, built in vTemp:** vpkuhum, vpkuwum, vpkuhus, vpkuwus (all with 128 forms), and vpkpx.
- **Other:** vspltish, vrfip(128), and vpkswss128, vslo128 and vsro128 added to their base cases.

**Fixes:**
- **Vector record forms:** any that don't set cr6 themselves now get `cr6.setFromMask(vD)`. vcmpgtub. and vcmpgtuh. were leaving cr6 untouched (333 bench tests).
- **vmaxfp/vminfp:** these follow VMX now. A NaN input gives that NaN quieted (vA's if both are NaN), max(-0,+0) is +0 and min(-0,+0) is -0. It costs about 5 extra SSE ops per site.
- **vlogefp:** a denormal input is treated as 0 (VMX NJ), so the result is -inf, not -149.

**Bench, before and after:**

| status | before | after |
|---|---|---|
| pass | 112,561 | 142,267 |
| fail | 29,001 | 28,169 |
| unimplemented | 28,884 | 10 |

No regressions. The 10 still unimplemented are vpkd3d128 types 1, 2 and 4 and lvx_permute (bad test data).

**Games:**
- FM2: 17 functions changed, all in 38 vmaxfp/vminfp sites and the vlogefp lines.
- PGR4: 162 functions changed, in 701 vmax/vmin sites and the vlogefp lines.
- All changed FM2 files compile, and so do 6 of PGR4's 34.

This needs a game test on the FX, since the vmax/vmin change is the first one in tonight's work that changes game behaviour. Only NaN and signed-zero edge cases differ, but PGR4 has 701 sites.

## Step 11: vpkd3d128, every type and pack (tree-11)

The three forms the games use keep their emitters: D3DCOLOR pack 1, float16_2 pack 1, and float16_4 pack 2 with shift 0–2. I checked by hand that each matches Xenia's select tables.

Every other type/pack/shift combination now goes through a generic path taken from Xenia's `InstrEmit_vpkd3d128` and its x64 `PACK`:
- vB is packed into P (big-endian elements x, y, z, w);
- each element of vD then keeps its old value or takes one of P's, by Xenia's (pack, shift) select tables.

That covers types 0–5 with packs 1–3. Type 6 (4:20:20:20) and pack 0 are problems. This adds types 1, 2 and 4, and fixes the D3DCOLOR pack 2/3 forms not zeroing the neighbouring word.

**D3DCOLOR NaN.** A NaN now packs as 0, as max-then-min does on the console; Xenia notes a game relies on this. It used to pack the NaN's low byte, so an input of FFFFFFFF gave FF. This changes FM2 and PGR4 output: 8 lines each at their D3DCOLOR sites.

**Half floats.** `ppc_float_to_xenos_half` and `ppc_pack_clamp` are now in ppc_context.h. The half helper is a verbatim copy of the ports' `ppc_float_to_half` (saturates to 0x7FFF, truncates), so generated code no longer depends on a port hook. The ports' own copy can stay, as the names don't clash.

**Bench:**
- All 28 vpkd3d128 tests pass (9 before).
- Overall: pass 142,286, fail 28,161, unimplemented 1 (lvx_permute, bad test data), compile-failed 0.
- No regressions.

**Games:**
- FM2: 19 functions changed (vmax/vmin from step 10, plus the D3DCOLOR NaN clamp and the helper rename). The changed files compile.
- PGR4: 165 functions changed. The changed files compile.

## Step 12: hardware estimates and fused madd, both opt-in (tree-12)

**New config options:**
- **`exact_estimates`:** frsqrte and vrsqrtefp(128) call `ppc_frsqrte` / `ppc_vrsqrtefp` (now in ppc_context.h). These are ported from Xenia's `PpcFrsqrte` / `PpcVrsqrtefpLane`: PowerISA table E-5, and the VMX 32-entry interpolated table. They are integer-only, so no FP mode switch. Xenia has no table for fres/vrefp (it divides), so those stay exact division.
- **`fused_madd`:** vmaddfp and vnmsubfp(128) become `simde_mm_fmadd_ps`, and `-(fmsub)` so the zero sign matches. They are only truly fused when the port builds with `-mfma`. Without it, simde does a plain multiply and add, so the cost is the same as now. ppc_context.h now includes `<x86/fma.h>`.

Both are off by default, and the FM2 and PGR4 output is unchanged.

FM2 with both options on has these call sites:

| call | sites |
|---|---|
| ppc_vrsqrtefp | 1,144 |
| fmadd | 948 |
| fmsub | 391 |
| ppc_frsqrte | 3 |

All of them compile with the port flags.

**Bench.** `run.py --options exact_estimates,fused_madd [--cflag=-mfma]` runs with recompiler options, writing to its own work directory and results file. Use `--cflag=` with an equals sign, because argparse rejects a value that starts with `-`. The driver reads the options from `REXENON_TEST_OPTIONS`.

| run | pass | fail |
|---|---|---|
| default | 142,286 | 28,161 |
| both options | 143,375 | 27,072 |
| both options + -mfma | 144,192 | 26,255 |

None of the runs had any regressions.

**For the port:** turning these on is a decision for the games. `exact_estimates` makes values match the console; vrsqrtefp is a helper call per element, so it could cost frames on the FX. `-mfma` needs an FMA3 CPU; the FX (Piledriver) has one. Neither has been tested in game.

## Step 13: function extents without hand lists, and fall-through tail calls (tree-13)

Three analysis changes, aimed at removing the hand `functions` lists (roadmap item 4).

1. **Switch-aware sizing.** `Function::Analyze` takes an optional map from each switch table's bctr address to its labels. The recompiler builds it from config.switchTables, using the first bctr within 32 words of the table base. Each case becomes a block of the function. Before, a bctr ended the block, and any case beyond the last reached instruction was cut into a function of its own.
2. **`MergeFragments`.** This runs before `SplitStrandedFunctions`. The analyser drops every block after a gap it didn't reach, and the rest of the function then appears as linear-scan "fragments" that its branches jump into. The pass extends a function sized by analysis over a fragment when one of its branches lands *inside* the fragment, or one of its switch cases reaches any part of it.
   - Every other function in the merged range must also be a fragment.
   - A `b` to a fragment's first instruction doesn't count, since it can be a tail call to a function that is only called indirectly.
   - .pdata and config functions are never resized.
3. **Fall-through.** A function whose last instruction can fall through now continues into the next function as a tail call (`sub_next(ctx, base); return;`). The old "ends prematurely" check was `#if 0`, so these functions just returned. Exceptions:
   - It doesn't apply after b, blr, bctr, padding or `tw(i) 31`.
   - It doesn't apply after a call either: that is a noreturn routine such as an assert, and if a port's hook of it returns, returning is safer than running into unrelated code.
   - With no function at the end, it's a problem.

**Results with the hand lists (current configs).**

FM2: output identical; no merges or fall-throughs.

PGR4 (4 functions changed, and they compile): 4 functions in today's PGR4 output fall through:
- **PGR4's memset (sub_828D8CA0), in two places (828D8D30 and 828D8D38).** This is the same bug FM2 had (its hand list comment: "memset's 1-3 byte tail ... lengths of 2 or 3 mod 4 lost their last bytes (Bink block types -> Arcade crash)"). PGR4 doesn't hook memset, so the shipped PGR4 build has this bug today. ReXenon fixes it with the tail calls. Tell the PGR4 session; I haven't touched D:\PGR4-ReGPU.
- 822C113C, after a conditional branch.
- 82744FD8, after what looks like data decoded as code.

**Results without the hand lists:**

| | problems before | problems after | function starts not in today's output | function starts lost |
|---|---|---|---|---|
| FM2 (28 entries removed) | 328 | 0 | 8 | 0 |
| PGR4 (262 entries removed) | 3,215 | 0 | 379 | 7 |

*FM2:* the 8 extra starts:
- memset's tail (8242E0E0, 8242E0E8): these now fall through correctly as tail calls, which is exactly the fix the hand entry made.
- The XtsClient shared epilogues (82789688, 827BD98C, 827C4D8C): these were "ERROR + return without restoring r1" when unresolved, and are now a tail call into the epilogue code, which is correct since all state is in ctx.
- Three others: 822AB818, 8267A8C0, 826C70B0.

So FM2 could plausibly drop its whole `functions` list. That needs a game test.

*PGR4:* the 7 lost starts are all "second pass" hand entries (8276AC48, 8276B030, 8278D298, 828DFF80, 828E2A10, 828EE048, 828EE858). Nothing branches to them directly any more, so they're internal now, or reached only through function pointers. If it's the latter, dropping them would break indirect calls. PGR4 should keep its list until data-pointer seeding (the roadmap's datafuncs.py rule) is done.

**Bench:** unchanged (142,286 pass), no regressions.

## Step 14: seed function starts from code pointers in data (tree-14)

New opt-in option `data_function_starts` (default off). It ports PGR4's `tools/datafuncs.py` rule into the analysis:
- Every big-endian word in `.rdata` and `.data` that points into code is a candidate if it is 4-aligned, the instruction there is non-zero, and the word before it is `blr`, padding (0) or an unconditional `b`.
- Switch labels, known symbols (including import thunks) and addresses inside a .pdata or config function are skipped.
- Each candidate is sized with `Function::Analyze` (switch-aware) before the bl-target and linear scans, and counts as an analysed function but not a scan fragment, so `MergeFragments` can never absorb it.

**Results:**

| run | seeded | problems | output vs step 13 |
|---|---|---|---|
| FM2, current config | 4,879 | 0 | identical |
| PGR4, current config | 2,839 | 0 | identical |
| FM2, no functions list | 4,887 | 0 | — |
| PGR4, no functions list | 2,919 | 0 | 379 added, 7 removed (same as step 13) |

So with today's configs the option changes nothing: every function it finds was already found by the other scans. It's a safety net, so a function only reachable through a vtable or callback can't be merged away by a future change to the analysis.

**Correction to step 13:** the 7 PGR4 "lost" starts are not function-pointer targets. 8276AC48 is a switch label (it's in pgr4_switch_tables.toml), and 8278D298 / 828DFF80 are targets of conditional branches in the code before them. The hand list split them off as separate functions. Without the list they become labels inside the function that branches to them, which is what they are. Nothing in .rdata/.data points at them, and nothing calls them with `bl` (0 problems). So PGR4's risk from dropping its list is lower than step 13 said, but it still needs a game test, as FM2's does.

No bench rerun: this step only changes analysis, not instruction emission.

**Housekeeping:** my first tree-14 snapshot accidentally included the `build*` folders, which wrote about 300 MB of loose, unreferenced git objects into `D:\ReXenon\.git`. The tree id in `tree-14.txt` is the correct, code-only one. Those objects can be removed later with `git prune`, but that would also delete the snapshot trees for steps 2–14, so only do it once the snapshots are no longer needed. Disk D: has about 2 GB free.

## Review of steps 1–14, and step 15: fixes for what it found (tree-15)

An independent review ran over the whole overnight source diff (`docs/overnight/review-input.patch`, HEAD to tree-14). It had four reviewers (scalar ops, vector ops, analysis, config/bench), and each one's findings went to a separate skeptic told to refute them. The scalar reviewer found nothing. 8 findings were confirmed and 1 was refuted (function_table_address isn't checked against the image: the code does what the config says, and that's the port's job). All 8 are fixed:

1. **vpkd3d128 FLOAT16_4 (type 5) with vD == vB and shift 1 or 2 packed garbage** (high). The old dedicated loop wrote vD halves while still reading vB. FM2 has 8 of these (`vpkd3d128 v0,v0,5,2,2`), so the shipped FM2 output has this bug today. Types 3 and 5 now take the step-11 generic path, which builds the packed words in vTemp before writing vD. Only D3DCOLOR keeps its dedicated code, which already staged through temps.
2. **vpkd3d128 FLOAT16_2 (type 3) with vD == vB and shift 2**: the same bug. Fixed the same way.
3. **`ppc_float_to_xenos_half` used IEEE-half limits.** The Xenos half has no inf, so its largest value is 131008 (0x7FFF). It now does exactly what Xenia's `emit_fast_f16_pack` does: saturate at 0x47FFE000 and above (inf and NaN included), flush below 2^-14 to a signed zero, and truncate. I checked it against a scalar transcription of Xenia's SSE sequence over all 2^32 inputs: 0 mismatches. The old type-5 code had the same 65504 limit and produced denormals.
4. **Conditional linking branches (`beql`, `bdnzl`, `bnectrl`, `beqlrl`, ...) set LR only when taken.** LK=1 writes LR either way. LR is now written before the condition. For bclrl, the old LR is read first.
5. **The fall-through rule treated a conditional call as a noreturn call.** Only an unconditional call ends a function now. A conditional call's not-taken path falls through as a tail call, or is reported as a problem.
6. **MergeFragments could absorb a fragment that is a real entry point** (a function reached only through a vtable or callback, with no .pdata entry). Fragments whose start is address-taken are never absorbed now. Address-taken means a word in any non-code section points there, or a lis + addi/ori pair in code builds that address. The lis/addi pairs inside known switch patterns are left out (they build the case code's base, not a function pointer), and so are switch labels. Pointers computed some other way still aren't seen; that is the remaining risk. Merges are unchanged: FM2 without its list still gets 23, PGR4 564, both with 0 problems.
7. **auto_switch_tables trusted the bounds-check guess.** `IsPlausibleSwitchTable` (switch_tables.cpp) now checks:
   - The case count is between 1 and 4096.
   - The cmplwi tests the register the pattern actually indexes with (the rlwinm source or the lbzx index), directly or through an `mr`.
   - Every label is an aligned code address.

   Rejected tables are printed, and the existing "jump table with no switch table entry" problem then reports them. `ReadTable` also no longer reads a table that runs outside the image. FM2 and PGR4: 0 rejected, auto output identical to config output, and XenonAnalyse's TOML files byte-identical for both games.
8. **A `keep_functions` entry that isn't an integer was undefined behaviour.** It now stops with an error.
9. **Bench: after a HANG, the next test was marked CRASH without running.** run.py now restarts right after a hung test.

**Found while testing, also fixed:** the new address-taken scan crashed on FM2. One of its sections has a virtual size that runs past the loaded image. Both that scan and step 14's data-pointer seeding now stop at the end of the image. Step 14 only read .rdata and .data, so it hadn't hit this, but it could have.

**Results:**
- FM2 (current config): 5 functions change, all vpkd3d128 FLOAT16 (the aliasing fix and the half helper).
- PGR4 (current config): 19 functions change, all vpkd3d128 FLOAT16.
- The changed chunks compile with each port's flags (FM2 36/37/39, PGR4 24/87).
- No game uses a conditional linking branch through the generic path, so finding 4 changes no game output.
- Bench: per-test results identical to step 12 (142,286 pass). The bench has no tests for the aliasing, half-range or not-taken-call cases, so those rest on the exhaustive check above and on reading the code.

**Game test needed (FX):** FM2's 5 changed vpkd3d128 functions pack data as half floats. Four have the aliased form: sub_823B76F8, sub_823B7768, sub_823E8DF8, sub_823E8EA0. sub_82395F70 changes only through the half limits. If they feed geometry, look for anything that changed visually.

**Independent check of step 15** (two reviewers, each with a refuter, working from `docs/overnight/review15-input.patch`):
- Emitted code: no findings. This covered vpkd3d128 types 3/5 through the generic path for every pack and shift, including vD == vB, compared with Xenia; the half helper; and the LR and fall-through changes.
- Analysis: one confirmed (low). `SectionBytes`, the new table-read guard, checked the section's virtual size but not the end of the loaded image. That is the same overrun as the FM2 crash, so a false pattern match could still read past the buffer. It now also checks the image end, and so does MergeFragments' code-word reader. Output is unchanged:
  - XenonAnalyse TOML byte-identical for both games.
  - FM2 auto output identical.
  - PGR4 without its list: 564 merges, 0 problems.

tree-15.txt was rewritten to include this.

## Step 16: output chunks by guest address (roadmap item 10) and step 17: switch-miss and indirect-call diagnostics (item 9) (both in tree-16)

**Step 16.** New opt-in option `chunk_by_address = <bytes>` (0, the default, keeps the old every-256-functions layout).
- `ppc_recomp.N.cpp` holds the functions in a fixed guest-address range `[code start + N*size, code start + (N+1)*size)`. Numbers can have gaps.
- Each chunk includes only `ppc_config.h` and `ppc_context.h` and declares just the functions it names. It no longer includes the 51k-line `ppc_recomp_shared.h`. The shared header is still written, for the port's own code and `genstubs.py`.
- Chunk files left over from another layout (`ppc_recomp.<digits>.cpp` not written this run) are deleted. Otherwise a port that globs `*.cpp`, as FM2's CMake does, would build duplicates. Other files are never touched. This was tested with a dummy `ppc_recomp.999.cpp` (removed) and `ppc_recomp.foo.cpp` (kept).

FM2 with `chunk_by_address = 0x8000` (FM2 has about 8 MB of code):

| | old layout | `chunk_by_address = 0x8000` |
|---|---|---|
| Chunk files | 200 | 243 (0.28–1.8 MB each) |
| Function bodies | — | identical to the old layout (fndiff: 0 changed) |
| Files that differ with vs without the functions list (8 functions plus merges) | 186 of 200, plus the shared header that every chunk includes, so all 200 rebuild | 10 of 243, and only those rebuild |

- All 243 chunks pass a syntax check with FM2's flags and `/FI ppc_hooks.h`, so the per-chunk declaration lists are complete. The two largest chunks and one other compile to objects.
- For FM2 to use it: add `chunk_by_address = 0x8000` to its config. Its CMake globs the folder, so nothing else changes. The first build after switching rebuilds everything once.
- Not done: a link and a game run with the new layout (needs the FX or this PC's game test). Clang PGO data is keyed by function name, so it should survive the renamed files.

**Step 17.** Run-time diagnostics, recompiler side:
- A switch's `default:` now emits `PPC_SWITCH_MISS(bctr address, index)`, not a bare `__builtin_unreachable()`. The macro's default in `ppc_context.h` is `__builtin_unreachable()`, so the compiled code is the same unless a port defines it, for example to log a table whose size it doubts. FM2: 195 functions change textually (321 switches), and they compile.
- `PPC_CHECK_INDIRECT_CALLS` (off by default): `PPC_CALL_INDIRECT_FUNC` checks the table slot and calls `PPC_INDIRECT_MISS(target)` for an empty one. That traps by default; a port can define it to log the guest address. It costs a branch per indirect call, so it's for debugging. Tested standalone in both modes.
- Not done (port side, so not touched tonight): filling empty table slots with a `ppc_unmapped` stub at load time (`loader.cpp`), which costs nothing per call.

Bench: per-test identical to step 15 (142,286 pass).

**Independent check of steps 16–17** (two reviewers, each with a refuter). 2 confirmed (both low) and 1 refuted, all now handled (tree-17):
- **Switching `chunk_by_address` back to 0 left high-numbered chunk files that define the same functions again**, which breaks a globbing build with duplicate symbols. FM2 isn't affected, because recomp.sh wipes `out/ppc` first. The stale-file pass is now a helper (`RemoveStaleChunkFiles`) that both layouts run. Tested: old layout written over the 243-chunk folder removed 44 stale files and left the 200 it wrote.
- **A port's `PPC_SWITCH_MISS` that only logs would fall through** and run the code after the switch. The default now emits `return;` after the macro, which costs nothing because the default macro is unreachable, and the comment says the macro should trap. FM2: 195 functions gain that line, and they compile.
- **Refuted, hardened anyway:** the checked indirect call read the table before checking, so a null or data-pointer target faulted before `PPC_INDIRECT_MISS`. It now checks the target is inside the code range first. Tested with an in-range empty slot and with null; both reach the miss handler.

Bench: per-test identical (142,286 pass).

## Step 18: write the analysis out for review and freezing (roadmap item 4, the `--emit-config` part) (tree-18)

New option `emit_analysis_path = "<folder>"` (relative to the config; empty by default, which writes nothing). At the end of analysis it writes two files:
- `functions.toml`: a `functions = [...]` list in the config format. It covers every function without a .pdata entry, plus .pdata functions whose size changed (split at a stranded branch target).
  - Functions the recompiler names itself (the `__savegprlr`/`__restfpr`/... helpers, which have their own config keys) are left out. Listing them would turn them into `sub_X`; the first round-trip test caught this (236 renamed, 9,160 callers changed).
  - Split .pdata functions are included because otherwise the frozen run gives them back their full .pdata size. The second round-trip test caught this (3 functions).
- `switch_tables.toml`: every switch table used (from the file and auto-found), sorted by address, in the switch table file format.

**Round trip** (emit from a list-free config with `auto_switch_tables`, paste `functions.toml` into a copy of that config, use `switch_tables.toml` as its switch file without auto, regenerate):

| game | functions written | switch tables written | frozen output vs emitting run |
|---|---|---|---|
| PGR4 | 31,049 | 795 | identical (fndiff 0 changed) |
| FM2 | 9,774 | 321 | identical (fndiff 0 changed) |

A port can now let the analysis find everything, review the two files, and freeze them, with nothing hand-written. With the option off, FM2's output is identical to before.

**Independent check of step 18** (one reviewer and a refuter): 1 confirmed and 4 refuted. Handled in tree-19:
- **Confirmed: write failures weren't reported.** The run printed "Wrote ..." and exited 0, leaving an old file that looked current. A folder that can't be created, a file that can't be opened, or a failed write now stops the run with `ERROR: emit_analysis_path: ...`. Tested with the path set to an existing file.
- **Refuted, guarded anyway (cheap):**
  - Duplicate addresses are written only once.
  - The run refuses to overwrite the switch table file it read.
  - The header says to *replace* any existing `functions` list. Pasting it next to an existing list is a TOML duplicate-key error.
- A normal run writes the same list as before.

## Step 20 (2026-11-14 morning): inline the save/restore helpers (roadmap item 7b), elide return LR stores (item 11a), unmapped-slot filler (item 9) (tree-20)

Three opt-in changes, all off by default. With them off, both games' output is byte-identical to 13ab3c1, except for the new macro text in the output's copy of ppc_context.h.

- **`inline_register_thunks = true`**: calls to `__savegprlr_N`, `__restgprlr_N`, `__savefpr_N`, `__restfpr_N`, `__savevmx_N` and `__restvmx_N` are replaced by the helper's own instructions, recompiled in place up to its blr.
  - Results:
    - FM2: 25,116 call sites (restgprlr 12,794, savegprlr 11,536, fpr 698, vmx 88).
    - PGR4: 23,436 call sites.
    - All restgprlr inlines come from a tail `b`, and all the rest from `bl`.
  - **Code size:** `.text` +11.6% on FM2 and +11% on PGR4, over 5 sample chunks. That is the cost the research predicted; only an FX A/B can say whether the saved calls are worth it.
  - **Guest state is unchanged.** A script folded every inlined block back into a call and compared it with the options-off output. Both games: 0 differences, and every inlined body matches the helper's own body line for line.
  - It also makes `non_argument_as_local` possible, because the save helper reads r12 from the caller.
  - **Falls back to a normal call (prints nothing):**
    - the helper isn't straight-line code (branch, sc, op 19);
    - it's in `keep_functions` or has a mid-asm hook on any word, the blr included;
    - any of its instructions fails to recompile. Everything the attempt printed or recorded is rolled back, so the helper's own function traps as before.
- **`elide_return_lr = true`**: `mtlr rX` directly before an unconditional blr doesn't store `ctx.lr`. Inside an inlined helper this applies only when the helper was reached by a tail branch.
  - Results:
    - FM2: 20,100 stores dropped, `.text` −0.4%.
    - PGR4: 4,643 stores dropped.
    - Every dropped store is `lwz r12; mtlr r12; blr` (no computed jumps).
  - **Why it is safe:**
    - A data-flow check over every function of both games found 0 LR reads (mflr, blrl, setjmp's `env = ctx`) reachable from a call without an LR write in between.
    - The check flags all of them when its entry state is forced to "after a call", so it does detect them.
    - No port hook writes `ctx.lr`, and hooks only read it at entry.
  - **Side effect, documented in the option's comment:** host code that samples `ctx.lr` at an arbitrary moment (FM2/PGR4 `crash.cpp` "guest lr", the spike sampler) can show an address inside the last callee instead of the caller. Leave the option off when those logs matter.
- **`PPC_DEFINE_UNMAPPED_FUNCTION(name)`** (ppc_context.h): a port expands it in **one** TU that sees its own `PPC_INDIRECT_MISS`. Its loader then writes `&name` into every empty indirect-call slot, before the hooks. A bad pointer then reaches `PPC_INDIRECT_MISS(ctr)` at no cost per call.
  - It is a macro, not an inline function. The ports' loaders don't include `ppc_hooks.h`, so an inline would have silently used the default trap (an ODR hazard; review finding).
- **Bench:**
  - Defaults: per-test identical to step 19 (142,286 pass).
  - `--options elide_return_lr` (new): also identical, since no test checks LR after a return.
  - The bench doesn't cover the helpers.
- **Review:** 3 reviewers, each finding checked by a skeptic, then a check of the fixes.
  - 5 low defects confirmed and fixed, all latent (none affects either game's output today):
    1. A failed inline dropped the instruction instead of trapping.
    2. A mid-asm hook or keep_functions entry on a helper was ignored.
    3. The diagnostics side effect above.
    4. The stub's ODR hazard.
    5. The mid-asm hook check skipped the helper's blr word.
  - After every fix, both games regenerate with 0 problems and output identical to the reviewed version.
  - Review input: `review20-input.patch`.
- **To try on FM2 (FX A/B):** `inline_register_thunks` and `elide_return_lr`, separately.
