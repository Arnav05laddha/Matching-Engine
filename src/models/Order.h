#pragma once

#include <cstdint>
#include "Constants.h"

namespace hyperengine
{
    namespace models
    {
        /*
        alignas(32): This is the most important word in the file. It tells the compiler: "Do not let this struct sit anywhere in memory. Its starting memory address must be a multiple of 32." * Why? (The Interview Answer): Modern CPUs load data from RAM into the ultra-fast L1 cache in chunks called Cache Lines, which are exactly 64 bytes wide. By forcing this struct to be exactly 32 bytes and aligned to 32, we guarantee that exactly two Order objects fit perfectly into a single 64-byte CPU cache line.
        If the struct were 36 bytes, or misaligned, an order might overlap two different cache lines. The CPU would have to fetch two memory chunks instead of one, doubling the latency (a "cache miss"). In trading, cache misses lose millions of dollars.
        */
        struct alignas(32) Order
        {
            uint64_t orderId;
            uint32_t price;
            uint32_t initialQty;
            uint32_t remainingQty;
            uint32_t next; // Used for active book AND the freelist
            uint32_t prev; // For O(1) cancels
            Side side;
            uint8_t active;
            uint16_t reserved; // Padding
        };

        static_assert(sizeof(Order) == 32, "Order size must be exactly 32 bytes for optimal L1 cache line packing.");

    } // namespace models
} // namespace hyperengine


/*
4. What does active mean? Why not just remove the node?
You are completely correct about the logic: if an order is cancelled or fully traded, we remove that node from the active Order Book's linked list.

But remember our strict rule: We never use the delete keyword.

When we remove the order from the Order Book list, we don't destroy it. We take that 32-byte chunk of memory and move it to a different linked list called the Freelist (the pool of empty, recycled orders waiting to be used).

The active flag (which acts as a boolean: 1 or 0) tells us which list this block of memory is currently in:

active = 1: This memory block contains a live order sitting in the Order Book.

active = 0: This memory block is a "ghost." It is sitting in the Freelist waiting for a new order to overwrite it.
*/