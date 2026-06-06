#pragma once

#include <bitset>
#include <vector>
#include "../concurrency/SPSCQueue.h"
#include "../models/Constants.h"
#include "FastBitset.h"
#include "../models/OrderRequest.h"
#include "../models/PriceLevel.h"
#include "../models/Trade.h"
#include "../memory/OrderPool.h"

namespace hyperengine
{
    namespace core
    {

        class OrderBook
        {
        private:
            memory::OrderPool &pool;
            /*
            memory::OrderPool &pool;: The order book does not own the memory; it holds a reference to the massive pre-allocated array of orders. This is crucial because it means the OrderBook never calls new or delete. It just asks the pool for an index.
            */

            models::PriceLevel bids[models::MAX_PRICE + 1];
            models::PriceLevel asks[models::MAX_PRICE + 1];

            FastBitset<models::MAX_PRICE + 1> activeBids;
            FastBitset<models::MAX_PRICE + 1> activeAsks;

            // Maps orderId -> poolIndex for O(1) cancellations
            std::vector<uint32_t> orderLookup;
            //orderLookup: When a trader says "Cancel Order#500", you don't search the book. You simply check orderLookup[500], which instantly gives you the exact index in the OrderPool where that order lives.(Note: As we discussed earlier, if orderId grows too large, this vector will consume massive amounts of RAM. In production, this is usually a dense flat hash map).
            // Captured trades when no egress queue is provided (e.g. unit tests)
            std::vector<models::Trade> executedTrades;

            // Mutable so they can be updated inside const getLowestActiveAsk / getHighestActiveBid
            mutable uint32_t highestActiveBidHint{0};
            mutable uint32_t lowestActiveAskHint{models::MAX_PRICE};

            // Helper function to keep branch logic clean
            inline void insertIntoLevel(models::PriceLevel &level, uint32_t poolIndex,
                                        FastBitset<models::MAX_PRICE + 1> &activeLevels,
                                        uint32_t price);

            template <models::Side S>
            void matchSide(models::OrderRequest &req, concurrency::SPSCQueue<models::Trade, 65536> *egressQueue);

            inline void unlinkOrderFromLevel(models::PriceLevel &level, uint32_t orderIndex, models::Order &order);
            uint32_t getLowestActiveAsk() const;
            uint32_t getHighestActiveBid() const;

        public:
            explicit OrderBook(memory::OrderPool &pool);

            void insertOrder(const models::OrderRequest &req);
            void cancelOrder(uint64_t orderId);
            void processOrder(models::OrderRequest &req, concurrency::SPSCQueue<models::Trade, 65536> *egressQueue = nullptr);

            // Accessors for tests and inspection
            const std::vector<models::Trade> &getExecutedTrades() const { return executedTrades; }
            void clearTrades() { executedTrades.clear(); }

            // Getters for testing
            const models::PriceLevel &getBidLevel(uint32_t price) const { return bids[price]; }
            const models::PriceLevel &getAskLevel(uint32_t price) const { return asks[price]; }
            bool isBidActive(uint32_t price) const { return activeBids.test(price); }
            bool isAskActive(uint32_t price) const { return activeAsks.test(price); }
        };

    } // namespace core
} // namespace hyperengine
