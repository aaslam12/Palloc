#include "dynamic_slab.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

// bootstrap buffer: satisfies allocations during dlsym symbol resolution
// (dlsym itself calls calloc internally)
static char   s_boot[65536] __attribute__((aligned(16)));
static size_t s_boot_used = 0;
static bool   s_in_init   = false;

static void* boot_alloc(size_t sz)
{
    sz = (sz + 15) & ~size_t(15);
    if (s_boot_used + sz > sizeof(s_boot)) return nullptr;
    void* p = s_boot + s_boot_used;
    s_boot_used += sz;
    return p;
}

static bool is_boot(void* p)
{
    return p >= (void*)s_boot && p < (void*)(s_boot + sizeof(s_boot));
}

// real libc pointers resolved via dlsym
using malloc_fn  = void*(*)(size_t);
using free_fn    = void (*)(void*);
using realloc_fn = void*(*)(void*, size_t);

static malloc_fn  real_malloc  = nullptr;
static free_fn    real_free    = nullptr;
static realloc_fn real_realloc = nullptr;

static void resolve()
{
    if (real_malloc) return;
    s_in_init = true;
    real_malloc  = (malloc_fn)  dlsym(RTLD_NEXT, "malloc");
    real_free    = (free_fn)    dlsym(RTLD_NEXT, "free");
    real_realloc = (realloc_fn) dlsym(RTLD_NEXT, "realloc");
    s_in_init = false;
}

// dynamic_slab: no thread_local, uses grow_mutex + radix tree for free routing
// free_unsized() eliminates the need to store size in a header
// lazy construction to avoid DSO-load-time issues
static char                      g_ds_buf[sizeof(AL::default_dynamic_slab)]
    __attribute__((aligned(64)));
static AL::default_dynamic_slab* g_ds      = nullptr;
static __thread bool             t_in_ds   = false; // re-entrancy guard (no malloc in __thread init)

static AL::default_dynamic_slab* get_ds()
{
    if (g_ds) return g_ds;
    if (s_in_init || t_in_ds) return nullptr;
    s_in_init = true;
    g_ds = new (g_ds_buf) AL::default_dynamic_slab();
    s_in_init = false;
    return g_ds;
}

extern "C" {

__attribute__((visibility("default")))
void* malloc(size_t size)
{
    if (s_in_init) return boot_alloc(size);
    resolve();

    if (!t_in_ds)
    {
        t_in_ds = true;
        AL::default_dynamic_slab* ds = get_ds();
        void* p = ds ? ds->palloc(size) : nullptr;
        t_in_ds = false;
        if (p) return p;
    }

    return real_malloc(size);
}

__attribute__((visibility("default")))
void free(void* ptr)
{
    if (!ptr || is_boot(ptr)) return;
    resolve();
    // free_unsized returns false if ptr is not owned by this dynamic_slab
    if (g_ds && g_ds->free_unsized(ptr)) return;
    real_free(ptr);
}

__attribute__((visibility("default")))
void* calloc(size_t nmemb, size_t size)
{
    if (s_in_init) return boot_alloc(nmemb * size);
    void* p = ::malloc(nmemb * size);
    if (p) memset(p, 0, nmemb * size);
    return p;
}

__attribute__((visibility("default")))
void* realloc(void* ptr, size_t size)
{
    if (!ptr) return ::malloc(size);
    if (!size) { ::free(ptr); return nullptr; }
    if (is_boot(ptr)) { void* np = ::malloc(size); if (np) memcpy(np, ptr, size); return np; }
    // old size unknown without header; copy only new size bytes (caller responsibility)
    void* np = ::malloc(size);
    if (!np) return nullptr;
    memcpy(np, ptr, size);
    ::free(ptr);
    return np;
}

__attribute__((visibility("default")))
int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    *memptr = ::malloc(size); // dynamic_slab aligns to block_size (power of two)
    return *memptr ? 0 : ENOMEM;
}

__attribute__((visibility("default")))
void* aligned_alloc(size_t alignment, size_t size)
{
    void* p = nullptr;
    posix_memalign(&p, alignment, size);
    return p;
}

} // extern "C"
