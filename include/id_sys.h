#ifndef ID_SYS_H
#define ID_SYS_H

#include "types.h"
#include "vec.h"
#include "hermite.h"
#ifdef TARGET_PC
#include "port/be.h"
#include "port/ptr32.h"

// On-disc vector (id table `pos`/`vtx[4]`/`rot` fields, IdData/IdData2 below): big-endian f32
// triplet, same reasoning as every other BE<T> field in this port (docs/port-phase3.md). Distinct
// from the shared, host-native `Vec` (include/vec.h) that IdUnit's own runtime fields use --
// IdUnit::pos0/ver[]/rot0/rot are computed values, not read straight off disc, so they stay plain
// Vec; only the copy sites in IDSystem::set() (game/id_sys.cpp) convert BeVec -> Vec explicitly.
struct BeVec {
    re4_port::BE<f32> x, y, z;
    operator Vec() const { return Vec{ (f32) x, (f32) y, (f32) z }; }
};
#endif

// Screen id (widget) unit (game/id_sys.cpp), 0x138 bytes on the original target. A runtime pool
// object (IDSystem::gameInit's `MEM_ALLOC(n * sizeof(IdUnit), ...)`), not on-disc data -- but that
// allocation's byte size is exactly what `cCard::getUseMemSize()` (card.cpp) budgets scratch heap
// space for, using the GameCube's own 0x138-byte size. Plain 8-byte host pointers here (pParent,
// path0, path1, curve[4]: 6 fields) would widen IdUnit to 0x158 (confirmed: `sizeof(IdUnit)` under
// TARGET_PC without this fix), overrunning that vendor-sized budget by 0x20 bytes/unit (0x2000
// bytes for the 0x100-unit m_IdSave pool alone) and starving the heap the card screen's own
// allocations then run in -- the real root cause of "IDSystem::gameInit() malloc failed" /
// "cDatTbl::end : memory failed" (both symptoms of the same heap exhaustion, traced live with lldb:
// `alloc[0x15800]:free[0x11d40]`, exactly the widened-pool size against too little free space).
// Every pointer this pool ever holds already lives inside the compressed-handle window (the arena
// MEM_ALLOC draws from, docs/port-phase2.md) -- Ptr32<T> under TARGET_PC restores the 0x138 size
// exactly (no call-site changes: every existing `u->pParent`/`u->path0`/`u->curve[n]` use already
// goes through Ptr32<T>'s implicit conversions the same way the rest of this port's on-disc pointer
// fields do).
struct IdUnit {
    u8 be_flag;        // 0x00  0xFF: free; 0x01: alive, 0x02: just set, 0x04: move, 0x08: visible, 0x10: drawing
    u8 unitNo;       // 0x01  own number (parent lookup key)
    u8 classNo;         // 0x02  id table type (IDSystem::set parameter)
    u8 markNo;           // 0x03  unit id inside the table
    u8 type;         // 0x04  1: group (children follow)
    u8 levelNo;        // 0x05  depth in the parent tree
    u8 parentNo;     // 0x06
    u8 rowNo;           // 0x07
    Mtx mat;         // 0x08  world matrix
    Mtx l_mat;    // 0x38
#ifdef TARGET_PC
    re4_port::Ptr32<IdUnit> pParent;  // 0x68
#else
    IdUnit* pParent;  // 0x68
#endif
    u8 texId;        // 0x6C
    u8 maskId;       // 0x6D
    u8 texNo;           // 0x6E  texture frame (stage: digit)
    u8 maskNo;       // 0x6F  mask texture frame
    u8 tex_ptn_no;       // 0x70
    u8 mask_ptn_no;      // 0x71
    u16 timer[4];    // 0x72  (converted as s16)  path / scale / color / rotation curve times
    u8 vtxType;      // 0x7A  low nibble: anchor (IdCalcVertex)
    u8 loop_flag;         // 0x7B  bit n: timer n loops
    u8 size_flag;    // 0x7C  0x10: scale x only, 0x20: y only
    u8 rot_flag;      // 0x7D
    u8 rev_flag;          // 0x7E  bit n: timer n counts up
    u8 tex_flag;     // 0x7F  0x01: mask texture, 0x02: no texture animation, 0x04: no mask animation
    u8 otType;           // 0x80
    u8 otNo;         // 0x81
    u8 trans_type;    // 0x82  0: common, 1: negative, 2/3: shimmer
    u8 pow;     // 0x83
    u8 blend_type;    // 0x84
    u8 anima_state;          // 0x85  bit n: timer n finished
    u8 pad_86[2];
    Vec pos0;         // 0x88  screen position
    Vec pos;         // 0x94  path offset + scr (world position used for drawing)
    Vec ver[4];      // 0xA0
    f32 size_W;       // 0xD0
    f32 size_H;       // 0xD4
    u8 pad_D8[8];
    u8 col0[4];      // 0xE0
    u8 col1[4];      // 0xE4
    f32 col[4];      // 0xE8
    Vec rot0;         // 0xF8
    Vec rot;         // 0x104
    f32 u0;          // 0x110
    f32 u1;          // 0x114
    f32 v0;          // 0x118
    f32 v1;          // 0x11C
#ifdef TARGET_PC
    // Ptr32<u8>, not Ptr32<void>: Ptr32<T>::operator[] needs a complete, reference-able T to declare
    // (even though never called on these two fields) once the class template gets instantiated --
    // Ptr32<void> fails to compile ("cannot form a reference to 'void'"); Ptr32<u8> has the same
    // opaque-byte-pointer semantics every existing `(u8*) u->path0` call site already assumes.
    re4_port::Ptr32<u8> path0;     // 0x120  FuncPath data
    re4_port::Ptr32<u8> path1;     // 0x124
    re4_port::Ptr32<Hermite1> curve[4];  // 0x128
#else
    void* path0;     // 0x120  FuncPath data
    void* path1;     // 0x124
    Hermite1* curve[4];  // 0x128
#endif
};

