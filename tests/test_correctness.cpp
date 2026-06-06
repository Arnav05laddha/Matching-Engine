#include <memory>
// ============================================================================
// test_correctness.cpp
//
// Comprehensive correctness tests for the HyperEngine matching engine.
// Covers: order book integrity, matching semantics, edge cases, cancel logic,
// market orders, and multi-level sweep behaviour.
//
// Uses Google Test. Run via: ctest or ./build/test_correctness
// ============================================================================

#include <gtest/gtest.h>
#include <vector>
#include <numeric>
#include <algorithm>

#include "../src/memory/OrderPool.h"
#include "../src/core/OrderBook.h"

using namespace hyperengine;
using namespace hyperengine::models;
using namespace hyperengine::memory;
using namespace hyperengine::core;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Creates a fresh pool + book pair for each test to avoid state leakage.
struct Fixture {
    OrderPool pool;
    std::unique_ptr<OrderBook> book;
    Fixture(size_t cap = 100'000) : pool(cap), book(std::make_unique<OrderBook>(pool)) {}

    void process(OrderRequest req) { book->processOrder(req); }

    // Shorthand builders
    OrderRequest limit(uint64_t id, uint32_t price, uint32_t qty, Side s) {
        return {id, price, qty, s, OrderType::LIMIT, 0};
    }
    OrderRequest market(uint64_t id, uint32_t qty, Side s) {
        // Market orders use price=0 for sells, MAX_PRICE for buys so they always match
        uint32_t p = (s == Side::BUY) ? MAX_PRICE : 0;
        return {id, p, qty, s, OrderType::MARKET, 0};
    }
    OrderRequest cancel(uint64_t id) {
        return {id, 0, 0, Side::BUY, OrderType::CANCEL, 0};
    }
};

// ============================================================================
// 1. STRUCT SIZE & ALIGNMENT GUARANTEES
// ============================================================================

TEST(MemoryLayout, OrderIs32Bytes) {
    EXPECT_EQ(sizeof(Order), 32u);
}
TEST(MemoryLayout, TradeIs32Bytes) {
    EXPECT_EQ(sizeof(Trade), 32u);
}
TEST(MemoryLayout, PriceLevelIs8Bytes) {
    EXPECT_EQ(sizeof(PriceLevel), 8u);
}
TEST(MemoryLayout, OrderRequestIs24Bytes) {
    EXPECT_EQ(sizeof(OrderRequest), 24u);
}
TEST(MemoryLayout, OrderAlignedTo32) {
    EXPECT_EQ(alignof(Order), 32u);
}

// ============================================================================
// 2. ORDER BOOK INSERT & STRUCTURE
// ============================================================================

TEST(Insert, SingleBidAppearsInBook) {
    Fixture f;
    f.process(f.limit(1, 100, 50, Side::BUY));
    const auto& lvl = f.book->getBidLevel(100);
    EXPECT_NE(lvl.head, INVALID_INDEX);
    EXPECT_EQ(lvl.head, lvl.tail);
    EXPECT_TRUE(f.book->isBidActive(100));
}

TEST(Insert, SingleAskAppearsInBook) {
    Fixture f;
    f.process(f.limit(1, 200, 50, Side::SELL));
    const auto& lvl = f.book->getAskLevel(200);
    EXPECT_NE(lvl.head, INVALID_INDEX);
    EXPECT_TRUE(f.book->isAskActive(200));
}

TEST(Insert, FIFOOrderingAtSamePrice) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.process(f.limit(2, 100, 20, Side::BUY));
    f.process(f.limit(3, 100, 30, Side::BUY));

    const auto& lvl = f.book->getBidLevel(100);
    // Head should be first inserted order
    const Order& head = f.pool.get(lvl.head);
    EXPECT_EQ(head.orderId, 1u);
    const Order& tail = f.pool.get(lvl.tail);
    EXPECT_EQ(tail.orderId, 3u);
}

