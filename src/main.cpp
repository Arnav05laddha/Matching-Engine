#include <thread>
#include <iostream>
#include <atomic>
#include <chrono>
#include "concurrency/SPSCQueue.h"
#include "concurrency/CpuAffinity.h"
#include "core/MatchingEngine.h"

using namespace hyperengine;
using namespace hyperengine::models;
using namespace hyperengine::concurrency;
using namespace hyperengine::core;

int main()
{
    std::cout << "HyperEngine Day 5: Multi-threaded Lock-Free Engine\n";

    auto ingressQueue = std::make_unique<SPSCQueue<OrderRequest, 65536>>();
    auto egressQueue = std::make_unique<SPSCQueue<Trade, 65536>>();

    auto engine = std::make_unique<MatchingEngine>(*ingressQueue, *egressQueue);

    std::thread matchingThread([&]()
                               {
        pinThreadToCore(1); // Pin to Core 1
        std::cout << "Matching Engine started on Core 1\n";
        engine->start(); });

    std::atomic<uint32_t> tradeCount{0};
    std::atomic<bool> loggerRunning{true};

    std::thread loggerThread([&]()
                             {
        pinThreadToCore(2); // Pin to Core 2
        Trade t;
        while (loggerRunning.load(std::memory_order_relaxed)) {
            while (egressQueue->pop(t)) {
                tradeCount.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
        // Drain any remaining after flag is flipped
        while (egressQueue->pop(t)) {
            tradeCount.fetch_add(1, std::memory_order_relaxed);
        } });

    pinThreadToCore(3); // Pin main thread to Core 3
    std::cout << "Ingress generator started on Core 3\n";

    // 1 million orders (500k buys, 500k sells) to generate 500k trades
    const uint32_t NUM_ORDERS = 1000000;

    for (uint32_t i = 1; i <= NUM_ORDERS; ++i)
    {
        Side side = (i % 2 == 0) ? Side::BUY : Side::SELL;
        OrderRequest req{static_cast<uint64_t>(i), 100, 10, side, OrderType::LIMIT, 0};

        while (!ingressQueue->push(req))
        {
            std::this_thread::yield();
        }
    }

    std::cout << "Finished injecting " << NUM_ORDERS << " orders.\n";

    // Signal engine to stop with an explicit SHUTDOWN sentinel
    OrderRequest stopReq{0, 0, 0, Side::BUY, OrderType::SHUTDOWN, 0};
    while (!ingressQueue->push(stopReq))
    {
        std::this_thread::yield();
    }

    matchingThread.join();

    // Give logger thread a moment to flush
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loggerRunning = false;
    loggerThread.join();

    std::cout << "Total trades recorded: " << tradeCount.load() << "\n";
    if (tradeCount.load() == NUM_ORDERS / 2)
    {
        std::cout << "Lock-free trading system successfully built and verified!\n";
    }
    else
    {
        std::cerr << "Error: Trade count mismatch.\n";
    }

    return 0;
}

/*
  
*/