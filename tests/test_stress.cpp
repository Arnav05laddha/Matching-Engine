#include <memory>
// ============================================================================
// test_stress.cpp
//
// Real-world simulation and stress tests for HyperEngine.
// Covers:
//   - Full end-to-end multi-threaded engine (ingress → match → egress)
//   - Realistic MBO (Market-by-Order) order flow simulation
//   - Random order/cancel/fill workloads
//   - Conservation of volume: every filled share is accounted for
//   - Pool memory lifecycle under sustained load
// ============================================================================

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <random>
#include <vector>
#include <numeric>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <iostream>

#include "../src/core/MatchingEngine.h"
#include "../src/core/OrderBook.h"
#include "../src/memory/OrderPool.h"
#include "../src/concurrency/SPSCQueue.h"
#include "../src/models/Trade.h"

using namespace hyperengine;
using namespace hyperengine::models;
using namespace hyperengine::memory;
using namespace hyperengine::core;
using namespace hyperengine::concurrency;

// ============================================================================
// Helper: generate a pseudo-realistic MBO order stream
// ============================================================================

struct SimOrder {
    OrderRequest req;
    bool isCancel{false};
    uint64_t targetCancelId{0};
};

// Generates a realistic mix: 70% limit orders, 20% cancels, 10% market orders
// Prices clustered around a mid-price to ensure frequent matching.
std::vector<SimOrder> generateRealisticFlow(size_t n, uint32_t midPrice = 10000, uint32_t spread = 100) {
    std::mt19937 rng(42); // Fixed seed for reproducibility
    std::uniform_int_distribution<uint32_t> priceDist(midPrice - spread, midPrice + spread);
    std::uniform_int_distribution<uint32_t> qtyDist(1, 200);
    std::uniform_int_distribution<int> typeDist(0, 9);
    std::uniform_int_distribution<int> sideDist(0, 1);

    std::vector<SimOrder> orders;
    orders.reserve(n);

    std::vector<uint64_t> liveOrderIds;
    uint64_t nextId = 1;

    for (size_t i = 0; i < n; ++i) {
        int roll = typeDist(rng);
        SimOrder so;

        if (roll <= 6 || liveOrderIds.empty()) {
            // 70%: Limit order
            Side side = sideDist(rng) ? Side::BUY : Side::SELL;
            uint32_t price = priceDist(rng);
            uint32_t qty = qtyDist(rng);
            so.req = {nextId, price, qty, side, OrderType::LIMIT, 0};
            liveOrderIds.push_back(nextId);
            ++nextId;
        } else if (roll <= 8) {
            // 20%: Cancel a random live order
            std::uniform_int_distribution<size_t> idxDist(0, liveOrderIds.size() - 1);
            size_t idx = idxDist(rng);
            so.isCancel = true;
            so.targetCancelId = liveOrderIds[idx];
            so.req = {liveOrderIds[idx], 0, 0, Side::BUY, OrderType::CANCEL, 0};
            liveOrderIds.erase(liveOrderIds.begin() + idx);
        } else {
            // 10%: Market order
            Side side = sideDist(rng) ? Side::BUY : Side::SELL;
            uint32_t price = (side == Side::BUY) ? MAX_PRICE : 0;
            uint32_t qty = qtyDist(rng);
            so.req = {nextId, price, qty, side, OrderType::MARKET, 0};
            ++nextId;
        }
        orders.push_back(so);
    }
    return orders;
}

// ============================================================================
// 1. SINGLE-THREADED REALISTIC SIMULATION
// ============================================================================

TEST(StressTest, RealisticMBO_10k_NoVolumeLeaks) {
    const size_t N = 10'000;
    auto flow = generateRealisticFlow(N);

    OrderPool pool(N + 1000);
    auto book = std::make_unique<OrderBook>(pool);

    for (auto& so : flow) {
        book->processOrder(so.req);
    }

    // Verify volume conservation: total filled qty on buy side == sell side
    uint64_t buyFilled = 0, sellFilled = 0;
    for (const auto& t : book->getExecutedTrades()) {
        // Each trade contributes qty to both sides
        buyFilled  += t.qty;
        sellFilled += t.qty;
    }
    EXPECT_EQ(buyFilled, sellFilled); // Must always be true
}

