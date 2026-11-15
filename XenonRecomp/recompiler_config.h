#pragma once

struct RecompilerSwitchTable
{
    uint32_t r;
    std::vector<uint32_t> labels;
};

struct RecompilerMidAsmHook
{
    std::string name;
    std::vector<std::string> registers;

    bool ret = false;
    bool returnOnTrue = false;
    bool returnOnFalse = false;

    uint32_t jumpAddress = 0;
    uint32_t jumpAddressOnTrue = 0;
    uint32_t jumpAddressOnFalse = 0;

    bool afterInstruction = false;
};

struct RecompilerConfig
{
    std::string directoryPath;
    std::string filePath;
    std::string patchFilePath;
    std::string patchedFilePath;
    std::string outDirectoryPath;
    std::string switchTableFilePath;
    std::unordered_map<uint32_t, RecompilerSwitchTable> switchTables;
    bool skipLr = false;
    bool ctrAsLocalVariable = false;
    bool xerAsLocalVariable = false;
    bool reservedRegisterAsLocalVariable = false;
    bool skipMsr = false;
    bool crRegistersAsLocalVariables = false;
    bool nonArgumentRegistersAsLocalVariables = false;
    bool nonVolatileRegistersAsLocalVariables = false;
    // Report unresolved branches and unimplemented instructions as warnings instead of failing.
    // The generated code traps at each one either way (PPC_UNRESOLVED_BRANCH / PPC_UNIMPLEMENTED).
    bool allowUnresolved = false;
    // Calls to functions that are only blr are left out (the call site still sets LR). A port that
    // hooks one of them must list it in keep_functions; the elided ones are written to
    // ppc_elided_functions.txt so a port can check.
    bool elideEmptyFunctions = false;
    // Calls to the register save/restore helpers (__savegprlr_N, __restfpr_N, ...) are replaced by
    // the helper's own instructions, recompiled in place. Guest state stays identical; the code is
    // larger (up to ~19 stores per call site) but skips a call and the helper's prologue.
    bool inlineRegisterThunks = false;
    // `mtlr rX` right before an unconditional blr doesn't store ctx.lr (the function returns, and
    // no caller reads LR after a call before setting it). ctx.lr after a return then holds the
    // last return address set inside the callee instead of the caller's own, so host code that
    // samples ctx.lr at an arbitrary moment (crash logs, profilers) can name a callee; it is only
    // exact at function entry.
    bool elideReturnLr = false;
    std::unordered_set<uint32_t> keepFunctions;
    // Functions the port replaces with host code (hooks). non_volatile_as_local's analysis
    // doesn't look at their guest code, and treats calls to them as ordinary calls. List
    // any hooked routine that switches register state, such as a fiber switch.
    std::unordered_set<uint32_t> hostFunctions;
    // Functions to keep on the context whatever the analysis says: a hook that reads its
    // caller's non-volatile registers (ctx.r14+) must be listed, so its callers copy them to ctx.
    std::unordered_set<uint32_t> forcedContextFunctions;
    // Guest address of the indirect-call table (8 bytes per 4-byte code word, i.e. 2x PPC_CODE_SIZE).
    // 0 = right after the image. See the note the generator prints about the displacement.
    uint32_t functionTableAddress = 0;
    // sync/lwsync/eieio become real fences (PPC_CONFIG_HARDWARE_FENCES) instead of compiler barriers.
    bool hardwareFences = false;
    // Find switch jump tables with XenonAnalyse's patterns at load time. Entries in
    // switch_table_file_path win over detected ones, so a file only needs the tables the patterns miss.
    bool autoSwitchTables = false;
    // frsqrte and vrsqrtefp use the console's estimate tables (ported from Xenia) instead of an
    // exact 1/sqrt. Bit-exact with the hardware; a little slower (a helper call per value).
    bool exactEstimates = false;
    // vmaddfp/vnmsubfp(128) are emitted as fused multiply-adds (simde_mm_fmadd_ps), like the
    // console. Only fused when the port compiles with FMA enabled (-mfma); otherwise simde does a
    // separate multiply and add, the same as without this option.
    bool fusedMadd = false;
    // Seed functions from code pointers in .rdata/.data placed like a function start (right after a
    // blr, padding or an unconditional b): virtual methods, thunks and callbacks that nothing calls
    // directly and that have no .pdata entry. PGR4's tools/datafuncs.py rule, done at analysis time.
    bool dataFunctionStarts = false;
    // Split the output by guest address instead of every 256 functions: ppc_recomp.N.cpp holds the
    // functions in [code start + N * size, code start + (N + 1) * size), and declares only the
    // functions it references instead of including ppc_recomp_shared.h. Adding or removing a
    // function then rewrites only its own file, so a port rebuilds one file instead of all of them.
    // Chunk files from an earlier layout are deleted. 0 (the default) keeps the old layout.
    uint32_t chunkByAddress = 0;
    // Write the analysis results into this folder (relative to the config file), for review and to
    // freeze them: functions.toml (every function not from .pdata, with its size, in the
    // `functions` format) and switch_tables.toml (every switch table used, file and auto-found,
    // in the switch table file format). Empty (the default) writes nothing.
    std::string emitAnalysisPath;
    uint32_t restGpr14Address = 0;
    uint32_t saveGpr14Address = 0;
    uint32_t restFpr14Address = 0;
    uint32_t saveFpr14Address = 0;
    uint32_t restVmx14Address = 0;
    uint32_t saveVmx14Address = 0;
    uint32_t restVmx64Address = 0;
    uint32_t saveVmx64Address = 0;
    uint32_t longJmpAddress = 0;
    uint32_t setJmpAddress = 0;
    std::unordered_map<uint32_t, uint32_t> functions;
    std::unordered_map<uint32_t, uint32_t> invalidInstructions;
    std::unordered_map<uint32_t, RecompilerMidAsmHook> midAsmHooks;

    void Load(const std::string_view& configFilePath);
};
