#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Image;

// Jump tables the compiler emitted for switch statements, found by their code patterns
// (lis/addi table address, a scaled load, mtctr, bctr) and sized from the bounds check
// (cmplwi + bgt/ble) just before. Shared by XenonAnalyse (which writes them to a TOML file) and
// the recompiler (auto_switch_tables).
enum SwitchTableType
{
    SWITCH_ABSOLUTE = 0,    // table of 32-bit addresses
    SWITCH_COMPUTED = 1,    // byte offsets, shifted, from a base
    SWITCH_BYTEOFFSET = 2,  // byte offsets from a base
    SWITCH_SHORTOFFSET = 3, // halfword offsets from a base
};

struct SwitchTable
{
    std::vector<size_t> labels{};
    size_t base{};
    size_t defaultLabel{};
    uint32_t r{};
    uint32_t type{};
};

// The tables of one pattern type, in address order.
std::vector<SwitchTable> FindSwitchTables(Image& image, SwitchTableType type);

// Checks a pattern-found table before it is trusted without review (auto_switch_tables): a sane
// case count, a bounds check on the register the pattern actually indexes with (directly or
// through an mr), and every label an aligned code address. On failure, reason says why.
bool IsPlausibleSwitchTable(const Image& image, const SwitchTable& table, std::string* reason = nullptr);
