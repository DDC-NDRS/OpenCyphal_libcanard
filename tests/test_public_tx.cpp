// This software is distributed under the terms of the MIT License.
// Copyright (c) 2016 OpenCyphal Development Team.

#include "exposed.hpp"
#include "helpers.hpp"
#include "catch.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>

TEST_CASE("TxBasic0")
{
    using exposed::TxItem;

    helpers::Instance ins;
    helpers::TxQueue  que(200, CANARD_MTU_CAN_FD, ins.makeCanardMemoryResource());

    auto& alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(CANARD_NODE_ID_UNSET == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_FD == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    alloc.setAllocationCeiling(496);

    CanardTransferMetadata meta{};

    // Single-frame with padding.
    meta.priority       = CanardPriorityNominal;
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.port_id        = 321;
    meta.remote_node_id = CANARD_NODE_ID_UNSET;
    meta.transfer_id    = 21;
    REQUIRE(1 == que.push(&ins.getInstance(), 1'000'000'000'000ULL, meta, {8, payload.data()}));
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    REQUIRE(10 < alloc.getTotalAllocatedAmount());
    REQUIRE(160 > alloc.getTotalAllocatedAmount());
    REQUIRE(que.peek()->tx_deadline_usec == 1'000'000'000'000ULL);
    REQUIRE(que.peek()->frame.payload.size == 12);  // Three bytes of padding.
    REQUIRE(que.peek()->getPayloadByte(0) == 0);    // Payload start.
    REQUIRE(que.peek()->getPayloadByte(1) == 1);
    REQUIRE(que.peek()->getPayloadByte(2) == 2);
    REQUIRE(que.peek()->getPayloadByte(3) == 3);
    REQUIRE(que.peek()->getPayloadByte(4) == 4);
    REQUIRE(que.peek()->getPayloadByte(5) == 5);
    REQUIRE(que.peek()->getPayloadByte(6) == 6);
    REQUIRE(que.peek()->getPayloadByte(7) == 7);   // Payload end.
    REQUIRE(que.peek()->getPayloadByte(8) == 0);   // Padding.
    REQUIRE(que.peek()->getPayloadByte(9) == 0);   // Padding.
    REQUIRE(que.peek()->getPayloadByte(10) == 0);  // Padding.
    REQUIRE(que.peek()->isStartOfTransfer());      // Tail byte at the end.
    REQUIRE(que.peek()->isEndOfTransfer());
    REQUIRE(que.peek()->isToggleBitSet());

    // Multi-frame. Priority low, inserted at the end of the TX queue.
    meta.priority    = CanardPriorityLow;
    meta.transfer_id = 22;
    que.setMTU(CANARD_MTU_CAN_CLASSIC);
    ins.setNodeID(42);
    REQUIRE(2 ==
            que.push(&ins.getInstance(), 1'000'000'000'100ULL, meta, {8, payload.data()}));  // 8 bytes --> 2 frames
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(20 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());

    // Check the TX queue.
    {
        const auto q = que.linearize();
        REQUIRE(3 == q.size());
        REQUIRE(q.at(0)->tx_deadline_usec == 1'000'000'000'000ULL);
        REQUIRE(q.at(0)->frame.payload.size == 12);
        REQUIRE(q.at(0)->isStartOfTransfer());
        REQUIRE(q.at(0)->isEndOfTransfer());
        REQUIRE(q.at(0)->isToggleBitSet());
        //
        REQUIRE(q.at(1)->tx_deadline_usec == 1'000'000'000'100ULL);
        REQUIRE(q.at(1)->frame.payload.size == 8);
        REQUIRE(q.at(1)->isStartOfTransfer());
        REQUIRE(!q.at(1)->isEndOfTransfer());
        REQUIRE(q.at(1)->isToggleBitSet());
        //
        REQUIRE(q.at(2)->tx_deadline_usec == 1'000'000'000'100ULL);
        REQUIRE(q.at(2)->frame.payload.size == 4);  // One leftover, two CRC, one tail.
        REQUIRE(!q.at(2)->isStartOfTransfer());
        REQUIRE(q.at(2)->isEndOfTransfer());
        REQUIRE(!q.at(2)->isToggleBitSet());
    }

    // Single-frame, OOM for item.
    alloc.setAllocationCeiling(alloc.getTotalAllocatedAmount());  // Seal up the heap at this level.
    meta.priority    = CanardPriorityLow;
    meta.transfer_id = 23;
    REQUIRE(-CANARD_ERROR_OUT_OF_MEMORY ==
            que.push(&ins.getInstance(), 1'000'000'000'200ULL, meta, {1, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    // The same, but OK for item allocation and OOM for payload data.
    alloc.setAllocationCeiling(alloc.getTotalAllocatedAmount() + sizeof(TxItem));  // Seal up the heap at this level.
    meta.priority    = CanardPriorityLow;
    meta.transfer_id = 23;
    REQUIRE(-CANARD_ERROR_OUT_OF_MEMORY ==
            que.push(&ins.getInstance(), 1'000'000'000'200ULL, meta, {1, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());

    // Multi-frame, first frame added successfully, then OOM. The entire transaction was rejected.
    alloc.setAllocationCeiling(alloc.getTotalAllocatedAmount() + sizeof(TxItem) + 10U);
    meta.priority    = CanardPriorityHigh;
    meta.transfer_id = 24;
    REQUIRE(-CANARD_ERROR_OUT_OF_MEMORY ==
            que.push(&ins.getInstance(), 1'000'000'000'300ULL, meta, {100, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(20 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());

    // Pop the queue.
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(8))).value)
    constexpr std::uint16_t CRC8 = 0x178DU;
    CanardTxQueueItem*      ti   = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 12);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 8));
    REQUIRE(0 == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[8]);   // Padding.
    REQUIRE(0 == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[9]);   // Padding.
    REQUIRE(0 == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[10]);  // Padding.
    REQUIRE((0b11100000U | 21U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[11]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'000ULL);
    ti = que.peek();
    REQUIRE(nullptr != ti);  // Make sure we get the same frame again.
    REQUIRE(ti->frame.payload.size == 12);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 8));
    REQUIRE((0b11100000U | 21U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[11]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'000ULL);
    auto* item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 8);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 7));
    REQUIRE((0b10100000U | 22U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[7]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'100ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 4);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 7U, 1));
    REQUIRE((CRC8 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[1]);
    REQUIRE((CRC8 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[2]);
    REQUIRE((0b01000000U | 22U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[3]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'100ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr == ti);
    REQUIRE(nullptr == que.pop(nullptr));
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr == ti);

    alloc.setAllocationCeiling(1000);

    // Multi-frame, success. CRC split over the frame boundary.
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(61))).value)
    constexpr std::uint16_t CRC61 = 0x554EU;
    que.setMTU(32);
    meta.priority    = CanardPriorityFast;
    meta.transfer_id = 25;
    // CRC takes 2 bytes at the end; 3 frames: (31+1) + (30+1+1) + (1+1)
    REQUIRE(3 == que.push(&ins.getInstance(), 1'000'000'001'000ULL, meta, {31 + 30, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(40 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());
    // Read the generated frames.
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 31));
    REQUIRE((0b10100000U | 25U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'001'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 31U, 30));
    REQUIRE((CRC61 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[30]);
    REQUIRE((0b00000000U | 25U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'001'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 2);  // The last byte of CRC plus the tail byte.
    REQUIRE((CRC61 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[0]);
    REQUIRE((0b01100000U | 25U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[1]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'001'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Multi-frame, success. CRC is in the last frame->
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(62))).value)
    constexpr std::uint16_t CRC62 = 0xA3AEU;
    que.setMTU(32);
    meta.priority    = CanardPrioritySlow;
    meta.transfer_id = 26;
    // CRC takes 2 bytes at the end; 3 frames: (31+1) + (31+1) + (2+1)
    REQUIRE(3 == que.push(&ins.getInstance(), 1'000'000'002'000ULL, meta, {31 + 31, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(40 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());
    // Read the generated frames.
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 31));
    REQUIRE((0b10100000U | 26U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'002'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 31U, 31));
    REQUIRE((0b00000000U | 26U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'002'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 3);  // The CRC plus the tail byte.
    REQUIRE((CRC62 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[0]);
    REQUIRE((CRC62 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[1]);
    REQUIRE((0b01100000U | 26U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[2]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'002'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Multi-frame with padding.
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(112)) + [0] * 12).value)
    constexpr std::uint16_t CRC112Padding12 = 0xE7A5U;
    que.setMTU(64);
    meta.priority    = CanardPriorityImmediate;
    meta.transfer_id = 27;
    // 63 + 63 - 2 = 124 bytes; 124 - 112 = 12 bytes of padding.
    REQUIRE(2 == que.push(&ins.getInstance(), 1'000'000'003'000ULL, meta, {112, payload.data()}));
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    // Read the generated frames.
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 64);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 63));
    REQUIRE((0b10100000U | 27U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[63]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'003'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 64);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 63U, 49));
    REQUIRE(std::all_of(reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data) + 49,  // Check padding.
                        reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data) + 61,
                        [](auto x) { return x == 0U; }));
    REQUIRE((CRC112Padding12 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[61]);    // CRC
    REQUIRE((CRC112Padding12 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[62]);  // CRC
    REQUIRE((0b01000000U | 27U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[63]);        // Tail
    REQUIRE(ti->tx_deadline_usec == 1'000'000'003'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Single-frame empty.
    meta.transfer_id = 28;
    REQUIRE(1 == que.push(&ins.getInstance(), 1'000'000'004'000ULL, meta, {0, nullptr}));
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    REQUIRE(120 > alloc.getTotalAllocatedAmount());
    REQUIRE(que.peek()->tx_deadline_usec == 1'000'000'004'000ULL);
    REQUIRE(que.peek()->frame.payload.size == 1);
    REQUIRE(que.peek()->isStartOfTransfer());
    REQUIRE(que.peek()->isEndOfTransfer());
    REQUIRE(que.peek()->isToggleBitSet());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 1);
    REQUIRE((0b11100000U | 28U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[0]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'004'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Nothing left to peek at.
    ti = que.peek();
    REQUIRE(nullptr == ti);

    // Invalid transfer.
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.remote_node_id = 42;
    meta.transfer_id    = 123;
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==
            que.push(&ins.getInstance(), 1'000'000'005'000ULL, meta, {8, payload.data()}));
    ti = que.peek();
    REQUIRE(nullptr == ti);

    // Error handling.
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT == canardTxPush(nullptr, nullptr, 0, nullptr, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT == canardTxPush(nullptr, nullptr, 0, &meta, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==
            canardTxPush(nullptr, &ins.getInstance(), 0, &meta, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==
            canardTxPush(&que.getInstance(), &ins.getInstance(), 0, nullptr, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT == que.push(&ins.getInstance(), 1'000'000'006'000ULL, meta, {1, nullptr}));

    REQUIRE(nullptr == canardTxPeek(nullptr));
    REQUIRE(nullptr == canardTxPop(nullptr, nullptr));             // No effect.
    REQUIRE(nullptr == canardTxPop(&que.getInstance(), nullptr));  // No effect.
}

TEST_CASE("TxBasic1")
{
    helpers::Instance ins;
    helpers::TxQueue  que(3, CANARD_MTU_CAN_FD, ins.makeCanardMemoryResource());  // Limit capacity at 3 frames.

    auto& alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(CANARD_NODE_ID_UNSET == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_FD == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    CanardTransferMetadata meta{};

    // Single-frame with padding.
    meta.priority       = CanardPriorityNominal;
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.port_id        = 321;
    meta.remote_node_id = CANARD_NODE_ID_UNSET;
    meta.transfer_id    = 21;
    REQUIRE(1 == que.push(&ins.getInstance(), 1'000'000'000'000ULL, meta, {8, payload.data()}));
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    REQUIRE(10 < alloc.getTotalAllocatedAmount());
    REQUIRE(160 > alloc.getTotalAllocatedAmount());
    REQUIRE(que.peek()->tx_deadline_usec == 1'000'000'000'000ULL);
    REQUIRE(que.peek()->frame.payload.size == 12);  // Three bytes of padding.
    REQUIRE(que.peek()->getPayloadByte(0) == 0);    // Payload start.
    REQUIRE(que.peek()->getPayloadByte(1) == 1);
    REQUIRE(que.peek()->getPayloadByte(2) == 2);
    REQUIRE(que.peek()->getPayloadByte(3) == 3);
    REQUIRE(que.peek()->getPayloadByte(4) == 4);
    REQUIRE(que.peek()->getPayloadByte(5) == 5);
    REQUIRE(que.peek()->getPayloadByte(6) == 6);
    REQUIRE(que.peek()->getPayloadByte(7) == 7);   // Payload end.
    REQUIRE(que.peek()->getPayloadByte(8) == 0);   // Padding.
    REQUIRE(que.peek()->getPayloadByte(9) == 0);   // Padding.
    REQUIRE(que.peek()->getPayloadByte(10) == 0);  // Padding.
    REQUIRE(que.peek()->isStartOfTransfer());      // Tail byte at the end.
    REQUIRE(que.peek()->isEndOfTransfer());
    REQUIRE(que.peek()->isToggleBitSet());

    // Multi-frame. Priority low, inserted at the end of the TX queue. Two frames exhaust the capacity of the queue.
    meta.priority    = CanardPriorityLow;
    meta.transfer_id = 22;
    que.setMTU(CANARD_MTU_CAN_CLASSIC);
    ins.setNodeID(42);
    REQUIRE(2 ==
            que.push(&ins.getInstance(), 1'000'000'000'100ULL, meta, {8, payload.data()}));  // 8 bytes --> 2 frames
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(20 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());

    // Check the TX queue.
    {
        const auto q = que.linearize();
        REQUIRE(3 == q.size());
        REQUIRE(q.at(0)->tx_deadline_usec == 1'000'000'000'000ULL);
        REQUIRE(q.at(0)->frame.payload.size == 12);
        REQUIRE(q.at(0)->isStartOfTransfer());
        REQUIRE(q.at(0)->isEndOfTransfer());
        REQUIRE(q.at(0)->isToggleBitSet());
        //
        REQUIRE(q.at(1)->tx_deadline_usec == 1'000'000'000'100ULL);
        REQUIRE(q.at(1)->frame.payload.size == 8);
        REQUIRE(q.at(1)->isStartOfTransfer());
        REQUIRE(!q.at(1)->isEndOfTransfer());
        REQUIRE(q.at(1)->isToggleBitSet());
        //
        REQUIRE(q.at(2)->tx_deadline_usec == 1'000'000'000'100ULL);
        REQUIRE(q.at(2)->frame.payload.size == 4);  // One leftover, two CRC, one tail.
        REQUIRE(!q.at(2)->isStartOfTransfer());
        REQUIRE(q.at(2)->isEndOfTransfer());
        REQUIRE(!q.at(2)->isToggleBitSet());
    }

    // Single-frame, OOM reported but the heap is not exhausted (because queue is filled up).
    meta.priority    = CanardPriorityLow;
    meta.transfer_id = 23;
    REQUIRE(-CANARD_ERROR_OUT_OF_MEMORY ==
            que.push(&ins.getInstance(), 1'000'000'000'200ULL, meta, {1, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());

    // Multi-frame, no frames are added -- bail early always.
    meta.priority    = CanardPriorityHigh;
    meta.transfer_id = 24;
    REQUIRE(-CANARD_ERROR_OUT_OF_MEMORY ==
            que.push(&ins.getInstance(), 1'000'000'000'300ULL, meta, {100, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(20 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());

    // Pop the queue.
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(8))).value)
    constexpr std::uint16_t CRC8 = 0x178DU;
    CanardTxQueueItem*      ti   = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 12);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 8));
    REQUIRE(0 == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[8]);   // Padding.
    REQUIRE(0 == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[9]);   // Padding.
    REQUIRE(0 == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[10]);  // Padding.
    REQUIRE((0b11100000U | 21U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[11]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'000ULL);
    ti = que.peek();
    REQUIRE(nullptr != ti);  // Make sure we get the same frame again.
    REQUIRE(ti->frame.payload.size == 12);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 8));
    REQUIRE((0b11100000U | 21U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[11]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'000ULL);
    auto* item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 8);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 7));
    REQUIRE((0b10100000U | 22U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[7]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'100ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 4);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 7U, 1));
    REQUIRE((CRC8 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[1]);
    REQUIRE((CRC8 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[2]);
    REQUIRE((0b01000000U | 22U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[3]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'000'100ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr == ti);
    REQUIRE(nullptr == que.pop(ti));  // Invocation when empty has no effect.
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr == ti);

    // Multi-frame, success. CRC split over the frame boundary.
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(61))).value)
    constexpr std::uint16_t CRC61 = 0x554EU;
    que.setMTU(32);
    meta.priority    = CanardPriorityFast;
    meta.transfer_id = 25;
    // CRC takes 2 bytes at the end; 3 frames: (31+1) + (30+1+1) + (1+1)
    REQUIRE(3 == que.push(&ins.getInstance(), 1'000'000'001'000ULL, meta, {31 + 30, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(40 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());
    // Read the generated frames.
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 31));
    REQUIRE((0b10100000U | 25U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'001'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 31U, 30));
    REQUIRE((CRC61 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[30]);
    REQUIRE((0b00000000U | 25U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'001'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 2);  // The last byte of CRC plus the tail byte.
    REQUIRE((CRC61 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[0]);
    REQUIRE((0b01100000U | 25U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[1]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'001'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Multi-frame, success. CRC is in the last frame->
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(62))).value)
    constexpr std::uint16_t CRC62 = 0xA3AEU;
    que.setMTU(32);
    meta.priority    = CanardPrioritySlow;
    meta.transfer_id = 26;
    // CRC takes 2 bytes at the end; 3 frames: (31+1) + (31+1) + (2+1)
    REQUIRE(3 == que.push(&ins.getInstance(), 1'000'000'002'000ULL, meta, {31 + 31, payload.data()}));
    REQUIRE(3 == que.getSize());
    REQUIRE(6 == alloc.getNumAllocatedFragments());
    REQUIRE(40 < alloc.getTotalAllocatedAmount());
    REQUIRE(496 > alloc.getTotalAllocatedAmount());
    // Read the generated frames.
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 31));
    REQUIRE((0b10100000U | 26U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'002'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 32);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 31U, 31));
    REQUIRE((0b00000000U | 26U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[31]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'002'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 3);  // The CRC plus the tail byte.
    REQUIRE((CRC62 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[0]);
    REQUIRE((CRC62 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[1]);
    REQUIRE((0b01100000U | 26U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[2]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'002'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Multi-frame with padding.
    // hex(pycyphal.transport.commons.crc.CRC16CCITT.new(list(range(112)) + [0] * 12).value)
    constexpr std::uint16_t CRC112Padding12 = 0xE7A5U;
    que.setMTU(64);
    meta.priority    = CanardPriorityImmediate;
    meta.transfer_id = 27;
    // 63 + 63 - 2 = 124 bytes; 124 - 112 = 12 bytes of padding.
    REQUIRE(2 == que.push(&ins.getInstance(), 1'000'000'003'000ULL, meta, {112, payload.data()}));
    REQUIRE(2 == que.getSize());
    REQUIRE(4 == alloc.getNumAllocatedFragments());
    // Read the generated frames.
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 64);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 63));
    REQUIRE((0b10100000U | 27U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[63]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'003'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 64);
    REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 63U, 49));
    REQUIRE(std::all_of(reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data) + 49,  // Check padding.
                        reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data) + 61,
                        [](auto x) { return x == 0U; }));
    REQUIRE((CRC112Padding12 >> 8U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[61]);    // CRC
    REQUIRE((CRC112Padding12 & 0xFFU) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[62]);  // CRC
    REQUIRE((0b01000000U | 27U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[63]);        // Tail
    REQUIRE(ti->tx_deadline_usec == 1'000'000'003'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Single-frame empty.
    meta.transfer_id = 28;
    REQUIRE(1 == que.push(&ins.getInstance(), 1'000'000'004'000ULL, meta, {0, nullptr}));
    REQUIRE(1 == que.getSize());
    REQUIRE(2 == alloc.getNumAllocatedFragments());
    REQUIRE(120 > alloc.getTotalAllocatedAmount());
    REQUIRE(que.peek()->tx_deadline_usec == 1'000'000'004'000ULL);
    REQUIRE(que.peek()->frame.payload.size == 1);
    REQUIRE(que.peek()->isStartOfTransfer());
    REQUIRE(que.peek()->isEndOfTransfer());
    REQUIRE(que.peek()->isToggleBitSet());
    ti = que.peek();
    REQUIRE(nullptr != ti);
    REQUIRE(ti->frame.payload.size == 1);
    REQUIRE((0b11100000U | 28U) == reinterpret_cast<const std::uint8_t*>(ti->frame.payload.data)[0]);
    REQUIRE(ti->tx_deadline_usec == 1'000'000'004'000ULL);
    item = que.pop(ti);
    que.freeItem(ins, item);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == alloc.getNumAllocatedFragments());

    // Nothing left to peek at.
    ti = que.peek();
    REQUIRE(nullptr == ti);

    // Invalid transfer.
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.remote_node_id = 42;
    meta.transfer_id    = 123;
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==
            que.push(&ins.getInstance(), 1'000'000'005'000ULL, meta, {8, payload.data()}));
    ti = que.peek();
    REQUIRE(nullptr == ti);

    // Error handling.
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT == canardTxPush(nullptr, nullptr, 0, nullptr, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT == canardTxPush(nullptr, nullptr, 0, &meta, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==
            canardTxPush(nullptr, &ins.getInstance(), 0, &meta, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==
            canardTxPush(&que.getInstance(), &ins.getInstance(), 0, nullptr, {0, nullptr}, 0, nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT == que.push(&ins.getInstance(), 1'000'000'006'000ULL, meta, {1, nullptr}));

    REQUIRE(nullptr == canardTxPeek(nullptr));
    REQUIRE(nullptr == canardTxPop(nullptr, nullptr));             // No effect.
    REQUIRE(nullptr == canardTxPop(&que.getInstance(), nullptr));  // No effect.
}

TEST_CASE("TxPayloadOwnership")
{
    helpers::Instance ins;
    helpers::TxQueue  que{3, CANARD_MTU_CAN_FD};  // Limit capacity at 3 frames.

    auto& tx_alloc  = que.getAllocator();
    auto& ins_alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(CANARD_NODE_ID_UNSET == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_FD == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());

    CanardTransferMetadata meta{};

    // 1. Push single-frame with padding, peek, take ownership of the payload, pop and free.
    {
        meta.priority       = CanardPriorityNominal;
        meta.transfer_kind  = CanardTransferKindMessage;
        meta.port_id        = 321;
        meta.remote_node_id = CANARD_NODE_ID_UNSET;
        meta.transfer_id    = 21;
        REQUIRE(1 == que.push(&ins.getInstance(), 1'000'000'000'000ULL, meta, {8, payload.data()}));
        REQUIRE(1 == que.getSize());
        REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
        REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

        // Peek and check the payload.
        CanardTxQueueItem* ti = que.peek();
        REQUIRE(nullptr != ti);  // Make sure we get the same frame again.
        REQUIRE(ti->frame.payload.size == 12);
        REQUIRE(ti->frame.payload.allocated_size == 12);
        REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 8));
        REQUIRE(ti->tx_deadline_usec == 1'000'000'000'000ULL);
        REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
        REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

        // Transfer ownership of the payload (by freeing it and nullifying the pointer).
        tx_alloc.deallocate(ti->frame.payload.data, ti->frame.payload.allocated_size);
        ti->frame.payload.data           = nullptr;
        ti->frame.payload.allocated_size = 0U;
        REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
        REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

        // Pop the item.
        ti = que.pop(ti);
        REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
        REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

        // Free TX item
        que.freeItem(ins, ti);
        REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
        REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(0 == ins_alloc.getTotalAllocatedAmount());
    }

    // 2. Push two-frames, peek, do NOT take ownership of the payload, pop and free.
    {
        que.setMTU(8);
        ins.setNodeID(42);
        meta.transfer_id = 22;
        REQUIRE(2 == que.push(&ins.getInstance(), 2'000'000'000'000ULL, meta, {8, payload.data()}));
        REQUIRE(2 == que.getSize());
        REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
        REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

        // a) Peek and check the payload of the 1st frame
        {
            CanardTxQueueItem* ti = que.peek();
            REQUIRE(nullptr != ti);
            REQUIRE(ti->frame.payload.size == 8);
            REQUIRE(ti->frame.payload.allocated_size == 8);
            REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 7));
            REQUIRE(ti->tx_deadline_usec == 2'000'000'000'000ULL);
            REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
            REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

            // Pop the item.
            ti = que.pop(ti);
            REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
            REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

            // Free TX item
            que.freeItem(ins, ti);
            REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
            REQUIRE(4 == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());
        }
        // b) Peek and check the payload of the 2nd frame
        {
            CanardTxQueueItem* ti = que.peek();
            REQUIRE(nullptr != ti);
            REQUIRE(ti->frame.payload.size == 4);
            REQUIRE(ti->frame.payload.allocated_size == 4);
            REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 7, 1));
            REQUIRE(ti->tx_deadline_usec == 2'000'000'000'000ULL);
            REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
            REQUIRE(4 == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

            // Pop the item.
            ti = que.pop(ti);
            REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
            REQUIRE(4 == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

            // Free TX item
            que.freeItem(ins, ti);
            REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
            REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 0 == ins_alloc.getTotalAllocatedAmount());
        }
    }
}

TEST_CASE("TxPushFlushExpired")
{
    helpers::Instance ins;
    helpers::TxQueue  que{2, CANARD_MTU_CAN_FD};  // Limit capacity at 2 frames.

    auto& tx_alloc  = que.getAllocator();
    auto& ins_alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(CANARD_NODE_ID_UNSET == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_FD == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());

    CanardMicrosecond       now      = 10'000'000ULL;  // 10s
    const CanardMicrosecond deadline = 1'000'000ULL;   // 1s

    CanardTransferMetadata meta{};

    // 1. Push single-frame with padding, peek. @ 10s
    {
        meta.priority       = CanardPriorityNominal;
        meta.transfer_kind  = CanardTransferKindMessage;
        meta.port_id        = 321;
        meta.remote_node_id = CANARD_NODE_ID_UNSET;
        meta.transfer_id    = 21;
        REQUIRE(1 == que.push(&ins.getInstance(), now + deadline, meta, {8, payload.data()}, now));
        REQUIRE(1 == que.getSize());
        REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
        REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

        // Peek and check the payload.
        CanardTxQueueItem* ti = que.peek();
        REQUIRE(nullptr != ti);  // Make sure we get the same frame again.
        REQUIRE(ti->frame.payload.size == 12);
        REQUIRE(ti->frame.payload.allocated_size == 12);
        REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 8));
        REQUIRE(ti->tx_deadline_usec == now + deadline);
        REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
        REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

        // Don't pop and free the item - we gonna flush it by the next push at 12s.
    }

    now += 2 * deadline;  // 10s -> 12s

    // 2. Push two-frames, peek. @ 12s (after 2x deadline)
    //    These 2 frames should still fit into the queue (with capacity 2) despite one expired frame still there.`
    {
        std::uint64_t frames_expired = 0;
        que.setMTU(8);
        ins.setNodeID(42);
        meta.transfer_id = 22;
        REQUIRE(2 == que.push(&ins.getInstance(), now + deadline, meta, {8, payload.data()}, now, frames_expired));
        REQUIRE(2 == que.getSize());
        REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
        REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());
        REQUIRE(1 == frames_expired);

        // a) Peek and check the payload of the 1st frame
        CanardTxQueueItem* ti = nullptr;
        {
            ti = que.peek();
            REQUIRE(nullptr != ti);
            REQUIRE(ti->frame.payload.size == 8);
            REQUIRE(ti->frame.payload.allocated_size == 8);
            REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data(), 7));
            REQUIRE(ti->tx_deadline_usec == now + deadline);
            REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
            REQUIRE((8 + 4) == tx_alloc.getTotalAllocatedAmount());
            REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
            REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

            // Don't pop and free the item - we gonna flush it by the next push @ 14s.
        }
        // b) Check the payload of the 2nd frame
        {
            ti = ti->next_in_transfer;
            REQUIRE(nullptr != ti);
            REQUIRE(ti->frame.payload.size == 4);
            REQUIRE(ti->frame.payload.allocated_size == 4);
            REQUIRE(0 == std::memcmp(ti->frame.payload.data, payload.data() + 7, 1));
            REQUIRE(ti->tx_deadline_usec == now + deadline);

            // Don't pop and free the item - we gonna flush it by the next push @ 14s.
        }
    }

    now += 2 * deadline;  // 12s -> 14s

    // 3. Push three-frames, peek. @ 14s (after another 2x deadline)
    //    These 3 frames should not fit into the queue (with capacity 2),
    //    but as a side effect, the expired frames (from push @ 12s) should be flushed as well.
    {
        std::uint64_t frames_expired = 0;
        meta.transfer_id             = 23;
        REQUIRE(-CANARD_ERROR_OUT_OF_MEMORY ==
                que.push(&ins.getInstance(), now + deadline, meta, {8ULL * 2ULL, payload.data()}, now, frames_expired));
        REQUIRE(0 == que.getSize());
        REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
        REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
        REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
        REQUIRE(0 == ins_alloc.getTotalAllocatedAmount());
        REQUIRE(2 == frames_expired);

        REQUIRE(nullptr == que.peek());
    }
}

TEST_CASE("TxPollSingleFrame")
{
    helpers::Instance ins;
    helpers::TxQueue  que{2, CANARD_MTU_CAN_FD};  // Limit capacity at 2 frames.

    que.setMTU(8);
    ins.setNodeID(42);

    auto& tx_alloc  = que.getAllocator();
    auto& ins_alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(42 == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_CLASSIC == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());

    CanardMicrosecond           now      = 10'000'000ULL;  // 10s
    constexpr CanardMicrosecond deadline = 1'000'000ULL;   // 1s

    CanardTransferMetadata meta{};

    // 1. Push single frame @ 10s
    //
    meta.priority       = CanardPriorityNominal;
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.port_id        = 321;
    meta.remote_node_id = CANARD_NODE_ID_UNSET;
    meta.transfer_id    = 21;
    REQUIRE(1 == que.push(&ins.getInstance(), now + deadline, meta, {7, payload.data()}, now));
    REQUIRE(1 == que.getSize());
    REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

    // 2. Poll with invalid arguments.
    //
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==  // null queue
            canardTxPoll(
                nullptr,
                &ins.getInstance(),
                0,
                nullptr,
                [](auto*, auto, auto*) -> std::int8_t { return 0; },
                nullptr,
                nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==  // null instance
            canardTxPoll(
                &que.getInstance(),
                nullptr,
                0,
                nullptr,
                [](auto*, auto, auto*) -> std::int8_t { return 0; },
                nullptr,
                nullptr));
    REQUIRE(-CANARD_ERROR_INVALID_ARGUMENT ==  // null handler
            canardTxPoll(&que.getInstance(), &ins.getInstance(), 0, nullptr, nullptr, nullptr, nullptr));

    // 3. Poll; emulate media is busy @ 10s + 100us
    //
    helpers::TxQueue::PollStats poll_stats;
    std::size_t                 total_handler_calls = 0;
    REQUIRE(0 == que.poll(
                     ins,
                     now + 100,
                     [&](auto deadline_usec, auto& frame) -> std::int8_t {
                         //
                         ++total_handler_calls;
                         REQUIRE(deadline_usec == now + deadline);
                         REQUIRE(frame.payload.size == 8);
                         REQUIRE(frame.payload.allocated_size == 8);
                         REQUIRE(0 == std::memcmp(frame.payload.data, payload.data(), 7));
                         return 0;  // Emulate that TX media is busy.
                     },
                     poll_stats));
    REQUIRE(1 == total_handler_calls);
    REQUIRE(1 == que.getSize());
    REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);

    // 4. Poll; emulate media is ready @ 10s + 200us
    //
    REQUIRE(1 == que.poll(
                     ins,
                     now + 200,
                     [&](auto deadline_usec, auto& frame) -> std::int8_t {
                         //
                         ++total_handler_calls;
                         REQUIRE(deadline_usec == now + deadline);
                         REQUIRE(frame.payload.size == 8);
                         REQUIRE(frame.payload.allocated_size == 8);
                         REQUIRE(0 == std::memcmp(frame.payload.data, payload.data(), 7));
                         return 1;  // Emulate that TX media accepted the frame.
                     },
                     poll_stats));
    REQUIRE(2 == total_handler_calls);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 0 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);

    // 3. Poll when queue is empty @ 10s + 300us
    //
    REQUIRE(0 == que.poll(
                     ins,
                     now + 300,
                     [&](auto, auto&) -> std::int8_t {
                         //
                         ++total_handler_calls;
                         FAIL("This should not be called.");
                         return -1;
                     },
                     poll_stats));
    REQUIRE(2 == total_handler_calls);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);
}

TEST_CASE("TxPollMultiFrame")
{
    helpers::Instance ins;
    helpers::TxQueue  que{2, CANARD_MTU_CAN_FD};  // Limit capacity at 2 frames.

    que.setMTU(8);
    ins.setNodeID(42);

    auto& tx_alloc  = que.getAllocator();
    auto& ins_alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(42 == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_CLASSIC == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());

    CanardMicrosecond           now      = 10'000'000ULL;  // 10s
    constexpr CanardMicrosecond deadline = 1'000'000ULL;   // 1s

    CanardTransferMetadata meta{};

    // 1. Push two frames @ 10s
    //
    meta.priority       = CanardPriorityNominal;
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.port_id        = 321;
    meta.remote_node_id = CANARD_NODE_ID_UNSET;
    meta.transfer_id    = 21;
    REQUIRE(2 == que.push(&ins.getInstance(), now + deadline, meta, {8, payload.data()}, now));
    REQUIRE(2 == que.getSize());
    REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 + 4 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

    // 2. Poll 1st frame @ 10s + 100us
    //
    helpers::TxQueue::PollStats poll_stats;
    std::size_t                 total_handler_calls = 0;
    REQUIRE(1 == que.poll(
                     ins,
                     now + 100,
                     [&](auto deadline_usec, auto& frame) -> std::int8_t {
                         //
                         ++total_handler_calls;
                         REQUIRE(deadline_usec == now + deadline);
                         REQUIRE(frame.payload.size == 8);
                         REQUIRE(frame.payload.allocated_size == 8);
                         REQUIRE(0 == std::memcmp(frame.payload.data, payload.data(), 7));
                         return 1;
                     },
                     poll_stats));
    REQUIRE(1 == total_handler_calls);
    REQUIRE(1 == que.getSize());
    REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(4 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);

    // 3. Poll 2nd frame @ 10s + 200us
    //
    REQUIRE(1 == que.poll(ins, now + 200, [&](auto deadline_usec, auto& frame) -> std::int8_t {
        //
        ++total_handler_calls;
        REQUIRE(deadline_usec == now + deadline);
        REQUIRE(frame.payload.size == 4);
        REQUIRE(frame.payload.allocated_size == 4);
        REQUIRE(0 == std::memcmp(frame.payload.data, payload.data() + 7, 1));
        return 1;
    }));
    REQUIRE(2 == total_handler_calls);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 0 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);
}

