#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <bit> // For std::has_single_bit

namespace hyperengine
{
    namespace concurrency
    {

        // Canonical SPSC (Single-Producer, Single-Consumer) lock-free queue.
        //
        // Memory layout is critical for performance:
        //   head  -> sits alone on its own cache line (producer writes)
        //   buffer -> shared read-only data, safe to share a line
        //   tail  -> sits alone on its own cache line (consumer writes)
        //
        // This prevents false sharing between the producer and consumer threads.
        template <typename T, size_t Size>
        class SPSCQueue
        {
            static_assert(std::has_single_bit(Size), "Queue size must be a power of 2");

            static constexpr size_t MASK = Size - 1;

        public:
            SPSCQueue() : head(0), tail(0) {}

            bool push(const T &item)
            {
                const size_t current_head = head.load(std::memory_order_relaxed);
                const size_t current_tail = tail.load(std::memory_order_acquire);

                if (current_head - current_tail == Size)
                {
                    return false;
                }

                buffer[current_head & MASK] = item;

                head.store(current_head + 1, std::memory_order_release);
                return true;
            }

            bool pop(T &item)
            {
                const size_t current_tail = tail.load(std::memory_order_relaxed);
                const size_t current_head = head.load(std::memory_order_acquire);

                if (current_tail == current_head)
                {
                    return false;
                }

                item = buffer[current_tail & MASK];

                tail.store(current_tail + 1, std::memory_order_release);
                return true;
            }

        private:
            // Producer-side: head is written by the producer thread only
            alignas(64) std::atomic<size_t> head;

            // Shared data buffer — neither producer nor consumer owns this exclusively
            std::array<T, Size> buffer;

            // Consumer-side: tail is written by the consumer thread only
            alignas(64) std::atomic<size_t> tail;
        };

    } // namespace concurrency
} // namespace hyperengine
