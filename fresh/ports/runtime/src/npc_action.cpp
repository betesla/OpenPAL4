#include "pal4/portable/npc_action.h"
#include "pal4/portable/scene_npcs.h"
#include "pal4/script_player_action.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
namespace {
void Require(bool value,const char* message){if(!value)throw std::logic_error(message);}
[[noreturn]] void Missing(){throw std::logic_error("NPC action operation is not bound");}
}
struct NpcActions::Impl final:pal4::IScriptPlayerActionRuntime {
    struct Handler {pal4::PlayerActionWaitState state{};std::string name;ScriptToken token;bool blocking{},closed{};float elapsed{};};
    struct EndHandler {pal4::PlayerEndActionWaitState state{};ScriptToken token;float elapsed{};bool closed{};};
    std::unique_ptr<EndHandler> pending_end;std::vector<std::unique_ptr<EndHandler>> end_handlers;
    unsigned end_calls{},end_completed{};float end_elapsed{},end_time{};bool end_observed_complete{};std::string end_action;
    SceneNpcs& npcs;std::unique_ptr<Handler> pending;std::vector<std::unique_ptr<Handler>> handlers;
    std::vector<NpcActionCompletion> completions;ScriptToken starting{};
    unsigned calls{},matched{},completed{},blocking_completed{},replaced{};std::int32_t flag{};bool wait{};
    std::string npc,action,completed_action;float completed_time{},completed_elapsed{};
    static constexpr unsigned manager=0x3000,executor=0x4000,queue=executor+0x54;
    explicit Impl(SceneNpcs& n):npcs(n){}
    unsigned Index(unsigned actor) const {Require(actor>=0x100000 && (actor-0x100000)%0x1000==0,"invalid NPC action handle");const auto i=(actor-0x100000)/0x1000;Require(i<npcs.States().size(),"NPC action outside scene");return i;}
    ActorPlayback& Animation(unsigned actor){auto* p=npcs.Playback(Index(actor));Require(p!=nullptr,"NPC action requires an animation owner");return *p;}
    unsigned PlayerManager() override{Missing();}
    unsigned PlayerByIndex(unsigned,int) override{Missing();}
    unsigned CurrentPlayer(unsigned) override{Missing();}
    unsigned NpcManager() override{return manager;}
    unsigned NpcByName(unsigned owner,std::string_view name) override {
        Require(owner==manager,"invalid NPC action manager");const auto states=npcs.States();
        for(unsigned i=0;i<states.size();++i) {
            if(states[i].name==name)return 0x100000+i*0x1000;
        }
        return 0;
    }
    int SetAction(unsigned actor,std::string_view name) override {
        // 原处理器允许C01缺失时尝试Z01；未找到首个请求动作则明确失败。
        return Animation(actor).Select(name)?1:0;
    }
    void SetFlag(unsigned actor,unsigned value) override{Require(value<=1,"invalid NPC restart flag");Animation(actor).SetRestart(value!=0);}
    void ClearFlag(unsigned actor,unsigned value) override{Require(value<=1,"invalid NPC pause flag");Animation(actor).SetPause(value!=0);}
    bool GetFlag(unsigned actor) override{return Animation(actor).Completed();}
    std::string_view ActionName(unsigned actor) override{return Animation(actor).MessageName();}
    void ApplyEquipmentVisuals(unsigned) override{}
    void ClearWeaponEffects(unsigned) override{}
    void UpdateEffectList(unsigned) override{}
    void UpdateEffectList2(unsigned) override{}
    void UpdateEffectList3(unsigned actor) override{Animation(actor).AttachHoldListener();}
    void UpdateEffectList4(unsigned actor) override{Animation(actor).RemoveHoldListener();}
    void CleanupWeaponEffects(unsigned actor) override{npcs.CleanupWeaponEffects(Index(actor));}
    std::string_view FormatActorName(unsigned) override{Missing();}
    pal4::PlayerActionWaitState* AllocateAction(unsigned size) override{Require(size==sizeof(pal4::PlayerActionWaitState) && !pending,"invalid NPC action allocation");pending=std::make_unique<Handler>();pending->token=starting;return &pending->state;}
    pal4::PlayerEndActionWaitState* AllocateEnd(unsigned size) override{Require(size==sizeof(pal4::PlayerEndActionWaitState) && !pending_end,"invalid NPC end allocation");pending_end=std::make_unique<EndHandler>();pending_end->token=starting;return &pending_end->state;}
    unsigned ScriptExecutor() override{return executor;}
    void Check(const pal4::PlayerActionWaitState& s) const{Require(pending && &pending->state==&s,"invalid pending NPC action");}
    void ConstructAction(pal4::PlayerActionWaitState& s,std::uint8_t mode,unsigned owner) override{Check(s);Require(mode<=1 && owner==queue,"invalid NPC action queue");pending->blocking=mode!=0;}
    void ConstructEnd(pal4::PlayerEndActionWaitState& s,unsigned mode,unsigned owner) override{Require(pending_end && &pending_end->state==&s && mode==1 && owner==queue,"invalid NPC end queue");}
    void AssignName(pal4::PlayerActionWaitState& s,std::string_view name) override{Check(s);pending->name=name;}
    void FlushAction(pal4::PlayerActionWaitState& s) override{Check(s);}
    void FlushEnd(pal4::PlayerEndActionWaitState& s) override{Require(pending_end && &pending_end->state==&s && s.vtable_address==pal4::ScriptPlayerAction::kEndVtable,"invalid NPC end flush");}
    void AddAction(unsigned owner,pal4::PlayerActionWaitState* s) override {
        Require(s!=nullptr,"null NPC action handler");Check(*s);
        Require(owner==queue && s->vtable_address==pal4::ScriptPlayerAction::kActionVtable && s->phase==3,"invalid NPC action registration");
        for(const auto& h:handlers)if(h->name==pending->name){++replaced;if(h->blocking)completions.push_back({h->token,"NPC action replaced"});}
        std::erase_if(handlers,[&](const auto& h){return h->name==pending->name;});handlers.push_back(std::move(pending));
    }
    unsigned AddEnd(unsigned owner,pal4::PlayerEndActionWaitState* s) override{Require(pending_end && &pending_end->state==s && owner==queue,"invalid NPC end registration");end_handlers.push_back(std::move(pending_end));return 1;}
    unsigned CleanupName() override{return 0;}
    void CloseAction(pal4::PlayerActionWaitState& s) override{for(auto& h:handlers)if(&h->state==&s){h->closed=true;return;}throw std::logic_error("NPC action close outside queue");}
    void CloseEnd(pal4::PlayerEndActionWaitState& s) override{for(auto& h:end_handlers)if(&h->state==&s){h->closed=true;return;}throw std::logic_error("NPC end close outside queue");}
};
NpcActions::NpcActions(SceneNpcs& npcs):impl_(std::make_unique<Impl>(npcs)){}
NpcActions::~NpcActions()=default;
ScriptResponse NpcActions::Start(ScriptToken token,std::string_view name,std::string_view action,int flag,bool wait) {
    Require(name.find('\0')==std::string_view::npos && action.find('\0')==std::string_view::npos,"embedded NUL in NPC action");
    auto& s=*impl_;++s.calls;s.npc=name;s.action=action;s.flag=flag;s.wait=wait;s.starting=token;
    const auto actor=s.NpcByName(Impl::manager,name);if(!actor)return {};
    if(!s.npcs.HasAction(s.Index(actor),action))return {{},false};++s.matched;
    (void)pal4::ScriptPlayerAction(s).StartNpc(name,action,flag,wait);return {{},wait};
}
ScriptResponse NpcActions::End(ScriptToken token,std::string_view name){
    Require(name.find('\0')==std::string_view::npos,"embedded NUL in NPC end action");
    auto& s=*impl_;++s.end_calls;s.starting=token;
    s.end_elapsed=0;s.end_time=0;s.end_action.clear();s.end_observed_complete=false;
    const auto actor=s.NpcByName(Impl::manager,name);if(!actor)return {};
    (void)pal4::ScriptPlayerAction(s).EndNpc(name);return {{},true};
}
std::vector<NpcActionCompletion> NpcActions::Advance(float elapsed) {
    Require(std::isfinite(elapsed) && elapsed>=0,"invalid NPC action elapsed");auto& s=*impl_;pal4::ScriptPlayerAction runtime(s);
    for(auto& h:s.handlers){
        h->elapsed+=elapsed;auto& p=s.Animation(h->state.actor);
        if(!s.npcs.States()[s.Index(h->state.actor)].active)p.Advance(elapsed);
        const auto action=std::string(p.Action());const auto time=p.Time();runtime.UpdateAction(h->state);
        if(h->closed){++s.completed;s.completed_action=action;s.completed_time=time;s.completed_elapsed=h->elapsed;if(h->blocking){++s.blocking_completed;s.completions.push_back({h->token,{}});}}}
    std::erase_if(s.handlers,[](const auto& h){return h->closed;});
    for(auto& h:s.end_handlers){
        h->elapsed+=elapsed;s.end_elapsed=h->elapsed;auto& p=s.Animation(h->state.actor);
        if(!s.npcs.States()[s.Index(h->state.actor)].active)p.Advance(elapsed);
        const bool done=p.Completed();const auto action=std::string(p.Action());const float time=p.Time();
        runtime.UpdateEnd(h->state);
        if(h->closed){++s.end_completed;s.end_elapsed=h->elapsed;s.end_time=time;s.end_action=action;s.end_observed_complete=done;s.completions.push_back({h->token,{}});}
    }
    std::erase_if(s.end_handlers,[](const auto& h){return h->closed;});auto result=std::move(s.completions);s.completions.clear();return result;
}
bool NpcActions::Requested() const{return impl_->calls!=0 || impl_->end_calls!=0;}
void NpcActions::WriteReport(std::ostream& out) const {
    const auto& s=*impl_;out<<std::setprecision(9)<<"{\"calls\":"<<s.calls<<",\"matched\":"<<s.matched<<",\"handlers\":"<<s.handlers.size()<<",\"completed\":"<<s.completed<<",\"blocking_completed\":"<<s.blocking_completed<<",\"replaced\":"<<s.replaced<<",\"npc\":"<<std::quoted(s.npc)<<",\"action\":"<<std::quoted(s.action)<<",\"execute_flag\":"<<s.flag<<",\"wait\":"<<(s.wait?"true":"false")<<",\"completed_action\":"<<std::quoted(s.completed_action)<<",\"completed_time\":"<<s.completed_time<<",\"completed_elapsed\":"<<s.completed_elapsed<<",\"end_calls\":"<<s.end_calls<<",\"end_handlers\":"<<s.end_handlers.size()<<",\"end_completed\":"<<s.end_completed
        <<",\"end_elapsed\":"<<s.end_elapsed<<",\"end_time\":"<<s.end_time<<",\"end_action\":"<<std::quoted(s.end_action)<<",\"end_observed_complete\":"<<(s.end_observed_complete?"true":"false")<<'}';
}
}
