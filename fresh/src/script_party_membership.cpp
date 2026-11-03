#include "pal4/script_party_membership.h"

#include "pal4/player_manager_leader_transition.h"

namespace pal4 {
namespace {

constexpr std::uint32_t kMembershipOffset = 0xb08u;
constexpr std::uint32_t kScriptOffset = 0x654u;

struct PartyListOwner {
    IScriptPartyMembershipRuntime& runtime;
    std::vector<std::uint32_t> players{};
    bool active{true};

    ~PartyListOwner() {
        if (active) (void)runtime.FreePartyList(players);
    }
    std::uint32_t Close() noexcept {
        active = false;
        return runtime.FreePartyList(players);
    }
};

struct HintTextOwner {
    IScriptPartyMembershipRuntime& runtime;
    std::uint32_t initialized{};

    ~HintTextOwner() {
        while (initialized != 0u)
            runtime.CleanupHintText(--initialized);
    }
};

std::uint32_t Change(std::uint32_t index, std::uint8_t show_hint,
                     bool adding, IScriptPartyMembershipRuntime& runtime) {
    const auto player = runtime.PlayerByIndex(runtime.PlayerManager(), index);
    if (player == 0u) return 0u;
    const bool in_party = runtime.ReadDword(player + kMembershipOffset) != 0u;
    if (in_party == adding) return player;

    if (!adding)
        (void)PlayerManagerSlotInventoryState::Update(player, 0u, runtime);

    PartyListOwner list{runtime};
    list.players.reserve(4u);
    for (std::uint32_t i = 0u; i < 4u; ++i) {
        const auto candidate = runtime.PlayerByIndex(runtime.PlayerManager(), i);
        if (candidate != 0u &&
            runtime.ReadDword(candidate + kMembershipOffset) != 0u)
            list.players.push_back(candidate);
    }
    runtime.SortPlayers(list.players,
                        PlayerManagerLeaderTransition::kComparator);
    if (adding) {
        (void)PlayerManagerSlotInventoryState::Update(player, 1u, runtime);
        list.players.push_back(player);
    }
    for (std::uint32_t i = 0u; i < list.players.size(); ++i)
        runtime.SetPartyOrder(list.players[i] + kScriptOffset, i);

    if (show_hint != 0u) {
        const auto window = runtime.CreateHintWindow(runtime.UiFrameManager());
        if (window != 0u) {
            HintTextOwner text{runtime};
            runtime.InitializeHintText(0u);
            text.initialized = 1u;
            runtime.InitializeHintText(1u);
            text.initialized = 2u;
            const auto name = runtime.PlayerName(player);
            const auto format = runtime.GameString(
                runtime.GameDatabase() + 0x34u, adding ? 0xfdu : 0xfeu);
            runtime.FormatHintText(format, name);
            runtime.AppendHintText();
            runtime.ShowHint(window, runtime.HintText(), 0.5f, 0.15f, 1.0f,
                             0u, 1u);
            runtime.PlaySound(runtime.AudioManager(), show_hint,
                              adding ? "gamedata\\PALSound\\WA41.wav"
                                     : "gamedata\\PALSound\\WA42.wav",
                              1u);
        }
    }
    return list.Close();
}

}  // namespace

std::uint32_t ScriptPartyMembership::Add(
    std::uint32_t index, std::uint8_t show_hint,
    IScriptPartyMembershipRuntime& runtime) {
    return Change(index, show_hint, true, runtime);
}

std::uint32_t ScriptPartyMembership::Remove(
    std::uint32_t index, std::uint8_t show_hint,
    IScriptPartyMembershipRuntime& runtime) {
    return Change(index, show_hint, false, runtime);
}

}  // namespace pal4
