#include "pal4/portable/player_action.h"
#include "pal4/portable/player_notifications.h"
#include "pal4/script_player_action.h"
#include "pal4/script_action_hold_release.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
namespace {[[noreturn]] void Missing(){throw std::logic_error("player action equipment/NPC/RPE service is not bound");}}
struct PlayerActions::Impl final:pal4::IScriptPlayerActionRuntime,pal4::IScriptActionHoldRuntime {
    struct Handler {pal4::PlayerActionWaitState state{};std::string name;ScriptToken token;bool blocking{},closed{};};
    struct EndHandler {pal4::PlayerEndActionWaitState state{};ScriptToken token;float elapsed{};bool closed{};};
    PlayerRoster& players;ActorMemberReader read;std::array<std::unique_ptr<ActorPlayback>,4> playbacks;
    std::array<float,4> speeds{1,1,1,1};
    std::unique_ptr<Handler> pending;std::vector<std::unique_ptr<Handler>> handlers;
    std::unique_ptr<EndHandler> pending_end;std::vector<std::unique_ptr<EndHandler>> end_handlers;
    std::vector<PlayerActionCompletion> completions;
    std::string name;ScriptToken starting_token{};unsigned calls{},completed{},last_index{};std::int32_t last_flag{};bool last_wait{};
    float wait_elapsed{};std::array<double,4> checksums{};
    unsigned unhold_calls{},end_calls{},completed_end{};float unhold_time{},end_elapsed{},end_time{};bool end_observed_complete{};std::string end_action;
    Impl(PlayerRoster& p,ActorMemberReader r):players(p),read(std::move(r)){}
    unsigned Index(unsigned actor) const {
        if(!players.Prepared() || actor<0x10000 || (actor-0x10000)%0xb14 || (actor-0x10000)/0xb14>=4)throw std::logic_error("invalid action player handle");
        return (actor-0x10000)/0xb14;
    }
    ActorPlayback& Animation(unsigned actor) {
        const auto index=Index(actor);auto& playback=playbacks[index];
        if(!playback){
            const auto& player=players.Slots()[index];
            playback=std::make_unique<ActorPlayback>(*player.actor,player.role.Text("RA_MODEL"),read);
            if(!player.animation.empty() && !playback->Select(player.animation))throw std::logic_error("current player action is missing from cache");
            playback->SetRestart(true);
        }
        return *playback;
    }
    unsigned PlayerManager() override{return 0x8000;}
    unsigned PlayerByIndex(unsigned manager,int index) override {
        if(manager!=0x8000 || index<0 || index>=4 || !players.Prepared())throw std::logic_error("invalid action player lookup");
        return 0x10000+static_cast<unsigned>(index)*0xb14;
    }
    unsigned CurrentPlayer(unsigned manager) override{return PlayerByIndex(manager,static_cast<int>(players.Leader()));}
    unsigned NpcManager() override{Missing();}
    unsigned NpcByName(unsigned,std::string_view) override{Missing();}
    std::int32_t SetAction(unsigned actor,std::string_view action) override {
        auto& animation=Animation(actor);
        if(!animation.Select(action))throw std::logic_error("original action clip is missing from cache: "+std::string(action));
        players.SelectAction(Index(actor),action);return 1;
    }
    bool HasAction(std::int32_t index,std::string_view action) const {
        if(!players.Prepared() || index<0 || index>=4)return false;
        const auto& clips=players.Slots()[static_cast<std::size_t>(index)].actor->clips;
        const auto same=[](std::string_view a,std::string_view b){
            if(a.size()!=b.size())return false;
            for(std::size_t i=0;i<a.size();++i){
                const auto fold=[](char c){return c>='A' && c<='Z'?static_cast<char>(c+'a'-'A'):c;};
                if(fold(a[i])!=fold(b[i]))return false;
            }
            return true;
        };
        return std::any_of(clips.begin(),clips.end(),[&](const auto& clip){return same(clip.name,action);});
    }
    void SetFlag(unsigned actor,unsigned flag) override{Animation(actor).SetRestart(flag!=0);}
    void ClearFlag(unsigned actor,unsigned flag) override{Animation(actor).SetPause(flag!=0);}
    bool GetFlag(unsigned actor) override{return Animation(actor).Completed();}
    std::string_view ActionName(unsigned actor) override{return Animation(actor).MessageName();}
    void ApplyEquipmentVisuals(unsigned) override{}
    void ClearWeaponEffects(unsigned) override{}
    void UpdateEffectList(unsigned) override{}
    void UpdateEffectList2(unsigned) override{}
    void UpdateEffectList3(unsigned actor) override{Animation(actor).AttachHoldListener();}
    void UpdateEffectList4(unsigned actor) override{Animation(actor).RemoveHoldListener();}
    void CleanupWeaponEffects(unsigned actor) override{ClearWeaponEffects(actor);}
    std::string_view FormatActorName(unsigned actor) override{name=players.Slots()[Index(actor)].name+"-Player-Movment";return name;}
    pal4::PlayerActionWaitState* AllocateAction(unsigned size) override {
        if(pending || size!=sizeof(pal4::PlayerActionWaitState))throw std::logic_error("invalid action handler allocation");
        pending=std::make_unique<Handler>();pending->token=starting_token;return &pending->state;
    }
    pal4::PlayerEndActionWaitState* AllocateEnd(unsigned size) override {
        if(pending_end || size!=sizeof(pal4::PlayerEndActionWaitState))throw std::logic_error("invalid end action allocation");
        pending_end=std::make_unique<EndHandler>();pending_end->token=starting_token;return &pending_end->state;
    }
    unsigned ScriptExecutor() override{return PlayerNotifications::executor;}
    void ConstructAction(pal4::PlayerActionWaitState& state,std::uint8_t mode,unsigned queue) override {
        RequirePending(state);if(queue!=PlayerNotifications::queue || mode>1)throw std::logic_error("invalid action queue construction");
        pending->blocking=mode!=0;state.base[1]=static_cast<std::byte>(mode);
    }
    void ConstructEnd(pal4::PlayerEndActionWaitState& state,unsigned mode,unsigned queue) override {
        if(!pending_end || &pending_end->state!=&state || mode!=1 || queue!=PlayerNotifications::queue)throw std::logic_error("invalid end action queue");
    }
    void RequirePending(pal4::PlayerActionWaitState& state) const{if(!pending || &pending->state!=&state)throw std::logic_error("invalid pending action record");}
    void AssignName(pal4::PlayerActionWaitState& state,std::string_view value) override{RequirePending(state);pending->name=value;}
    void FlushAction(pal4::PlayerActionWaitState& state) override{RequirePending(state);}
    void FlushEnd(pal4::PlayerEndActionWaitState& state) override {
        if(!pending_end || &pending_end->state!=&state || state.vtable_address!=pal4::ScriptPlayerAction::kEndVtable)throw std::logic_error("invalid end action handler");
    }
    void AddAction(unsigned queue,pal4::PlayerActionWaitState* state) override {
        if(!state)throw std::logic_error("null action handler");
        RequirePending(*state);
        if(queue!=PlayerNotifications::queue || state->phase!=3 || state->vtable_address!=pal4::ScriptPlayerAction::kActionVtable)throw std::logic_error("invalid action notification");
        for(const auto& handler:handlers)if(handler->name==pending->name && handler->blocking)completions.push_back({handler->token,"player action replaced"});
        std::erase_if(handlers,[&](const auto& handler){return handler->name==pending->name;});handlers.push_back(std::move(pending));
    }
    unsigned AddEnd(unsigned queue,pal4::PlayerEndActionWaitState* state) override {
        if(!state || !pending_end || &pending_end->state!=state || queue!=PlayerNotifications::queue)throw std::logic_error("invalid end action registration");
        end_handlers.push_back(std::move(pending_end));return 1;
    }
    unsigned CleanupName() override{return 0;}
    void CloseAction(pal4::PlayerActionWaitState& state) override {
        for(auto& handler:handlers)if(&handler->state==&state){handler->closed=true;state.base[0]=std::byte{1};return;}
        throw std::logic_error("action close outside active queue");
    }
    void CloseEnd(pal4::PlayerEndActionWaitState& state) override {
        for(auto& handler:end_handlers)if(&handler->state==&state){handler->closed=true;return;}
        throw std::logic_error("end action close outside active queue");
    }
    unsigned ReadWord(unsigned) override{Missing();}
    unsigned EmptyStringAddress() override{Missing();}
    unsigned FindNpc(unsigned,unsigned) override{Missing();}
    void ClearAnimationFlag(unsigned component,unsigned value) override {
        if(value!=0 || component<0x170)throw std::logic_error("invalid animation hold release");
        auto& animation=Animation(component-0x170);unhold_time=animation.Time();animation.SetPause(false);
    }
    unsigned UpdatePlayerEffects(unsigned actor) override{Animation(actor).RemoveHoldListener();return 1;}
    unsigned UpdateNpcEffects(unsigned) override{Missing();}
};
PlayerActions::PlayerActions(PlayerRoster& p,ActorMemberReader r):impl_(std::make_unique<Impl>(p,std::move(r))){}
PlayerActions::~PlayerActions()=default;
ScriptResponse PlayerActions::Start(ScriptToken token,std::int32_t index,std::string_view action,std::int32_t flag,bool wait) {
    if(index<0 || index>=4)throw std::out_of_range("action player index");
    auto& s=*impl_;++s.calls;s.last_index=static_cast<unsigned>(index);s.last_flag=flag;s.last_wait=wait;s.wait_elapsed=0;s.starting_token=token;
    if(!s.HasAction(index,action))return {{},false};
    pal4::ScriptPlayerAction runtime(s);(void)runtime.StartIndexed(index,action,flag,wait);
    return {{},wait};
}
bool PlayerActions::SelectTaskAction(std::uint32_t index,std::string_view action,float blend) {
    auto& s=*impl_;if(index>=4 || !s.players.Prepared())throw std::out_of_range("task action player index");
    if(!std::isfinite(blend) || blend<0)throw std::invalid_argument("invalid task action blend");
    auto& animation=s.Animation(0x10000+index*0xb14);
    const bool selected=blend>0?animation.SelectBlended(action,blend):animation.Select(action);
    if(selected)s.players.SelectAction(index,action);
    return selected;
}
void PlayerActions::SetTaskRestart(std::uint32_t index,bool enabled){
    if(index>=4)throw std::out_of_range("task restart player index");
    impl_->Animation(0x10000+index*0xb14).SetRestart(enabled);
}
void PlayerActions::SetTaskSpeed(std::uint32_t index,float speed){
    if(index>=4 || !std::isfinite(speed) || speed<0)throw std::invalid_argument("invalid task animation speed");
    impl_->speeds[index]=speed;
}
void PlayerActions::CleanupTaskWeaponEffects(std::uint32_t index){
    if(index>=4)throw std::out_of_range("task effect player index");
    impl_->ClearWeaponEffects(0x10000+index*0xb14);
}
void PlayerActions::Unhold(std::int32_t index) {
    auto& s=*impl_;++s.unhold_calls;
    if(index<0 || index>=4 || !s.players.Prepared())return;
    (void)pal4::ScriptActionHoldRelease::Player(index,s);
}
ScriptResponse PlayerActions::End(ScriptToken token,std::int32_t index) {
    auto& s=*impl_;++s.end_calls;
    if(index<0 || index>=4 || !s.players.Prepared())return {};
    s.starting_token=token;s.last_index=static_cast<unsigned>(index);
    s.end_elapsed=0;s.end_observed_complete=false;s.end_action.clear();
    pal4::ScriptPlayerAction runtime(s);(void)runtime.EndIndexed(index);return {{},true};
}
std::vector<PlayerActionCompletion> PlayerActions::Advance(float elapsed) {
    if(!std::isfinite(elapsed) || elapsed<0)throw std::invalid_argument("invalid player action elapsed");
    auto& s=*impl_;if(!s.handlers.empty())s.wait_elapsed+=elapsed;
    for(std::size_t i=0;i<s.playbacks.size();++i)if(s.playbacks[i] && s.playbacks[i]->Clip()>=0)s.playbacks[i]->Advance(elapsed*s.speeds[i]);
    pal4::ScriptPlayerAction runtime(s);
    for(auto& handler:s.handlers) {
        runtime.UpdateAction(handler->state);
        if(handler->closed){++s.completed;if(handler->blocking)s.completions.push_back({handler->token,{}});}
    }
    std::erase_if(s.handlers,[](const auto& handler){return handler->closed;});
    for(auto& handler:s.end_handlers){
        handler->elapsed+=elapsed;
        s.end_elapsed=handler->elapsed;
        auto& animation=s.Animation(handler->state.actor);
        const bool done=animation.Completed();const auto action=std::string(animation.Action());const auto time=animation.Time();
        runtime.UpdateEnd(handler->state);
        if(handler->closed){++s.completed_end;s.end_elapsed=handler->elapsed;s.end_action=action;s.end_time=time;s.end_observed_complete=done;s.completions.push_back({handler->token,{}});}
    }
    std::erase_if(s.end_handlers,[](const auto& handler){return handler->closed;});
    auto completed=std::move(s.completions);s.completions.clear();return completed;
}
const ActorPlayback* PlayerActions::Playback(std::uint32_t index) const{return impl_->playbacks.at(index).get();}
void PlayerActions::Sample(std::uint32_t index,std::vector<Vertex>& output,std::vector<Matrix>* frames) {
    auto& playback=impl_->playbacks.at(index);if(!playback)throw std::logic_error("player animation has not been selected");playback->Sample(output,frames);
    double checksum=0;for(std::size_t i=0;i<output.size();++i)for(unsigned axis=0;axis<3;++axis)checksum+=output[i].position[axis]*double((i%17+1)*(axis+1));impl_->checksums[index]=checksum;
}
void PlayerActions::WriteReport(std::ostream& output,bool include_source) const {
    const auto& s=*impl_;output<<std::setprecision(9)<<"{\"calls\":"<<s.calls<<",\"active_handlers\":"<<s.handlers.size()<<",\"completed_handlers\":"<<s.completed
        <<",\"player\":"<<s.last_index<<",\"execute_flag\":"<<s.last_flag<<",\"wait_mode\":"<<(s.last_wait?1:0)<<",\"wait_elapsed\":"<<s.wait_elapsed
        <<",\"unhold_calls\":"<<s.unhold_calls<<",\"unhold_time\":"<<s.unhold_time<<",\"end_calls\":"<<s.end_calls
        <<",\"active_end_handlers\":"<<s.end_handlers.size()<<",\"completed_end_handlers\":"<<s.completed_end
        <<",\"end_elapsed\":"<<s.end_elapsed<<",\"end_observed_complete\":"<<(s.end_observed_complete?"true":"false")
        <<",\"end_action\":"<<std::quoted(s.end_action)<<",\"end_time\":"<<s.end_time
        <<",\"pose_checksum\":"<<std::setprecision(14)<<s.checksums[s.last_index]<<",\"playback\":";
    if(s.playbacks[s.last_index])s.playbacks[s.last_index]->WriteReport(output,include_source);else output<<"null";
    output<<'}';
}
}
