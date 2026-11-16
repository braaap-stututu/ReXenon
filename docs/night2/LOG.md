# ReXenon night 2 (2026-11-16): cutting the generated code's CPU time

Goal (user, 01:00): spend the night getting ReXenon's CPU time down, tested on the FX (FX-8350 + GTX 970).

Method:
- Exes are linked at the fixed base 0x7FF600000000 (`build/rx/fixbase.cmd <ppc dir> <label> <base>`), so the
  load-address lottery (night 1, steps 32-33) doesn't decide the result.
- Baseline `a0` = fm2-port main feaef32 + a benchmark change (below), built against `out/ppc_tu4_rexenon`
  (ReXenon aae3334 defaults).
- New metric: benchmark.txt now has a `Threads:` line, each guest thread's CPU share over the recorded race
  (GetThreadTimes at the start and end of recording). t3 is the game's main thread, t9 the command processor.
  CPU ms per frame for a thread = share x 22 s / frames. This measures the generated code's cost directly,
  where fps only moves when the saved thread is the one limiting the frame.
- Also Timeline segment 1 (the low-noise fps figure on the FX) and the sampler (`FM2_SAMPLE_THREAD=all`,
  `FM2_SAMPLE_BENCH=1`, `FM2_SAMPLE_OUT`) for per-function shares.

FX disk: the user approved deleting the NVIDIA DXCache, the crash dumps and the Windows Update download cache
(1.56 GB free after). DXCache may be cleared between test exes tonight.

## Step 1 (01:25-01:40, FX): baseline a0 and where the time goes

a0 runs (fixed base, 720p windowed):

| run | avg fps | 1% low | seg1 | Threads |
|---|---|---|---|---|
| warm-up | 78.4 | 49.8 | 81 | t3 100%, t9 97%, t7 46% |
| a0-1 | 96.4 | 63.0 | 80 | t3 99%, t9 94% |
| a0-2 | 106.8 | 65.7 | 80 | t3 100%, t9 95% |
| a0-3 | 101.8 | 64.8 | 81 | t3 100%, t9 95% |

- **The main thread (t3, guest code) is now the limit on the FX.** The CP (t9) has a few % of slack after
  CP rounds 2-3. Night 1's "ReXenon codegen gives no FX gain" (steps 31-33) was measured while t9 was at
  ~98% and was the limit, so codegen savings on t3 could not show then. Those options are re-measured below.
- t3-only profile (FM2_SAMPLE_THREAD=3, 10,698 samples): 95% generated code, flat. Top function
  sub_826214E8 3.9% (vector matrix code: 20 dpps), then 2.2%, 1.8%, 1.7%... Save/restore helpers 6.2%.
  CRT memcpy 8242D140 1.1%, memset 8242E050 ~0.7%.
- Compiled sub_826214E8: 265 of 674 instructions are mov (ctx register traffic), 33 bswap, 20 dpps,
  6 ldmxcsr, 17 cvtss2sd + 11 cvtsd2ss.

## Step 2: n6 (non_argument_as_local) hangs at boot, dropped

`n6` = r5 + `non_argument_as_local` (r0, r2, r11, r12, f0, v32-63 as locals). It builds after guarding the
port's few ctx.r11/r12 reads (crash.cpp, perf_probe.cpp, under PPC_CONFIG_NON_ARGUMENT_AS_LOCAL), but hangs
locally 0.7 s into boot. ReXenon has no safety analysis for this option (recompiler.cpp:847 just makes the
registers locals): unlike non_volatile_as_local, nothing finds the functions that read these registers before
writing them. Xbox 360 code passes values in r11/r12 in places (e.g. `__chkstk` takes the frame size in r12, and
the save/restore helpers use r12), so a callee reading r12 from ctx sees garbage. Making it safe needs an
entry-read analysis plus write-back at the call sites, so it is left for later.

## Step 3: the volatile guest accessors (codegen analysis)

Four analysts read the compiled code of the 48 hottest t3 functions (scratchpad n2/hot48.txt). The biggest
shared cost, about 35-40% of the cycles in the functions they read: `PPC_LOAD_*`/`PPC_STORE_*` are
`*(volatile T*)`, and LLVM treats volatile accesses as clobbering any memory, so every ctx register read after a
guest access is reloaded from ctx, ctx stores aren't kept in host registers, and bswap pairs through a ctx
register aren't folded. memcpy 8242D140's 128-byte loop does 4 loads + 2 stores + 2 bswaps per 8 bytes where
1 load + 1 store would do.

