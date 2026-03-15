#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace AL
{

static constexpr size_t BYTES_IN_ADDRESS = sizeof(uintptr_t);
static constexpr size_t PAGE_SHIFT = 12;
static constexpr size_t LEVELS = 5;
static constexpr size_t MAX_RANGES_PER_NODE = 8;

// Lock-free radix tree for O(1) pointer-to-slab lookups.
//
// Leaf-only design: data is stored exclusively at leaf level, one entry per page.
// No range arrays — pure multi-level page directory. Each page maps directly
// to its owner, eliminating linear search and overflow risks.
//
// 5-level tree keyed on page numbers (addr >> 12):
//   Level 0: bits 32-35 (4 bits, max 16 entries used)
//   Level 1: bits 24-31
//   Level 2: bits 16-23
//   Level 3: bits 8-15
//   Level 4: bits 0-7  <- leaf level, stores data
class radix_tree
{
    struct range_entry
    {
        uintptr_t start;
        uintptr_t end;
        std::size_t slab_id;
    };

    struct radix_node
    {
        std::array<radix_node*, 256> children{};
        std::vector<range_entry> ranges{};
    };

    radix_node* root;

    void delete_tree(radix_node* current);
    static uint8_t extract_byte(uintptr_t page_num, int level);
    static std::size_t find_in_ranges(const std::vector<range_entry>& ranges, uintptr_t addr);

public:
    radix_tree();
    ~radix_tree();

    radix_tree(const radix_tree&) = delete;
    radix_tree& operator=(const radix_tree&) = delete;
    radix_tree(radix_tree&&) = delete;
    radix_tree& operator=(radix_tree&&) = delete;

    // NOT thread-safe — caller must synchronize.
    void insert(void* start, void* end, std::size_t slab_id);
    std::size_t lookup(void* ptr) const;
    void remove(void* start, void* end);
    void clear();
};

} // namespace AL
