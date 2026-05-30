#pragma once

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#define PALLOC_ZONE(name)       ZoneScopedN(name)
#define PALLOC_PLOT(name, val)  TracyPlot(name, val)
#else
#define PALLOC_ZONE(name)       ((void)0)
#define PALLOC_PLOT(name, val)  ((void)0)
#endif