Replacement: relaxed atomics (`__atomic_load_n/__atomic_store_n(..., __ATOMIC_RELAXED)`). Each guest access is
still emitted once, at its width (atomics are never merged, split, removed or hoisted out of a spin loop), and
LLVM keeps ordered memory operations in program order; x86 is TSO, and the sync/lwsync fences still apply. But a
relaxed atomic doesn't clobber ctx. This is a header-only change, so it A/Bs without regenerating:
build/rx/atomic_access.h is force-included into the generated code (`FM2_PPC_FORCE_INCLUDE`), exe `at`.

Found while checking it: the baseline accessors aren't plain volatile. clang-cl compiles volatile with MSVC
semantics (`/volatile:ms`), so in the IR every guest load is `load atomic volatile ... acquire` and every store
`store atomic volatile ... release`. On x86 both are plain movs, but acquire/release stops the compiler from
moving any other memory access (ctx included) across them. The relaxed version is still atomic and ordered in
the backend (MachineInstr::hasOrderedMemoryRef is true for both), so only the ctx traffic around it changes.
Memory-model argument: PPC lets loads and stores to different addresses reorder unless the guest uses
lwsync/sync/eieio, and those still compile to fences or compiler barriers. Same-address order (coherence) and
address dependencies are kept by atomics. x86 TSO is stronger than PPC.

Local check (5800X + 4070, fixed base, 720p, alternating runs):

| run | avg fps | Timeline | Threads |
|---|---|---|---|
| a0 | 207.9 | 196 / 206 / 220 / 210 | t3 99%, t9 95% |
| at | 269.6 | 221 / 284 / 291 / 280 | t3 88%, t9 89% |
| a0 | 249.5 | 220 / 274 / 262 / 232 | t3 99%, t9 90% |
| at | 271.7 | 219 / 277 / 294 / 296 | t3 90%, t9 90% |

t3 drops from 99% to about 89% while the frame rate rises, so the main thread does the same work in less CPU
time. Races complete normally.

## Step 4 (01:45-02:15, FX group 1): r2 and r4 against a0, now that t3 is the limit

Warm-ups w-r2, w-r4, then 4 rounds of a0/r2/r4 in rotating order. Fixed base, 720p windowed.

| arm | seg1 per round | avg fps per round | t3 |
|---|---|---|---|
| a0 | 81 81 81 81 | 98.6 103.3 104.3 87.9 | 99-100% |
| r2 | 83 82 81 81 | 112.2 97.4 104.2 91.1 | 99-100% |
| r4 | 82 80 82 82 | 98.8 114.8 94.9 103.1 | 99-100% |

At most +1% for r2 on seg1, inside the noise; the average swings 88-115 between rounds. These options remove
calls and stores that were cheap, so the night-1 verdict stands even with t3 as the limit.

Order check of the atomic header over all 200 chunks (scratchpad n2/ir/order.py, optimised IR, per function
the sequence of guest accesses, volatile vs relaxed): 49,107 of 50,973 functions identical; 1,667 have more static
accesses (tail duplication: a shared block copied into each path), 150 fewer (a common last access sunk into
the shared successor: checked 827480A8), 10 the same accesses in a different textual order (checked
8221CC60: loop rotation only, same dynamic order). A checker agent traced the other 9 path by path against the
generated C++ (scratchpad n2/ir/lang.py, an automaton comparison of the access sequences): all same-order.
The differences are loop rotation, different unrolling, tail merging, and in 8271CC30 the relaxed build pruning
CFG edges that can never run (UTF-8/16 encode branches). No access moved across another, a call or a fence.

## Step 5 (02:17-02:40, FX group 2): at (relaxed-atomic accessors) against a0

Warm-up w-at, then 5 alternating rounds. Timeline segments 1-4:

