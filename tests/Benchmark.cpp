#include <memory>
#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include <emmintrin.h> // For _mm_lfence()

#include "../src/core/OrderBook.h"

using namespace hyperengine;
using namespace hyperengine::models;
using namespace hyperengine::core;
using namespace hyperengine::memory;

std::vector<OrderRequest> loadHistoricalData(const std::string& filepath) {
    std::vector<OrderRequest> orders;
    orders.reserve(1000000);
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open CSV: " << filepath << "\n";
        return orders;
    }
    std::string line;
    std::getline(file, line); // header
    
    // Very basic fast CSV parser for predefined format
    while (std::getline(file, line)) {
        if(line.empty()) continue;
        unsigned long long orderId;
        unsigned int price, qty;
        int side, type;
        if (sscanf(line.c_str(), "%llu,%u,%u,%d,%d", &orderId, &price, &qty, &side, &type) == 5) {
            orders.push_back(OrderRequest{
                static_cast<uint64_t>(orderId), 
                static_cast<uint32_t>(price), 
                static_cast<uint32_t>(qty), 
                static_cast<Side>(side), 
                static_cast<OrderType>(type), 
                0
            });
        }
    }
    return orders;
}

void measureThroughput(const std::vector<OrderRequest>& orders) {
    std::cout << "\n--- THROUGHPUT BENCHMARK (REAL MBO DATA) ---\n";
    OrderPool pool{15000000};
    auto book = std::make_unique<OrderBook>(pool);

    // Warmup 10k orders if possible
    size_t warmup = std::min(orders.size(), size_t(10000));
    for (size_t i = 0; i < warmup; i++) {
        OrderRequest req = orders[i];
        book->processOrder(req);
    }
    book->clearTrades(); // Discard warmup trades

    auto start_time = std::chrono::high_resolution_clock::now();
    for (size_t i = warmup; i < orders.size(); ++i) {
        OrderRequest req = orders[i];
        book->processOrder(req);
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    
    std::chrono::duration<double> diff = end_time - start_time;
    double throughput = (orders.size() - warmup) / diff.count();
    
    std::cout << "Throughput:   " << throughput << " orders/second\n";
    std::cout << "Total Time:   " << diff.count() << " seconds\n";
    std::cout << "Trades fired: " << book->getExecutedTrades().size() << "\n";
}

void measureLatency(const std::vector<OrderRequest>& orders) {
    std::cout << "\n--- LATENCY BENCHMARK (REAL MBO DATA) ---\n";
    OrderPool pool{15000000};
    auto book = std::make_unique<OrderBook>(pool);

    std::vector<uint64_t> latencies(orders.size());

    size_t warmup = std::min(orders.size(), size_t(10000));
    for (size_t i = 0; i < warmup; i++) {
        OrderRequest req = orders[i];
        book->processOrder(req);
    }
    book->clearTrades();

    for (size_t i = warmup; i < orders.size(); ++i) {
        OrderRequest req = orders[i];
        _mm_lfence();
        uint64_t start = __rdtsc();
        _mm_lfence();
        
        book->processOrder(req);
        
        _mm_lfence();
        uint64_t end = __rdtsc();
        latencies[i] = end - start;
    }

    std::sort(latencies.begin() + warmup, latencies.end());
    size_t count = latencies.size() - warmup;
    
    if (count > 0) {
        std::cout << "p50 (Median): " << latencies[warmup + count * 0.50] << " cycles\n";
        std::cout << "p90:          " << latencies[warmup + count * 0.90] << " cycles\n";
        std::cout << "p99:          " << latencies[warmup + count * 0.99] << " cycles\n";
        std::cout << "p99.9:        " << latencies[warmup + count * 0.999] << " cycles\n";
        std::cout << "p99.99:       " << latencies[warmup + count * 0.9999] << " cycles\n";
    }
}

int main() {
    std::cout << "HyperEngine Level 3 Market-By-Order Benchmark\n";
    std::cout << "Loading real tick data...\n";
    
    auto orders = loadHistoricalData("../data/mbo_data.csv");
    if (orders.empty()) {
        std::cerr << "No orders loaded. Did you run generate_mbo.py?\n";
        return 1;
    }
    std::cout << "Loaded " << orders.size() << " historical orders.\n";
    
    measureThroughput(orders);
    measureLatency(orders);
    
    return 0;
}