// One entry of an id data table (IDSystem::set), version 1 = 0x88 bytes, version 2 = 0x8C bytes.
struct IdData {
    u8 pad_0[3];
    u8 flags;        // 0x03
    u8 id;           // 0x04
    u8 no;           // 0x05
    u8 level;        // 0x06
    u8 parentNo;     // 0x07
    u8 rowNo;        // 0x08  -> IdUnit::rowNo (PS2 ID_DATA_V2 rowNo)
    u8 kind;         // 0x09
    u8 Id;           // 0x0A  (PS2 ID_DATA_V2 Id; the game does not read it)
    u8 texId;        // 0x0B
    u8 vtxType;      // 0x0C
    u8 loop;         // 0x0D
    u8 scaleType;    // 0x0E
    u8 rotAxis;      // 0x0F
    u8 dir;          // 0x10
    u8 pad_11[3];
#ifdef TARGET_PC
    BeVec pos;        // 0x14
    BeVec vtx[4];     // 0x20
    re4_port::BE<f32> sizeX;  // 0x50
    re4_port::BE<f32> sizeY;  // 0x54
#else
    Vec pos;         // 0x14
    Vec vtx[4];      // 0x20
    f32 sizeX;       // 0x50
    f32 sizeY;       // 0x54
#endif
    u8 col0[4];      // 0x58
    // version 1
#ifdef TARGET_PC
    BeVec rot;        // 0x5C
#else
    Vec rot;         // 0x5C
#endif
    u8 blendType;    // 0x68
    u8 transType;    // 0x69
    u8 maskId;       // 0x6A
    u8 flags_7F;     // 0x6B
    u8 transSub;     // 0x6C
    u8 pad_6D[3];
    // Byte offsets from the table start (path0, path1, curve[4]); 0 = none. On-disc, big-endian
    // (Phase 3, docs/port-phase3.md) -- BE<u32> under TARGET_PC (IDSystem::set adds these straight
    // to `data`'s address; unswapped on this little-endian host they are huge garbage offsets that
    // walk FuncPathParametrize/Hermite1 pointers off the end of the loaded buffer).
#ifdef TARGET_PC
    re4_port::BE<u32> ofs[6];      // 0x70
#else
    u32 ofs[6];      // 0x70
#endif
};