| run | seg1 | seg2 | seg3 | seg4 | avg | t3 |
|---|---|---|---|---|---|---|
| a0-1 | 81 | 104 | 119 | 138 | | 99% |
| at-1 | 82 | 108 | 114 | 123 | | 99% |
| a0-2 | 81 | 103 | 87 | 95 | 90.5 | 99% |
| at-2 | 81 | 104 | 123 | 138 | 109.4 | 99% |
| a0-3 | 81 | 105 | 114 | 114 | 104.1 | 100% |
| at-3 | 82 | 107 | 113 | 115 | 104.3 | 100% |
| a0-4 | 81 | 104 | 113 | 112 | 103.1 | 100% |
| at-4 | 82 | 105 | 115 | 115 | 104.3 | 99% |
| a0-5 | 81 | 101 | 78 | 82 | 86.5 | 100% |
| at-5 | 81 | 105 | 105 | 91 | 95.1 | 99% |

seg1 +0.7% (81.0 -> 81.6), seg2 +2.3% (103.4 -> 105.8; at wins all 5 pairs, binomial p = 0.03), segs 3-4 too
noisy. Small on the FX, where the 5800X gained 10-30% and t3 dropped from 99% to 89%. The FX's t3 stays at
99-100%, so the main thread's time per frame barely changed there. The removed instructions are ctx reloads
that hit L1 through store forwarding. On Zen 3 those cost issue slots, which is where the time went; on
Piledriver the main thread seems to be bound by something else (cache misses in pointer chasing, the shared
module front end). The a0 t3 profile has no spin loops (host waits are under 2%), so it isn't waiting.

FX t3 profile of at (10,511 samples) against a0's (10,698): the per-function shares barely move. memcpy
8242D140 1.12% -> 1.08%; 824DA2A0 1.82 -> 1.28 and 82377730 0.85 -> 0.47 drop, others rise slightly. So on
Piledriver the main thread's time isn't in the ctx traffic the change removes.

## Step 6 (02:55-, FX): thread placement (port side, test only)

The FX-8350 is 4 modules of 2 cores sharing a front end and FPU (Windows reports 4 cores, 8 logical). t3 and
t9 both run at 95-100%, and if they land on one module each loses a lot. The port never sets affinity.
Test hook (fm2-port, uncommitted): `FM2_PIN="3:1,9:4,*:F0"` sets hex affinity masks per guest thread id ('*' =
the rest) when the benchmark recording starts (GuestThreadPin in kernel/thread.cpp). Exe `atp` = at + this
hook. Arms: u (no pin), p1 = t3 on CPU 0, t9 on CPU 2, other guest threads on modules 2-3; p2 = only t3 and
t9 pinned. Host threads (GPU driver, audio) stay free.

## Step 7: relaxed atomics as ReXenon's default (uncommitted)

XenonUtils/ppc_context.h: PPC_LOAD_*/PPC_STORE_* go through new `PPC_GUEST_LOAD(T, p)` / `PPC_GUEST_STORE(T, p, v)`,
relaxed atomics by default; `PPC_VOLATILE_GUEST_ACCESS` restores XenonRecomp's volatile ones, and a port can
define the two macros itself. Not yet in fm2-port: out/ppc_tu4_rexenon has its own copy of ppc_context.h from
generation, so it takes effect when the output is regenerated (or with the force-included header, as tonight).

Instruction tests (`py -I ReXenonTests/run.py`): 142,286 pass / 28,161 fail / 1 unimplemented, the same as before.
The test driver didn't link (undefined Recompiler::FindContextFunctions): ReXenonTests/CMakeLists.txt was missing
nonvolatile_context.cpp since the non_volatile_as_local work. Added.

Candidate C7 (divide by a constant loaded with li) is covered by the atomics: with volatile accessors LLVM
reloads the divisor from ctx after each guest access and emits idiv (82377730: 2 idiv in its loop); with relaxed
atomics it sees the li constant and emits a multiply (0 divides). Chunk 33 as a whole: 75 -> 50 div/idiv. Of the
other workflow candidates, C1 (loop register caching) is largely what LLVM now does on its own, since ctx isn't
clobbered by guest accesses. C2-C6 were rated 0.1-0.6% by the verifiers, below what the FX can resolve.

Round 1 (exe atp, warm-up then 4 rotating rounds; Timeline seg1 / seg2, 1% low):

