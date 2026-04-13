#include "skinchanger.hpp"
#include "../../sdk/offsets.hpp"
#include "../../core/memory.hpp"
#include "../../core/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace cs2 {

bool SkinChanger::s_initialized = false;
std::string SkinChanger::s_status_message = "Not initialized";
std::shared_mutex SkinChanger::s_mutex;
std::unordered_map<int, SkinConfig> SkinChanger::s_weapon_skins;
std::vector<uint64_t> SkinChanger::s_injected_item_ids;
uint64_t SkinChanger::s_next_fake_item_id = 0xF000000000000001ULL;

KnifeConfig SkinChanger::s_knife;
GloveConfig SkinChanger::s_gloves;
AgentConfig SkinChanger::s_agent_t;
AgentConfig SkinChanger::s_agent_ct;
MusicKitConfig SkinChanger::s_music_kit;

int SkinChanger::s_glove_frame = 0;
SkinChanger::GloveInfo SkinChanger::s_added_gloves;

uintptr_t SkinChanger::s_client_base = 0;
uintptr_t SkinChanger::s_fn_create_econ_item = 0;
uintptr_t SkinChanger::s_fn_set_dynamic_attr = 0;
uintptr_t SkinChanger::s_fn_get_econ_item_system = 0;
uintptr_t SkinChanger::s_fn_inventory_manager = 0;
uintptr_t SkinChanger::s_fn_update_subclass = 0;
uintptr_t SkinChanger::s_fn_set_mesh_group_mask = 0;
uintptr_t SkinChanger::s_fn_set_model = 0;
uintptr_t SkinChanger::s_force_viewmodel_fn = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

bool SkinChanger::Initialize() {
    if (s_initialized) return true;

    s_status_message = "Initializing...";
    SC_LOG_INFO("SkinChanger: Initializing...");

    PatternScanner::GetModuleHandle(L"client.dll");
    s_client_base = PatternScanner::GetModuleBase("client.dll");
    if (!s_client_base) {
        s_status_message = "Failed: client.dll not found";
        SC_LOG_ERROR("SkinChanger: client.dll not found");
        return false;
    }
    SC_LOG_INFO("SkinChanger: client.dll base = 0x" + std::to_string(s_client_base));

    HMODULE hClient = PatternScanner::GetModuleHandle(L"client.dll");

    // Scan all required patterns
    #define SCAN_PATTERN(name, sig) \
        name = PatternScanner::FindPattern(hClient, sig); \
        SC_LOG_PATTERN("client.dll", sig, name != 0);

    SCAN_PATTERN(s_fn_create_econ_item,
        "48 83 EC 28 B9 48 00 00 00 E8 ? ? ? ? 48 85");
    SCAN_PATTERN(s_fn_set_dynamic_attr,
        "48 89 6C 24 ? 57 41 56 41 57 48 81 EC ? ? ? ? 48 8B FA C7 44 24 ? ? ? ? ? 4D 8B F8");
    SCAN_PATTERN(s_fn_get_econ_item_system,
        "48 83 EC 28 48 8B 05 ? ? ? ? 48 85 C0 0F 85 81");
    SCAN_PATTERN(s_fn_inventory_manager,
        "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 8B 91 ? ? ? ? B8");
    SCAN_PATTERN(s_fn_update_subclass,
        "40 53 48 83 EC 30 48 8B 41 10 48 8B D9 8B 50 30");
    SCAN_PATTERN(s_fn_set_mesh_group_mask,
        "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8D 99 ? ? ? ? 48 8B 71");
    SCAN_PATTERN(s_fn_set_model,
        "40 53 48 83 EC ? 48 8B D9 4C 8B C2 48 8B 0D ? ? ? ? 48 8D 54 24");

    #undef SCAN_PATTERN

    // Force viewmodel update pattern
    uintptr_t vm_addr = PatternScanner::FindPattern(hClient,
        "E8 ? ? ? ? 8B 45 ? 85 C0 74 ? 8B 08");
    if (vm_addr) {
        s_force_viewmodel_fn = PatternScanner::ResolveRelativeAddress(vm_addr, 1, 5);
        SC_LOG_DEBUG("ForceViewModelUpdate resolved: 0x" + std::to_string(s_force_viewmodel_fn));
    } else {
        SC_LOG_WARN("ForceViewModelUpdate pattern not found");
    }

    // Dump items from game schema at runtime
    s_status_message = "Dumping item schema from game memory...";
    SC_LOG_INFO("SkinChanger: Dumping item schema...");
    if (!ItemDatabase::DumpFromSchema()) {
        s_status_message = "Schema dump failed, using fallback data";
        SC_LOG_WARN("SkinChanger: Schema dump failed");
    } else {
        SC_LOG_INFO("SkinChanger: Schema dump complete — " +
            std::to_string(ItemDatabase::GetItemCount()) + " items, " +
            std::to_string(ItemDatabase::GetPaintKitCount()) + " paint kits");
    }

    s_initialized = true;
    s_status_message = "Ready - " + std::to_string(ItemDatabase::GetItemCount()) +
        " items, " + std::to_string(ItemDatabase::GetPaintKitCount()) + " paint kits";

    SC_LOG_INFO("SkinChanger: Initialization complete");
    ConfigManager::Load();
    return true;
}