struct IdData2 {
    u8 pad_0[3];
    u8 flags;        // 0x03
    u8 id;           // 0x04
    u8 no;           // 0x05
    u8 level;        // 0x06
    u8 parentNo;     // 0x07
    u8 rowNo;        // 0x08  -> IdUnit::rowNo (PS2 ID_DATA_V2 rowNo)
    u8 kind;         // 0x09
    u8 Id;           // 0x0A  (PS2 ID_DATA_V2 Id; the game does not read it)
    u8 texId;        // 0x0B
    u8 vtxType;      // 0x0C
    u8 loop;         // 0x0D
    u8 scaleType;    // 0x0E
    u8 rotAxis;      // 0x0F
    u8 dir;          // 0x10
    u8 pad_11[3];
#ifdef TARGET_PC
    BeVec pos;        // 0x14
    BeVec vtx[4];     // 0x20
    re4_port::BE<f32> sizeX;  // 0x50
    re4_port::BE<f32> sizeY;  // 0x54
#else
    Vec pos;         // 0x14
    Vec vtx[4];      // 0x20
    f32 sizeX;       // 0x50
    f32 sizeY;       // 0x54
#endif
    u8 col0[4];      // 0x58
    u8 col1[4];      // 0x5C
#ifdef TARGET_PC
    BeVec rot;        // 0x60
#else
    Vec rot;         // 0x60
#endif
    u8 blendType;    // 0x6C
    u8 transType;    // 0x6D
    u8 maskId;       // 0x6E
    u8 flags_7F;     // 0x6F
    u8 transSub;     // 0x70
    u8 pad_71[3];
    // Same reasoning as IdData::ofs above -- BE<u32> under TARGET_PC.
#ifdef TARGET_PC
    re4_port::BE<u32> ofs[6];      // 0x74
#else
    u32 ofs[6];      // 0x74
#endif
};

// Id data table header: version string, entry count, entries from 0x08.
struct IdDataHeader {
    char version[5];  // 0x00  "1.00" / "2.00"
    u8 num;           // 0x05
    u8 pad_6[2];
};

// Id class (PS2 ID_CLASS): the `type` / classNo of IDSystem::set/kill/setCk/dispSw/unitPtr and the IdSet*
// helpers; IDC_NUM_00..IDC_NUM_61 are the 62 digit classes, IDC_ANY matches every class.
enum ID_CLASS {
    IDC_SSCRN_MAIN_MENU = 0,
    IDC_SSCRN_BACK_GROUND = 1,
    IDC_SSCRN_PESETA = 2,
    IDC_SSCRN_CONFIRM = 3,
    IDC_SSCRN_ETC = 4,
    IDC_SSCRN_NEAR_0 = 16,
    IDC_SSCRN_NEAR_1 = 17,
    IDC_SSCRN_NEAR_2 = 18,
    IDC_SSCRN_NEAR_3 = 19,
    IDC_SSCRN_0 = 20,
    IDC_SSCRN_1 = 21,
    IDC_SSCRN_2 = 22,
    IDC_SSCRN_3 = 23,
    IDC_SSCRN_FAR_0 = 24,
    IDC_SSCRN_FAR_1 = 25,
    IDC_SSCRN_FAR_2 = 26,
    IDC_SSCRN_FAR_3 = 27,
    IDC_SSCRN_CKPT_0 = 28,
    IDC_SSCRN_CKPT_1 = 29,
    IDC_SSCRN_CKPT_2 = 30,
    IDC_SSCRN_CKPT_3 = 31,
    IDC_ACT_BUTTON = 32,
    IDC_LIFE_METER = 33,
    IDC_GAUGE = 34,
    IDC_COUNT_DOWN = 35,
    IDC_BINOCULAR = 36,
    IDC_SCOPE = 37,
    IDC_EXAMINE = 38,
    IDC_DATA = 39,
    IDC_TITLE = 40,
    IDC_TITLE_MENU = 41,
    IDC_OPTION = 42,
    IDC_OPTION_BG = 43,
    IDC_EVENT = 44,
    IDC_DEAD = 45,
    IDC_CONTINUE = 46,
    IDC_MSG_WINDOW = 47,
    IDC_CINESCO = 48,
    IDC_WIP = 49,
    IDC_BLLT_ICON = 50,
    IDC_SUB_MISSION = 51,
    IDC_LASER_GAUGE = 52,
    IDC_BATTERY_TARGET = 53,
    IDC_NUM_00 = 64,
    IDC_NUM_61 = 125,
    IDC_PRICE_00 = 128,
    IDC_PRICE_01 = 129,
    IDC_PRICE_02 = 130,
    IDC_PRICE_03 = 131,
    IDC_PRICE_04 = 132,
    IDC_TOOL = 254,
    IDC_ANY = 255
};

