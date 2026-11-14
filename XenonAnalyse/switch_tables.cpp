#include "switch_tables.h"
#include <cassert>
#include <iterator>
#include <disasm.h>
#include <image.h>
#include <xbox.h>

// The bytes [address, address + size) if they lie inside one section with data, else nullptr.
// (Image::Find returns a pointer for any address, so a table read must be bounded here.)
static const uint8_t* SectionBytes(const Image& image, size_t address, size_t size)
{
    for (const auto& section : image.sections)
    {
        if (section.data != nullptr && address >= section.base && address + size <= section.base + section.size)
        {
            // A section's virtual size can run past the loaded image (a zero-filled tail).
            const uint8_t* p = section.data + (address - section.base);
            if (p < image.data.get() || p + size > image.data.get() + image.size)
                return nullptr;
            return p;
        }
    }
    return nullptr;
}

// Reads the table's entries into its labels. False if the table doesn't lie inside the image
// (a bounds check that belongs to some other code gave a size far too large).
static bool ReadTable(Image& image, SwitchTable& table)
{
    uint32_t pOffset;
    ppc_insn insn;
    auto* code = (uint32_t*)image.Find(table.base);
    ppc::Disassemble(code, table.base, insn);
    pOffset = insn.operands[1] << 16;

    ppc::Disassemble(code + 1, table.base + 4, insn);
    pOffset += insn.operands[2];

    const size_t entrySize = table.type == SWITCH_ABSOLUTE ? 4 : table.type == SWITCH_SHORTOFFSET ? 2 : 1;
    if (SectionBytes(image, pOffset, table.labels.size() * entrySize) == nullptr)
        return false;

    if (table.type == SWITCH_ABSOLUTE)
    {
        const auto* offsets = (be<uint32_t>*)image.Find(pOffset);
        for (size_t i = 0; i < table.labels.size(); i++)
        {
            table.labels[i] = offsets[i];
        }
    }
    else if (table.type == SWITCH_COMPUTED)
    {
        uint32_t base;
        uint32_t shift;
        const auto* offsets = (uint8_t*)image.Find(pOffset);

        ppc::Disassemble(code + 4, table.base + 0x10, insn);
        base = insn.operands[1] << 16;

        ppc::Disassemble(code + 5, table.base + 0x14, insn);
        base += insn.operands[2];

        ppc::Disassemble(code + 3, table.base + 0x0C, insn);
        shift = insn.operands[2];

        for (size_t i = 0; i < table.labels.size(); i++)
        {
            table.labels[i] = base + (offsets[i] << shift);
        }
    }
    else if (table.type == SWITCH_BYTEOFFSET || table.type == SWITCH_SHORTOFFSET)
    {
        if (table.type == SWITCH_BYTEOFFSET)
        {
            const auto* offsets = (uint8_t*)image.Find(pOffset);
            uint32_t base;

            ppc::Disassemble(code + 3, table.base + 0x0C, insn);
            base = insn.operands[1] << 16;

            ppc::Disassemble(code + 4, table.base + 0x10, insn);
            base += insn.operands[2];

            for (size_t i = 0; i < table.labels.size(); i++)
            {
                table.labels[i] = base + offsets[i];
            }
        }
        else if (table.type == SWITCH_SHORTOFFSET)
        {
            const auto* offsets = (be<uint16_t>*)image.Find(pOffset);
            uint32_t base;

            ppc::Disassemble(code + 4, table.base + 0x10, insn);
            base = insn.operands[1] << 16;

            ppc::Disassemble(code + 5, table.base + 0x14, insn);
            base += insn.operands[2];

            for (size_t i = 0; i < table.labels.size(); i++)
            {
                table.labels[i] = base + offsets[i];
            }
        }
    }
    else
    {
        assert(false);
    }
    return true;
}

static void ScanTable(const uint32_t* code, size_t base, SwitchTable& table)
{
    ppc_insn insn;
    uint32_t cr{ (uint32_t)-1 };
    for (int i = 0; i < 32; i++)
    {
        ppc::Disassemble(&code[-i], base - (4 * i), insn);
        if (insn.opcode == nullptr)
        {
            continue;
        }

        if (cr == -1 && (insn.opcode->id == PPC_INST_BGT || insn.opcode->id == PPC_INST_BGTLR || insn.opcode->id == PPC_INST_BLE || insn.opcode->id == PPC_INST_BLELR))
        {
            cr = insn.operands[0];
            if (insn.opcode->operands[1] != 0)
            {
                table.defaultLabel = insn.operands[1];
            }
        }
        else if (cr != -1)
        {
            if (insn.opcode->id == PPC_INST_CMPLWI && insn.operands[0] == cr)
            {
                table.r = insn.operands[1];
                table.labels.resize(insn.operands[2] + 1);
                table.base = base;
                break;
            }
        }
    }
}

