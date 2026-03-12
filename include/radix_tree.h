#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace AL
{

static constexpr size_t BYTES_IN_ADDRESS = sizeof(uintptr_t);
static constexpr size_t LEVELS = BYTES_IN_ADDRESS;

// radix tree for O(1) pointer-to-slab lookups.
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
    static uint8_t extract_byte(uintptr_t addr, int level);
    static std::size_t find_in_ranges(const std::vector<range_entry>& ranges, uintptr_t addr);

public:
    radix_tree();
    ~radix_tree();

    void insert(void* start, void* end, std::size_t slab_id);
    std::size_t lookup(void* ptr) const;
};

} // namespace AL