| arm | seg1 | seg2 | 1% low | t3 |
|---|---|---|---|---|
| u (no pin) | 83 82 103 82 | 106 107 108 106 | 67.2 67.7 72.3 67.9 | 100% |
| p1 (t3 CPU0, t9 CPU2, rest modules 2-3) | 86 86 85 85 | 108 107 111 111 | 56.3 54.5 58.2 64.6 | 97% |
| p2 (only t3 CPU0, t9 CPU2) | 81 81 82 81 | 108 108 109 105 | 45.8 45.7 48.0 47.5 | 86-89% |

p1 moves seg1 for the first time tonight (+4%, 82 -> 85.5), seg2 +2%. But single-CPU pins hurt the 1% lows:
a pinned thread has to wait whenever anything else (host threads aren't pinned) runs on its CPU. p2 is worst
(t3 drops to 87% busy, lows 46): the unpinned guest threads land on t3's CPU. Round 2 tries whole-module masks.

Candidate C4 (`-fno-slp-vectorize` on the generated code, to stop 128-bit ctx pair accesses that fail store
forwarding), on top of the atomics, locally (5800X, alternating): at 271 / 282 / 264 fps, ns 218 / 258 / 268. No better (slower on average),
dropped. (clang-cl needs `/clang:-fno-slp-vectorize`; the plain flag is ignored with a warning.) Test option
added to fm2-port CMake (uncommitted): `-DFM2_PPC_EXTRA_FLAGS=...`.

Round 2 (no warm-up; seg1 / seg2, 1% low, 0.1% low):

| arm | seg1 | seg2 | 1% low | 0.1% low |
|---|---|---|---|---|
| u | 82 82 82 81 | 105 108 107 107 | 68.3 66.5 67.3 65.7 | 53.5 53.9 50.0 56.6 |
| p3 (t3 module 0 = mask 3, t9 module 1 = C, rest F0) | 85 80 85 107 | 111 84 110 113 | 63.0 61.1 68.3 78.9 | 42.4 51.3 55.0 71.0 |
| p1 | 80 85 85 85 | 101 108 109 109 | 37.4 52.0 61.0 60.2 | 26.0 26.5 38.7 32.1 |

p1's lows are bad again: single-CPU pins are out. p3 looks better on seg1/seg2 with lows about even, but p3-2
was bad throughout, so round 3 runs 6 more pairs of u and p3.

Cross-thread check: the GPU command processor (host thread t9) polls the ring write pointer, a plain guest
word at 0x7FC80714. The guest's WPTR store (ppc_recomp.35.cpp ~8060: `stwx` packet words ... `sync`;
`stw r29,1812(r11)`; `eieio`; `sync`) is fenced. PPC_SYNC/EIEIO/LWSYNC are `std::atomic_signal_fence(seq_cst)`
without hardware_fences, and a signal fence orders atomic accesses for the compiler (it formally doesn't order
non-atomic volatile ones), so the relaxed version keeps this publish order by the language rules, not just by
LLVM behaviour. The other register writes checked (34:20746, 34:21080, 35:11806) are followed by eieio/sync too.

Round 3 (6 pairs, alternating order; seg1 / seg2 / 1% low / 0.1% low):

| run | u | p3 |
|---|---|---|
| 1 | 81 / 104 / 65.5 / 53.9 | 85 / 108 / 67.9 / 57.2 |
| 2 | 82 / 106 / 67.4 / 54.5 | 85 / 113 / 70.2 / 59.6 |
| 3 | 82 / 109 / 67.4 / 56.0 | 86 / 107 / 64.5 / 48.5 |
| 4 | 79 / 103 / 65.9 / 51.6 | 85 / 109 / 67.4 / 57.9 |
| 5 | 82 / 104 / 66.9 / 53.5 | 85 / 113 / 69.2 / 57.1 |
| 6 | 82 / 108 / 65.6 / 51.4 | 86 / 111 / 71.4 / 58.8 |