void SkinChanger::Shutdown() {
    SC_LOG_INFO("SkinChanger: Shutting down...");
    std::unique_lock lock(s_mutex);
    s_weapon_skins.clear();
    s_injected_item_ids.clear();
    s_initialized = false;
    s_status_message = "Shutdown";

    s_fn_create_econ_item = 0;
    s_fn_set_dynamic_attr = 0;
    s_fn_get_econ_item_system = 0;
    s_fn_inventory_manager = 0;
    s_fn_update_subclass = 0;
    s_fn_set_mesh_group_mask = 0;
    s_fn_set_model = 0;
    s_force_viewmodel_fn = 0;
    s_next_fake_item_id = 0xF000000000000001ULL;
    s_glove_frame = 0;
    s_added_gloves = {};
    SC_LOG_INFO("SkinChanger: Shutdown complete");
}

// ============================================================================
// INVENTORY HELPERS
// ============================================================================

static uintptr_t GetInventoryManager() {
    if (!SkinChanger::s_fn_inventory_manager) return 0;
    using Fn = uintptr_t(__fastcall*)();
    return reinterpret_cast<Fn>(SkinChanger::s_fn_inventory_manager)();
}

static uintptr_t GetLocalInventory(uintptr_t manager) {
    if (!manager) return 0;
    // vtable index 68 (wh-esports) — was 57 in older versions
    using Fn = uintptr_t(__fastcall*)(void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(manager))[68])(manager);
}

static bool EquipItemInLoadout(uintptr_t manager, int team, int slot, uint64_t item_id) {
    if (!manager) return false;
    // vtable index 65 (wh-esports) — was 54 in older versions
    using Fn = bool(__fastcall*)(void*, int, int, uint64_t);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(manager))[65])(manager, team, slot, item_id);
}

static uint64_t GetInventoryOwner(uintptr_t inventory) {
    if (!inventory) return 0;
    return *reinterpret_cast<uint64_t*>(inventory + 0x10);
}

// ============================================================================
// ITEM CREATION & EQUIPPING
// ============================================================================

static CEconItem_t* CreateEconItem() {
    if (!SkinChanger::s_fn_create_econ_item) return nullptr;
    using Fn = CEconItem_t*(__cdecl*)();
    return reinterpret_cast<Fn>(SkinChanger::s_fn_create_econ_item)();
}

static void SetDynamicAttributeValue(CEconItem_t* item, uint16_t attr_index, float value) {
    if (!SkinChanger::s_fn_set_dynamic_attr || !item) return;
    using Fn = void(__fastcall*)(CEconItem_t*, uint16_t, float);
    reinterpret_cast<Fn>(SkinChanger::s_fn_set_dynamic_attr)(item, attr_index, value);
}

