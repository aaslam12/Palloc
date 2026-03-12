#include "radix_tree.h"

namespace AL
{

radix_tree::radix_tree() : root(nullptr)
{}

radix_tree::~radix_tree()
{
    if (root)
        delete_tree(root);
}

uint8_t radix_tree::extract_byte(uintptr_t addr, int level)
{
    int shift = (BYTES_IN_ADDRESS - 1 - level) * 8;
    return static_cast<uint8_t>((addr >> shift) & 0xFF);
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

    if (!root)
        root = new radix_node{};

    // walk down using shared prefix bytes of start and last 
    // when bytes diverge, store the range at the current node — the deepest
    // ancestor that covers the entire range.
    radix_node* current = root;

    for (int level = 0; level < static_cast<int>(LEVELS); ++level)
    {
        uint8_t start_byte = extract_byte(start_addr, level);
        uint8_t last_byte = extract_byte(last_addr, level);

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

std::size_t radix_tree::lookup(void* ptr) const
{
    if (!ptr || !root)
        return 0;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    radix_node* current = root;

    for (int level = 0; level < static_cast<int>(LEVELS); ++level)
    {
        std::size_t result = find_in_ranges(current->ranges, addr);
        if (result != 0)
            return result;

        uint8_t byte = extract_byte(addr, level);
        current = current->children[byte];
        if (!current)
            return 0;
    }

    // check at the deepest (leaf) node
    return find_in_ranges(current->ranges, addr);
}

} // namespace AL
