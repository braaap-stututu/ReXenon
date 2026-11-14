#pragma once
#include "symbol.h"
#include <set>

class SymbolTable : public std::multiset<Symbol, SymbolComparer>
{
public:
    const_iterator find(size_t address) const
    {
        auto [beginIt, endIt] = equal_range(address);
        if (beginIt == endIt)
        {
            return end();
        }

        size_t closest{ address - beginIt->address };
        auto match = end();
        for (auto it = beginIt; it != endIt; ++it)
        {
            if (address < it->address || address >= it->address + it->size)
            {
                continue;
            }

            const size_t distance = address - it->address;
            if (distance <= closest)
            {
                match = it;
                closest = distance;
            }
        }

        return match;
    }

    // The function whose [address, address + size) contains address, if any; find() only matches
    // symbols that start exactly at address. Looks at the nearest few starts below address, which is
    // enough for the overlaps .pdata and analysis produce.
    const_iterator findContaining(size_t address) const
    {
        auto it = upper_bound(address);
        for (int i = 0; i < 64 && it != begin(); i++)
        {
            --it;
            if (it->type == Symbol_Function && address < it->address + it->size)
                return it;
        }
        return end();
    }

    iterator find(size_t address)
    {
        auto [beginIt, endIt] = equal_range(address);
        if (beginIt == endIt)
        {
            return end();
        }

        size_t closest{ address - beginIt->address };
        auto match = end();
        for (auto it = beginIt; it != endIt; ++it)
        {
            if (address < it->address || address >= it->address + it->size)
            {
                continue;
            }

            const size_t distance = address - it->address;
            if (distance <= closest)
            {
                match = it;
                closest = distance;
            }
        }

        return match;
    }
};