bool SkinChanger::CreateAndEquipItem(uintptr_t inventory, uintptr_t manager,
    int def_index, int team, int slot, int paint_kit, int seed, float wear, int stattrak)
{
    if (!inventory || !manager) return false;

    CEconItem_t* item = CreateEconItem();
    if (!item) return false;

    uint64_t fake_id = s_next_fake_item_id++;
    item->m_ulID = fake_id;
    item->m_ulOriginalID = fake_id;
    item->m_unDefIndex = static_cast<uint16_t>(def_index);
    item->m_unAccountID = static_cast<uint32_t>(GetInventoryOwner(inventory) & 0xFFFFFFFF);
    item->m_unInventory = 1 << 30;
    item->m_unFlags = ItemDatabase::IsKnife(def_index) ? 3 : 4;

    if (paint_kit > 0) {
        SetDynamicAttributeValue(item, static_cast<uint16_t>(AttrDef::PaintKit), static_cast<float>(paint_kit));
        SetDynamicAttributeValue(item, static_cast<uint16_t>(AttrDef::Seed), static_cast<float>(seed));
        SetDynamicAttributeValue(item, static_cast<uint16_t>(AttrDef::Wear), wear);
    }
    if (stattrak >= 0) {
        SetDynamicAttributeValue(item, static_cast<uint16_t>(AttrDef::StatTrak), static_cast<float>(stattrak));
        SetDynamicAttributeValue(item, static_cast<uint16_t>(AttrDef::StatTrakType), 0.0f);
    }

    // Add to SO cache via SOCreated (vtable 0)
    using SOCreatedFn = void(__fastcall*)(void*, SOID_t, CEconItem_t*, int);
    SOID_t owner = *reinterpret_cast<SOID_t*>(inventory + 0x10);
    reinterpret_cast<SOCreatedFn>((*reinterpret_cast<void***>(inventory))[0])(inventory, owner, item, 0);

    bool equipped = EquipItemInLoadout(manager, team, slot, fake_id);
    if (equipped) {
        s_injected_item_ids.push_back(fake_id);
    }
    return equipped;
}

// ============================================================================
// PUBLIC API
// ============================================================================

uint64_t SkinChanger::GenerateItemID(int def_index) {
    uint64_t id = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    id &= 0x0000FFFFFFFFFFFF;
    id |= (static_cast<uint64_t>(def_index) << 48);
    return id;
}

bool SkinChanger::ApplySkin(const SkinConfig& config) {
    if (!s_initialized) {
        SC_LOG_WARN("ApplySkin: Not initialized");
        return false;
    }

    std::unique_lock lock(s_mutex);
    s_weapon_skins[config.definition_index] = config;

    SC_LOG_SKIN(config.definition_index, config.paint_kit_id,
        "def:" + std::to_string(config.definition_index) +
        " kit:" + std::to_string(config.paint_kit_id));
    SC_LOG_UI("ApplySkin", "def_index=" + std::to_string(config.definition_index) +
        " paint_kit=" + std::to_string(config.paint_kit_id));
    return true;
}

bool SkinChanger::RemoveSkin(uint64_t item_id) {
    std::unique_lock lock(s_mutex);
    auto it = s_weapon_skins.find(static_cast<int>(item_id & 0xFFFF));
    if (it == s_weapon_skins.end()) {
        SC_LOG_WARN("RemoveSkin: Item not found (id=" + std::to_string(item_id) + ")");
        return false;
    }
    s_weapon_skins.erase(it);
    SC_LOG_UI("RemoveSkin", "item_id=" + std::to_string(item_id));
    return true;
}

void SkinChanger::RemoveAllSkins() {
    std::unique_lock lock(s_mutex);
    size_t count = s_weapon_skins.size();
    s_weapon_skins.clear();
    s_injected_item_ids.clear();
    s_added_gloves = {};
    s_next_fake_item_id = 0xF000000000000001ULL;
    ForceViewModelUpdate();
    SC_LOG_UI("RemoveAllSkins", "removed " + std::to_string(count) + " skins");
}

bool SkinChanger::EquipItem(int loadout_slot, uint64_t item_id) {
    if (!s_initialized) return false;
    uintptr_t mgr = GetInventoryManager();
    if (!mgr) return false;
    return EquipItemInLoadout(mgr, 2, loadout_slot, item_id);
}

const std::vector<DumpedItemDef>& SkinChanger::GetDumpedItems() {
    return ItemDatabase::GetItems();
}

const std::vector<DumpedPaintKit>& SkinChanger::GetDumpedPaintKits() {
    return ItemDatabase::GetPaintKits();
}

std::vector<DumpedItemDef> SkinChanger::Search(std::string_view query) {
    return ItemDatabase::Search(query);
}

std::vector<DumpedItemDef> SkinChanger::Search(std::string_view query, ItemType type) {
    return ItemDatabase::Search(query, type);
}

bool SkinChanger::LoadConfig(const std::string& path) {
    return ConfigManager::Load(path);
}

bool SkinChanger::SaveConfig(const std::string& path) {
    return ConfigManager::Save(path);
}

std::string SkinChanger::GetStatusMessage() {
    std::shared_lock lock(s_mutex);
    return s_status_message;
}

int SkinChanger::GetActiveSkinCount() {
    std::shared_lock lock(s_mutex);
    return static_cast<int>(s_weapon_skins.size());
}