static void* SearchMask(const void* source, const uint32_t* compare, size_t compareCount, size_t size)
{
    assert(size % 4 == 0);
    uint32_t* src = (uint32_t*)source;
    size_t count = size / 4;
    ppc_insn insn;

    for (size_t i = 0; i < count; i++)
    {
        size_t c = 0;
        for (c = 0; c < compareCount; c++)
        {
            ppc::Disassemble(&src[i + c], 0, insn);
            if (insn.opcode == nullptr || insn.opcode->id != compare[c])
            {
                break;
            }
        }

        if (c == compareCount)
        {
            return &src[i];
        }
    }

    return nullptr;
}

std::vector<SwitchTable> FindSwitchTables(Image& image, SwitchTableType type)
{
    uint32_t absoluteSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_RLWINM,
        PPC_INST_LWZX,
        PPC_INST_MTCTR,
        PPC_INST_BCTR,
    };

    uint32_t computedSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_LBZX,
        PPC_INST_RLWINM,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    uint32_t offsetSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_LBZX,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    uint32_t wordOffsetSwitch[] =
    {
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_RLWINM,
        PPC_INST_LHZX,
        PPC_INST_LIS,
        PPC_INST_ADDI,
        PPC_INST_ADD,
        PPC_INST_MTCTR,
    };

    const uint32_t* pattern = nullptr;
    size_t count = 0;
    switch (type)
    {
    case SWITCH_ABSOLUTE: pattern = absoluteSwitch; count = std::size(absoluteSwitch); break;
    case SWITCH_COMPUTED: pattern = computedSwitch; count = std::size(computedSwitch); break;
    case SWITCH_BYTEOFFSET: pattern = offsetSwitch; count = std::size(offsetSwitch); break;
    case SWITCH_SHORTOFFSET: pattern = wordOffsetSwitch; count = std::size(wordOffsetSwitch); break;
    }

    std::vector<SwitchTable> switches;
    for (const auto& section : image.sections)
    {
        if (!(section.flags & SectionFlags_Code))
            continue;

        size_t base = section.base;
        uint8_t* data = section.data;
        uint8_t* dataStart = section.data;
        uint8_t* dataEnd = section.data + section.size;
        while (data < dataEnd && data != nullptr)
        {
            data = (uint8_t*)SearchMask(data, pattern, count, dataEnd - data);
            if (data != nullptr)
            {
                SwitchTable table{};
                table.type = type;
                ScanTable((uint32_t*)data, base + (data - dataStart), table);
                if (table.base != 0 && ReadTable(image, table))
                    switches.emplace_back(std::move(table));
                data += 4;
            }
        }
    }
    return switches;
}

bool IsPlausibleSwitchTable(const Image& image, const SwitchTable& table, std::string* reason)
{
    auto fail = [&](const char* why) { if (reason) *reason = why; return false; };

    if (table.labels.empty() || table.labels.size() > 4096)
        return fail("implausible case count");

    // The register the pattern indexes with: the rlwinm source (scaled tables) or the lbzx index.
    const uint32_t* code = (const uint32_t*)SectionBytes(image, table.base, 3 * 4);
    if (code == nullptr)
        return fail("pattern outside the image");
    ppc_insn tableAddress, indexing;
    ppc::Disassemble(code + 1, table.base + 4, tableAddress);
    ppc::Disassemble(code + 2, table.base + 8, indexing);
    if (tableAddress.opcode == nullptr || indexing.opcode == nullptr)
        return fail("pattern doesn't decode");
    uint32_t index;
    if (indexing.opcode->id == PPC_INST_RLWINM)
        index = indexing.operands[1];
    else if (indexing.opcode->id == PPC_INST_LBZX)
        index = indexing.operands[1] == tableAddress.operands[0] ? indexing.operands[2] : indexing.operands[1];
    else
        return fail("unexpected indexing instruction");

    // The bounds check must test that register, or one copied into it (mr) before the pattern.
    bool matches = table.r == index;
    for (int i = 1; i <= 32 && !matches; i++)
    {
        const uint32_t* word = (const uint32_t*)SectionBytes(image, table.base - 4 * i, 4);
        if (word == nullptr)
            break;
        ppc_insn insn;
        ppc::Disassemble(word, table.base - 4 * i, insn);
        if (insn.opcode != nullptr && insn.opcode->id == PPC_INST_MR &&
            ((insn.operands[0] == index && insn.operands[1] == table.r) || (insn.operands[0] == table.r && insn.operands[1] == index)))
            matches = true;
    }
    if (!matches)
        return fail("the bounds check tests a different register");

    for (size_t label : table.labels)
    {
        bool inCode = false;
        for (const auto& section : image.sections)
        {
            if ((section.flags & SectionFlags_Code) && label >= section.base && label < section.base + section.size)
                inCode = true;
        }
        if ((label & 3) != 0 || !inCode)
            return fail("a case label isn't an aligned code address");
    }
    return true;
}
