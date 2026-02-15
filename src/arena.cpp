#include "arena.h"
#include <unistd.h>

arena::arena() : memory(nullptr), used(0), capacity(0)
{}

arena::~arena()
{
    if (memory != nullptr)
    {
        // ::free(memory);
    }
}

void* arena::alloc(size_t length)
{
    if (length == 0)
        return nullptr;
    return nullptr;
}

int arena::reset()
{
    used = 0;
    return 0;
}

size_t arena::get_used() const
{
    return used;
}

size_t arena::get_capacity() const
{
    return capacity;
}