// ============================================================================
// GLOVE APPLICATION
// ============================================================================

void SkinChanger::InvalidateGloveMaterials(uintptr_t view_model) {
    if (!view_model) return;
    MaterialInfo* mat_info = reinterpret_cast<MaterialInfo*>(
        reinterpret_cast<uint8_t*>(view_model) + VIEWMODEL_GLOVE_MATERIAL_OFFSET);
    if (!mat_info || mat_info->count == 0 || !mat_info->records) return;

    for (uint32_t i = 0; i < mat_info->count; ++i) {
        if (mat_info->records[i].identifier == GLOVE_MATERIAL_MAGIC) {
            mat_info->records[i].type_index = 0xFFFFFFFF;
            break;
        }
    }
}

void SkinChanger::ApplyGloves(uintptr_t inventory, uintptr_t pawn, uintptr_t view_model) {
    if (!pawn || !view_model || !s_added_gloves.item_id) return;

    // Check alive
    int32_t health = *reinterpret_cast<int32_t*>(pawn + 0x344);
    if (health <= 0) return;

    uintptr_t glove_view = pawn + offsets::C_CSPlayerPawn::m_EconGloves;

    if (s_glove_frame > 0) {
        InvalidateGloveMaterials(view_model);
        *reinterpret_cast<bool*>(glove_view + offsets::C_EconItemView::m_bInitialized) = true;
        *reinterpret_cast<bool*>(pawn + offsets::C_CSPlayerPawn::m_bNeedToReApplyGloves) = true;
        s_glove_frame--;
    }

    uint64_t cur_id = *reinterpret_cast<uint64_t*>(glove_view + offsets::C_EconItemView::m_iItemID);
    if (cur_id != static_cast<uint64_t>(s_added_gloves.item_id)) {
        s_glove_frame = 2;
        *reinterpret_cast<bool*>(glove_view + offsets::C_EconItemView::m_bDisallowSOC) = false;
        *reinterpret_cast<uint64_t*>(glove_view + offsets::C_EconItemView::m_iItemID) = s_added_gloves.item_id;
        *reinterpret_cast<uint32_t*>(glove_view + offsets::C_EconItemView::m_iItemIDHigh) = s_added_gloves.high;
        *reinterpret_cast<uint32_t*>(glove_view + offsets::C_EconItemView::m_iItemIDLow) = s_added_gloves.low;
        *reinterpret_cast<uint32_t*>(glove_view + offsets::C_EconItemView::m_iAccountID) =
            static_cast<uint32_t>(GetInventoryOwner(inventory) & 0xFFFFFFFF);
        *reinterpret_cast<uint16_t*>(glove_view + offsets::C_EconItemView::m_iItemDefinitionIndex) =
            static_cast<uint16_t>(s_added_gloves.def_index);
        *reinterpret_cast<bool*>(glove_view + offsets::C_EconItemView::m_bDisallowSOC) = false;

        // SetMeshGroupMask on viewmodel
        if (s_fn_set_mesh_group_mask) {
            using Fn = void(__fastcall*)(uintptr_t, uint32_t);
            __try { ((Fn)s_fn_set_mesh_group_mask)(view_model, 1); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

// ============================================================================
// MUSIC KIT
// ============================================================================

void SkinChanger::ApplyMusicKit(uintptr_t controller) {
    if (!s_music_kit.enabled || !controller) return;
    uintptr_t inv_services = *reinterpret_cast<uintptr_t*>(controller + offsets::CCSPlayerController::m_pInventoryServices);
    if (!inv_services) return;
    uint16_t music_offset = 0x58; // m_unMusicID from CCSPlayerController_InventoryServices
    *reinterpret_cast<uint16_t*>(inv_services + music_offset) = static_cast<uint16_t>(s_music_kit.music_kit_id);
}

// ============================================================================
// FRAME STAGE NOTIFY — Main skin application
// ============================================================================

void SkinChanger::OnFrameStageNotify(int stage) {
    // Apply at stage 7 (FRAME_NET_UPDATE_POSTDATAUPDATE) — prevents knife flicker
    if (stage != static_cast<int>(FrameStage::FRAME_NET_UPDATE_POSTDATAUPDATE)) return;
    if (!s_initialized) return;

    uintptr_t entity_list = PatternScanner::GetModuleBase("client.dll") + offsets::client_dll::dwEntityList;
    if (!entity_list) return;

    uintptr_t controller_ptr = PatternScanner::GetModuleBase("client.dll") + offsets::client_dll::dwLocalPlayerController;
    if (!controller_ptr) return;

    uintptr_t controller = *reinterpret_cast<uintptr_t*>(controller_ptr);
    if (!controller) return;

    // Apply Music Kit
    ApplyMusicKit(controller);

    // Get inventory
    uintptr_t mgr = GetInventoryManager();
    if (!mgr) return;
    uintptr_t inventory = GetLocalInventory(mgr);
    if (!inventory) return;

    // Get local pawn
    uint32_t pawn_handle = *reinterpret_cast<uint32_t*>(controller + offsets::CCSPlayerController::m_hPlayerPawn);
    uintptr_t entity_list_entry = *reinterpret_cast<uintptr_t*>(entity_list);
    if (!entity_list_entry) return;

    // Entity list structure: [0] = controller list, [1] = pawn list
    uintptr_t pawn_list = *reinterpret_cast<uintptr_t*>(entity_list_entry + 0x10);
    if (!pawn_list) return;

    uint32_t idx = pawn_handle & 0x7FFF;
    uint32_t slot = idx >> 9;
    uint32_t offset = idx & 0x1FF;
    uintptr_t pawn = *reinterpret_cast<uintptr_t*>(pawn_list + slot * 0x78 + 0x10 + offset * 0x78);
    if (!pawn) return;

    // Check alive - moved outside __try block
    int32_t health = *reinterpret_cast<int32_t*>(pawn + offsets::C_BaseEntity::m_iHealth);
    if (health <= 0) return;

    // Get viewmodel
    uint32_t vm_handle = *reinterpret_cast<uint32_t*>(pawn + 0xE48); // m_hViewModel[0]
    uintptr_t vm_list = *reinterpret_cast<uintptr_t*>(entity_list_entry + 0x8);
    uint32_t vm_idx = vm_handle & 0x7FFF;
    uint32_t vm_slot = vm_idx >> 9;
    uint32_t vm_offset = vm_idx & 0x1FF;
    uintptr_t view_model = *reinterpret_cast<uintptr_t*>(vm_list + vm_slot * 0x78 + 0x10 + vm_offset * 0x78);

    // Apply Gloves
    ApplyGloves(inventory, pawn, view_model);

    // Iterate weapon entities
    uint64_t steam_id = GetInventoryOwner(inventory);
    if (!steam_id) return;

    int highest = *reinterpret_cast<int32_t*>(entity_list + offsets::client_dll::dwGameEntitySystem_highestEntityIndex);
    for (int i = 65; i <= highest; i++) {
        uintptr_t entity = 0;
        if (entity_list_entry) {
            uint32_t slot_i = i >> 9;
            uint32_t offset_i = i & 0x1FF;
            uintptr_t list_base = *reinterpret_cast<uintptr_t*>(entity_list_entry + 0x10);
            if (list_base)
                entity = *reinterpret_cast<uintptr_t*>(list_base + slot_i * 0x78 + 0x10 + offset_i * 0x78);
        }
        if (!entity) continue;

        // Get CAttributeManager -> C_EconItemView
        uintptr_t attr_container = entity + offsets::C_EconEntity::m_AttributeManager;
        uintptr_t item_view = attr_container + offsets::C_AttributeContainer::m_Item;

        uint16_t def_index = *reinterpret_cast<uint16_t*>(item_view + offsets::C_EconItemView::m_iItemDefinitionIndex);
        uint32_t owner_low = *reinterpret_cast<uint32_t*>(entity + offsets::C_EconEntity::m_OriginalOwnerXuidLow);
        uint32_t owner_high = *reinterpret_cast<uint32_t*>(entity + offsets::C_EconEntity::m_OriginalOwnerXuidHigh);
        uint64_t owner_xuid = (static_cast<uint64_t>(owner_high) << 32) | owner_low;

        if (owner_xuid != steam_id) continue;

        // --- Knife ---
        if (ItemDatabase::IsKnife(def_index)) {
            if (!s_knife.enabled) continue;

            int target_def = s_knife.def_index;

            // Update EconItemView def index
            *reinterpret_cast<uint16_t*>(item_view + offsets::C_EconItemView::m_iItemDefinitionIndex) =
                static_cast<uint16_t>(target_def);

            // Apply knife skin via fallback
            auto& knife_skin = s_knife.skin;
            if (knife_skin.enabled) {
                *reinterpret_cast<uint32_t*>(item_view + offsets::C_EconItemView::m_iItemIDHigh) = 0xFFFFFFFF;
                *reinterpret_cast<int32_t*>(entity + offsets::C_EconEntity::m_nFallbackPaintKit) = knife_skin.paint_kit_id;
                *reinterpret_cast<int32_t*>(entity + offsets::C_EconEntity::m_nFallbackSeed) = knife_skin.seed;
                *reinterpret_cast<float*>(entity + offsets::C_EconEntity::m_flFallbackWear) = knife_skin.wear;
                if (knife_skin.stattrak >= 0)
                    *reinterpret_cast<int32_t*>(entity + offsets::C_EconEntity::m_nFallbackStatTrak) = knife_skin.stattrak;
            }

            // Set model on weapon + viewmodel
            if (s_fn_set_model) {
                const DumpedItemDef* def = ItemDatabase::FindByDefIndex(target_def);
                if (def && !def->model_path.empty()) {
                    using SetModelFn = void(__fastcall*)(uintptr_t, const char*);
                    __try {
                        ((SetModelFn)s_fn_set_model)(entity, def->model_path.c_str());
                        if (view_model)
                            ((SetModelFn)s_fn_set_model)(view_model, def->model_path.c_str());
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
            }

            // MeshGroupMask
            if (s_fn_set_mesh_group_mask) {
                using Fn = void(__fastcall*)(uintptr_t, uint32_t);
                __try {
                    ((Fn)s_fn_set_mesh_group_mask)(entity, 2);
                    if (view_model)
                        ((Fn)s_fn_set_mesh_group_mask)(view_model, 2);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            continue;
        }

        // --- Weapon Skin ---
        auto it = s_weapon_skins.find(def_index);
        if (it == s_weapon_skins.end() || !it->second.enabled) continue;

        auto& skin = it->second;

        // Apply via fallback path
        *reinterpret_cast<uint32_t*>(item_view + offsets::C_EconItemView::m_iItemIDHigh) = 0xFFFFFFFF;
        if (skin.paint_kit_id > 0)
            *reinterpret_cast<int32_t*>(entity + offsets::C_EconEntity::m_nFallbackPaintKit) = skin.paint_kit_id;
        if (skin.seed > 0)
            *reinterpret_cast<int32_t*>(entity + offsets::C_EconEntity::m_nFallbackSeed) = skin.seed;
        *reinterpret_cast<float*>(entity + offsets::C_EconEntity::m_flFallbackWear) = skin.wear;
        if (skin.stattrak >= 0)
            *reinterpret_cast<int32_t*>(entity + offsets::C_EconEntity::m_nFallbackStatTrak) = skin.stattrak;

        // Custom name
        if (!skin.custom_name.empty()) {
            char* name_ptr = reinterpret_cast<char*>(item_view + offsets::C_EconItemView::m_szCustomName);
            std::memset(name_ptr, 0, 161);
            std::strncpy(name_ptr, skin.custom_name.c_str(), 160);
        }

        // MeshGroupMask — 2 for legacy, 1 for CS2-native
        if (s_fn_set_mesh_group_mask) {
            using Fn = void(__fastcall*)(uintptr_t, uint32_t);
            bool legacy = false;
            if (const DumpedPaintKit* pk = ItemDatabase::FindPaintKit(skin.paint_kit_id))
                legacy = pk->is_legacy;
            uint32_t mask = legacy ? 2 : 1;
            __try {
                ((Fn)s_fn_set_mesh_group_mask)(entity, mask);
                if (view_model) {
                    uint32_t vm_weapon = *reinterpret_cast<uint32_t*>(view_model + 0x38);
                    if (vm_weapon == static_cast<uint32_t>(i))
                        ((Fn)s_fn_set_mesh_group_mask)(view_model, mask);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}

void SkinChanger::OnSetModel(void* entity, const char*& model_path) {
    if (!entity || !model_path) return;
    if (!s_knife.enabled) return;

    // Redirect viewmodel model to custom knife model
    const DumpedItemDef* def = ItemDatabase::FindByDefIndex(s_knife.def_index);
    if (def && !def->model_path.empty()) {
        model_path = def->model_path.c_str();
    }
}

void SkinChanger::OnPreFireEvent(void* event) {
    if (!event) return;
    // TODO: RE REQUIRED - Kill event weapon name fix for knives
    // Hook IGameEventManager2::FireEvent and modify weapon name
}

void SkinChanger::ForceViewModelUpdate() {
    if (s_force_viewmodel_fn)
        reinterpret_cast<void(*)()>(s_force_viewmodel_fn)();
}

}
