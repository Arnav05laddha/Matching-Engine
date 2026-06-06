#include "OrderBook.h"
#include <stdexcept>
#include <algorithm>
#include <thread>

namespace hyperengine
{
    namespace core
    {

        OrderBook::OrderBook(memory::OrderPool &pool) : pool(pool)
        {
        }

        inline void OrderBook::insertIntoLevel(models::PriceLevel &level, uint32_t poolIndex,
                                               FastBitset<models::MAX_PRICE + 1> &activeLevels,
                                               uint32_t price)
        {
            models::Order &newOrder = pool.get(poolIndex);

            if (level.head == models::INVALID_INDEX)
            {
                // SCENARIO A: Price level is currently empty
                level.head = poolIndex;
                level.tail = poolIndex;
                newOrder.prev = models::INVALID_INDEX;

                // Mark this price as active in the bitset for ultra-fast matching later
                activeLevels.set(price);
            }
            else
            {
                // SCENARIO B: Orders already exist at this price
                // Link the current tail to the new order
                models::Order &oldTail = pool.get(level.tail);
                oldTail.next = poolIndex;

                // Link the new order back to the old tail
                newOrder.prev = level.tail;

                // Update the level's tail pointer
                level.tail = poolIndex;
            }
        }

        void OrderBook::insertOrder(const models::OrderRequest &req)
        {
            if (req.price > models::MAX_PRICE)
            {
                throw std::invalid_argument("Price exceeds MAX_PRICE");
            }

            // 1. Allocate O(1) from your custom arena
            uint32_t poolIndex = pool.allocate();
            if (poolIndex == models::INVALID_INDEX)
            {
                throw std::runtime_error("OrderPool exhausted");
            }

            // 2. Get direct memory reference to the new order slot
            models::Order &newOrder = pool.get(poolIndex);

            // 3. Populate the struct
            newOrder.orderId = req.orderId;
            newOrder.price = req.price;
            newOrder.initialQty = req.qty;
            newOrder.remainingQty = req.qty;
            newOrder.side = req.side;
            newOrder.active = 1;
            newOrder.next = models::INVALID_INDEX; // It will be the tail, so next is null

            // 4. Route to the correct side of the book
            if (req.side == models::Side::BUY)
            {
                insertIntoLevel(bids[req.price], poolIndex, activeBids, req.price);
                highestActiveBidHint = std::max(highestActiveBidHint, req.price);
            }
            else
            {
                insertIntoLevel(asks[req.price], poolIndex, activeAsks, req.price);
                lowestActiveAskHint = std::min(lowestActiveAskHint, req.price);
            }

            // 5. Update O(1) lookup map
            if (req.orderId >= orderLookup.size())
            {
                orderLookup.resize(std::max(static_cast<size_t>(req.orderId + 1), orderLookup.size() * 2), models::INVALID_INDEX);
            }
            orderLookup[req.orderId] = poolIndex;
        }

        void OrderBook::cancelOrder(uint64_t orderId)
        {
            // 1. O(1) Memory Lookup
            if (orderId >= orderLookup.size())
                return; // Bounds check
            uint32_t poolIndex = orderLookup[orderId];

            // Check if it's already filled, canceled, or invalid
            if (poolIndex == models::INVALID_INDEX)
                return;

            models::Order &order = pool.get(poolIndex);
            if (order.active == 0)
                return; // Order is dead

            // 2. Identify the correct Price Level
            models::PriceLevel &level = (order.side == models::Side::BUY) ? bids[order.price] : asks[order.price];

            // 3. Patch Pointers (unlinkOrderFromLevel sets active=0)
            unlinkOrderFromLevel(level, poolIndex, order);

            // 4. Update the Bitset if the level is now empty
            if (level.head == models::INVALID_INDEX)
            {
                if (order.side == models::Side::BUY)
                {
                    activeBids.reset(order.price);
                }
                else
                {
                    activeAsks.reset(order.price);
                }
            }

            // 5. Clean up memory and lookup state
            pool.free(poolIndex);                         // Push back to intrusive freelist
            orderLookup[orderId] = models::INVALID_INDEX; // Invalidate lookup
        }

        uint32_t OrderBook::getLowestActiveAsk() const
        {
            uint32_t p = activeAsks.find_next(lowestActiveAskHint);
            if (p != models::INVALID_INDEX)
            {
                lowestActiveAskHint = p; // mutable — no const_cast needed
            }
            return p;
        }

