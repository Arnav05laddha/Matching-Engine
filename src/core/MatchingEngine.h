#pragma once
#include "../concurrency/SPSCQueue.h"
#include "../models/OrderRequest.h"
#include "../models/Trade.h"
#include "OrderBook.h"
#include <atomic>
#include <thread>
#include <immintrin.h>
namespace hyperengine
{
    namespace core
    {

        // Configuration for the matching engine.
        // Centralises all magic numbers to a single, self-documenting place.
        struct EngineConfig
        {
            size_t poolCapacity{10'000'000}; // Maximum concurrent live orders
        };

        class MatchingEngine
        {
        public:
            MatchingEngine(concurrency::SPSCQueue<models::OrderRequest, 65536> &ingress,
                           concurrency::SPSCQueue<models::Trade, 65536> &egress,
                           EngineConfig cfg = {})
                : ingressQueue(ingress), egressQueue(egress), pool(cfg.poolCapacity), book(pool) {}

            void start()
            {
                running = true;
                models::OrderRequest req;

                while (running.load(std::memory_order_relaxed))
                {
                    if (ingressQueue.pop(req))
                    {
                        // Explicit SHUTDOWN order type — no fragile magic sentinel
                        if (req.type == models::OrderType::SHUTDOWN)
                        {
                            break;
                        }
                        book.processOrder(req, &egressQueue);
                    }
                    else
                    {
                        _mm_pause();
                    }
                }
            }

            void stop()
            {
                running = false;
            }

        private:
            concurrency::SPSCQueue<models::OrderRequest, 65536> &ingressQueue;
            concurrency::SPSCQueue<models::Trade, 65536> &egressQueue;

            memory::OrderPool pool;
            OrderBook book;

            std::atomic<bool> running{false};
        };

    } // namespace core
} // namespace hyperengine
