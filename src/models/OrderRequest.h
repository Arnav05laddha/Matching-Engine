#pragma once

#include <cstdint>
#include "Constants.h"

namespace hyperengine
{
    namespace models
    {

        struct OrderRequest
        {
            uint64_t orderId;
            uint32_t price;
            uint32_t qty;
            Side side;
            OrderType type;
            uint16_t reserved; // Padding to 24 bytes
        };

        static_assert(sizeof(OrderRequest) == 24, "OrderRequest size must be 24 bytes.");

    } // namespace models
} // namespace hyperengine
