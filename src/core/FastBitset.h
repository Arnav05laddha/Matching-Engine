#pragma once

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <bit>
#include "../models/Constants.h"

namespace hyperengine
{
    namespace core
    {

        template <size_t N>
        class FastBitset
        {
        private:
            static constexpr size_t DATA_WORDS = (N + 63) / 64;
            static constexpr size_t SUMMARY_WORDS = (DATA_WORDS + 63) / 64;

            uint64_t data[DATA_WORDS] = {0};
            uint64_t summary[SUMMARY_WORDS] = {0};

        public:
            inline void set(size_t i)
            {
                assert(i < N && "FastBitset::set() index out of bounds");
                size_t w = i / 64;
                size_t b = i % 64;
                data[w] |= (1ULL << b);

                size_t sw = w / 64;
                size_t sb = w % 64;
                summary[sw] |= (1ULL << sb);
            }

            inline void reset(size_t i)
            {
                assert(i < N && "FastBitset::reset() index out of bounds");
                size_t w = i / 64;
                size_t b = i % 64;
                data[w] &= ~(1ULL << b);

                // CATASTROPHIC TRAP FIX:
                // Only clear the summary bit if the ENTIRE 64-bit data block is completely zero.
                if (data[w] == 0)
                {
                    size_t sw = w / 64;
                    size_t sb = w % 64;
                    summary[sw] &= ~(1ULL << sb);
                }
            }

            inline bool test(size_t i) const
            {
                return (data[i / 64] & (1ULL << (i % 64))) != 0;
            }

            // Used to find Lowest Ask (Scanning UPWARD)
            inline size_t find_next(size_t start_idx) const
            {
                if (start_idx >= N)
                    return models::INVALID_INDEX;

                size_t w = start_idx / 64;
                size_t b = start_idx % 64;

                // 1. Check current data block
                uint64_t mask = ~0ULL << b;
                uint64_t word = data[w] & mask;
                if (word)
                {
                    return (w * 64) + std::countr_zero(word);
                }

                // 2. Scan summary layer upwards
                size_t sw = w / 64;
                size_t sb = (w % 64) + 1; // start looking at the NEXT block

                while (sw < SUMMARY_WORDS)
                {
                    uint64_t sum_mask = (sb >= 64) ? 0 : (~0ULL << sb);
                    uint64_t sum_word = summary[sw] & sum_mask;

                    if (sum_word)
                    {
                        // Found an active data block!
                        size_t next_w = (sw * 64) + std::countr_zero(sum_word);
                        // Guaranteed to be non-zero
                        return (next_w * 64) + std::countr_zero(data[next_w]);
                    }
                    sw++;
                    sb = 0; // Look at all blocks in the next summary word
                }

                return models::INVALID_INDEX;
            }

            // Used to find Highest Bid (Scanning DOWNWARD)
            inline size_t find_prev(size_t start_idx) const
            {
                if (start_idx >= N)
                {
                    start_idx = N - 1;
                }

                size_t w = start_idx / 64;
                size_t b = start_idx % 64;

                // 1. Check current data block
                uint64_t mask = (b == 63) ? ~0ULL : ((1ULL << (b + 1)) - 1);
                uint64_t word = data[w] & mask;
                if (word)
                {
                    return (w * 64) + (63 - std::countl_zero(word));
                }

                if (w == 0)
                    return models::INVALID_INDEX;

                // 2. Scan summary layer downwards
                size_t sw = (w - 1) / 64;
                size_t sb = (w - 1) % 64;

                while (true)
                {
                    uint64_t sum_mask = (sb == 63) ? ~0ULL : ((1ULL << (sb + 1)) - 1);
                    uint64_t sum_word = summary[sw] & sum_mask;

                    if (sum_word)
                    {
                        // Found an active data block!
                        size_t prev_w = (sw * 64) + (63 - std::countl_zero(sum_word));
                        // Guaranteed to be non-zero
                        return (prev_w * 64) + (63 - std::countl_zero(data[prev_w]));
                    }

                    if (sw == 0)
                        break;
                    sw--;
                    sb = 63; // Look at all blocks in the next lower summary word
                }

                return models::INVALID_INDEX;
            }
        };

    } // namespace core
} // namespace hyperengine
