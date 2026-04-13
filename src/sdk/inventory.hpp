#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstddef>

namespace cs2 {

template<typename T>
inline T CallVFunc(void* thisptr, size_t index, auto&&... args) {
    using Fn = T(__thiscall*)(void*, decltype(args)...);
    return (*reinterpret_cast<Fn**>(thisptr))[index](thisptr, args...);
}

struct SOID_t {
    uint64_t id;
    uint32_t type;
    uint32_t pad;
};

struct CSharedObject {
    uint8_t pad[0x20];
};

enum ESOCacheEvent : int {
    eSOCacheEvent_Incremental = 0,
    eSOCacheEvent_FullUpdate = 1
};

class C_EconItemView {
public:
    uint16_t& m_iItemDefinitionIndex() { return *reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(this) + 0x1BA); }
    int32_t& m_iEntityQuality() { return *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1BC); }
    uint64_t& m_iItemID() { return *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(this) + 0x1C8); }
    uint32_t& m_iItemIDHigh() { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1D0); }
    uint32_t& m_iItemIDLow() { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1D4); }
    uint32_t& m_iAccountID() { return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(this) + 0x1D8); }
    bool& m_bInitialized() { return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + 0x1E8); }
    bool& m_bDisallowSOC() { return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + 0x1E9); }
    bool& m_bIsStoreItem() { return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + 0x1EA); }
    bool& m_bIsTradeItem() { return *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(this) + 0x1EB); }
    char* m_szCustomName() { return reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(this) + 0x2F8); }
};

class C_AttributeContainer {
public:
    C_EconItemView& m_Item() { return *reinterpret_cast<C_EconItemView*>(reinterpret_cast<uintptr_t>(this) + 0x50); }
};

struct CEconItem_t {
    void*    vtable[2];
    uint64_t m_ulID;
    uint64_t m_ulOriginalID;
    void*    m_pCustomDataOptimized;
    uint32_t m_unAccountID;
    uint32_t m_unInventory;
    uint16_t m_unDefIndex;
    uint16_t m_unFlags;
    int16_t  m_iItemSet;
    int32_t  m_bSOUpdateFrame;
    uint8_t  m_unExtraFlags;
};

class CCSPlayerInventory {
public:
    static CCSPlayerInventory* GetInstance();

    SOID_t& GetOwner() { return *reinterpret_cast<SOID_t*>(reinterpret_cast<uintptr_t>(this) + 0x10); }

    C_EconItemView* GetItemInLoadout(int iClass, int iSlot) {
        return CallVFunc<C_EconItemView*, 8>(this, iClass, iSlot);
    }

    void SOCreated(SOID_t owner, CSharedObject* pObject, ESOCacheEvent eEvent) {
        CallVFunc<void, 0>(this, owner, pObject, eEvent);
    }

    void SOUpdated(SOID_t owner, CSharedObject* pObject, ESOCacheEvent eEvent) {
        CallVFunc<void, 1>(this, owner, pObject, eEvent);
    }

    void SODestroyed(SOID_t owner, CSharedObject* pObject, ESOCacheEvent eEvent) {
        CallVFunc<void, 2>(this, owner, pObject, eEvent);
    }

    bool AddEconItem(CEconItem_t* pItem);
    void RemoveEconItem(CEconItem_t* pItem);
    C_EconItemView* GetItemViewForItem(uint64_t itemID);
    CEconItem_t* GetSOCDataForItem(uint64_t itemID);
};

class CCSInventoryManager {
public:
    static CCSInventoryManager* GetInstance();

    void EquipItemInLoadout(int iTeam, int iSlot, uint64_t iItemID) {
        CallVFunc<void, 54>(this, iTeam, iSlot, iItemID);
    }

    CCSPlayerInventory* GetLocalInventory() {
        return CallVFunc<CCSPlayerInventory*, 57>(this);
    }
};

}
