#include "radix_tree.h"
#include <cassert>

namespace AL
{

radix_tree::radix_tree() : root(nullptr)
{}

radix_tree::~radix_tree()
{
    if (root)
        delete_tree(root);
}

uint8_t radix_tree::extract_byte(uintptr_t page_num, int level)
{
    // 5-level page-number tree:
    // L0 = bits 32-35 (4-bit fanout subset), L1..L4 = bits 24..0 in 8-bit chunks.
    if (level == 0)
        return static_cast<uint8_t>((page_num >> 32) & 0x0F);

    const int shift = (4 - level) * 8;
    return static_cast<uint8_t>((page_num >> shift) & 0xFF);
}

void radix_tree::delete_tree(radix_node* current)
{
    if (!current)
        return;
    for (auto* child : current->children)
    {
        if (child)
            delete_tree(child);
    }
    delete current;
}

std::size_t radix_tree::find_in_ranges(const std::vector<range_entry>& ranges, uintptr_t addr)
{
    for (const auto& range : ranges)
    {
        if (addr >= range.start && addr < range.end)
            return range.slab_id;
    }
    return 0;
}

void radix_tree::insert(void* start, void* end, std::size_t slab_id)
{
    if (!start || !end || start >= end || slab_id == 0)
        return;

    uintptr_t start_addr = reinterpret_cast<uintptr_t>(start);
    uintptr_t end_addr = reinterpret_cast<uintptr_t>(end);
    uintptr_t last_addr = end_addr - 1;
    uintptr_t start_page = start_addr >> PAGE_SHIFT;
    uintptr_t last_page = last_addr >> PAGE_SHIFT;

    if (!root)
        root = new radix_node{};

    radix_node* current = root;

    for (int level = 0; level < static_cast<int>(LEVELS); ++level)
    {
        uint8_t start_byte = extract_byte(start_page, level);
        uint8_t last_byte = extract_byte(last_page, level);

        if (start_byte != last_byte)
            break; // divergence. store at current node

        if (!current->children[start_byte])
            current->children[start_byte] = new radix_node{};
        current = current->children[start_byte];
    }

    for (auto& range : current->ranges)
    {
        if (range.start == start_addr && range.end == end_addr)
        {
            range.slab_id = slab_id;
            return;
        }
    }
    current->ranges.push_back({start_addr, end_addr, slab_id});
}

void radix_tree::remove(void* start, void* end)
{
    if (!start || !end || start >= end || !root)
        return;

    uintptr_t start_addr = reinterpret_cast<uintptr_t>(start);
    uintptr_t end_addr = reinterpret_cast<uintptr_t>(end);
    uintptr_t last_addr = end_addr - 1;
    uintptr_t start_page = start_addr >> PAGE_SHIFT;
    uintptr_t last_page = last_addr >> PAGE_SHIFT;

    radix_node* current = root;

    for (int level = 0; level < static_cast<int>(LEVELS); ++level)
    {
        uint8_t start_byte = extract_byte(start_page, level);
        uint8_t last_byte = extract_byte(last_page, level);

        if (start_byte != last_byte)
            break;

        radix_node* child = current->children[start_byte];
        if (!child)
            return;
        current = child;
    }

    // find and remove the matching range by swapping with last entry
    auto& ranges = current->ranges;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        if (ranges[i].start == start_addr && ranges[i].end == end_addr)
        {
            ranges[i] = ranges.back();
            ranges.pop_back();
            return;
        }
    }
}

void radix_tree::clear()
{
    if (root)
    {
        delete_tree(root);
        root = nullptr;
    }
}

std::size_t radix_tree::lookup(void* ptr) const
{
    if (!ptr || !root)
        return 0;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t page = addr >> PAGE_SHIFT;
    radix_node* current = root;

    for (int level = 0; level < static_cast<int>(LEVELS); ++level)
    {
        std::size_t result = find_in_ranges(current->ranges, addr);
        if (result != 0)
            return result;

        uint8_t byte = extract_byte(page, level);
        current = current->children[byte];
        if (!current)
            return 0;
    }

    // check at the deepest (leaf) node
    return find_in_ranges(current->ranges, addr);
}

} // namespace AL
