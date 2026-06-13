#pragma once

// dynamic_slab has been removed. Use slab<> (default_slab), which grows on demand.
// Any attempt to instantiate this type is a compile error.

#include "slab_config.h"

namespace AL
{

template<slab_config_type Tconfig>
class [[deprecated("dynamic_slab is removed. Use default_slab (slab<slab_config<>>) instead.")]] dynamic_slab
{
    static_assert(!sizeof(Tconfig), "dynamic_slab is removed. Use default_slab instead.");
};

using default_dynamic_slab = dynamic_slab<slab_config<>>;

} // namespace AL
