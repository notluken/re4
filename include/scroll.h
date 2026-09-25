#ifndef SCROLL_H
#define SCROLL_H

#include "types.h"
#include "vec.h"
#include "obj.h"
#ifdef TARGET_PC
#include "port/be.h"
#endif

#ifdef TARGET_PC
// On-disc big-endian f32 triplet (docs/port-phase3.md), same shape as id_sys.h's BeVec: SmdWork is
// read straight off the room archive ("SMD" tag) with no separate byte-swap pass, unlike a runtime
// cObj's own plain Vec fields.
struct SmdVec {
    re4_port::BE<f32> x, y, z;
    operator Vec() const { return Vec{ (f32) x, (f32) y, (f32) z }; }
};
#endif

// Scroll (room model) data file `SMD` (game/scroll.cpp). One SmdWork per placed model.
struct SmdWork {
#ifdef TARGET_PC
    SmdVec pos;    // 0x00
    SmdVec rot;    // 0x0C
    SmdVec scale;  // 0x18
#else
    Vec pos;       // 0x00
    Vec rot;       // 0x0C
    Vec scale;     // 0x18
#endif
    u8 binNo;      // 0x24  bin table index (0xFF: none)
    u8 tplNo;      // 0x25  tpl table index (0xFF: none)
    u8 motNo;      // 0x26  motion table index (0xFF: none)
    u8 id;         // 0x27  scroll object id (0xFF: unused, 0xFE: not registered)
    u8 pad_28[0x44 - 0x28];
    union {
#ifdef TARGET_PC
        re4_port::BE<u32> flags;   // 0x44  bit4: bin/tpl come from the common SMD, bit6: motion too
#else
        u32 flags;   // 0x44  bit4: bin/tpl come from the common SMD, bit6: motion too
#endif
        struct {
            u8 pad_44[3];
            u8 attr;  // 0x47  low byte of flags -> cObj::attr
        } b;
    };
};

class cSmd {
public:
    u8 Version;    // 0x00
    u8 Flag;      // 0x01  bit0: group count table in front of the works
#ifdef TARGET_PC
    re4_port::BE<u16> nModel;     // 0x02
    re4_port::BE<u32> BinTblOfs;    // 0x04  offset table of the bins
    re4_port::BE<u32> TplTblOfs;    // 0x08  offset table of the tpls
    re4_port::BE<u32> MotTblOfs;    // 0x0C  offset table of the motions
#else
    u16 nModel;     // 0x02
    u32 BinTblOfs;    // 0x04  offset table of the bins
    u32 TplTblOfs;    // 0x08  offset table of the tpls
    u32 MotTblOfs;    // 0x0C  offset table of the motions
#endif
    union {
        SmdWork work[1];   // 0x10
        struct {
#ifdef TARGET_PC
            re4_port::BE<u32> nGroup;    // 0x10
            re4_port::BE<u32> num[1];    // 0x14  works per group
#else
            u32 nGroup;    // 0x10
            u32 num[1];    // 0x14  works per group
#endif
        } grp;
    };

    void slide(int offset);
    SmdWork* getWorkPtr(int id);
    void* getBinPtr(int id);
    void* getTplPtr(int id);
    void* getMotPtr(int id);
    int getWorkNum();
};

// Scroll extra data `SMX`: per-id object parameters.
struct SmxWork {
    u8 id;         // 0x00
    u8 type;       // 0x01  -> cModel::type
    u8 type2;      // 0x02  -> cModel::x12F
    u8 CullMode;   // 0x03  -> cModel::CullMode
    u32 SelectMask;  // 0x04  -> cLightInfo::SelectMask
    u32 flags;     // 0x08  SmxSetFlag bits
    u32 color;     // 0x0C  -> cModelInfo::color
    u8 work[0x74]; // 0x10  copied to cObj::work (0x78 bytes including color2)
    u32 color2;    // 0x84
    f32 uvScrollU; // 0x88
    f32 uvScrollV; // 0x8C
};

class cSmx {
public:
    u8 x0;         // 0x00
    u8 nWork;      // 0x01
    u8 pad_2[0x10 - 0x02];
    SmxWork work[1];   // 0x10
};

// nScrWork is not declared here on purpose: the .sbss order of scroll.cpp follows the first
// declarations (pSmd, pSmdComn, pSmx, scrObjTbl, scrTbl, nScrWork).
extern cSmd* pSmd;
extern cSmd* pSmdComn;

int SmdInit(cSmd* pSh, cSmx* pSmxh, cSmd* pShCmn);
void SmdClear(int mode);
void workInit(cObj* pObj);
void SmdSetup(int blockNo);
int setObj(int blkNo);
int SmdSetParam(cObj* pObj, SmdWork* pSw);
void SmxSetFlag(cObj* pObj, u32 flag);
int SmxGetFlag(cObj* pObj);
void smxInit(cObj* obj, u8 id);
void smxInit(cObj* obj, SmxWork* w);
void* SmdGetTplPtr(int idx);
cObj* SmdGetObjPtr(u32 idx);
int SmdGetObjNum();
int SmdGetWorkId(cObj* pObj);
void BlockCreate(int blkNo, cSmd* pBlock);
void BlockDestroy(int blkNo);
SmdWork* SmdGetWorkPtr(int idx);
cObj* SmdGetGroupObjPtr(u32 idx);
cObj* SmdGetGroupObjPtr2(u32 idx);
cObj* SmdGetGroupNext(cObj* pObj00);
void SmdSetTrans(u32 idx, int onoff);
cObj* SetObjSmd(void* bin, void* tpl, Vec* pos, Vec* rot, int lightFlag, int front);

#endif
