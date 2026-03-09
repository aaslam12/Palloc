#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
namespace AL
{

static constexpr size_t BYTES_IN_ADDRESS = sizeof(uintptr_t);
static constexpr size_t LEVELS = BYTES_IN_ADDRESS;

// radix tree that helps perform O(1) pointer look ups
class radix_tree
{
    struct radix_node
    {
        std::array<radix_node*, 256> children{};
        std::size_t slab_id{};
    };

    radix_node* root;

    radix_node* get_or_create_node(radix_node* current, int level, uintptr_t addr);
    radix_node* find_node(radix_node* current, int level, uintptr_t addr) const;
    void delete_tree(radix_node* current);
    uint8_t extract_byte(uintptr_t addr, int level) const;

public:
    radix_tree();
    ~radix_tree();

    void insert(void* start, void* end, std::size_t slab_id);
    std::size_t lookup(void* ptr) const;
};

} // namespace AL
