#include <memory>
#include <gtest/gtest.h>
#include "../src/memory/OrderPool.h"
#include "../src/models/Order.h"

using namespace hyperengine;
using namespace hyperengine::models;
using namespace hyperengine::memory;

TEST(OrderSizeTest, IsExactly32Bytes) {
    EXPECT_EQ(sizeof(Order), 32);
}

TEST(OrderPoolTest, AllocateAndFree1Million) {
    const size_t capacity = 1000000;
    OrderPool pool(capacity);

    std::vector<uint32_t> allocatedIndices;
    allocatedIndices.reserve(capacity);

    // Allocate 1 million orders
    for (size_t i = 0; i < capacity; ++i) {
        uint32_t index = pool.allocate();
        EXPECT_NE(index, INVALID_INDEX);
        allocatedIndices.push_back(index);
    }

    // Next allocation should fail
    EXPECT_EQ(pool.allocate(), INVALID_INDEX);

    // Free all
    for (uint32_t index : allocatedIndices) {
        pool.free(index);
    }

    // Should be able to allocate 1 million again
    for (size_t i = 0; i < capacity; ++i) {
        uint32_t index = pool.allocate();
        EXPECT_NE(index, INVALID_INDEX);
    }
}