        uint32_t OrderBook::getHighestActiveBid() const
        {
            uint32_t p = activeBids.find_prev(highestActiveBidHint);
            if (p != models::INVALID_INDEX)
            {
                highestActiveBidHint = p; // mutable — no const_cast needed
            }
            return p;
        }

        inline void OrderBook::unlinkOrderFromLevel(models::PriceLevel &level, uint32_t orderIndex, models::Order &order)
        {
            if (order.prev != models::INVALID_INDEX)
            {
                models::Order &prevOrder = pool.get(order.prev);
                prevOrder.next = order.next;
            }
            else
            {
                // If it has no prev, it was the head of the list
                level.head = order.next;
            }

            if (order.next != models::INVALID_INDEX)
            {
                models::Order &nextOrder = pool.get(order.next);
                nextOrder.prev = order.prev;
            }
            else
            {
                // If it has no next, it was the tail of the list
                level.tail = order.prev;
            }

            order.active = 0;
        }

        // Unified matching logic templated on side to eliminate matchBuy/matchSell duplication.
        // S == BUY  -> incoming buy sweeps asks upward  (lowest ask first)
        // S == SELL -> incoming sell sweeps bids downward (highest bid first)
        template <models::Side S>
        void OrderBook::matchSide(models::OrderRequest &req, concurrency::SPSCQueue<models::Trade, 65536> *egressQueue)
        {
            constexpr bool isBuy = (S == models::Side::BUY);

            // Select the opposing side's book and hint
            auto getBestPrice = [&]() -> uint32_t
            {
                return isBuy ? getLowestActiveAsk() : getHighestActiveBid();
            };

            auto priceMatches = [&](uint32_t bestPrice) -> bool
            {
                if (bestPrice == models::INVALID_INDEX) return false;
                return isBuy ? (bestPrice <= req.price) : (bestPrice >= req.price);
            };

            auto &activeLevels = isBuy ? activeAsks : activeBids;
            auto *levels       = isBuy ? asks         : bids;

            uint32_t currentBestPrice = getBestPrice();

            while (req.qty > 0 && priceMatches(currentBestPrice))
            {
                models::PriceLevel &level = levels[currentBestPrice];
                uint32_t restingIndex = level.head;

                // Walk the intrusive linked list at this price level
                while (restingIndex != models::INVALID_INDEX && req.qty > 0)
                {
                    models::Order &restingOrder = pool.get(restingIndex);

                    // Calculate fill quantity
                    uint32_t fillQty = std::min(req.qty, restingOrder.remainingQty);

                    // Mutate state
                    req.qty -= fillQty;
                    restingOrder.remainingQty -= fillQty;

                    // Emit Trade
                    models::Trade t{req.orderId, restingOrder.orderId, currentBestPrice, fillQty, 0};
                    if (egressQueue)
                    {
                        while (!egressQueue->push(t))
                        {
                            std::this_thread::yield();
                        }
                    }
                    else
                    {
                        executedTrades.push_back(t);
                    }

                    if (restingOrder.remainingQty == 0)
                    {
                        // SCENARIO: Resting order is completely filled
                        uint32_t nextIndex = restingOrder.next;
                        unlinkOrderFromLevel(level, restingIndex, restingOrder);
                        pool.free(restingIndex);
                        restingIndex = nextIndex;
                    }
                    else
                    {
                        // SCENARIO: Incoming order is filled, resting order is partially filled
                        break;
                    }
                }

                // If the entire price level is now empty, clear it from the bitset
                if (level.head == models::INVALID_INDEX)
                {
                    activeLevels.reset(currentBestPrice);
                    currentBestPrice = getBestPrice();
                }
                else
                {
                    break;
                }
            }
        }

        void OrderBook::processOrder(models::OrderRequest &req, concurrency::SPSCQueue<models::Trade, 65536> *egressQueue)
        {
            if (req.type == models::OrderType::CANCEL)
            {
                cancelOrder(req.orderId);
                return;
            }

            if (req.side == models::Side::BUY)
            {
                matchSide<models::Side::BUY>(req, egressQueue);
                // If there is still quantity left after matching, it rests in the book
                if (req.qty > 0 && req.type == models::OrderType::LIMIT)
                {
                    insertOrder(req);
                }
            }
            else
            {
                matchSide<models::Side::SELL>(req, egressQueue);
                if (req.qty > 0 && req.type == models::OrderType::LIMIT)
                {
                    insertOrder(req);
                }
            }
        }

    } // namespace core
} // namespace hyperengine
