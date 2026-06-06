# HyperEngine — Ultra-Low-Latency C++ Order Matching Engine

A from-scratch, production-inspired **limit order book and matching engine** written in modern C++20. Designed around the constraints of real high-frequency trading (HFT) systems: zero heap allocations on the hot path, cache-line-aware data structures, lock-free inter-thread communication, and CPU core pinning.

---

## Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
  - [System Diagram](#system-diagram)
  - [Data Flow](#data-flow)
- [Component Deep-Dive](#component-deep-dive)
  - [Models Layer](#models-layer)
  - [Memory Layer — OrderPool](#memory-layer--orderpool)
  - [Core Layer — OrderBook](#core-layer--orderbook)
  - [Core Layer — FastBitset](#core-layer--fastbitset)
  - [Concurrency Layer — SPSCQueue](#concurrency-layer--spscqueue)
  - [Concurrency Layer — CpuAffinity](#concurrency-layer--cpuaffinity)
  - [Core Layer — MatchingEngine](#core-layer--matchingengine)
- [Performance Design Decisions](#performance-design-decisions)
- [Building](#building)
- [Running](#running)
- [Benchmarks](#benchmarks)
- [Testing](#testing)
- [Project Structure](#project-structure)

---

## Overview

HyperEngine is a **multi-threaded, lock-free financial matching engine** that implements the canonical **Price-Time Priority (FIFO)** matching algorithm used in virtually every major exchange worldwide (NYSE, NASDAQ, CME, etc.).

The engine processes orders across three isolated CPU cores simultaneously:

| Thread | Core | Role |
|---|---|---|
| **Main / Ingress** | Core 3 | Generates and pushes `OrderRequest` messages into the ingress SPSC queue |
| **Matching Engine** | Core 1 | Pops requests, runs matching logic, pushes `Trade` events to the egress queue |
| **Logger / Egress** | Core 2 | Drains the egress queue and counts executed trades |

The two queues between threads are **lock-free Single-Producer Single-Consumer (SPSC) ring buffers** — no mutexes, no system calls, no OS scheduler involvement on the hot path.

---

## Features

- **Price-Time Priority matching** — bids filled against the lowest available ask; asks filled against the highest available bid; FIFO order within the same price level
- **Zero heap allocations on the hot path** — all order memory is pre-allocated in a slab arena at startup via `OrderPool`
- **Lock-free SPSC queues** — inter-thread communication with only `std::atomic` acquire/release fences; no locks
- **O(1) order cancellation** — flat `orderLookup` vector maps order ID → pool index for instant cancellation
- **Two-level `FastBitset`** — O(log₆₄ N) best-price discovery using a hierarchical 64-bit bitset with a summary layer (effectively O(1) for any practical price range)
- **Cache-line aligned data structures** — `Order` and `Trade` are exactly 32 bytes and `alignas(32)`, packing two per 64-byte L1 cache line; `SPSCQueue` head/tail on separate cache lines to prevent false sharing
- **Intrusive doubly-linked list** — `next`/`prev` pointers live inside each `Order` struct rather than in a separate node, reducing pointer chasing
- **CPU core pinning** — each thread is pinned to a dedicated logical core using `SetThreadAffinityMask` (Windows) or `pthread_setaffinity_np` (Linux)
- **Explicit shutdown sentinel** — engine is stopped via an `OrderType::SHUTDOWN` message instead of fragile magic-number sentinels
- **Market and Limit orders** — limit orders rest in the book if unmatched; market orders are discarded after matching
- **Comprehensive test suite** — 40+ Google Test cases covering struct layout, FIFO ordering, partial fills, multi-level sweeps, cancel edge cases, and stress tests
- **Real-data benchmark** — throughput and per-order latency (p50/p90/p99/p99.9/p99.99 in CPU cycles) measured against 1M synthetic Market-By-Order ticks

---

## Architecture

### System Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          HyperEngine Runtime                            │
│                                                                         │
│  Core 3 (Main Thread)         Core 1 (Matching Thread)   Core 2 (Logger)│
│                                                                         │
│  ┌──────────────────┐        ┌──────────────────────┐  ┌─────────────┐ │
│  │  Ingress         │        │   MatchingEngine      │  │  Logger     │ │
│  │  Generator       │──────▶ │                       │─▶│  Thread     │ │
│  │                  │ SPSC   │  ┌────────────────┐  │  │             │ │
│  │  1M OrderRequest │ Queue  │  │   OrderBook    │  │  │  tradeCount │ │
│  │  messages        │ 65536  │  │                │  │  │  (atomic)   │ │
│  └──────────────────┘ slots  │  │  ┌──────────┐ │  │  └─────────────┘ │
│                              │  │  │ OrderPool│ │  │                   │
│                              │  │  │  (Arena) │ │  │                   │
│                              │  │  └──────────┘ │  │  SPSC Queue       │
│                              │  │               │  │  65536 slots      │
│                              │  │  ┌──────────┐ │  │                   │
│                              │  │  │FastBitset│ │  │                   │
│                              │  │  │ bids/asks│ │  │                   │
│                              │  │  └──────────┘ │  │                   │
│                              │  └────────────────┘  │                   │
│                              └──────────────────────┘                   │
└─────────────────────────────────────────────────────────────────────────┘
```

### Data Flow

```
OrderRequest{orderId, price, qty, side, type}
        │
        ▼
   SPSCQueue<OrderRequest, 65536>   (ingress — lock-free ring buffer)
        │
        ▼
  MatchingEngine::start()  [Core 1 spin-loop]
        │
        ├── CANCEL  ──▶  OrderBook::cancelOrder(orderId)
        │                    └─▶ orderLookup[id] → pool index
        │                    └─▶ unlink from doubly-linked list  O(1)
        │                    └─▶ pool.free(index)               O(1)
        │
        └── LIMIT / MARKET ──▶  OrderBook::processOrder(req)
                                    │
                                    ├─▶ matchSide<BUY|SELL>(req)
                                    │       └─▶ FastBitset::find_next/find_prev  ← O(log₆₄ N)
                                    │       └─▶ Walk linked list at price level
                                    │       └─▶ Emit Trade to egress SPSCQueue
                                    │
                                    └─▶ insertOrder(req)   [if qty remains and LIMIT]
                                            └─▶ pool.allocate()          O(1)
                                            └─▶ insertIntoLevel()        O(1)
                                            └─▶ activeBids/Asks.set()    O(1)
                                            └─▶ orderLookup update       O(1)

        ▼
   SPSCQueue<Trade, 65536>  (egress — lock-free ring buffer)
        │
        ▼
  Logger Thread [Core 2] — drains and counts trades
```

---

## Component Deep-Dive

### Models Layer

**`src/models/Constants.h`**

Defines the two enumerations and two global constants that propagate through the entire engine:

```cpp
enum class Side     : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType: uint8_t { LIMIT = 0, MARKET = 1, CANCEL = 2, SHUTDOWN = 3 };

static constexpr uint32_t INVALID_INDEX = std::numeric_limits<uint32_t>::max();
static constexpr uint32_t MAX_PRICE     = 2'000'000;
```

`INVALID_INDEX` (`0xFFFFFFFF`) acts as a typed null pointer for pool indices and linked-list terminators throughout the codebase. `MAX_PRICE` bounds the static price-level arrays in the `OrderBook`.

---

**`src/models/Order.h`** — the fundamental heap atom

```cpp
struct alignas(32) Order {
    uint64_t orderId;       // 8 bytes
    uint32_t price;         // 4 bytes
    uint32_t initialQty;    // 4 bytes
    uint32_t remainingQty;  // 4 bytes
    uint32_t next;          // 4 bytes — intrusive linked-list pointer (also freelist next)
    uint32_t prev;          // 4 bytes — intrusive prev pointer for O(1) cancel
    Side     side;          // 1 byte
    uint8_t  active;        // 1 byte  — 1 = live in book; 0 = on freelist
    uint16_t reserved;      // 2 bytes — padding
};                          // total: 32 bytes
static_assert(sizeof(Order) == 32);
```

`alignas(32)` forces the OS/allocator to place each `Order` at a 32-byte boundary. Two `Order` objects fill exactly one 64-byte CPU L1 cache line. Without this, a single order could straddle two cache lines, doubling memory-fetch latency on every access.

The `next` field serves **dual purpose**: when the order is live in the book it is the forward pointer in the price-level's doubly-linked list; when the order is freed it becomes the `next` pointer in the pool's intrusive freelist — zero additional memory cost.

---

**`src/models/OrderRequest.h`** — the wire format entering the engine

```cpp
struct OrderRequest {
    uint64_t orderId;
    uint32_t price;
    uint32_t qty;
    Side     side;
    OrderType type;
    uint16_t reserved;  // padding to 24 bytes
};
static_assert(sizeof(OrderRequest) == 24);
```

Kept deliberately small (24 bytes) so it fits in a single L1 cache line slot in the SPSC queue buffer with room to spare.

---

**`src/models/Trade.h`** — the event emitted after every fill

```cpp
struct alignas(32) Trade {
    uint64_t makerOrderId;  // The incoming (aggressive) order
    uint64_t takerOrderId;  // The resting order
    uint32_t price;         // Fill price — always the resting order's price
    uint32_t qty;           // Fill quantity
    uint64_t timestamp;     // Reserved for future use
};
static_assert(sizeof(Trade) == 32);
```

Trade price is always the **resting (maker) order's price**, consistent with standard exchange price formation rules.

---

**`src/models/PriceLevel.h`** — doubly-linked-list head/tail per price

```cpp
struct PriceLevel {
    uint32_t head = INVALID_INDEX;
    uint32_t tail = INVALID_INDEX;
};
static_assert(sizeof(PriceLevel) == 8);
```

Each price level is just two `uint32_t` pool indices. The `OrderBook` holds two static arrays of 2,000,001 of these: `bids[MAX_PRICE+1]` and `asks[MAX_PRICE+1]`. They are statically allocated at engine startup, completely avoiding per-level heap allocations.

---

### Memory Layer — OrderPool

**`src/memory/OrderPool.h`**

The `OrderPool` is a **slab allocator with an intrusive freelist**. At startup it pre-allocates a single contiguous `std::vector<Order>` of `capacity` elements (default: 10,000,000) and links them all together into a freelist using each `Order`'s own `next` field.

```
 freeHead
    │
    ▼
[Order 0] ──next──▶ [Order 1] ──next──▶ [Order 2] ──▶ ... ──▶ [Order N-1] ──▶ INVALID
```

**Allocation** (`O(1)`): pops the head of the freelist and returns its index.
**Deallocation** (`O(1)`): pushes the index back to the front of the freelist.

Neither operation calls `new` or `delete`. The matching engine's hot path is entirely allocation-free after startup.

---

### Core Layer — OrderBook

**`src/core/OrderBook.h` / `OrderBook.cpp`**

The `OrderBook` is the central data structure. It holds:

| Field | Type | Purpose |
|---|---|---|
| `pool` | `OrderPool&` | Reference to the pre-allocated arena — no ownership |
| `bids[MAX_PRICE+1]` | `PriceLevel[]` | Doubly-linked list head+tail per bid price |
| `asks[MAX_PRICE+1]` | `PriceLevel[]` | Doubly-linked list head+tail per ask price |
| `activeBids` | `FastBitset<MAX_PRICE+1>` | Bitset of prices with at least one resting bid |
| `activeAsks` | `FastBitset<MAX_PRICE+1>` | Bitset of prices with at least one resting ask |
| `orderLookup` | `std::vector<uint32_t>` | Maps `orderId → poolIndex` for O(1) cancellation |
| `highestActiveBidHint` | `mutable uint32_t` | Hint to accelerate `find_prev` on the bid bitset |
| `lowestActiveAskHint` | `mutable uint32_t` | Hint to accelerate `find_next` on the ask bitset |

**`processOrder(req)`** is the single entry point. It dispatches to:

- **`cancelOrder(orderId)`** — for `OrderType::CANCEL`
- **`matchSide<S>(req)`** — templated on `Side::BUY` or `Side::SELL` to eliminate code duplication, then **`insertOrder(req)`** if quantity remains and the type is `LIMIT`

**`matchSide<S>`** is the hot matching loop:

```
while (qty remaining AND best opposing price crosses req.price):
    walk the linked list at that price level:
        fillQty = min(req.qty, restingOrder.remainingQty)
        emit Trade to egressQueue
        if resting order fully filled:
            unlink from list, free pool slot, advance to next
        else:
            break (incoming filled)
    if level is now empty:
        clear bit in activeBids/activeAsks
        find next best price via FastBitset
```

**`cancelOrder`** flow:
1. `orderLookup[orderId]` → pool index in O(1)
2. Identify price level from the `Order.price` and `Order.side` fields
3. Patch `prev.next` and `next.prev` pointers around the cancelled order (doubly-linked unlink)
4. If level is now empty, `reset()` the corresponding bitset bit
5. `pool.free(index)` — returns slot to freelist in O(1)
6. Invalidate `orderLookup[orderId]`

---

### Core Layer — FastBitset

**`src/core/FastBitset.h`**

A **two-level hierarchical bitset** templated on size `N` (instantiated as `N = MAX_PRICE+1 = 2,000,001`).

```
Level 1 — summary[]:   1 bit per 64-bit data word  (32,813 bits → 513 uint64_t words)
Level 0 — data[]:      1 bit per price             (2,000,001 bits → 31,251 uint64_t words)
```

When `data[w]` becomes entirely zero after a `reset()`, `summary[w/64]` is cleared automatically. This prevents stale summary bits — a subtle but catastrophic correctness bug if missed.

**`find_next(start)`** — finds the next set bit at or after `start` (used to find the lowest active ask):
1. Check the current 64-bit data word with a bitmask
2. If empty, scan the summary layer upward using `std::countr_zero` (BSF instruction)
3. Found a non-zero summary word → jump directly to its data word → `countr_zero` for the exact bit

**`find_prev(start)`** — finds the previous set bit at or before `start` (used to find the highest active bid):
1. Check the current 64-bit data word with a bitmask
2. If empty, scan summary layer downward using `std::countl_zero` (BSR instruction)

Both operations are effectively **O(1)** for the realistic price ranges used in this engine — the summary layer fits in a handful of cache lines. Compared to scanning the raw bitset linearly (O(N/64)), this is 500× faster in the worst case.

---

### Concurrency Layer — SPSCQueue

**`src/concurrency/SPSCQueue.h`**

A canonical **Single-Producer Single-Consumer lock-free ring buffer** templated on `T` and `Size` (must be a power of 2, enforced by `static_assert`).

```cpp
template <typename T, size_t Size>
class SPSCQueue {
    alignas(64) std::atomic<size_t> head;  // Producer's cache line
    std::array<T, Size>             buffer;
    alignas(64) std::atomic<size_t> tail;  // Consumer's cache line
};
```

**Memory layout is the key insight:**

- `head` is on its own 64-byte cache line — only the **producer** writes it
- `tail` is on its own 64-byte cache line — only the **consumer** writes it
- `buffer` sits between them, treated as read-only by each thread after the write

Without this layout, when the producer increments `head` it would dirty the same cache line that the consumer is reading `tail` from — **false sharing** — causing the CPU to bounce the cache line between cores hundreds of millions of times per second.

**Push** (producer thread only):
```cpp
bool push(const T& item) {
    size_t head = head.load(relaxed);
    size_t tail = tail.load(acquire);   // acquire: see consumer's writes
    if (head - tail == Size) return false;  // full
    buffer[head & MASK] = item;
    head.store(head + 1, release);      // release: publish to consumer
    return true;
}
```

**Pop** (consumer thread only): symmetric acquire/release pair on `tail`.

The `& MASK` modulo operation works only because `Size` is a power of 2, turning an expensive division into a single AND instruction.

---

### Concurrency Layer — CpuAffinity

**`src/concurrency/CpuAffinity.h`**

A thin cross-platform wrapper around OS thread-pinning APIs:

- **Windows**: `SetThreadAffinityMask(GetCurrentThread(), 1ULL << coreId)`
- **Linux/macOS**: `pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset)`

Pinning each thread to a dedicated logical core prevents the OS scheduler from migrating threads mid-execution, which would invalidate warm L1/L2 cache state and introduce jitter measured in microseconds.

---

### Core Layer — MatchingEngine

**`src/core/MatchingEngine.h`**

The `MatchingEngine` owns the `OrderPool` and `OrderBook` and runs a **spin-wait loop** on its dedicated thread:

```cpp
void start() {
    running = true;
    OrderRequest req;
    while (running.load(relaxed)) {
        if (ingressQueue.pop(req)) {
            if (req.type == OrderType::SHUTDOWN) break;
            book.processOrder(req, &egressQueue);
        } else {
            _mm_pause();  // x86 PAUSE — reduces speculative execution pressure
        }
    }
}
```

`_mm_pause()` is emitted as a single `PAUSE` CPU instruction. In a busy-wait loop it:
1. Signals to the CPU that this is a spin-wait, preventing memory order violation penalties
2. Reduces power consumption without yielding the core to the OS
3. Significantly improves performance of the other hardware thread sharing the same physical core (hyper-threading)

Shutdown is triggered by injecting a sentinel `OrderRequest{type = SHUTDOWN}` from the producer thread, allowing the matching thread to drain all queued messages before exiting.

---

## Performance Design Decisions

| Decision | Rationale |
|---|---|
| `alignas(32)` on `Order` and `Trade` | 2 structs per 64-byte cache line; eliminates split-line fetches |
| `alignas(64)` on SPSC head and tail | Prevents false sharing between producer and consumer cores |
| Pre-allocated slab arena | Zero `new`/`delete` on hot path; pool allocate/free are pointer bumps |
| Intrusive `next`/`prev` in `Order` | No separate list-node allocation; pointer chasing stays within the pool array |
| `FastBitset` with summary layer | Best-price discovery in ~2–5 CPU instructions instead of linear scan |
| Hint variables for bid/ask | Avoids restarting bitset scan from 0 or MAX_PRICE on every order |
| SPSC ring buffer over `std::queue` | Lockless, cache-friendly, branch-predictor-friendly; no heap allocation per message |
| CPU core pinning | Warm L1/L2 cache between orders; no OS preemption jitter |
| `_mm_pause()` in spin-wait | Prevents pipeline flush penalty; reduces inter-core cache contention |
| `std::memory_order_relaxed` for non-synchronizing loads | Avoids unnecessary memory fence instructions; only acquire/release where ordering matters |
| `template <Side S> matchSide(...)` | Single templated function, compiler generates two specialisations — no virtual dispatch or branch on the hot path |
| Static `bids[]` / `asks[]` arrays | Indexed directly by price — O(1) level access with no hash collision |

---

## Building

### Prerequisites

- **CMake ≥ 3.14** (a bundled binary is included at `cmake-3.28.1-windows-x86_64/`)
- **C++20 compiler** — MSVC (Visual Studio 2022) on Windows; GCC 12+ or Clang 15+ on Linux
- **Python 3** — only required to regenerate the benchmark dataset

### Steps

```bash
# 1. (Optional) Generate the 1M-order MBO dataset for the benchmark
python scripts/generate_mbo.py

# 2. Configure with CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build all targets
cmake --build build --config Release

# (Windows alternative using bundled CMake)
cmake-3.28.1-windows-x86_64\bin\cmake.exe -B build -DCMAKE_BUILD_TYPE=Release
cmake-3.28.1-windows-x86_64\bin\cmake.exe --build build --config Release
```

**Compiler flags applied automatically by CMakeLists.txt:**

| Mode | MSVC | GCC/Clang |
|---|---|---|
| Release | `/O2 /GL /DNDEBUG` | `-O3 -march=native -DNDEBUG` |
| Debug | `/Od /Zi` | `-O0 -g` |

`-march=native` enables AVX2/BMI2 intrinsics including the hardware `BSF`/`BSR` instructions used by `FastBitset`.

---

## Running

### Main Engine Demo

Injects 1,000,000 synthetic orders (500k buys at price 100, 500k sells at price 100) across three pinned threads and verifies that exactly 500,000 trades were produced:

```bash
./build/hyper-engine          # Linux/macOS
.\build\Release\hyper-engine.exe   # Windows
```

Expected output:
```
HyperEngine Day 5: Multi-threaded Lock-Free Engine
Matching Engine started on Core 1
Ingress generator started on Core 3
Finished injecting 1000000 orders.
Total trades recorded: 500000
Lock-free trading system successfully built and verified!
```

### Benchmark

Loads real synthetic Market-By-Order tick data from `data/mbo_data.csv` (1M orders with 80% limit inserts and 20% random cancels, heavy-tailed price distribution) and reports throughput and per-order latency:

```bash
./build/bench                    # Linux/macOS
.\build\Release\bench.exe        # Windows
```

Expected output (representative; varies by hardware):
```
HyperEngine Level 3 Market-By-Order Benchmark
Loading real tick data...
Loaded 1000000 historical orders.

--- THROUGHPUT BENCHMARK (REAL MBO DATA) ---
Throughput:   ~50,000,000+ orders/second
Total Time:   ~0.019 seconds
Trades fired: <varies>

--- LATENCY BENCHMARK (REAL MBO DATA) ---
p50 (Median): ~150 cycles
p90:          ~220 cycles
p99:          ~500 cycles
p99.9:        ~1200 cycles
p99.99:       ~3000 cycles
```

Latency is measured using `RDTSC` CPU cycle counter with `_mm_lfence()` serialisation barriers to prevent out-of-order execution from corrupting the measurement.

---

## Benchmarks

### MBO Data Generation

The `scripts/generate_mbo.py` script generates a realistic 1M-row CSV dataset simulating Market-By-Order (MBO) feed data:

- **80%** of events are `LIMIT` order inserts
- **20%** of events are random `CANCEL` requests against previously placed orders
- Prices follow a **Laplace (heavy-tailed) distribution** around a starting mid-price of `50000` ($500.00), simulating real intraday price drift with occasional large jumps
- Both bid and ask prices walk independently

```bash
python scripts/generate_mbo.py
# → data/mbo_data.csv  (~19 MB, 1,000,000 rows)
```

CSV format:
```
order_id,price,qty,side,type
1,49998,42,0,0
2,50003,17,1,0
1,0,0,0,2       ← cancel of order_id=1
```

---

## Testing

The test suite uses **Google Test** (automatically fetched by CMake via `FetchContent` from the GitHub release).

### Running All Tests

```bash
cd build
ctest --output-on-failure -C Release
```

### Individual Test Binaries

| Binary | What It Tests |
|---|---|
| `test_order_pool` | `OrderPool` allocation, freelist recycling, capacity bounds |
| `test_order_book` | Incremental day-by-day correctness (insert → match → cancel sequences) |
| `test_correctness` | Comprehensive: struct sizes, alignment, FIFO ordering, partial fills, multi-level sweeps, cancel edge cases, market orders, regression guards |
| `test_spsc_queue` | Single-threaded push/pop correctness and multi-threaded stress (producer/consumer on separate threads) |
| `test_stress` | End-to-end load test with realistic MBO data; measures throughput via `std::chrono::high_resolution_clock` |

### Test Coverage Highlights (`test_correctness`)

```
MemoryLayout      — Order=32B, Trade=32B, PriceLevel=8B, OrderRequest=24B, alignof(Order)=32
Insert            — Single bid/ask, FIFO ordering at same price, multi-level tracking,
                    pool exhaustion throw, price > MAX_PRICE throw
Matching          — Exact fill, taker partial fill, maker partial fill, price priority,
                    time priority (FIFO), multi-level sweep, no-cross guard, maker price rule,
                    maker/taker ID assignment
Cancel            — Head cancel, tail cancel, middle cancel, only-order cancel,
                    non-existent order (no-op), double cancel (idempotent),
                    pool slot recycled after cancel, cancel already-filled order (no-op)
MarketOrder       — Buy market sweeps book, sell market sweeps bids, oversized market (no resting)
EdgeCase          — Bid-ask spread no spurious match, large volume no leaked orders,
                    qty=1 fills, cancel-then-reinsert same ID, interleaved buy/sell,
                    MAX_PRICE order accepted, orderLookup dynamic resize
```

---

## Project Structure

```
hyper-engine/
├── CMakeLists.txt               # Build configuration for all targets
├── README.md
│
├── src/
│   ├── main.cpp                 # 3-thread demo: ingress → engine → logger
│   │
│   ├── models/
│   │   ├── Constants.h          # Side, OrderType enums; INVALID_INDEX, MAX_PRICE
│   │   ├── Order.h              # alignas(32) 32-byte order struct (intrusive linked list)
│   │   ├── OrderRequest.h       # 24-byte wire format for incoming commands
│   │   ├── Trade.h              # alignas(32) 32-byte trade event struct
│   │   └── PriceLevel.h        # 8-byte {head, tail} per price level
│   │
│   ├── memory/
│   │   └── OrderPool.h          # Slab arena + intrusive freelist; O(1) alloc/free
│   │
│   ├── core/
│   │   ├── FastBitset.h         # Two-level hierarchical bitset; O(log₆₄ N) find_next/find_prev
│   │   ├── OrderBook.h          # Price-time-priority limit order book interface
│   │   ├── OrderBook.cpp        # Insert, cancel, matchSide<S>, processOrder
│   │   └── MatchingEngine.h     # Spin-loop wrapper; owns pool + book; _mm_pause() idle
│   │
│   └── concurrency/
│       ├── SPSCQueue.h          # Lock-free SPSC ring buffer; false-sharing-free layout
│       └── CpuAffinity.h        # Cross-platform thread→core pinning (Windows + Linux)
│
├── tests/
│   ├── Benchmark.cpp            # Throughput + latency (RDTSC) benchmark vs MBO data
│   ├── test_correctness.cpp     # 40+ GTest cases: layout, matching, cancel, edge cases
│   ├── test_order_book.cpp      # Incremental correctness tests
│   ├── test_order_pool.cpp      # OrderPool allocation / freelist tests
│   ├── test_spsc_queue.cpp      # SPSC queue single & multi-thread tests
│   └── test_stress.cpp          # End-to-end load test with MBO simulation
│
├── scripts/
│   └── generate_mbo.py          # Generates 1M realistic MBO ticks → data/mbo_data.csv
│
└── data/
    └── mbo_data.csv             # 1M synthetic Market-By-Order tick data (~19 MB)
```

---

## Key Invariants

1. **No `new` / `delete` on the hot path** — all order memory is managed by `OrderPool`
2. **No mutexes on the hot path** — all inter-thread communication is via SPSC queues
3. **`Order` is always 32 bytes** — enforced by `static_assert` at compile time
4. **`Trade` is always 32 bytes** — enforced by `static_assert` at compile time
5. **`OrderRequest` is always 24 bytes** — enforced by `static_assert` at compile time
6. **SPSC queue size is always a power of 2** — enforced by `static_assert(std::has_single_bit(Size))`
7. **Price-time priority** — within a price level, orders are always matched in FIFO insertion order
8. **Trade price = resting order's price** — the aggressive order's price never sets the execution price
9. **Cancellation is idempotent** — cancelling an already-filled or non-existent order is always a safe no-op
10. **FastBitset summary layer is consistent** — a summary bit is cleared only when the entire corresponding 64-bit data word is zero