TEST(Insert, MultiplePriceLevelsTracked) {
    Fixture f;
    for (uint32_t p : {100u, 101u, 102u}) {
        f.process(f.limit(p, p, 10, Side::BUY));
    }
    EXPECT_TRUE(f.book->isBidActive(100));
    EXPECT_TRUE(f.book->isBidActive(101));
    EXPECT_TRUE(f.book->isBidActive(102));
    EXPECT_FALSE(f.book->isBidActive(99));
}

TEST(Insert, PoolExhaustedThrows) {
    Fixture f(5);
    for (int i = 1; i <= 5; ++i)
        f.process(f.limit(i, 100, 1, Side::BUY));
    EXPECT_THROW(f.process(f.limit(6, 100, 1, Side::BUY)), std::runtime_error);
}

TEST(Insert, PriceExceedsMaxThrows) {
    Fixture f;
    EXPECT_THROW(f.book->insertOrder({1, MAX_PRICE + 1, 10, Side::BUY, OrderType::LIMIT, 0}),
                 std::invalid_argument);
}

// ============================================================================
// 3. PRICE-TIME PRIORITY MATCHING
// ============================================================================

TEST(Matching, ExactFill) {
    Fixture f;
    f.process(f.limit(1, 100, 50, Side::SELL));
    f.process(f.limit(2, 100, 50, Side::BUY));

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].qty, 50u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].price, 100u);

    // Both sides should be empty
    EXPECT_FALSE(f.book->isAskActive(100));
    EXPECT_FALSE(f.book->isBidActive(100));
}

TEST(Matching, TakerPartialFill_MakerRemains) {
    Fixture f;
    f.process(f.limit(1, 100, 100, Side::SELL)); // Maker: 100 qty
    f.process(f.limit(2, 100,  40, Side::BUY));  // Taker: 40 qty

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].qty, 40u);

    // Maker still has 60 remaining
    const auto& lvl = f.book->getAskLevel(100);
    ASSERT_NE(lvl.head, INVALID_INDEX);
    EXPECT_EQ(f.pool.get(lvl.head).remainingQty, 60u);
    EXPECT_EQ(f.pool.get(lvl.head).orderId, 1u);
}

TEST(Matching, MakerPartialFill_TakerRests) {
    Fixture f;
    f.process(f.limit(1, 100,  40, Side::SELL)); // Maker: 40 qty
    f.process(f.limit(2, 100, 100, Side::BUY));  // Taker: 100 qty — fills maker, 60 left

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].qty, 40u);

    // Ask side empty
    EXPECT_FALSE(f.book->isAskActive(100));

    // Buyer rests with 60 remaining
    const auto& lvl = f.book->getBidLevel(100);
    ASSERT_NE(lvl.head, INVALID_INDEX);
    EXPECT_EQ(f.pool.get(lvl.head).remainingQty, 60u);
}

TEST(Matching, PricePriority_BestAskFillsFirst) {
    Fixture f;
    f.process(f.limit(1, 101, 10, Side::SELL)); // Worse ask
    f.process(f.limit(2, 100, 10, Side::SELL)); // Better ask (lower price)

    f.process(f.limit(3, 102, 20, Side::BUY));  // Sweep both

    const auto& trades = f.book->getExecutedTrades();
    ASSERT_EQ(trades.size(), 2u);
    // First trade must be at price 100 (best ask)
    EXPECT_EQ(trades[0].price, 100u);
    EXPECT_EQ(trades[1].price, 101u);
}

TEST(Matching, TimePriority_SamePriceFIFO) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::SELL)); // First in, should fill first
    f.process(f.limit(2, 100, 10, Side::SELL));
    f.process(f.limit(3, 100, 10, Side::SELL));

    f.process(f.limit(10, 100, 15, Side::BUY)); // Sweep first 1.5 orders

    const auto& trades = f.book->getExecutedTrades();
    ASSERT_EQ(trades.size(), 2u);
    // First trade against orderId=1 (FIFO)
    EXPECT_EQ(trades[0].takerOrderId, 1u);
    EXPECT_EQ(trades[0].qty, 10u);
    // Second trade against orderId=2 (partial)
    EXPECT_EQ(trades[1].takerOrderId, 2u);
    EXPECT_EQ(trades[1].qty, 5u);
}

