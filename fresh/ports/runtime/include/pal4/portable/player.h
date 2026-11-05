#pragma once
#include "pal4/portable/actor.h"
#include "pal4/portable/role.h"
#include <functional>
#include <memory>
#include <span>

namespace pal4::portable {
// 均为数值；不包含原对象的虚表或32位地址。
struct PlayerStats {
    std::array<std::uint32_t,8> core{}; // 生命、怒气增量、真气、武、防、速、运、灵
    std::array<float,4> rates{}; // 四个浮点系数；字段顺序见实现。
    std::array<std::uint32_t,6> attack{};
    std::array<float,6> react{}, extract{};
    std::array<std::uint32_t,5> elements{}; // 水火雷风土
};
struct UpgradeRecord {
    std::uint32_t id{}, role_id{}, level{}, experience{};
    PlayerStats stats;
};
std::vector<UpgradeRecord> ReadUpgrades(std::istream& input);
struct PlayerProgress {
    PlayerStats base, effective;
    std::array<std::uint32_t,5> element_bonus{};
    std::int32_t level{}, experience{}, hp{}, mp{}, rage{};
    bool dead{};
};
// 对应581690/57DB70：按原表顺序选择首条匹配，保留五灵基础值与加值。
PlayerProgress InitialPlayerProgress(const RoleRecord& role, std::span<const UpgradeRecord> upgrades);
struct PlayerSlot {
    std::string name;
    std::string world,scene,animation;
    RoleRecord role;
    std::unique_ptr<Actor> actor;
    PlayerProgress progress;
    std::array<float,3> position{}, rotation{};
    std::array<std::uint32_t,6> equipment{};
    std::array<std::optional<std::uint64_t>,2> named_weapon_effects{};
    std::array<std::int32_t,4> favor{};
    std::vector<std::uint32_t> skills{};
    std::uint32_t script_index{}; // 原脚本组件的排序字段；换队长时交换。
    bool auto_seek{true};
    bool visible{true};
    bool active{}, inventory_enabled{};
};
class PlayerRoster {
public:
    using ActorLoader=std::function<Actor(std::string_view)>;
    void Clear() noexcept;
    void Prepare(const RoleCatalog& roles, std::span<const UpgradeRecord> upgrades, const ActorLoader& load);
    void EnterScene(std::string_view world,std::string_view scene,std::uint32_t index=0);
    // IDA 441730: same index returns before any scene work. The callback
    // binds controller/GPU resources after arena membership has transferred.
    void SetLeader(std::int32_t index,const std::function<void(std::uint32_t)>& bind_actor);
    // IDA 5D4950/5D4B60: giPlayerInTeam/giPlayerOutTeam membership and order.
    void SetInTeam(std::int32_t index,bool in_team);
    void SetFullHp();
    void SetFullMp();
    void SetFullRage();
    void TakeARest();
    void SetHealth(std::uint32_t index,std::int32_t hp,std::int32_t mp);
    void SetLevel(std::int32_t index,std::int32_t level);
    std::int32_t GetLevel(std::int32_t index) const;
    void AddSkill(std::int32_t index,std::uint32_t skill_id);
    void AddFavor(std::int32_t first_index,std::int32_t second_index,std::int32_t delta);
    void SetVisible(std::int32_t index,bool visible);
    void SetAutoSeek(bool enabled);
    void SelectAction(std::uint32_t index,std::string_view action);
    void SetPosition(std::uint32_t index,std::array<float,3> position);
    void SetRotation(std::uint32_t index,std::array<float,3> rotation);
    std::uint32_t Leader() const noexcept { return leader_; }
    const std::array<PlayerSlot,4>& Slots() const noexcept { return slots_; }
    bool Prepared() const noexcept { return prepared_; }
    // Original4404D0 mirrors the party priority in two x86 fields. Portable
    // consumers share script_index as their single canonical representation.
    void SetPartyOrder(std::uint32_t player,std::uint32_t order);
private:
    friend class PlayerEquipment;
    struct LeaderRuntime;
    std::array<PlayerSlot,4> slots_;
    std::string world_,scene_;
    std::uint32_t leader_{};
    bool prepared_{};
};
void WritePlayerReport(std::ostream& output, const PlayerRoster& roster);
} // namespace pal4::portable
