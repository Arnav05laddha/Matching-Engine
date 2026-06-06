#include <memory>
// ============================================================================
// test_spsc_queue.cpp
//
// Tests for the lock-free SPSCQueue used as the inter-thread transport.
// Covers: single-threaded correctness, full/empty boundary conditions,
// and a multi-threaded producer/consumer stress test measuring zero message loss.
// ============================================================================

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <numeric>
#include <vector>
#include <chrono>

#include "../src/concurrency/SPSCQueue.h"
#include "../src/models/Trade.h"

using namespace hyperengine::concurrency;
using namespace hyperengine::models;

// ============================================================================
// 1. SINGLE-THREAD CORRECTNESS
// ============================================================================

TEST(SPSCQueue, PushPopSingleElement) {
    SPSCQueue<int, 64> q;
    EXPECT_TRUE(q.push(42));
    int val = 0;
    EXPECT_TRUE(q.pop(val));
    EXPECT_EQ(val, 42);
}

TEST(SPSCQueue, PopOnEmptyReturnsFalse) {
    SPSCQueue<int, 64> q;
    int val = 0;
    EXPECT_FALSE(q.pop(val));
}

TEST(SPSCQueue, PushOnFullReturnsFalse) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.push(1));
    EXPECT_TRUE(q.push(2));
    EXPECT_TRUE(q.push(3));
    EXPECT_TRUE(q.push(4));
    EXPECT_FALSE(q.push(5)); // Full
}

TEST(SPSCQueue, FIFOOrdering) {
    SPSCQueue<int, 64> q;
    for (int i = 0; i < 10; ++i) q.push(i);

    for (int i = 0; i < 10; ++i) {
        int val = -1;
        ASSERT_TRUE(q.pop(val));
        EXPECT_EQ(val, i);
    }
}

TEST(SPSCQueue, WrapAroundCorrect) {
    SPSCQueue<int, 4> q;
    // Fill, drain, fill again — exercises the ring-buffer wrap-around
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 4; ++i) ASSERT_TRUE(q.push(i * 10 + round));
        for (int i = 0; i < 4; ++i) {
            int val = -1;
            ASSERT_TRUE(q.pop(val));
            EXPECT_EQ(val, i * 10 + round);
        }
    }
}

TEST(SPSCQueue, TradeStructTransfer) {
    SPSCQueue<Trade, 64> q;
    Trade t{1, 2, 100, 50, 12345};
    ASSERT_TRUE(q.push(t));
    Trade out{};
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out.makerOrderId, 1u);
    EXPECT_EQ(out.takerOrderId, 2u);
    EXPECT_EQ(out.price, 100u);
    EXPECT_EQ(out.qty, 50u);
    EXPECT_EQ(out.timestamp, 12345u);
}

// ============================================================================
// 2. MULTI-THREADED STRESS: ZERO MESSAGE LOSS
// ============================================================================

TEST(SPSCQueue, MultiThreadedZeroLoss_1M) {
    static constexpr size_t N = 1'000'000;
    SPSCQueue<uint64_t, 65536> q;

    std::atomic<uint64_t> sum_sent{0};
    std::atomic<uint64_t> sum_recv{0};
    std::atomic<bool> done{false};

    // Producer thread: pushes 0..N-1
    std::thread producer([&]() {
        for (uint64_t i = 0; i < N; ++i) {
            while (!q.push(i)) std::this_thread::yield();
            sum_sent.fetch_add(i, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer thread: pops until done + drained
    std::thread consumer([&]() {
        uint64_t count = 0;
        uint64_t val;
        while (count < N) {
            if (q.pop(val)) {
                sum_recv.fetch_add(val, std::memory_order_relaxed);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    // If every message was received exactly once, the sums must match
    EXPECT_EQ(sum_sent.load(), sum_recv.load());
}

TEST(SPSCQueue, MultiThreadedZeroLoss_HighContention) {
    // Small queue forces tight producer/consumer contention
    static constexpr size_t N = 500'000;
    SPSCQueue<uint32_t, 16> q; // Very small — lots of back-pressure

    uint64_t sum_recv = 0;

    std::thread producer([&]() {
        for (uint32_t i = 1; i <= N; ++i) {
            while (!q.push(i)) std::this_thread::yield();
        }
    });

    for (uint64_t count = 0; count < N; ) {
        uint32_t val;
        if (q.pop(val)) {
            sum_recv += val;
            ++count;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();

    uint64_t expected = (uint64_t)N * (N + 1) / 2;
    EXPECT_EQ(sum_recv, expected);
}

// ============================================================================
// 3. THROUGHPUT MEASUREMENT (informational, always passes)
// ============================================================================

TEST(SPSCQueue, ThroughputBaseline_1M) {
    static constexpr size_t N = 1'000'000;
    SPSCQueue<uint64_t, 65536> q;

    std::atomic<bool> start{false};
    std::atomic<uint64_t> received{0};

    std::thread consumer([&]() {
        while (!start.load(std::memory_order_acquire)) {}
        uint64_t val, cnt = 0;
        while (cnt < N) {
            if (q.pop(val)) ++cnt;
            else std::this_thread::yield();
        }
        received.store(cnt, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    auto t0 = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < N; ++i)
        while (!q.push(i)) std::this_thread::yield();

    consumer.join();
    auto t1 = std::chrono::high_resolution_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    double mops = (N / secs) / 1e6;
    std::cout << "\n[SPSC Throughput] " << mops << " M messages/sec over " << N << " messages\n";

    EXPECT_EQ(received.load(), (uint64_t)N);
}
