#include "slab.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

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

static char              g_slab_buf[sizeof(AL::default_slab)] __attribute__((aligned(64)));
static AL::default_slab* g_slab = nullptr;
static __thread bool     t_in_slab = false;

static AL::default_slab* get_slab()
{
    if (g_slab) return g_slab;
    if (s_in_init || t_in_slab) return nullptr;
    s_in_init = true;
    g_slab = new (g_slab_buf) AL::default_slab();
    s_in_init = false;
    return g_slab;
}

static constexpr size_t HDR      = 2 * sizeof(size_t);
static constexpr size_t TAG_SLAB = 1;
static constexpr size_t TAG_LIBC = 2;
static constexpr size_t MAX_USER = 4096 - HDR;

extern "C" {

__attribute__((visibility("default")))
void* malloc(size_t size)
{
    if (s_in_init) return boot_alloc(size);
    resolve();

    if (size <= MAX_USER && !t_in_slab)
    {
        t_in_slab = true;
        AL::default_slab* slab = get_slab();
        void* raw = slab ? slab->palloc(size + HDR) : nullptr;
        t_in_slab = false;
        if (raw)
        {
            auto* h = static_cast<size_t*>(raw);
            h[0] = TAG_SLAB; h[1] = size + HDR;
            return h + 2;
        }
    }

    void* raw = real_malloc(size + HDR);
    if (!raw) return nullptr;
    auto* h = static_cast<size_t*>(raw);
    h[0] = TAG_LIBC; h[1] = size;
    return h + 2;
}

__attribute__((visibility("default")))
void free(void* ptr)
{
    if (!ptr || is_boot(ptr)) return;
    resolve();
    auto* h = static_cast<size_t*>(ptr) - 2;
    if (h[0] == TAG_SLAB)
        { if (g_slab) g_slab->free(h, h[1]); }
    else
        real_free(h);
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
    auto* h = static_cast<size_t*>(ptr) - 2;
    size_t old_user = (h[0] == TAG_SLAB) ? h[1] - HDR : h[1];
    void* np = ::malloc(size);
    if (!np) return nullptr;
    memcpy(np, ptr, old_user < size ? old_user : size);
    ::free(ptr);
    return np;
}

__attribute__((visibility("default")))
int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    *memptr = ::malloc(size);
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