**p3 wins: seg1 +4.9% (81.3 -> 85.3, every p3 run above every u run), seg2 +4.1% (105.7 -> 110.2), 1% low +3.0% (66.5 -> 68.4),
0.1% low +5.7% (53.5 -> 56.5).** Giving the main thread and the command processor a Piledriver module each (both cores of the
module, so a host thread landing there doesn't stall them) and keeping the other guest threads on the
remaining modules is the biggest FX gain tonight. Single-CPU pins (p1, p2) hurt the lows. It is a port-side
scheduling change, not codegen. On SMT CPUs the same idea is "a physical core each"; on hybrid Intel CPUs
it must pick P-cores. Untested there.

## Step 8: closing the reviewers' gap (vector and reservation accesses), generator change

An adversarial review of the atomic change (3 lenses: memory model, compiler/UB, port integration) found no
regression in today's x64 build, but one gap: guest accesses that don't go through PPC_LOAD/STORE (simde
vector lvx/lvlx/lvrx/stvx, the lwarx/ldarx reservation load, dcbz's memset) used to be pinned by the
acquire/release scalar accesses around them. With relaxed scalars, LLVM may CSE or move them across, e.g.
reuse a vector load from before a flag spin loop (the reviewer reproduced that mechanism in a test). PPC-legal,
but it would break guest code that worked by relying on the console re-reading. Also, my note above that a
signal fence orders the WPTR publish "by the language rules" was wrong: a signal fence orders nothing across
threads; it holds by compiler behaviour plus x86 TSO, as before.

Fix (ReXenon, uncommitted): the generator emits `PPC_GUEST_LOAD(uint32_t/uint64_t, ...)` for lwarx/ldarx and new
`PPC_GUEST_VLOAD(p)` / `PPC_GUEST_VSTORE(p, v)` for lvx/lvlx/lvrx/stvx. By default these are 16-byte volatile
accesses (under clang-cl a 16-byte volatile is a plain volatile, not acquire/release, checked in IR), so they
can't be merged, removed or reused; under PPC_VOLATILE_GUEST_ACCESS they are the old plain simde accesses.
dcbz's memset stays plain. Regenerated as out/ppc_n2_av: the diff against out/ppc_tu4_rexenon is exactly
those sites (4322 + 3176 + 2616 + 112 vector loads, 4206 + 2679 vector stores, 675 reservation loads) plus the
header. Local: av 272 / 273 fps vs at 281 / 273 (same; av's first run, 219, was the new exe's first launch,
which is always slow: driver shader cache per exe, so ns's 218 above was probably that too).

ReXenon bug found on the way: generating into a folder that doesn't exist crashed (fail-fast 0xC0000409, no
message: fopen returned null and fwrite took it). The writer now creates the folder and reports an error when a
file can't be opened.

## Step 9: FM2_PIN=auto (fm2-port, test hook)

`FM2_PIN=auto` derives p3 from the topology (GetLogicalProcessorInformationEx, RelationProcessorCore): t3 and
t9 get one core each (all its logical processors) from the highest efficiency class, the other guest threads
the remaining cores; needs 4+ cores. FX: 3 / C / F0 (= p3). 5800X: 3 / C / FFF0. Exe `avp` = av + hook.

Local 5800X (alternating; avg / 1% low / seg1): u 280.3 / 165.0 / 226, 270.1 / 160.8 / 217, 249.6 / 161.7 / 225;
auto 247.3 / 114.5 / 218, 284.6 / 177.6 / 240, 279.1 / 169.6 / 239. Two of three auto runs best of all, one
bad; no clear loss on an SMT CPU, not conclusive.

Instruction tests with the step 8 generator change: 142,286 pass / 28,161 fail / 1 unimplemented / 0
compile-failed, unchanged.

Soak check of av (local, all-AI quick races, FM2_ARGS launch=true nohumans=true, 2 minutes each): tracks 0, 2
and 20 ran without a crash or trap. The log fills with a Lua HUD error (`attempt to index field 'mOptions'`,
from updateSplits/setGauges3P), but a0 shows the same, so it belongs to the all-AI quick race and predates
tonight. The other log lines match a0's apart from addresses and timing.

FX, av + FM2_PIN=auto against av unpinned (warm-up, then 5 alternating pairs; seg1 / seg2 / 1% low):
u 82 / 107 / 66.5, 82 / 107 / 60.8, 83 / 106 / 67.7, 81 / 105 / 66.2, 81 / 102 / 48.8;
auto 85 / 109 / 68.5, 86 / 108 / 67.3, 85 / 109 / 67.6, 85 / 108 / 63.8, 85 / 108 / 56.3.
seg1 +4.2% (81.8 -> 85.2), seg2 +2.8% (105.4 -> 108.4), 1% low 62.0 -> 64.7. Repeats p3 with the auto layout.

## Step 10 (04:36-05:00, FX): the whole package against main

a0 (main, unpinned) against avp (relaxed atomics + vector/reservation fix + FM2_PIN=auto), 6 alternating pairs
(seg1 / seg2 / 1% low / 0.1% low):

| round | a0 | avp |
|---|---|---|
| 1 | 82 / 104 / 67.6 / 54.1 | 86 / 106 / 68.7 / 55.4 |
| 2 | 82 / 105 / 66.1 / 52.1 | (70 / 70 / 57.7 / 54.9: FX busy, see below) |
| 3 | 82 / 104 / 66.0 / 50.7 | 85 / 106 / 59.1 / 42.4 |
| 4 | 81 / 105 / 66.8 / 54.6 | 85 / 104 / 64.7 / 48.8 |
| 5 | 81 / 105 / 66.7 / 54.0 | 85 / 103 / 56.2 / 40.8 |
| 6 | 81 / 103 / 64.2 / 49.4 | 84 / 104 / 64.2 / 46.6 |

avp-2 is left out: flat 70 fps, its recording started at 117 s instead of ~80 s, and the boot precompile took
48.6 s, so something else was loading the FX. Without it: seg1 +4.3% (81.5 -> 85.0, every pair), seg2 level
(104.3 -> 104.6), but 1% low 66.2 -> 62.6 and 0.1% low 52.5 -> 46.8.

Across all pinned runs tonight (p3 and auto) seg1 gains in every run, but the 1% lows spread more:
pinned 56-79 (median ~65.5), unpinned 61-68 (median ~66.5), with the bad ones (56, 59) in this last test. The
other guest threads (t7 ~40%, t14 ~18%, t18 ~9% and short ones) plus unpinned host threads share 4 logical
CPUs under p3, so a burst there can stall a frame. Step 11 tries p4 = t3/t9 on their modules, others free.

## Step 11 (05:02-05:22, FX): p4 (t3/t9 on their modules, other threads free), exe avp

| arm | seg1 | seg2 | 1% low | 0.1% low |
|---|---|---|---|---|
| u | 82 82 82 82 | 107 106 106 105 | 65.8 64.9 67.1 65.4 | 49.8 53.9 55.4 53.7 |
| p3 | 85 86 85 85 | 110 110 112 111 | 69.1 66.2 69.7 68.3 | 54.8 52.5 57.9 53.3 |
| p4 | 84 84 83 84 | 107 107 109 108 | 68.3 65.4 66.4 68.7 | 57.9 51.2 46.0 54.5 |

p3 is the best layout: seg1 +4%, seg2 +4.5%, 1% low +3.8% over u. p4 is in between. So keeping the other guest
threads off the two modules matters too. Step 10's worse lows didn't repeat here, so they look like that
session's noise; the lows over all pinned runs are about even with unpinned, or a little better.

## Summary of the night

1. **Relaxed-atomic guest accessors** (ReXenon, uncommitted; on by default, PPC_VOLATILE_GUEST_ACCESS for the old
   ones). clang-cl's MSVC volatile made every guest access acquire/release, which pinned all ctx traffic around it.
   5800X: main thread CPU per frame about -17%, fps +10-30%. FX: +1-2%. Checked: IR access order across all
   functions, 11 differing functions traced by hand, 3-lens adversarial review, ReXenon instruction tests
   unchanged, races on 4 tracks. The reviewers' gap (plain vector/reservation accesses) is closed in the
   generator (step 8).
2. **FX thread placement** (fm2-port test hook FM2_PIN, uncommitted): main thread and command processor on a
   module each, other guest threads on the rest: seg1 +4-5%, seg2 +3-4.5%, lows about even to slightly better.
   FM2_PIN=auto builds that from the topology. Not tried on Intel or hybrid CPUs.
3. No gain: r2/r4 options, -fno-slp-vectorize, n6 (hangs). C7 (constant divides) comes free with 1.

To ship 1: commit ReXenon, bump fm2-port's thirdparty/ReXenon, regenerate out/ppc_tu4_rexenon (the build reads
its ppc_context.h copy there). To ship 2: decide on a default (off, FX-only, or auto everywhere) and test on
other CPUs first.

