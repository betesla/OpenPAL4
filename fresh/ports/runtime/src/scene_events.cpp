#include "pal4/portable/scene_events.h"
#include "pal4/game_arena_clump_callbacks.h"
#include "pal4/script_event_volume_visibility.h"
#include "pal4/pal_game_object_factory_script.h"
#include <iomanip>
#include <bit>
#include <cmath>
#include <limits>
#include "pal4/pal_actor_control_effect_selector.h"
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
namespace {
void Require(bool ok,const char* error){if(!ok)throw std::logic_error(error);}
bool Identifier(std::string_view name){return !name.empty() && name.size()<=128 &&
    name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")==std::string_view::npos &&
    name.front()>='A';}
}
struct SceneEvents::Impl final:pal4::IGameArenaClumpRuntime,pal4::IScriptEventVolumeVisibilityRuntime,pal4::IPalGameObjectFactoryScriptRuntime {
    static constexpr std::uint32_t record_base=0x30000000,resource_base=0x40000000,clump_base=0x50000000,
        world=0x60000000,adapter=0x61000000,primary=0x62000000,secondary=0x63000000,
        payload=0x70000000,name_text=0x71000000,function_text=0x72000000,path_text=0x73000000;
    const ArenaResources& arena;SceneCollision& collision;ISceneEventScripts& scripts;
    std::vector<SceneEventState> states;std::vector<std::vector<std::uint32_t>> shapes;
    std::size_t current{};std::string query,function;unsigned entries{},leaves{},pushes{};
    Impl(const ArenaResources& a,SceneCollision& c,ISceneEventScripts& s):arena(a),collision(c),scripts(s) {
        Require(a.description.event_collision_cache && a.event_collisions.size()==a.catalog.events.size(),"complete EVF collision cache is required");
        Require(a.catalog.events.size()<65536,"too many event handles");
        for(std::size_t i=0;i<a.catalog.events.size();++i){const auto& e=a.catalog.events[i];
            Require(e.effect_type<=10,"EVF local-float/sound controls are not bound");
            Require(e.clump.empty() || a.event_collisions[i].has_value(),"EVF geometry is missing");
            SceneEventState state;state.name=e.name;state.type=e.effect_type;states.push_back(std::move(state));
        }
        shapes.resize(states.size());
    }
    static std::uint32_t Record(std::size_t i){return record_base+static_cast<std::uint32_t>(i)*256;}
    static std::uint32_t Resource(std::size_t i){return resource_base+static_cast<std::uint32_t>(i)*256;}
    std::size_t Index(std::uint32_t address,std::uint32_t base) const {
        Require(address>=base && (address-base)%256==0 && (address-base)/256<states.size(),"invalid EVF numeric handle");return (address-base)/256;
    }
    const SceneEvent& Event() const{return arena.catalog.events.at(current);}
    void Detach(std::size_t i){for(auto handle:shapes[i])collision.Remove(handle);shapes[i].clear();states[i].movement_shapes=0;}
    void Clear(){for(std::size_t i=0;i<states.size();++i){Detach(i);states[i].active=false;states[i].camera_requested=false;states[i].secondary_requested=false;}}
    std::uint32_t ReadWord(std::uint32_t address) override {
        if(address==world+48)return 1;
        if(address==world+200)return path_text;
        if(address==world+212)return 0;
        if(address==adapter)return primary;
        if(address==adapter+4)return secondary;
        if(address==payload+4)return name_text;
        if(address==resource_base+static_cast<std::uint32_t>(current)*256+4)return Record(current)+4;
        if(address>=record_base && address<record_base+states.size()*256){
            const auto i=(address-record_base)/256;const auto offset=(address-record_base)%256;
            switch(offset){case 4:return states[i].type;case 52:return function_text;case 56:return static_cast<std::uint32_t>(arena.catalog.events[i].script.size());
            case 64:return Resource(i);case 68:return arena.catalog.events[i].clump.empty()?0:clump_base+static_cast<std::uint32_t>(i)*256;case 76:return states[i].active;default:break;}
        }
        throw std::logic_error("unsupported EVF handle read");
    }
    void WriteWord(std::uint32_t address,std::uint32_t value) override {
        if(address==primary+88 && value==1){collision.InterruptEventTraversal();return;}
        Require(address>=record_base && address<record_base+states.size()*256 && (address-record_base)%256==76 && value<=1,"unsupported EVF handle write");
        states[(address-record_base)/256].active=value!=0;
    }
    std::uint32_t Call(pal4::GameArenaClumpCall op,std::uint32_t first,std::uint32_t second,std::uint32_t third,std::uint32_t fourth,std::uint32_t fifth,std::uint32_t sixth) override {
        using Op=pal4::GameArenaClumpCall;
        switch(op){
        case Op::GetMovementAdapter:return adapter;
        case Op::GetCameraSubsystem:return 0x64000000;
        case Op::AddCameraClump:states[current].camera_requested=true;return second;
        case Op::RemoveCameraClump:states[current].camera_requested=false;return second;
        case Op::GetResourceProperty:Require(first==Resource(current),"EVF property owner mismatch");return states[current].type==0 || states[current].type==4;
        case Op::SetClumpProperties:{
            Require(second==clump_base+current*256 && sixth==Resource(current) && fourth==1 && fifth==1,"EVF registration properties mismatch");
            if(first==secondary){states[current].secondary_requested=true;return second;}
            Require(first==primary && shapes[current].empty(),"EVF primary registration mismatch");
            CollisionObjectProperties properties;properties.mode=third;properties.owner=Record(current);properties.callback=Resource(current);
            properties.hard_block=true;properties.proximity=true;properties.plugin=true;
            try{for(const auto& atomic:arena.event_collisions[current]->atomics)if(!atomic.triangles.empty())
                shapes[current].push_back(collision.Add(CollisionGroup::object,atomic.triangles,properties));}
            catch(...){Detach(current);throw;}
            states[current].movement_shapes=shapes[current].size();return second;
        }
        case Op::RestoreRenderCallback:
            if(first==secondary)states[current].secondary_requested=false;
            else {Require(first==primary,"EVF unregister owner mismatch");Detach(current);}return second;
        }
        throw std::logic_error("unsupported EVF registration operation");
    }
    void Enter(std::size_t i){current=i;(void)pal4::GameArenaClumpCallbacks::Enter(Record(i),world,*this);}
    void Leave(std::size_t i){current=i;(void)pal4::GameArenaClumpCallbacks::Leave(Record(i),world,*this);}
    std::uint32_t EmptyStringAddress() override{return name_text;}
    std::uint32_t EventVolumeManager() override{return 0x65000000;}
    std::uint32_t FindVolume(std::uint32_t manager,std::uint32_t text) override {
        Require(manager==0x65000000 && text==name_text,"EVF visibility query mismatch");
        for(std::size_t i=0;i<states.size();++i)if(states[i].name==query)return Record(i);
        return 0;
    }
    std::uint32_t ArenaManager() override{return world;}
    std::uint32_t Deactivate(std::uint32_t owner,std::uint32_t record) override{Require(owner==world,"EVF deactivate owner mismatch");Leave(Index(record,record_base));return record;}
    std::uint32_t Activate(std::uint32_t owner,std::uint32_t record) override{Require(owner==world,"EVF activate owner mismatch");Enter(Index(record,record_base));return record;}
    std::uint32_t MovementAdapter() override{return adapter;}
    void InitializeScript() override{}
    bool IsCsbMode() override{return true;}
    std::string ReadBytes(std::uint32_t address,std::uint32_t size) override{Require(address==function_text && size==Event().script.size(),"EVF function length mismatch");return Event().script;}
    std::string ReadCString(std::uint32_t address) override{Require(address==path_text,"EVF script path mismatch");return arena.description.world;}
    std::string Declaration() const{Require(Identifier(function),"EVF script function is not an identifier");return "void "+function+"()";}
    bool ScriptCacheContains(std::string_view key) override{
        function=Event().script;Require(key==arena.description.world+"["+function+"]","EVF script cache key mismatch");return scripts.Contains(arena.description.world,Declaration());
    }
    std::uint32_t CreateScriptInterpreter(bool csb,std::string_view path,std::string_view,std::string_view name,std::uint32_t context) override {
        Require(csb && path==arena.description.world && name==function && context==0,"EVF interpreter request mismatch");return 1;
    }
    bool AddScriptModule(std::uint32_t interpreter) override{Require(interpreter==1,"EVF interpreter handle mismatch");scripts.LoadWorld(arena.description.world);return true;}
    std::string ScriptFunctionName(std::uint32_t) override{return function;}
    std::string ScriptFullPath(std::uint32_t) override{return arena.description.world;}
    std::int32_t PushStackModule(std::string_view path,std::string_view name) override {
        Require(path==arena.description.world && name==function,"EVF stack push mismatch");const auto result=scripts.Push(path,Declaration());if(result==0)++pushes;return result;
    }
    void ReleaseInterpreter(std::uint32_t) override{}
    void ShowMissingScriptMessage() override{throw std::runtime_error("EVF original script interpreter is missing");}
    void CleanupAllScripts() override{}
};
SceneEvents::SceneEvents(const ArenaResources& a,SceneCollision& c,ISceneEventScripts& s):impl_(std::make_unique<Impl>(a,c,s)){}
SceneEvents::~SceneEvents(){impl_->Clear();}
void SceneEvents::ActivateAll(){auto& s=*impl_;try{for(std::size_t i=0;i<s.states.size();++i)s.Enter(i);}catch(...){s.Clear();throw;}}
bool SceneEvents::SetVisible(std::string_view name,bool visible){auto& s=*impl_;Require(name.find('\0')==std::string_view::npos,"invalid EVF name");s.query=name;return pal4::ScriptEventVolumeVisibility::Set(Impl::payload,visible,s)!=0;}
void SceneEvents::OnCollision(std::uint32_t resource,bool entering,std::uint32_t previous){auto& s=*impl_;s.current=s.Index(resource,Impl::resource_base);Require(s.states[s.current].active && !s.shapes[s.current].empty(),"inactive EVF collision callback");
    if(!entering){++s.leaves;return;}++s.entries;
    if(s.states[s.current].type==1)(void)pal4::PalGameObjectFactoryScript::ExecuteLocalFloor(resource,previous,s);
    else Require(s.states[s.current].type<=10,"unsupported EVF collision callback type");
}
std::optional<std::size_t> SceneEvents::FindInteraction(Point3 position,float vertical_tolerance,float range) const {
    for(float value:position)Require(std::isfinite(value),"invalid EVF interaction position");
    Require(std::isfinite(vertical_tolerance) && vertical_tolerance>=0 && std::isfinite(range) && range>=0,"invalid EVF interaction range");
    struct Query final:pal4::IActorControlEffectSelectorRuntime {
        const SceneEvent& event;Point3 position;float tolerance;bool selected{};
        Query(const SceneEvent& e,Point3 p,float t):event(e),position(p),tolerance(t){}
        pal4::ActorControlEffectPoint Vertex(pal4::ActorControlEffectObject&,std::int32_t i) override {
            const auto& p=event.properties.at(static_cast<std::size_t>(i));
            return {std::bit_cast<float>(p[0]),std::bit_cast<float>(p[1]),std::bit_cast<float>(p[2])};
        }
        pal4::ActorControlEffectPoint ActorPosition() override{return {position[0],position[1],position[2]};}
        float VerticalTolerance() override{return tolerance;}
        void SelectEffect(pal4::ActorControlEffectObject&) override{selected=true;}
    };
    const auto& s=*impl_;std::optional<std::size_t> selected;float best=range;
    for(std::size_t i=0;i<s.states.size();++i){
        const auto& event=s.arena.catalog.events[i];
        Require(event.properties.size()<=(std::numeric_limits<std::int32_t>::max()-256u)/80u,"too many EVF interaction properties");
        pal4::ActorControlEffectObject object;object.words[1]=s.states[i].type;object.words[19]=s.states[i].active;
        object.words[21]=event.properties.empty()?0:256;
        object.words[22]=256+static_cast<std::uint32_t>(event.properties.size())*80;
        Query query(event,position,vertical_tolerance);
        (void)pal4::PalActorControlEffectSelector::Visit(object,best,query);
        if(query.selected)selected=i;
    }
    return selected;
}
bool SceneEvents::TriggerInteraction(std::size_t index){
    auto& s=*impl_;const auto& state=s.states.at(index);
    if(state.type!=5 || !state.active)return false;
    s.current=index;
    return pal4::PalGameObjectFactoryScript::ExecuteMachine(Impl::Resource(index),0,s)!=0;
}
std::span<const SceneEventState> SceneEvents::States() const{return impl_->states;}
void SceneEvents::WriteReport(std::ostream& out) const{const auto& s=*impl_;out<<"{\"entries\":"<<s.entries<<",\"leaves\":"<<s.leaves<<",\"pushes\":"<<s.pushes<<",\"camera_collision_bound\":false,\"secondary_collision_bound\":false,\"events\":[";
    for(std::size_t i=0;i<s.states.size();++i){if(i)out<<',';const auto& e=s.states[i];out<<"{\"name\":"<<std::quoted(e.name)<<",\"type\":"<<e.type<<",\"active\":"<<(e.active?"true":"false")<<",\"movement_shapes\":"<<e.movement_shapes<<",\"camera_requested\":"<<(e.camera_requested?"true":"false")<<",\"secondary_requested\":"<<(e.secondary_requested?"true":"false")<<'}';}out<<"]}";
}
}
