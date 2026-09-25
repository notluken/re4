#ifndef ROOM_JMP_H
#define ROOM_JMP_H

#include "types.h"
#ifdef TARGET_PC
#include "port/be.h"
#include "port/ptr32.h"
#endif

// game/room_jmp.cpp: debug "AREA JUMP" tool over the room info table (roomInfoAddr).
//
// Table: u32 stage count, u32 offset per stage (0 = no rooms), each stage block starts with a u32
// room count followed by CRoomInfo[count]. The string offsets are relative to the table until
// cRoomJmp's constructor turns them into pointers.

#ifdef TARGET_PC
// On-disc big-endian f32 triplet (docs/port-phase3.md), same shape as id_sys.h's BeVec but declared
// locally: CRoomInfo::pos is read straight off disc (room_jmp.cpp does no separate byte-swap pass),
// unlike IdUnit's own runtime Vec fields.
struct RoomInfoVec {
    re4_port::BE<f32> x, y, z;
};
#endif

struct CRoomInfo {
    // roomNo now IS made BE<u16> under TARGET_PC (docs/port-boot.md): this table is raw on-disc
    // big-endian bytes read straight through this struct with no separate byte-swap pass (same
    // reasoning as flag/pos/angle below), so a plain host u16 read of roomNo would come out
    // byte-swapped. Previously left un-swapped on purpose because it fed global.h's G_ROOM_ID/
    // pG->room_id, which aliased pG->stage_no/pG->room_no through a raw pointer-cast/union trick
    // that was itself wrong the same way on this host -- the two wrongs cancelled (New Game -> room
    // navigation worked "by luck"). Now that GlobalWork's room_id union is fixed to read correctly on
    // its own (global.h's TARGET_PC reorder), that cancellation is gone, so roomNo must be correct on
    // its own too. The `.stage`/`.room` single-byte reads below still need no swap either way (a
    // one-byte read is the same regardless of host endianness).
#ifdef TARGET_PC
    re4_port::BE<u16> flag;      // 0x00  bit 0: pos/angle valid
#else
    u16 flag;      // 0x00  bit 0: pos/angle valid
#endif
    union {
#ifdef TARGET_PC
        re4_port::BE<u16> roomNo;  // 0x02  stage << 8 | room
#else
        u16 roomNo;  // 0x02  stage << 8 | room
#endif
        struct {
            u8 stage;  // 0x02
            u8 room;   // 0x03
        };
    };
#ifdef TARGET_PC
    RoomInfoVec pos; // 0x04
    re4_port::BE<f32> angle;      // 0x10
#else
    Vec pos;        // 0x04
    f32 angle;      // 0x10
#endif
#ifdef TARGET_PC
    re4_port::Ptr32<char> name;     // 0x14
    re4_port::Ptr32<char> person;      // 0x18
    re4_port::Ptr32<char> person2;     // 0x1C
#else
    char* name;     // 0x14
    char* person;      // 0x18
    char* person2;     // 0x1C
#endif

    void setNextPos();
};

class cRoomJmp {
public:
    u32* tbl;  // 0x00

    cRoomJmp(void* tbl);
    s8 getIndexNum(s8 stage);
    s8 getPointNum(s8 stage, s8 room);
    CRoomInfo* getRoomInfo(u8 st, u8 idx);
    u8 getRoomIdx(u8 st, u8 room);
    void setNextPos(u8 Stage, u8 Room);
    s8 getNextStageNo(s8 stage, int add);
    s8 getNextRoomNo(s8 stage, s8 idx, int add);
    s8 getNextPointNo(s8 stage, s8 room, s8 point, s8 add);
    s8 checkRoomNo(s8 stage, s8 room);
};

// room_jmp.cpp and title.cpp each own a file-scope `cRoomJmp* pRj` of their own.

void RoomJump();
extern "C" void GetNextPos(u8 stage, u8 room);

#endif
