#include <memory>
#include <iostream>
#include <cassert>
#include <stdexcept>
#include "../src/memory/OrderPool.h"
#include "../src/core/OrderBook.h"

using namespace hyperengine;
using namespace hyperengine::models;
using namespace hyperengine::memory;
using namespace hyperengine::core;

void RunTest1_SingleInsert() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest req{1001, 100, 50, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(req);

    const PriceLevel& level = book->getBidLevel(100);
    assert(level.head == 0);
    assert(level.tail == 0);
    assert(book->isBidActive(100));
}

void RunTest2_FIFOAppend() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest req1{1001, 100, 50, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(req1);

    OrderRequest req2{1002, 100, 30, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(req2);

    const PriceLevel& level = book->getBidLevel(100);
    assert(level.head == 0);
    assert(level.tail == 1);
    
    const Order& firstOrder = pool.get(0);
    const Order& secondOrder = pool.get(1);

    assert(firstOrder.next == 1);
    assert(secondOrder.prev == 0);
}

void RunTest3_MemoryBoundary() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    for (int i = 0; i < 10; ++i) {
        OrderRequest req{static_cast<uint64_t>(1000 + i), 100, 50, Side::BUY, OrderType::LIMIT, 0};
        book->insertOrder(req);
    }

    OrderRequest reqFailed{2000, 100, 50, Side::BUY, OrderType::LIMIT, 0};
    bool threw = false;
    try {
        book->insertOrder(reqFailed);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

// Day 3 Tests
void RunTest4_PartialFillMaker() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest sellReq{1, 50, 100, Side::SELL, OrderType::LIMIT, 0};
    book->processOrder(sellReq);

    OrderRequest buyReq{2, 50, 40, Side::BUY, OrderType::LIMIT, 0};
    book->processOrder(buyReq);

    assert(book->getExecutedTrades().size() == 1);
    assert(book->getExecutedTrades()[0].qty == 40);

    const PriceLevel& askLevel = book->getAskLevel(50);
    assert(askLevel.head != INVALID_INDEX);
    const Order& restingSell = pool.get(askLevel.head);
    
    assert(restingSell.remainingQty == 60);
    assert(buyReq.qty == 0);
    std::cout << "Test 4 Passed: Partial Fill (Maker)\n";
}

void RunTest5_PartialFillTaker() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest sellReq{1, 50, 100, Side::SELL, OrderType::LIMIT, 0};
    book->processOrder(sellReq);

    OrderRequest buyReq{2, 50, 150, Side::BUY, OrderType::LIMIT, 0};
    book->processOrder(buyReq);

    assert(book->getExecutedTrades().size() == 1);
    assert(book->getExecutedTrades()[0].qty == 100);

    const PriceLevel& askLevel = book->getAskLevel(50);
    assert(askLevel.head == INVALID_INDEX); // Empties out
    assert(!book->isAskActive(50));

    const PriceLevel& bidLevel = book->getBidLevel(50);
    assert(bidLevel.head != INVALID_INDEX);
    const Order& restingBuy = pool.get(bidLevel.head);
    
    assert(restingBuy.remainingQty == 50);
    assert(buyReq.qty == 50); // It matched 100, so 50 is left over and was placed in the book
    std::cout << "Test 5 Passed: Partial Fill (Taker)\n";
}

void RunTest6_PricePriority() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest sellReq1{1, 50, 10, Side::SELL, OrderType::LIMIT, 0};
    OrderRequest sellReq2{2, 51, 10, Side::SELL, OrderType::LIMIT, 0};
    
    book->processOrder(sellReq1);
    book->processOrder(sellReq2);

    OrderRequest sweepBuy{3, 52, 15, Side::BUY, OrderType::LIMIT, 0};
    book->processOrder(sweepBuy);

    assert(book->getExecutedTrades().size() == 2);
    // $50 should fill first
    assert(book->getExecutedTrades()[0].price == 50);
    assert(book->getExecutedTrades()[0].qty == 10);
    
    // Then $51
    assert(book->getExecutedTrades()[1].price == 51);
    assert(book->getExecutedTrades()[1].qty == 5);

    const PriceLevel& askLevel50 = book->getAskLevel(50);
    assert(askLevel50.head == INVALID_INDEX);

    const PriceLevel& askLevel51 = book->getAskLevel(51);
    assert(askLevel51.head != INVALID_INDEX);
    assert(pool.get(askLevel51.head).remainingQty == 5);

    std::cout << "Test 6 Passed: Price Priority\n";
}

// Day 4 Tests
void RunTest7_CancelHead() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest a{1, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    OrderRequest b{2, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    OrderRequest c{3, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(a);
    book->insertOrder(b);
    book->insertOrder(c);

    book->cancelOrder(1); // Cancel Head A

    const PriceLevel& level = book->getBidLevel(100);
    // B should now be the head
    // A was index 0, B was 1, C was 2.
    assert(level.head == 1);
    assert(pool.get(1).prev == INVALID_INDEX);
    std::cout << "Test 7 Passed: Cancel Head\n";
}

void RunTest8_CancelTail() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest a{1, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    OrderRequest b{2, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    OrderRequest c{3, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(a);
    book->insertOrder(b);
    book->insertOrder(c);

    book->cancelOrder(3); // Cancel Tail C

    const PriceLevel& level = book->getBidLevel(100);
    // B should now be the tail
    assert(level.tail == 1);
    assert(pool.get(1).next == INVALID_INDEX);
    std::cout << "Test 8 Passed: Cancel Tail\n";
}

void RunTest9_CancelMid() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest a{1, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    OrderRequest b{2, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    OrderRequest c{3, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(a);
    book->insertOrder(b);
    book->insertOrder(c);

    book->cancelOrder(2); // Cancel Mid B

    // A is index 0, C is index 2.
    assert(pool.get(0).next == 2);
    assert(pool.get(2).prev == 0);
    std::cout << "Test 9 Passed: Cancel Mid\n";
}

void RunTest10_LevelDepletion() {
    OrderPool pool(10);
    auto book = std::make_unique<OrderBook>(pool);

    OrderRequest a{1, 100, 10, Side::BUY, OrderType::LIMIT, 0};
    book->insertOrder(a);

    book->cancelOrder(1); // Cancel only order A

    const PriceLevel& level = book->getBidLevel(100);
    assert(level.head == INVALID_INDEX);
    assert(level.tail == INVALID_INDEX);
    assert(!book->isBidActive(100));
    std::cout << "Test 10 Passed: Level Depletion\n";
}


int old_main() {
    RunTest1_SingleInsert();
    RunTest2_FIFOAppend();
    RunTest3_MemoryBoundary();
    
    std::cout << "Day 2 Tests Passed.\n";

    RunTest4_PartialFillMaker();
    RunTest5_PartialFillTaker();
    RunTest6_PricePriority();

    std::cout << "Day 3 Tests Passed.\n";

    RunTest7_CancelHead();
    RunTest8_CancelTail();
    RunTest9_CancelMid();
    RunTest10_LevelDepletion();

    std::cout << "All Day 4 Tests Passed successfully.\n";
    return 0;
}
