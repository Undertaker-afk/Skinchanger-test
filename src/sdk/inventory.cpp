#include "inventory.hpp"
#include "offsets.hpp"
#include "../core/memory.hpp"

namespace cs2 {

static CCSInventoryManager* g_inventory_manager = nullptr;

CCSInventoryManager* CCSInventoryManager::GetInstance() {
    if (g_inventory_manager) return g_inventory_manager;

    HMODULE hClient = PatternScanner::GetModuleHandle("client.dll");
    if (!hClient) return nullptr;

    // Pattern: 48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 91
    uintptr_t addr = PatternScanner::FindPattern(hClient,
        "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 91");
    if (!addr) return nullptr;

    g_inventory_manager = reinterpret_cast<CCSInventoryManager*>(
        PatternScanner::ResolveRelativeAddress(addr, 3, 7));
    return g_inventory_manager;
}

static CCSPlayerInventory* g_player_inventory = nullptr;

CCSPlayerInventory* CCSPlayerInventory::GetInstance() {
    if (g_player_inventory) return g_player_inventory;

    CCSInventoryManager* mgr = CCSInventoryManager::GetInstance();
    if (!mgr) return nullptr;

    g_player_inventory = mgr->GetLocalInventory();
    return g_player_inventory;
}

bool CCSPlayerInventory::AddEconItem(CEconItem_t* pItem) {
    if (!pItem) return false;

    // TODO: RE REQUIRED - Full implementation needs the CEconItem creation pattern
    // Pattern: 48 83 EC 28 B9 48 00 00 00 E8 ? ? ? ? 48 85
    // Then call inventory's internal add function
    // For now, this is a stub that should be hooked from item creation
    return false;
}

void CCSPlayerInventory::RemoveEconItem(CEconItem_t* pItem) {
    if (!pItem) return;

    // TODO: RE REQUIRED - Item removal via SODestroyed or inventory removal function
    // Call SODestroyed with the item's SO data
    SOID_t owner = GetOwner();
    SODestroyed(owner, reinterpret_cast<CSharedObject*>(pItem), eSOCacheEvent_Incremental);
}

C_EconItemView* CCSPlayerInventory::GetItemViewForItem(uint64_t itemID) {
    // Iterate the item vector at offset 0x20
    struct ItemEntry {
        uint64_t item_id;
        C_EconItemView* view;
    };

    // The item vector structure varies; this is a simplified lookup
    // TODO: RE REQUIRED - Proper iteration of inventory item list
    // The vector at offset 0x20 contains C_EconItemView* pointers
    // Each view's m_iItemID() should be compared against itemID

    // Fallback: check loadout slots
    for (int cls = 0; cls < 2; ++cls) {
        for (int slot = 0; slot < 56; ++slot) {
            C_EconItemView* item = GetItemInLoadout(cls, slot);
            if (item && item->m_iItemID() == itemID)
                return item;
        }
    }
    return nullptr;
}

CEconItem_t* CCSPlayerInventory::GetSOCDataForItem(uint64_t itemID) {
    // TODO: RE REQUIRED - SOC data lookup
    // The SOC (Shared Object Cache) stores the actual item data
    // This requires traversing the cache or using the type cache
    return nullptr;
}

}