class IDSystem {
public:
    s32 m_maxId;          // 0x00
    s32 m_nId;       // 0x04
    s32 m_levelMax;     // 0x08
    u32 m_set_flag[8];        // 0x0C  table types set
    u32 m_disp_off[8];      // 0x2C  table types hidden
    IdUnit* m_IdUnit;    // 0x4C

    static Mtx m_scrn_mat;

    void gameInit(int n);
    void roomInit();
    void free();
    int setCk(int classNo);
    void dispSw(int classNo, int sw);
    void unitPush(IdUnit* u);
    IdUnit* unitPull();
    void unitLevel(IdUnit* u, u8 level);
    void unitParent(IdUnit* parent, IdUnit* child);
    IdUnit* unitPtr(u8 id, int type);
    void set(void* data, u8 id, int type, u8 ot, u8 prio, u8 mode);
    void kill(u8 id, int type);
    void stop();
    void move();
    void beMove(IdUnit* u, int on_off);
    void setTime(IdUnit* u, s16 time);
    void movePos(IdUnit* u);
    void trans();
    void unitTrans(IdUnit* u);
};

extern IDSystem IdSys;
extern void* g_pIdBuff;
extern int IdBuffType;

// game/id_tex.cpp
struct TexWk;
struct TexAnm;
void IdTexSet(u8 id, u8 no);
int IdGetAnmAddr(u8 id, TexAnm** ppAnm);
void IdChannelSet(IdUnit* pIdUnit);
TexWk* IdGetTexWk(u8 id, int bNoDispErrMsg);

extern "C" {
void idSysMove00(IdUnit* u);
void IdCalcVertex(IdUnit* u);
void idSysMove01(IdUnit* u);
void idSysMove02(IdUnit* u);
void idSysMove03(IdUnit* u);
void idSysMove04(IdUnit* u);
void IdGeneralTrans(IdUnit* u);
void IdCommonTrans(IdUnit* u);
void IdNegativeTrans(IdUnit* u, u32 pow);
void IdShimmerTrans(IdUnit* u, int u_pow, int Refract_type);
void IdAllocBuffer();
void IdFreeBuffer();
void IdDebugAllocBuffer();
void IdDebugFreeBuffer();
void* IdGetBufferAddr(int type);
void IdSetBufferType(int type);
void IdTexGameInit();
void IdTexRoomInit();
enum TEX_OWNER {
    TEX_OWNER_NONE = 0,
    TEX_OWNER_CORE = 1,
    TEX_OWNER_ROOM = 2,
    TEX_OWNER_ID_TOOL = 3,
    TEX_OWNER_ID_COCKPIT = 4,
    TEX_OWNER_ID_CINESCO = 5,
    TEX_OWNER_ID_EVENT = 6,
    TEX_OWNER_ID_TITLE = 7,
    TEX_OWNER_ID_SHARE = 8,
    TEX_OWNER_ID_SSCRN = 9,
    TEX_OWNER_ID_DEAD = 10,
    TEX_OWNER_ID_SCOPE = 11,
    TEX_OWNER_MAX = 12
};

void IdTexRelease(int owner);
int IdTexDataLoad(void* data, int id);
}

#endif