TEST_CASE("TxPollDropFrameOnFailure")
{
    helpers::Instance ins;
    helpers::TxQueue  que{2, CANARD_MTU_CAN_FD};  // Limit capacity at 2 frames.

    que.setMTU(8);
    ins.setNodeID(42);

    auto& tx_alloc  = que.getAllocator();
    auto& ins_alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(42 == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_CLASSIC == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());

    constexpr CanardMicrosecond now      = 10'000'000ULL;  // 10s
    constexpr CanardMicrosecond deadline = 1'000'000ULL;   // 1s

    CanardTransferMetadata meta{};

    // 1. Push two frames @ 10s
    //
    meta.priority       = CanardPriorityNominal;
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.port_id        = 321;
    meta.remote_node_id = CANARD_NODE_ID_UNSET;
    meta.transfer_id    = 21;
    REQUIRE(2 == que.push(&ins.getInstance(), now + deadline, meta, {8, payload.data()}, now));
    REQUIRE(2 == que.getSize());
    REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 + 4 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

    // 2. Poll 1st frame; emulate media failure @ 10s + 100us
    //
    helpers::TxQueue::PollStats poll_stats;
    std::size_t                 total_handler_calls = 0;
    REQUIRE(-1 == que.poll(
                      ins,
                      now + 100,
                      [&](auto deadline_usec, auto& frame) -> std::int8_t {
                          //
                          ++total_handler_calls;
                          REQUIRE(deadline_usec == now + deadline);
                          REQUIRE(frame.payload.size == 8);
                          REQUIRE(frame.payload.allocated_size == 8);
                          REQUIRE(0 == std::memcmp(frame.payload.data, payload.data(), 7));
                          return -1;
                      },
                      poll_stats));
    REQUIRE(1 == total_handler_calls);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 0 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(2 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);
}

