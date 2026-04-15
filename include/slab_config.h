#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "platform.h"

namespace AL
{

struct size_class
{
    std::size_t byte_size;
    std::size_t num_blocks;
    std::size_t batch_size;
};

constexpr bool is_power_of_two(std::size_t x) noexcept
{
    return x && ((x & (x - 1)) == 0);
}

consteval bool is_valid_config(const auto& arr) noexcept
{
    std::size_t prev_size = 0;
    for (std::size_t i = 0; i < arr.size(); ++i)
    {
        auto const& sc = arr[i];
        if (sc.byte_size == 0)
            return false;
        if (!is_power_of_two(sc.byte_size))
            return false;
        if (sc.num_blocks == 0)
            return false;
        if (sc.batch_size == 0)
            return false;
        if (sc.batch_size > sc.num_blocks)
            return false;
        if (i > 0 && sc.byte_size <= prev_size)
            return false;
        if (sc.num_blocks > (std::numeric_limits<std::size_t>::max() / sc.byte_size))
            return false;
        prev_size = sc.byte_size;
    }
    return true;
}

template<std::size_t Tnum = 10,
         std::array<size_class, Tnum> Tsize_class_config =
             {
                 size_class{   .byte_size = 8, .num_blocks = 512, .batch_size = 64},
                 size_class{  .byte_size = 16, .num_blocks = 512, .batch_size = 64},
                 size_class{  .byte_size = 32, .num_blocks = 256, .batch_size = 32},
                 size_class{  .byte_size = 64, .num_blocks = 256, .batch_size = 32},
                 size_class{ .byte_size = 128, .num_blocks = 128, .batch_size = 16},
                 size_class{ .byte_size = 256, .num_blocks = 128, .batch_size = 16},
                 size_class{ .byte_size = 512,  .num_blocks = 64,  .batch_size = 8},
                 size_class{.byte_size = 1024,  .num_blocks = 64,  .batch_size = 8},
                 size_class{.byte_size = 2048,  .num_blocks = 32,  .batch_size = 4},
                 size_class{.byte_size = 4096,  .num_blocks = 32,  .batch_size = 4}
},
         std::size_t Tnum_cached_classes = Tnum,
         std::size_t Tvirtual_mem_prealloc_size = AL::ONE_GB * 100>
struct slab_config
{
    using palloc_slab_config_tag = void;

    static_assert(Tnum_cached_classes <= Tnum, "NUM_CACHED_CLASSES must be <= total size-class count");

    static_assert(Tnum > 0, "at least one size class required");
    static_assert(is_valid_config(Tsize_class_config),
                  "Invalid SIZE_CLASS_CONFIG: power-of-two, strictly increasing sizes, non-zero counts required");

    inline static constexpr std::array<size_class, Tnum> SIZE_CLASS_CONFIG = Tsize_class_config;
    static constexpr std::size_t NUM_SIZE_CLASSES = Tnum;
    static constexpr std::size_t NUM_CACHED_CLASSES = Tnum_cached_classes;
    static constexpr std::size_t VIRTUAL_MEM_PREALLOC_SIZE = Tvirtual_mem_prealloc_size;

    static constexpr std::size_t INDEX_SPAN =
        std::bit_width(Tsize_class_config[Tnum - 1].byte_size) - std::bit_width(Tsize_class_config[0].byte_size) + 1;

    // gaps in the config round up to the next available size class.
    // e.g. in config {8,64}: sizes 9-64 all go to 64B pool.
    static consteval auto compute_index_lut()
    {
        std::array<std::size_t, INDEX_SPAN> lut{};
        std::size_t min_bw = std::bit_width(Tsize_class_config[0].byte_size);
        for (std::size_t vi = 0; vi < INDEX_SPAN; ++vi)
        {
            std::size_t target_size = std::size_t(1) << (min_bw + vi - 1);
            lut[vi] = static_cast<std::size_t>(-1);
            for (std::size_t j = 0; j < Tnum; ++j)
            {
                if (Tsize_class_config[j].byte_size >= target_size)
                {
                    lut[vi] = j;
                    break;
                }
            }
        }
        return lut;
    }

    inline static constexpr auto INDEX_LUT = compute_index_lut();

    // compute total bytes needed for all pools' sub-regions (with alignment padding).
    // assumes page-aligned base (mmap), so first pool always starts aligned.
    static constexpr std::size_t compute_total_region_size()
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < Tnum; ++i)
        {
            auto const& sc = Tsize_class_config[i];
            // align cursor to block_size
            std::size_t mask = sc.byte_size - 1;
            total = (total + mask) & ~mask;
            // add region for this pool
            std::size_t bitmap_words = (sc.num_blocks + 63) / 64;
            std::size_t bitmap_bytes = bitmap_words * sizeof(uint64_t);
            std::size_t aligned_offset = ((bitmap_bytes + sc.byte_size - 1) / sc.byte_size) * sc.byte_size;
            total += aligned_offset + sc.byte_size * sc.num_blocks;
        }
        return total;
    }
};

template<typename T>
concept slab_config_type = requires {
    typename T::palloc_slab_config_tag;
    requires std::same_as<typename T::palloc_slab_config_tag, void>;
    { T::NUM_SIZE_CLASSES } -> std::convertible_to<std::size_t>;
    { T::NUM_CACHED_CLASSES } -> std::convertible_to<std::size_t>;
    { T::VIRTUAL_MEM_PREALLOC_SIZE } -> std::convertible_to<std::size_t>;
    { T::INDEX_SPAN } -> std::convertible_to<std::size_t>;
    { T::SIZE_CLASS_CONFIG };
    { T::INDEX_LUT };
    { T::compute_total_region_size() } -> std::convertible_to<std::size_t>;
};

} // namespace AL
