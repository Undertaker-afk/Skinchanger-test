#include "items.hpp"
#include "../../core/memory.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace cs2 {

std::vector<DumpedItemDef> ItemDatabase::s_items;
std::vector<DumpedPaintKit> ItemDatabase::s_paint_kits;
bool ItemDatabase::s_dump_ready = false;
std::shared_mutex ItemDatabase::s_mutex;

static uintptr_t g_econ_item_schema = 0;

static bool ResolveEconItemSchema() {
    if (g_econ_item_schema) return true;

    HMODULE hClient = PatternScanner::GetModuleHandle(L"client.dll");
    if (!hClient) return false;

    // Pattern: 48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81
    // This is the C_EconItemSystem getter function
    uintptr_t fn = PatternScanner::FindPattern(hClient,
        "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81");
    if (!fn) return false;

    // The function reads a global pointer at RIP+3
    uintptr_t schema_ptr = PatternScanner::ResolveRelativeAddress(fn, 3, 7);
    if (!schema_ptr) return false;

    // C_EconItemSystem + 0x8 = C_EconItemSchema*
    g_econ_item_schema = *reinterpret_cast<uintptr_t*>(schema_ptr + 0x8);
    return g_econ_item_schema != 0;
}

bool ItemDatabase::DumpFromSchema(uintptr_t econ_item_schema) {
    if (!econ_item_schema) return false;

    std::unique_lock lock(s_mutex);
    s_items.clear();
    s_paint_kits.clear();

    // C_EconItemSchema layout (from wh-esports):
    //   +0x128: CUtlMap<int, C_EconItemDefinition*>  m_ItemDefinitions
    //   +0x2F0: CUtlMap<int, C_PaintKit*>            m_PaintKits

    auto& item_map = *reinterpret_cast<CUtlMap<C_EconItemDefinition_t*>*>(econ_item_schema + 0x128);
    auto& paint_map = *reinterpret_cast<CUtlMap<C_PaintKit_t*>*>(econ_item_schema + 0x2F0);

    // --- Dump Item Definitions ---
    int key = -1;
    while ((key = item_map.GetNextKey(key)) != -1) {
        C_EconItemDefinition_t* def = *item_map.FindByKey(key);
        if (!def) continue;

        // Validate pointer
        if (reinterpret_cast<uintptr_t>(def) < 0x10000 || reinterpret_cast<uintptr_t>(def) > 0x7FFFFFFFFFFFULL)
            continue;

        if (!def->m_bEnabled) continue;

        DumpedItemDef entry{};
        entry.def_index = def->m_nDefIndex;

        // Get name via vtable
        if (def->m_pszItemBaseName && def->m_pszItemBaseName[0])
            entry.name = def->m_pszItemBaseName;
        else if (def->GetName() && def->GetName()[0])
            entry.name = def->GetName();
        else
            entry.name = "Unknown_" + std::to_string(entry.def_index);

        entry.base_name = def->m_pszItemBaseName ? def->m_pszItemBaseName : "";
        entry.type_name = def->m_pszItemTypeName ? def->m_pszItemTypeName : "";
        entry.model_path = def->GetModelName() ? def->GetModelName() : "";
        entry.loadout_slot = def->GetLoadoutSlot();
        entry.rarity = def->GetRarity();

        if (def->IsKnife())       entry.type = ItemType::Knife;
        else if (def->IsGlove())  entry.type = ItemType::Glove;
        else if (def->IsAgent())  entry.type = ItemType::Agent;
        else if (def->IsWeapon()) entry.type = ItemType::Weapon;
        else                      entry.type = ItemType::Unknown;

        if (entry.type != ItemType::Unknown)
            s_items.push_back(std::move(entry));
    }

    // --- Dump Paint Kits ---
    key = -1;
    while ((key = paint_map.GetNextKey(key)) != -1) {
        C_PaintKit_t* kit = *paint_map.FindByKey(key);
        if (!kit) continue;

        if (reinterpret_cast<uintptr_t>(kit) < 0x10000 || reinterpret_cast<uintptr_t>(kit) > 0x7FFFFFFFFFFFULL)
            continue;

        if (kit->m_id == 0 || kit->m_id == 9001) continue;
        if (!kit->m_name || kit->m_name[0] == '\0') continue;

        DumpedPaintKit entry{};
        entry.id = kit->m_id;
        entry.name = kit->m_name;
        entry.description = kit->m_description_tag ? kit->m_description_tag : "";
        entry.rarity = kit->m_rarity;
        entry.is_legacy = kit->IsLegacy();

        s_paint_kits.push_back(std::move(entry));
    }

    s_dump_ready = true;
    return true;
}