TEST(Matching, MultilevelSweep) {
    Fixture f;
    // Build an ask side: 5 levels x 10 qty each
    for (uint32_t price = 100; price <= 104; ++price)
        f.process(f.limit(price, price, 10, Side::SELL));

    // Buy 45 units at price 104 — should sweep all 5 levels except last 5
    f.process(f.limit(200, 104, 45, Side::BUY));

    const auto& trades = f.book->getExecutedTrades();
    ASSERT_EQ(trades.size(), 5u);

    uint32_t totalFilled = 0;
    for (auto& t : trades) totalFilled += t.qty;
    EXPECT_EQ(totalFilled, 45u);

    // First 4 levels empty
    for (uint32_t price = 100; price <= 103; ++price)
        EXPECT_FALSE(f.book->isAskActive(price));

    // Level 104 partially filled (5 remaining)
    EXPECT_TRUE(f.book->isAskActive(104));
    const auto& lvl104 = f.book->getAskLevel(104);
    EXPECT_EQ(f.pool.get(lvl104.head).remainingQty, 5u);
}

TEST(Matching, BuyDoesNotMatchIfPriceTooLow) {
    Fixture f;
    f.process(f.limit(1, 105, 10, Side::SELL)); // Ask at 105
    f.process(f.limit(2, 100, 10, Side::BUY));  // Bid at 100 — no cross

    EXPECT_EQ(f.book->getExecutedTrades().size(), 0u);
    EXPECT_TRUE(f.book->isAskActive(105));
    EXPECT_TRUE(f.book->isBidActive(100));
}

TEST(Matching, SellDoesNotMatchIfPriceTooHigh) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));  // Bid at 100
    f.process(f.limit(2, 105, 10, Side::SELL)); // Ask at 105 — no cross

    EXPECT_EQ(f.book->getExecutedTrades().size(), 0u);
}

TEST(Matching, TradePriceIsRestingOrderPrice) {
    Fixture f;
    // Resting ask at 100, aggressive buy at 110 — trade price should be 100 (maker's price)
    f.process(f.limit(1, 100, 10, Side::SELL));
    f.process(f.limit(2, 110, 10, Side::BUY));

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].price, 100u); // Maker's price
}

TEST(Matching, MakerTakerIdsCorrect) {
    Fixture f;
    f.process(f.limit(42, 100, 10, Side::SELL)); // maker
    f.process(f.limit(99, 100, 10, Side::BUY));  // taker

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    const auto& t = f.book->getExecutedTrades()[0];
    EXPECT_EQ(t.makerOrderId, 99u);  // incoming is stored as maker field
    EXPECT_EQ(t.takerOrderId, 42u);  // resting is stored as taker field
}

// ============================================================================
// 4. CANCEL CORRECTNESS
// ============================================================================

TEST(Cancel, CancelHead_NextBecomesHead) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.process(f.limit(2, 100, 10, Side::BUY));
    f.process(f.limit(3, 100, 10, Side::BUY));

    f.book->cancelOrder(1);
    const auto& lvl = f.book->getBidLevel(100);
    EXPECT_EQ(f.pool.get(lvl.head).orderId, 2u);
    EXPECT_EQ(f.pool.get(lvl.head).prev, INVALID_INDEX);
}

TEST(Cancel, CancelTail_PrevBecomesTail) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.process(f.limit(2, 100, 10, Side::BUY));
    f.process(f.limit(3, 100, 10, Side::BUY));

    f.book->cancelOrder(3);
    const auto& lvl = f.book->getBidLevel(100);
    EXPECT_EQ(f.pool.get(lvl.tail).orderId, 2u);
    EXPECT_EQ(f.pool.get(lvl.tail).next, INVALID_INDEX);
}