TEST_CASE("TxPollDropExpired")
{
    helpers::Instance ins;
    helpers::TxQueue  que{2, CANARD_MTU_CAN_FD};  // Limit capacity at 2 frames.

    que.setMTU(8);
    ins.setNodeID(42);

    auto& tx_alloc  = que.getAllocator();
    auto& ins_alloc = ins.getAllocator();

    std::array<std::uint8_t, 1024> payload{};
    std::iota(payload.begin(), payload.end(), 0U);

    REQUIRE(42 == ins.getNodeID());
    REQUIRE(CANARD_MTU_CAN_CLASSIC == que.getMTU());
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());

    CanardMicrosecond           now      = 10'000'000ULL;  // 10s
    constexpr CanardMicrosecond deadline = 1'000'000ULL;   // 1s

    CanardTransferMetadata meta{};

    // 1. Push nominal priority frame @ 10s
    //
    meta.priority       = CanardPriorityNominal;
    meta.transfer_kind  = CanardTransferKindMessage;
    meta.port_id        = 321;
    meta.remote_node_id = CANARD_NODE_ID_UNSET;
    meta.transfer_id    = 21;
    REQUIRE(1 == que.push(&ins.getInstance(), now + deadline, meta, {7, payload.data()}, now));
    REQUIRE(1 == que.getSize());
    REQUIRE(1 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(1 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 1 == ins_alloc.getTotalAllocatedAmount());

    // 2. Push high priority frame @ 10s + 1'000us
    //
    meta.priority      = CanardPriorityHigh;
    meta.transfer_kind = CanardTransferKindMessage;
    meta.port_id       = 321;
    meta.transfer_id   = 22;
    REQUIRE(1 == que.push(&ins.getInstance(), now + deadline - 1, meta, {7, payload.data() + 100}, now + 1'000));
    REQUIRE(2 == que.getSize());
    REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 + 8 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());

    // 3. Poll a frame (should be the high priority one); emulate media is busy @ 10s + 2'000us
    //
    helpers::TxQueue::PollStats poll_stats;
    std::size_t                 total_handler_calls = 0;
    REQUIRE(0 == que.poll(
                     ins,
                     now + 2'000,
                     [&](auto deadline_usec, auto& frame) -> std::int8_t {
                         //
                         ++total_handler_calls;
                         REQUIRE(deadline_usec == now + deadline - 1);
                         REQUIRE(frame.payload.size == 8);
                         REQUIRE(frame.payload.allocated_size == 8);
                         REQUIRE(0 == std::memcmp(frame.payload.data, payload.data() + 100, 7));
                         return 0;
                     },
                     poll_stats));
    REQUIRE(1 == total_handler_calls);
    REQUIRE(2 == que.getSize());
    REQUIRE(2 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(8 + 8 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(2 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 2 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(0 == poll_stats.frames_expired);

    // 3. Poll a frame (should be nominal priority one b/c the high has been expired) @ 10s + deadline
    //
    REQUIRE(1 == que.poll(
                     ins,
                     now + deadline,
                     [&](auto deadline_usec, auto& frame) -> std::int8_t {
                         //
                         ++total_handler_calls;
                         REQUIRE(deadline_usec == now + deadline);
                         REQUIRE(frame.payload.size == 8);
                         REQUIRE(frame.payload.allocated_size == 8);
                         REQUIRE(0 == std::memcmp(frame.payload.data, payload.data(), 7));
                         return 1;
                     },
                     poll_stats));
    REQUIRE(2 == total_handler_calls);
    REQUIRE(0 == que.getSize());
    REQUIRE(0 == tx_alloc.getNumAllocatedFragments());
    REQUIRE(0 == tx_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == ins_alloc.getNumAllocatedFragments());
    REQUIRE(sizeof(CanardTxQueueItem) * 0 == ins_alloc.getTotalAllocatedAmount());
    REQUIRE(0 == poll_stats.frames_failed);
    REQUIRE(1 == poll_stats.frames_expired);
}