bool ItemDatabase::DumpFromSchema() {
    if (!ResolveEconItemSchema()) return false;
    return DumpFromSchema(g_econ_item_schema);
}

const std::vector<DumpedItemDef>& ItemDatabase::GetItems() {
    return s_items;
}

const std::vector<DumpedItemDef>& ItemDatabase::GetItems(ItemType type) {
    static std::vector<DumpedItemDef> filtered;
    filtered.clear();
    std::shared_lock lock(s_mutex);
    for (const auto& item : s_items) {
        if (item.type == type) filtered.push_back(item);
    }
    return filtered;
}

std::vector<DumpedItemDef> ItemDatabase::Search(std::string_view query) {
    std::shared_lock lock(s_mutex);
    std::vector<DumpedItemDef> results;
    if (query.empty()) return s_items;

    std::string lower_query;
    lower_query.reserve(query.size());
    for (char c : query) lower_query += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const auto& item : s_items) {
        std::string lower_name = item.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
            [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
        if (lower_name.find(lower_query) != std::string::npos)
            results.push_back(item);
    }
    return results;
}

std::vector<DumpedItemDef> ItemDatabase::Search(std::string_view query, ItemType type) {
    std::shared_lock lock(s_mutex);
    std::vector<DumpedItemDef> results;
    if (query.empty()) {
        for (const auto& item : s_items)
            if (item.type == type) results.push_back(item);
        return results;
    }

    std::string lower_query;
    lower_query.reserve(query.size());
    for (char c : query) lower_query += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const auto& item : s_items) {
        if (item.type != type) continue;
        std::string lower_name = item.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
            [](unsigned char c) -> char { return static_cast<char>(std::tolower(c)); });
        if (lower_name.find(lower_query) != std::string::npos)
            results.push_back(item);
    }
    return results;
}

const DumpedItemDef* ItemDatabase::FindByDefIndex(int def_index) {
    std::shared_lock lock(s_mutex);
    for (const auto& item : s_items)
        if (item.def_index == def_index) return &item;
    return nullptr;
}

const DumpedPaintKit* ItemDatabase::FindPaintKit(int id) {
    std::shared_lock lock(s_mutex);
    for (const auto& kit : s_paint_kits)
        if (kit.id == id) return &kit;
    return nullptr;
}

const std::vector<DumpedPaintKit>& ItemDatabase::GetPaintKits() {
    return s_paint_kits;
}

bool ItemDatabase::IsKnife(int def_index) {
    return def_index >= 500 && def_index <= 527;
}

bool ItemDatabase::IsGlove(int def_index) {
    // Sport gloves: 4722-4729, Moto: 5027-5034, Specialist: 5035
    // Also: 4725-4729, 5027-5035
    return (def_index >= 4722 && def_index <= 4729) ||
           (def_index >= 5027 && def_index <= 5035);
}

bool ItemDatabase::IsWeapon(int def_index) {
    return !IsKnife(def_index) && !IsGlove(def_index) && def_index > 0 && def_index < 500;
}

ItemType ItemDatabase::CategorizeDefIndex(int def_index) {
    if (IsKnife(def_index)) return ItemType::Knife;
    if (IsGlove(def_index)) return ItemType::Glove;
    if (def_index >= 5000) return ItemType::Agent;
    if (def_index > 0 && def_index < 500) return ItemType::Weapon;
    return ItemType::Unknown;
}

int ItemDatabase::GetLoadoutSlotForDefIndex(int def_index) {
    if (IsKnife(def_index)) return static_cast<int>(LoadoutSlot::Melee);

    // Primary weapons: rifles, SMGs, shotguns, machine guns, snipers
    static const int primary[] = {7,8,9,10,11,13,14,16,17,19,23,24,25,26,27,28,29,33,34,35,38,39,40,60};
    for (int p : primary) if (def_index == p) return static_cast<int>(LoadoutSlot::Rifle0);

    // Secondary weapons: pistols
    static const int secondary[] = {1,2,3,4,30,32,36,61,63,64};
    for (int s : secondary) if (def_index == s) return static_cast<int>(LoadoutSlot::Secondary0);

    if (IsGlove(def_index)) return static_cast<int>(LoadoutSlot::Gloves);
    return -1;
}

}