TEST(Cancel, CancelMiddle_LinkedListIntact) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.process(f.limit(2, 100, 10, Side::BUY));
    f.process(f.limit(3, 100, 10, Side::BUY));

    f.book->cancelOrder(2);

    // Get indices by looking at pool slots (0,1,2 for ids 1,2,3)
    const auto& lvl = f.book->getBidLevel(100);
    uint32_t headIdx = lvl.head;
    uint32_t tailIdx = lvl.tail;

    const Order& head = f.pool.get(headIdx);
    const Order& tail = f.pool.get(tailIdx);

    EXPECT_EQ(head.orderId, 1u);
    EXPECT_EQ(tail.orderId, 3u);
    EXPECT_EQ(head.next, tailIdx);
    EXPECT_EQ(tail.prev, headIdx);
}

TEST(Cancel, CancelOnlyOrder_LevelCleared) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.book->cancelOrder(1);

    const auto& lvl = f.book->getBidLevel(100);
    EXPECT_EQ(lvl.head, INVALID_INDEX);
    EXPECT_EQ(lvl.tail, INVALID_INDEX);
    EXPECT_FALSE(f.book->isBidActive(100));
}

TEST(Cancel, CancelNonExistentOrder_NoOp) {
    Fixture f;
    // Should not crash or throw
    EXPECT_NO_THROW(f.book->cancelOrder(9999999));
}

TEST(Cancel, DoubleCancelIdempotent) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.book->cancelOrder(1);
    // Second cancel should be a safe no-op
    EXPECT_NO_THROW(f.book->cancelOrder(1));
    EXPECT_FALSE(f.book->isBidActive(100));
}

TEST(Cancel, PoolSlotRecycledAfterCancel) {
    Fixture f(3);
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.process(f.limit(2, 100, 10, Side::BUY));
    f.process(f.limit(3, 100, 10, Side::BUY));

    // Pool exhausted. Free one by cancelling, then a new order should succeed.
    f.book->cancelOrder(2);
    EXPECT_NO_THROW(f.process(f.limit(4, 101, 10, Side::BUY)));
}

TEST(Cancel, CancelAlreadyFilledOrder_NoOp) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::SELL));
    f.process(f.limit(2, 100, 10, Side::BUY)); // Fills order 1 completely

    // Trying to cancel the now-filled order 1 should be safe
    EXPECT_NO_THROW(f.book->cancelOrder(1));
}

// ============================================================================
// 5. MARKET ORDERS
// ============================================================================

TEST(MarketOrder, BuyMarketFillsEntireBook) {
    Fixture f;
    for (uint32_t p = 100; p <= 105; ++p)
        f.process(f.limit(p, p, 10, Side::SELL));

    // Market buy of 60 — should sweep all 6 levels
    f.process(f.market(200, 60, Side::BUY));

    const auto& trades = f.book->getExecutedTrades();
    uint32_t totalFilled = 0;
    for (auto& t : trades) totalFilled += t.qty;
    EXPECT_EQ(totalFilled, 60u);

    for (uint32_t p = 100; p <= 105; ++p)
        EXPECT_FALSE(f.book->isAskActive(p));
}

TEST(MarketOrder, SellMarketFillsEntireBidSide) {
    Fixture f;
    for (uint32_t p = 95; p <= 100; ++p)
        f.process(f.limit(p, p, 10, Side::BUY));

    f.process(f.market(200, 60, Side::SELL));

    const auto& trades = f.book->getExecutedTrades();
    uint32_t totalFilled = 0;
    for (auto& t : trades) totalFilled += t.qty;
    EXPECT_EQ(totalFilled, 60u);
}

TEST(MarketOrder, MarketOrderLargerThanBook_NoResting) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::SELL));
    // Market buy of 50, only 10 available — no resting market order left
    f.process(f.market(2, 50, Side::BUY));

    // What was filled
    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].qty, 10u);

    // Ask is gone, and since MARKET type, remainder is NOT rested
    EXPECT_FALSE(f.book->isAskActive(100));
    EXPECT_FALSE(f.book->isBidActive(MAX_PRICE));
}

