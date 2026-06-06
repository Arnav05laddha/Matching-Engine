#pragma once

#include <cstdint>

namespace hyperengine
{
    namespace models
    {

        struct alignas(32)Trade
        {
            uint64_t makerOrderId;
            uint64_t takerOrderId;
            uint32_t price;
            uint32_t qty;
            uint64_t timestamp;
        };

        static_assert(sizeof(Trade) == 32, "Trade size must be 32 bytes.");

    } // namespace models
} // namespace hyperengine

/*

Maker = The Resting Order (Memory Pool). This is an order that arrived early. It couldn't find a match, so the engine inserted it into the Order Book's linked list. It is sitting completely still in RAM, waiting.

Taker = The Incoming Order (The Queue). This is the order that just popped out of the SPSCQueue. The engine is currently holding it in the CPU registers, actively scanning the Order Book to see if it matches anything.

*/