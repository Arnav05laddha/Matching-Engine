#pragma once

#include <cassert>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include "../models/Order.h"
#include "../models/Constants.h"

namespace hyperengine
{
    namespace memory
    {

        class OrderPool
        {
        private:
            std::vector<models::Order> arena;
            uint32_t freeHead;
            size_t capacity;

        public:
            explicit OrderPool(size_t capacity) : capacity(capacity)
            {
                if (capacity >= models::INVALID_INDEX)
                {
                    throw std::invalid_argument("Capacity too large");
                }

                arena.resize(capacity);

                // Initialize the intrusive freelist by linking all next pointers together
                for (uint32_t i = 0; i < capacity - 1; ++i)
                {
                    arena[i].next = i + 1;
                }
                arena[capacity - 1].next = models::INVALID_INDEX;
                freeHead = 0;
            }

            // O(1) allocation
            uint32_t allocate()
            {
                if (freeHead == models::INVALID_INDEX)
                {
                    return models::INVALID_INDEX; // Pool exhausted
                }
                uint32_t index = freeHead;
                freeHead = arena[freeHead].next;
                return index;
            }

            // O(1) deallocation
            void free(uint32_t index)
            {
                if (index >= capacity)
                    return;
                arena[index].next = freeHead;
                freeHead = index;
            }

            // Direct array access — asserts in debug builds to catch stale/corrupted indices
            models::Order &get(uint32_t index)
            {
                assert(index < capacity && "OrderPool::get() index out of bounds");
                return arena[index];
            }

            const models::Order &get(uint32_t index) const
            {
                assert(index < capacity && "OrderPool::get() index out of bounds");
                return arena[index];
            }

            size_t getCapacity() const { return capacity; }
        };

    } // namespace memory
} // namespace hyperengine