// ============================================================================
// 6. EDGE CASES & REGRESSION GUARDS
// ============================================================================

TEST(EdgeCase, BidAskSpread_NoSpuriousMatch) {
    Fixture f;
    // Place 1000 bids and 1000 asks with a gap in the middle
    for (uint32_t p = 90; p < 100; ++p)
        f.process(f.limit(p, p, 10, Side::BUY));
    for (uint32_t p = 110; p < 120; ++p)
        f.process(f.limit(p, p, 10, Side::SELL));

    EXPECT_EQ(f.book->getExecutedTrades().size(), 0u);
}

TEST(EdgeCase, LargeVolumeNoLeakedOrders) {
    Fixture f(200'000);
    // Alternate 1000 buys and 1000 sells at the same price
    for (uint64_t i = 1; i <= 1000; ++i)
        f.process(f.limit(i,       100, 1, Side::BUY));
    for (uint64_t i = 1001; i <= 2000; ++i)
        f.process(f.limit(i, 100, 1, Side::SELL));

    const auto& trades = f.book->getExecutedTrades();
    uint32_t totalFilled = 0;
    for (auto& t : trades) totalFilled += t.qty;
    EXPECT_EQ(totalFilled, 1000u); // all 1000 buys matched against 1000 sells
    EXPECT_FALSE(f.book->isBidActive(100));
    EXPECT_FALSE(f.book->isAskActive(100));
}

TEST(EdgeCase, Qty1FillsCleanly) {
    Fixture f;
    f.process(f.limit(1, 100, 1, Side::SELL));
    f.process(f.limit(2, 100, 1, Side::BUY));

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].qty, 1u);
    EXPECT_FALSE(f.book->isAskActive(100));
    EXPECT_FALSE(f.book->isBidActive(100));
}

TEST(EdgeCase, CancelThenReinsertSameId) {
    Fixture f;
    f.process(f.limit(1, 100, 10, Side::BUY));
    f.book->cancelOrder(1);
    // Re-insert with same orderId
    f.process(f.limit(1, 101, 20, Side::BUY));
    EXPECT_TRUE(f.book->isBidActive(101));
    EXPECT_FALSE(f.book->isBidActive(100));
}

TEST(EdgeCase, InterleavedBuysAndSells) {
    Fixture f;
    // Simulate realistic interleaved order flow
    f.process(f.limit(1,  100, 100, Side::BUY));
    f.process(f.limit(2,  102, 50,  Side::SELL)); // No match
    f.process(f.limit(3,  101, 30,  Side::SELL)); // No match
    f.process(f.limit(4,  100, 20,  Side::SELL)); // Matches bid@100 for 20
    f.book->cancelOrder(2);                         // Cancel order 2
    f.process(f.limit(5,  101, 80,  Side::SELL)); // No match (best bid=100, ask=101)

    ASSERT_EQ(f.book->getExecutedTrades().size(), 1u);
    EXPECT_EQ(f.book->getExecutedTrades()[0].qty, 20u);
    EXPECT_FALSE(f.book->isAskActive(102)); // Cancelled
    EXPECT_TRUE(f.book->isBidActive(100));  // Partially filled, 80 remaining
}

TEST(EdgeCase, MaxPriceOrderAccepted) {
    Fixture f;
    EXPECT_NO_THROW(f.process(f.limit(1, MAX_PRICE, 1, Side::BUY)));
    EXPECT_TRUE(f.book->isBidActive(MAX_PRICE));
}

TEST(EdgeCase, OrderLookupGrowsDynamically) {
    Fixture f;
    // Use very high order IDs to force orderLookup vector resize
    uint64_t highId = 1'000'000;
    EXPECT_NO_THROW(f.process(f.limit(highId, 100, 10, Side::BUY)));
    EXPECT_NO_THROW(f.book->cancelOrder(highId));
    EXPECT_FALSE(f.book->isBidActive(100));
}
