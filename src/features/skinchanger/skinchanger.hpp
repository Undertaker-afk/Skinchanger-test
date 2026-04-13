#pragma once

#include <Windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <cstdint>
#include "items.hpp"
#include "config.hpp"
#include "../../sdk/inventory.hpp"

namespace cs2 {

enum class FrameStage : int {
    FRAME_START = 0,
    FRAME_NET_UPDATE_START = 1,
    FRAME_NET_UPDATE_POSTDATAUPDATE_START = 2,
    FRAME_NET_UPDATE_POSTDATAUPDATE_END = 3,
    FRAME_NET_UPDATE_END = 4,
    FRAME_RENDER_START = 5,
    FRAME_NET_UPDATE_POSTDATAUPDATE = 7
};

struct KnifeConfig {
    bool     enabled = false;
    int      def_index = 507;
    SkinConfig skin;
};

struct GloveConfig {
    bool     enabled = false;
    int      def_index = 0;
    int      paint_kit = 0;
    float    wear = 0.0001f;
    int      seed = 0;
};

struct AgentConfig {
    bool     enabled = false;
    int      def_index = 0;
};

struct MusicKitConfig {
    bool     enabled = false;
    int      music_kit_id = 0;
};

class SkinChanger {
public:
    static bool Initialize();
    static void Shutdown();

    static bool ApplySkin(const SkinConfig& config);
    static bool RemoveSkin(uint64_t item_id);
    static void RemoveAllSkins();
    static bool EquipItem(int loadout_slot, uint64_t item_id);

    static const std::vector<DumpedItemDef>& GetDumpedItems();
    static const std::vector<DumpedPaintKit>& GetDumpedPaintKits();
    static std::vector<DumpedItemDef> Search(std::string_view query);
    static std::vector<DumpedItemDef> Search(std::string_view query, ItemType type);

    static bool LoadConfig(const std::string& path = "");
    static bool SaveConfig(const std::string& path = "");

    static void OnFrameStageNotify(int stage);
    static void OnSetModel(void* entity, const char*& model_path);
    static void OnPreFireEvent(void* event);

    static void ForceViewModelUpdate();

    static bool IsInitialized() { return s_initialized; }
    static bool IsDumpComplete() { return ItemDatabase::IsDumpReady(); }
    static std::string GetStatusMessage();
    static int GetActiveSkinCount();

    static KnifeConfig& GetKnifeConfig() { return s_knife; }
    static GloveConfig& GetGloveConfig() { return s_gloves; }
    static AgentConfig& GetAgentTConfig() { return s_agent_t; }
    static AgentConfig& GetAgentCTConfig() { return s_agent_ct; }
    static MusicKitConfig& GetMusicKitConfig() { return s_music_kit; }

    static std::unordered_map<int, SkinConfig>& GetWeaponSkins() { return s_weapon_skins; }

private:
    static uint64_t GenerateItemID(int def_index);
    static void ApplyWeaponSkin(uintptr_t entity, uintptr_t item_view, const SkinConfig& config);
    static void ApplyKnife(uintptr_t entity, uintptr_t item_view, uintptr_t view_model);
    static void ApplyGloves(uintptr_t inventory, uintptr_t pawn, uintptr_t view_model);
    static void InvalidateGloveMaterials(uintptr_t view_model);
    static void ApplyMusicKit(uintptr_t controller);

    static bool CreateAndEquipItem(uintptr_t inventory, uintptr_t manager,
        int def_index, int team, int slot, int paint_kit, int seed, float wear, int stattrak);

    static bool s_initialized;
    static std::string s_status_message;
    static std::shared_mutex s_mutex;
    static std::unordered_map<int, SkinConfig> s_weapon_skins;
    static std::vector<uint64_t> s_injected_item_ids;
    static uint64_t s_next_fake_item_id;

    static KnifeConfig s_knife;
    static GloveConfig s_gloves;
    static AgentConfig s_agent_t;
    static AgentConfig s_agent_ct;
    static MusicKitConfig s_music_kit;

    static int s_glove_frame;
    struct GloveInfo { int item_id = 0; uint32_t high = 0; uint32_t low = 0; int def_index = 0; };
    static GloveInfo s_added_gloves;

    static uintptr_t s_client_base;
    
public:
    static uintptr_t s_fn_create_econ_item;
    static uintptr_t s_fn_set_dynamic_attr;
    static uintptr_t s_fn_get_econ_item_system;
    static uintptr_t s_fn_inventory_manager;
    static uintptr_t s_fn_update_subclass;
    static uintptr_t s_fn_set_mesh_group_mask;
    static uintptr_t s_fn_set_model;
    static uintptr_t s_force_viewmodel_fn;

    static constexpr uint32_t GLOVE_MATERIAL_MAGIC = 0xF143B82A;
    static constexpr size_t VIEWMODEL_GLOVE_MATERIAL_OFFSET = 0xF80;

    struct MaterialRecord {
        uint32_t unknown;
        uint32_t identifier;
        uint32_t handle;
        uint32_t type_index;
    };

    struct MaterialInfo {
        MaterialRecord* records;
        uint32_t count;
    };
};

}