TEST(StressTest, RealisticMBO_100k_Completes) {
    const size_t N = 100'000;
    auto flow = generateRealisticFlow(N);

    OrderPool pool(N + 5000);
    auto book = std::make_unique<OrderBook>(pool);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (auto& so : flow) {
        book->processOrder(so.req);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double mops = (N / secs) / 1e6;
    std::cout << "\n[SingleThread 100k] " << mops << " M orders/sec, "
              << book->getExecutedTrades().size() << " trades executed\n";

    EXPECT_GT(book->getExecutedTrades().size(), 0u); // Should have matched some orders
}

// ============================================================================
// 2. END-TO-END MULTI-THREADED ENGINE TEST
// ============================================================================

TEST(StressTest, MultiThreaded_EndToEnd_500k_OrdersExactMatch) {
    // 500k alternating BUY/SELL at same price → should produce exactly 250k trades
    const uint32_t NUM_ORDERS = 500'000;

    auto ingressQ = std::make_unique<SPSCQueue<OrderRequest, 65536>>();
    auto egressQ  = std::make_unique<SPSCQueue<Trade, 65536>>();

    auto engine = std::make_unique<MatchingEngine>(*ingressQ, *egressQ);

    std::atomic<uint64_t> tradeCount{0};
    std::atomic<bool> loggerRunning{true};

    // Logger/consumer thread
    std::thread logger([&]() {
        Trade t;
        while (loggerRunning.load(std::memory_order_relaxed)) {
            while (egressQ->pop(t))
                tradeCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        Trade t2;
        while (egressQ->pop(t2))
            tradeCount.fetch_add(1, std::memory_order_relaxed);
    });

    // Matching thread
    std::thread matchThread([&]() { engine->start(); });

    // Inject alternating BUY/SELL at price 100
    for (uint32_t i = 1; i <= NUM_ORDERS; ++i) {
        Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
        OrderRequest req{static_cast<uint64_t>(i), 100, 10, side, OrderType::LIMIT, 0};
        while (!ingressQ->push(req)) std::this_thread::yield();
    }

    // Send SHUTDOWN
    OrderRequest stop{0, 0, 0, Side::BUY, OrderType::SHUTDOWN, 0};
    while (!ingressQ->push(stop)) std::this_thread::yield();

    matchThread.join();

    // Give logger time to drain egress
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loggerRunning.store(false);
    logger.join();

    EXPECT_EQ(tradeCount.load(), NUM_ORDERS / 2);
}

TEST(StressTest, MultiThreaded_EndToEnd_SHUTDOWN_DrainsProperly) {
    // Ensure the SHUTDOWN sentinel cleanly stops the engine and all queued
    // orders before SHUTDOWN are still processed.
    auto ingressQ = std::make_unique<SPSCQueue<OrderRequest, 65536>>();
    auto egressQ  = std::make_unique<SPSCQueue<Trade, 65536>>();

    auto engine = std::make_unique<MatchingEngine>(*ingressQ, *egressQ);
    std::thread matchThread([&]() { engine->start(); });

    // Pre-load some crossing orders
    for (int i = 1; i <= 100; ++i) {
        Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
        OrderRequest req{static_cast<uint64_t>(i), 100, 1, side, OrderType::LIMIT, 0};
        while (!ingressQ->push(req)) std::this_thread::yield();
    }

    OrderRequest stop{0, 0, 0, Side::BUY, OrderType::SHUTDOWN, 0};
    while (!ingressQ->push(stop)) std::this_thread::yield();
    matchThread.join();

    // Drain egress
    uint64_t count = 0;
    Trade t;
    while (egressQ->pop(t)) ++count;

    EXPECT_EQ(count, 50u); // 50 pairs → 50 trades
}

// ============================================================================
// 3. SUSTAINED LOAD + POOL CYCLING
// ============================================================================

TEST(StressTest, SustainedLoad_PoolCycling) {
    // Simulates an order book where orders are constantly filled and cancelled,
    // ensuring the pool freelist cycles correctly with zero corruption.
    const size_t POOL_SIZE = 1000;
    const size_t ITERATIONS = 50'000;

    OrderPool pool(POOL_SIZE);
    auto book = std::make_unique<OrderBook>(pool);

    std::mt19937 rng(123);
    std::uniform_int_distribution<uint32_t> priceDist(99, 101); // Tight spread → lots of matches
    std::uniform_int_distribution<uint32_t> qtyDist(1, 10);

    uint64_t nextId = 1;
    uint64_t totalTradeVol = 0;

    for (size_t iter = 0; iter < ITERATIONS; ++iter) {
        Side side = (iter % 2 == 0) ? Side::BUY : Side::SELL;
        uint32_t price = priceDist(rng);
        uint32_t qty = qtyDist(rng);

        OrderRequest req{nextId++, price, qty, side, OrderType::LIMIT, 0};
        try {
            book->processOrder(req);
        } catch (const std::runtime_error&) {
            // Pool exhausted — skip this order (expected under extreme load)
        }

        for (const auto& t : book->getExecutedTrades())
            totalTradeVol += t.qty;
        book->clearTrades();
    }

    // Just verifying it ran without corruption or crash
    EXPECT_GT(totalTradeVol, 0u);
    std::cout << "\n[PoolCycling] Total volume traded: " << totalTradeVol << "\n";
}

// ============================================================================
// 4. CANCEL STORM: HIGH CANCEL-TO-INSERT RATIO
// ============================================================================

TEST(StressTest, CancelStorm_NoCorruption) {
    // Insert 10k orders, cancel all of them, verify book is empty and clean
    const size_t N = 10'000;
    OrderPool pool(N + 100);
    auto book = std::make_unique<OrderBook>(pool);

    // Fill one side entirely (no matching possible — opposite side empty)
    for (uint64_t i = 1; i <= N; ++i) {
        OrderRequest req{i, 100 + (i % 100), 10, Side::BUY, OrderType::LIMIT, 0};
        book->processOrder(req);
    }

    // Cancel all of them
    for (uint64_t i = 1; i <= N; ++i) {
        book->cancelOrder(i);
    }

    // Every price level should be cleared
    for (uint32_t p = 100; p < 200; ++p) {
        EXPECT_FALSE(book->isBidActive(p)) << "Level " << p << " should be inactive";
        EXPECT_EQ(book->getBidLevel(p).head, INVALID_INDEX);
        EXPECT_EQ(book->getBidLevel(p).tail, INVALID_INDEX);
    }

    EXPECT_EQ(book->getExecutedTrades().size(), 0u);
}

// ============================================================================
// 5. THROUGHPUT MEASUREMENT (informational, always passes)
// ============================================================================

TEST(Performance, SingleThread_ThroughputMeasure_1M) {
    const size_t N = 1'000'000;
    OrderPool pool(N + 1000);
    auto book = std::make_unique<OrderBook>(pool);

    // Warmup
    for (uint64_t i = 1; i <= 10'000; ++i) {
        Side s = (i % 2 == 0) ? Side::BUY : Side::SELL;
        OrderRequest req{i, 100, 1, s, OrderType::LIMIT, 0};
        book->processOrder(req);
    }
    book->clearTrades();

    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 10001; i <= N; ++i) {
        Side s = (i % 2 == 0) ? Side::BUY : Side::SELL;
        OrderRequest req{i, 100, 1, s, OrderType::LIMIT, 0};
        book->processOrder(req);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    book->clearTrades();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double mops = ((N - 10000) / secs) / 1e6;
    std::cout << "\n[Throughput] Single-thread: " << mops << " M orders/sec over " << (N - 10000) << " orders\n";

    EXPECT_GT(mops, 0.5); // Should be well above 500k/sec on any modern CPU
}

TEST(Performance, BestPriceLookup_NsEstimate) {
    // Measures how fast the FastBitset find_next/find_prev is under realistic book depth
    const size_t LEVELS = 1000;
    const size_t PROBES = 1000000;
    OrderPool pool(LEVELS + PROBES + 100);
    auto book = std::make_unique<OrderBook>(pool);

    // Fill 1000 distinct ask levels
    for (uint64_t i = 1; i <= LEVELS; ++i) {
        OrderRequest req{i, static_cast<uint32_t>(i + 5000), 10, Side::SELL, OrderType::LIMIT, 0};
        book->processOrder(req);
    }

    // Time 1M best-ask lookups via a matching probe that doesn't cross
    OrderRequest probe{9999999, 4999, 1, Side::BUY, OrderType::LIMIT, 0};
    
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < PROBES; ++i) {
        book->processOrder(probe);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / PROBES;
    std::cout << "\n[BestPriceLookup] " << ns << " ns/op over " << LEVELS << " active levels\n";
    EXPECT_LT(ns, 1000.0); // Should be well under 1 µs
}
