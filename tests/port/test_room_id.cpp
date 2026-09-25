// Host unit test for the GlobalWork::room_id / room_id_prev TARGET_PC endian fix (docs/port-boot.md,
// "room_id / stage_no / room_no endianness"). Checks that GlobalWork's room_id union -- and
// CRoomInfo::roomNo, room_jmp.h -- both agree with the GameCube's `stage << 8 | room` value on this
// little-endian host, without needing every one of the ~150 pG->room_id / G_ROOM_ID call sites
// individually verified.
#include "global.h"
#include "room_jmp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            std::exit(1);                                                                          \
        }                                                                                           \
    } while (0)

GlobalWork Global;
GlobalWork* pG = &Global;

int main()
{
    std::memset(&Global, 0, sizeof(Global));

    // AREA JUMP's own case (room_jmp.cpp, G_ROOM_ID = 0x120 for stage 1 room 0x20): writing the two
    // byte fields must make the plain u16 view read back stage << 8 | room.
    pG->stage_no = 1;
    pG->room_no = 0x20;
    CHECK(pG->room_id == 0x120);
    CHECK(G_ROOM_ID == 0x120);

    // The other direction: writing the combined u16 view (as title.cpp's `G_ROOM_ID = ...->roomNo`
    // and room_jmp.cpp's `pG->room_id_prev = pG->room_id` do) must split back into the same bytes.
    pG->room_id = 0x333;
    CHECK(pG->stage_no == 3);
    CHECK(pG->room_no == 0x33);

    // room_id_prev / stage_prev / room_prev: the same reorder, independently.
    pG->stage_prev = 2;
    pG->room_prev = 0x09;
    CHECK(pG->room_id_prev == 0x209);
    pG->room_id_prev = 0xFFF;
    CHECK(pG->stage_prev == 0x0F);
    CHECK(pG->room_prev == 0xFF);

    // CRoomInfo::roomNo (room_jmp.h): raw on-disc big-endian bytes (stage byte, then room byte, same
    // physical order as a real GameCube's memory) must still decode to the same stage<<8|room value
    // through the now-BE<u16> roomNo view, matching the fixed pG->room_id/G_ROOM_ID above (the
    // "double cancellation" this fix removes, docs/port-boot.md).
    unsigned char raw[sizeof(CRoomInfo)];
    std::memset(raw, 0, sizeof(raw));
    raw[2] = 0x01; // stage (disc byte order: flag u16, then stage byte, then room byte)
    raw[3] = 0x20; // room
    CRoomInfo* info = reinterpret_cast<CRoomInfo*>(raw);
    CHECK(info->roomNo == 0x120);
    CHECK(info->stage == 0x01);
    CHECK(info->room == 0x20);

    std::printf("test_room_id: OK\n");
    return 0;
}
