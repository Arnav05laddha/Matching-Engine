#pragma once

#include <cstdint>
#include <limits>

namespace hyperengine
{
        namespace models
        {

                enum class Side : uint8_t
                {
                        BUY = 0,
                        SELL = 1
                };

                enum class OrderType : uint8_t
                {
                        LIMIT = 0,
                        MARKET = 1,
                        CANCEL = 2,
                        SHUTDOWN = 3 // Explicit engine stop signal — avoids fragile magic sentinels
                };

                static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();
                static constexpr uint32_t MAX_PRICE = 2000000;

        } // namespace models
} // namespace hyperengine

/*
*/