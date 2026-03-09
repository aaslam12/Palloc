#include "radix_tree.h"

namespace AL
{

radix_tree::radix_node* radix_tree::get_or_create_node(radix_node* current, int level, uintptr_t addr)
{
    return nullptr;
}

radix_tree::radix_node* radix_tree::find_node(radix_node* current, int level, uintptr_t addr) const
{
    return nullptr;
}

void radix_tree::delete_tree(radix_node* current)
{}

uint8_t radix_tree::extract_byte(uintptr_t addr, int level) const
{
    return 0;
}

radix_tree::radix_tree()
{}

radix_tree::~radix_tree()
{}

void radix_tree::insert(void* start, void* end, std::size_t slab_id)
{}

std::size_t radix_tree::lookup(void* ptr) const
{
    return 0;
}

} // namespace AL
