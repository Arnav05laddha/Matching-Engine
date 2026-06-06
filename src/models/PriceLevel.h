#pragma once

#include <cstdint>
#include "Constants.h"

namespace hyperengine
{
    namespace models
    {

        struct PriceLevel
        {
            uint32_t head = INVALID_INDEX;
            uint32_t tail = INVALID_INDEX;
        };

        static_assert(sizeof(PriceLevel) == 8, "PriceLevel size must be 8 bytes.");

    } // namespace models
} // namespace hyperengine
