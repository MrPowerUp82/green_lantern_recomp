#include "gpu/packet_stream.h"
#include "test_support.h"

using namespace Gpu;
using namespace TestSupport;

TEST(stream_reads_big_endian_dwords) {
    const GuestMemory mem(GuestBase());
    mem.StoreBe32(kScratchBase + 0x10, 0xAABBCCDD);
    mem.StoreBe32(kScratchBase + 0x14, 0x01020304);

    PacketStream s(mem, kScratchBase + 0x10, 8, 0, 2);
    uint32_t v = 0;
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0xAABBCCDD);
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x01020304);
    CHECK_EQ(s.Remaining(), 0);
    CHECK(!s.Read(&v));
}

TEST(stream_wraps_at_capacity) {
    const GuestMemory mem(GuestBase());
    const GuestAddr base = kScratchBase + 0x100;
    for (uint32_t i = 0; i < 4; i++) mem.StoreBe32(base + i * 4, 0x100 + i);

    PacketStream s(mem, base, 4, 3, 3);  // le o indice 3, depois da a volta: 0, 1
    uint32_t v = 0;
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x103);
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x100);
    CHECK(s.Read(&v));
    CHECK_EQ(v, 0x101);
    CHECK_EQ(s.ReadIndex(), 2);
}

TEST(stream_rejects_null_and_overflowing_addresses) {
    const GuestMemory mem(GuestBase());
    uint32_t v = 0;

    PacketStream nullStream(mem, 0, 4, 0, 4);
    CHECK(!nullStream.Read(&v));

    PacketStream highStream(mem, 0xFFFFFFFC, 8, 1, 4);  // indice 1 -> endereco 0x1'0000'0000
    CHECK(!highStream.Read(&v));
}

TEST(stream_rejects_invalid_capacity_index_and_alignment) {
    const GuestMemory mem(GuestBase());
    uint32_t word = 0;
    PacketStream zeroCapacity(mem, kRingBase, 0, 0, 1);
    CHECK(!zeroCapacity.Read(&word));
    PacketStream badIndex(mem, kRingBase, 4, 4, 1);
    CHECK(!badIndex.Read(&word));
    PacketStream unaligned(mem, kRingBase + 1, 4, 0, 1);
    CHECK(!unaligned.Read(&word));
}
