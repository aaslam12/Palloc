#include "allocator.h"
#include "arena.h"
#include "bitmap.h"
#include "dynamic_slab.h"
#include "palloc_atomic.h"
#include "platform.h"
#include "pool.h"
#include "pool_view.h"
#include "slab.h"
#include "slab_config.h"

namespace AL
{
// single-threaded aliases — zero atomic overhead, no LOCK-prefixed instructions
using st_arena = arena<PALLOC_DEFAULT_ALIGNMENT, false>;
using st_pool_view = pool_view<false>;
using st_pool = pool<false>;
using st_slab = slab<slab_config<>, false>;
} // namespace AL
