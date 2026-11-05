#include "pal4/portable/scene_objects.h"
#include "pal4/portable/scene_events.h"
#include "pal4/gob_normal.h"
#include "pal4/script_object_research.h"
#include "pal4/pal_game_object_manager_visit.h"
#include "pal4/game_vector_math.h"
#include <numeric>
#include "pal4/portable/animation_table.h"
#include "pal4/script_object_visibility.h"
#include "pal4/game_arena_script_clump_callbacks.h"
#include "pal4/gob_object_base.h"
#include "pal4/gob_action.h"
#include "pal4/gob_sound.h"
#include "pal4/gob_sound_config.h"
#include "pal4/portable/gob_effect_instance.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
namespace {
void Require(bool valid,const char* message){if(!valid)throw std::logic_error(message);}
const pal4::BinaryXmlAttribute* Attribute(const pal4::BinaryXmlNode& node,std::string_view key) {
    if(const auto* value=node.FindAttribute(key))return value;
    for(const auto& child:node.children)if(const auto* value=Attribute(child,key))return value;
    return nullptr;
}
std::int32_t Integer(float value) {
    if(!std::isfinite(value) || value<-2147483648.f || value>=2147483648.f)return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
}
struct SceneObjects::Impl final:pal4::IScriptObjectVisibilityRuntime,pal4::IGameArenaScriptClumpRuntime,
    pal4::IGobObjectParameterValueRuntime,pal4::IGobObjectVisibilityRuntime,pal4::IGobActionVisibilityRuntime,pal4::IGobActionUpdateRuntime,pal4::IGobClumpAdapterRuntime,pal4::IGobInteractionBoundsRuntime,pal4::IGobResearchUseRuntime {
    struct Object {std::map<std::uint32_t,std::uint32_t> words;std::unique_ptr<ActorPlayback> playback;std::unique_ptr<GobEffectInstance> effect;std::string initial_alias,initial_resource;bool initial_effect_pending{};
        std::vector<std::uint32_t> collision_shapes;Matrix collision_model{};float collision_time{-1};std::string collision_action,initial_effect_issue;
    };
    const ArenaResources& arena;SceneMinimap& minimap;GobMemberReader read;
    EffectPlayback* effects{};SceneCollision* movement_collision{};CollisionEligibility collision_eligibility;
    std::vector<SceneObjectState> states;std::vector<Object> objects;std::size_t loading{};
    std::uint32_t calls{},matched{},logs{},updates{};std::string last_name;bool last_visible{};
    static constexpr std::uint32_t world=0x2000,manager=0x3000,map=0x4000;
    Impl(const ArenaResources& a,SceneMinimap& m,GobMemberReader r,EffectPlayback* e):arena(a),minimap(m),read(std::move(r)),effects(e){}
    static std::uint32_t Handle(std::size_t i){return 0x100000+static_cast<std::uint32_t>(i)*0x1000;}
    std::size_t Index(std::uint32_t handle) const {
        Require(handle>=0x100000,"invalid scene object handle");const auto i=(handle-0x100000)/0x1000;
        Require(i<states.size(),"scene object handle outside collection");return i;
    }
    std::size_t ClumpIndex(std::uint32_t clump) const {
        Require(clump>=0x10000 && clump-0x10000<states.size(),"invalid scene object clump");return clump-0x10000;
    }
    ActorPlayback& Animation(std::uint32_t state){const auto i=Index(state);Require(state==Handle(i)+pal4::GobAction::kAnimationOffset && bool(objects[i].playback),"missing GOBAction animation");return *objects[i].playback;}
    std::uint32_t ReadWord(std::uint32_t address) override {
        if(address==world+48)return 1;
        if(address==world+8)return 1;
        if(address==world+0xe0)return 2;
        const auto i=Index(address);const auto offset=address-Handle(i);const auto& s=states[i];
        if(offset>=0xac && offset<=0xb4 && offset%4==0)return std::bit_cast<std::uint32_t>(s.position[(offset-0xac)/4]);
        switch(offset){case 0x14:return (s.actor || s.type==8)?0x10000+static_cast<std::uint32_t>(i):0;case 0x18:return 0;case 0x170:return s.active;case 0x184:return s.visible;case 0x198:return s.uses;case 0x1b0:return s.marker;default:return objects[i].words.at(offset);}
    }
    void WriteWord(std::uint32_t address,std::uint32_t value) override {
        const auto i=Index(address);const auto offset=address-Handle(i);auto& s=states[i];
        if(offset==0x184)s.visible=value;
        else if(offset==0x198)s.uses=value;
        else if(offset==0x1b0)s.marker=value;
        else objects[i].words[offset]=value;
    }
    float InteractionThreshold(std::uint32_t object) override{return std::bit_cast<float>(ReadWord(object+0x19c));}
    double PlanarMagnitude(float x,float z) override {
        static const auto tables=[]{pal4::GameMathTables value;(void)value.CalculateSquareRootTable();return value;}();
        return pal4::GameVectorMath::ApproximateMagnitude({x,0,z},tables);
    }
    void AddActiveSceneObject(std::uint32_t object,float seconds) override {
        Require(seconds==1,"unexpected GOB disappearance context");states[Index(object)].disappearance_requested=true;
    }
    std::uint32_t ProcessMarker() override{Refresh();return 1;}
    bool Enabled(std::size_t i) const{return states.at(i).visible!=0 && states[i].uses!=0 && objects[i].words.at(0x18c)!=0;}
    bool Within(std::size_t i,Point3 position,float radius,float* distance=nullptr) {
        return pal4::GobObjectBase::IsWithinInteractionBounds(Handle(i),{position[0],position[1],position[2]},radius,distance,*this);
    }
    unsigned SelectableAtomics(std::size_t i) const {
        if(!states[i].actor)return 0;
        const auto& bindings=arena.actors[*states[i].actor].actor.render_bindings;
        Require(bindings.has_value(),"GOB interaction needs original Atomic mapping");unsigned count{};
        for(std::size_t a=0;a<bindings->atomics.size();++a)
            if(!collision_eligibility || collision_eligibility(i,a))++count;
        return count;
    }
    Matrix Model(std::size_t i) const {
        const auto& state=states[i];auto matrix=InstanceMatrix(state.position,state.rotation);
        for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)matrix[c*4+r]*=state.scale;
        return matrix;
    }
    std::vector<Matrix> CollisionFrames(std::size_t i){
        std::vector<Vertex> ignored;std::vector<Matrix> frames;const auto& state=states[i];Require(state.actor.has_value(),"GOB collision model is unavailable");
        if(objects[i].playback)objects[i].playback->Sample(ignored,&frames);
        else SampleActorVertices(arena.actors[*state.actor].actor,-1,0,ignored,AnimationTimeMode::loop,&frames);
        return frames;
    }
    void ClearMovement(std::size_t i){
        auto& object=objects[i];if(movement_collision)for(auto handle:object.collision_shapes)if(handle)movement_collision->Remove(handle);
        object.collision_shapes.clear();object.collision_time=-1;object.collision_action.clear();states[i].movement_shapes=0;
    }
    std::uint32_t MovementAdapter() override{return 0x6000;}
    std::uint32_t RenderProperty(std::uint32_t object) override {
        const auto type=states[Index(object)].type;
        Require(type<=10,"unknown GOB movement callback class");
        return pal4::GobSound::ReturnZero();
    }
    std::uint32_t SetClumpProperties(std::uint32_t adapter,std::uint32_t clump,std::uint32_t mode,std::uint32_t hard,std::uint32_t proximity,std::uint32_t callback) override {
        Require(movement_collision && adapter==0x6000 && hard==1 && proximity==0 && callback==0,"invalid original GOB collision registration");
        const auto i=ClumpIndex(clump);auto& object=objects[i];const auto& state=states[i];Require(state.actor.has_value(),"GOB collision actor is missing");
        const auto& geometry=arena.actors[*state.actor].collision;Require(geometry.has_value(),"GOB collision cache is missing; rebuild arena cache v3");
        const auto frames=CollisionFrames(i);const auto model=Model(i);object.collision_shapes.assign(geometry->atomics.size(),0);
        try{for(std::size_t a=0;a<geometry->atomics.size();++a){
            if(geometry->atomics[a].triangles.empty())continue;
            CollisionObjectProperties properties;properties.owner=Handle(i);properties.mode=mode;properties.hard_block=hard!=0;properties.proximity=proximity!=0;properties.callback=callback;
            auto triangles=TransformObjectCollision(geometry->atomics[a],frames,model);
            object.collision_shapes[a]=movement_collision->Add(CollisionGroup::object,triangles,properties,[this,i,a]{return collision_eligibility(i,a);});
        }}catch(...){ClearMovement(i);throw;}
        object.collision_model=model;object.collision_time=object.playback?object.playback->Time():0;
        object.collision_action=object.playback?std::string(object.playback->Action()):std::string{};
        states[i].movement_shapes=std::count_if(object.collision_shapes.begin(),object.collision_shapes.end(),[](auto handle){return handle!=0;});return clump;
    }
    void RegisterMovement(std::size_t i){
        // 无模型的特效 GOB 只有变换根，没有可注册的碰撞几何。
        if(movement_collision && states[i].actor && objects[i].collision_shapes.empty())
            (void)pal4::GobSound::ApplyClumpAdapterProperties(Handle(i),*this);
        states[i].movement_registered=true;
    }
    void SyncMovement(std::size_t i){
        auto& object=objects[i];if(!movement_collision || object.collision_shapes.empty())return;
        const auto model=Model(i);const auto time=object.playback?object.playback->Time():0;
        const auto action=object.playback?std::string(object.playback->Action()):std::string{};
        if(model==object.collision_model && time==object.collision_time && action==object.collision_action)return;
        const auto frames=CollisionFrames(i);const auto& geometry=*arena.actors[*states[i].actor].collision;
        for(std::size_t a=0;a<object.collision_shapes.size();++a)if(object.collision_shapes[a])movement_collision->Replace(object.collision_shapes[a],TransformObjectCollision(geometry.atomics[a],frames,model));
        object.collision_model=model;object.collision_time=time;object.collision_action=action;
    }
    void DetachMovement(){
        if(!movement_collision)return;
        for(std::size_t i=0;i<objects.size();++i){ClearMovement(i);states[i].movement_backend_bound=false;}
        movement_collision=nullptr;collision_eligibility={};
    }
    float ReadFloat(std::string_view key,float fallback) override {
        const auto* attr=Attribute(arena.catalog.gobs[loading].config.Root(),key);return attr?attr->ToFloat(fallback):fallback;
    }
    std::int32_t ReadInt(std::string_view key,std::int32_t fallback) override {
        const auto* attr=Attribute(arena.catalog.gobs[loading].config.Root(),key);return attr?std::bit_cast<std::int32_t>(attr->ToInt(static_cast<std::uint32_t>(fallback))):fallback;
    }
    std::int32_t FloatToInteger(float value) override{return Integer(value);}
    void ApplyScale(std::uint32_t object,float scale,std::uint32_t mode) override {
        Require(mode==0 && std::isfinite(scale),"invalid GOB scale");states[Index(object)].scale=scale;
    }
    bool DialogNeedsUpdate(std::uint32_t handle) override{Require(handle==Handle(Index(handle))+0x1cc,"invalid GOB dialog handle");return false;}
    void UpdateDialogContent(std::uint32_t) override{throw std::logic_error("active GOB dialog effect is not bound");}
    bool EffectReadyToShow(std::uint32_t handle) override{Require(handle==Handle(Index(handle))+0x1d4,"invalid GOB effect handle");return false;}
    std::uint32_t RemoveEffect(std::uint32_t) override{throw std::logic_error("active GOB effect is not bound");}
    void SetBaseVisible(std::uint32_t object,std::uint32_t visible) override{(void)pal4::GobObjectBase::SetVisible(object,visible,*this);}
    std::uint32_t ClearAnimationFlag(std::uint32_t state,std::uint32_t flag) override{Animation(state).SetPause(flag!=0);return flag;}
    float AnimationLength(std::uint32_t state) override{return Animation(state).Duration();}
    void SetAnimationTime(std::uint32_t state,float time) override{Animation(state).Seek(time);}
    void SetObjectState(std::uint32_t object,std::uint32_t value) override {
        const auto index=Index(object);const auto type=states[index].type;
        if(type==6 && objects[index].playback)pal4::GobAction::SetVisibleAndAnimationState(object,value,*this);
        else if(type==8){
            Require(effects!=nullptr,"GOB effect playback owner is not bound");auto& target=objects[index];
            if(!target.effect){const auto config=ReadGobEffectParameters(arena.catalog.gobs[index].config);Require(config.has_value(),"GOB effect configuration is missing");target.initial_effect_issue=effects->SupportIssue(config->name);if(!target.initial_effect_issue.empty()){states[index].visible=value;target.initial_effect_pending=false;return;}const auto& state=states[index];auto matrix=InstanceMatrix(state.position,state.rotation);for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)matrix[c*4+r]*=state.scale;target.effect=std::make_unique<GobEffectInstance>(*config,matrix,*effects);}
            target.effect->SetVisible(value!=0);states[index].visible=value;target.initial_effect_pending=false;
        }else SetBaseVisible(object,value);
    }
    void Refresh() {
        std::vector<MinimapObject> snapshot;snapshot.reserve(states.size());
        for(const auto& s:states)snapshot.push_back({s.name,s.position,s.marker,s.visible,s.uses});
        minimap.RefreshObjects(snapshot);
    }
    std::uint32_t ObjectManager() override{return manager;}
    std::uint32_t FindByName(std::uint32_t owner,std::string_view name) override {
        Require(owner==manager,"invalid scene object manager");
        for(std::size_t i=0;i<states.size();++i)if(states[i].name==name)return Handle(i);
        return 0;
    }
    std::uint32_t ArenaManager() override{return world;}
    std::uint32_t MinimapManager() override{return map;}
    void RefreshDialog(std::uint32_t owner) override{Require(owner==map,"invalid scene minimap");Refresh();}
    void RemoveAnimatedObject(std::uint32_t owner,std::uint32_t object) override{Require(owner==world,"invalid GOB removal arena");(void)pal4::GameArenaScriptClumpCallbacks::RemoveAnimatedObject(owner,object,*this);}
    void AddAnimatedObject(std::uint32_t owner,std::uint32_t object) override{Require(owner==world,"invalid GOB addition arena");(void)pal4::GameArenaScriptClumpCallbacks::AddAnimatedObject(owner,object,*this);}
    std::uint32_t LogVisibility(std::uint32_t line,std::string_view name,std::uint32_t w,std::uint32_t s,std::uint8_t visible) override {
        Require(line==0x356 && w==1 && s==2,"invalid GOB visibility log");last_name=name;last_visible=visible!=0;++logs;return 0;
    }
    std::uint32_t Call(pal4::GameArenaScriptClumpCall operation,std::initializer_list<std::uint32_t> args) override {
        using Op=pal4::GameArenaScriptClumpCall;const auto first=args.size()?*args.begin():0;
        const auto second=args.size()>1?*(args.begin()+1):0;
        switch(operation){
        case Op::GetCameraSubsystem:return 0x5000;
        case Op::CleanupObject:if(states[Index(first)].marker)Refresh();return 0;
        case Op::Virtual156:SetObjectState(first,second);return 0;
        case Op::Virtual72:RegisterMovement(Index(first));return 0;
        case Op::Virtual76:ClearMovement(Index(first));states[Index(first)].movement_registered=false;return 0;
        case Op::Virtual140:return ReadWord(first+0x194);
        case Op::AddClump:Require(first==world,"invalid GOB world attach");states[ClumpIndex(second)].world_attached=true;return second;
        case Op::RemoveClump:Require(first==world,"invalid GOB world detach");states[ClumpIndex(second)].world_attached=false;return second;
        case Op::ProcessClumpAtomics:Require(second==world+300,"invalid GOB geometry context");states[ClumpIndex(first)].geometry_bound=true;return first;
        case Op::ProcessAtomics:states[ClumpIndex(first)].material_bound=true;return first;
        case Op::ProcessAtomicsFinal:states[ClumpIndex(first)].render_bound=true;return first;
        case Op::ResetAtomicCallbacks:states[ClumpIndex(first)].render_bound=false;return first;
        case Op::ResetMaterialCallbacks:states[ClumpIndex(first)].material_bound=false;return first;
        case Op::ProcessClumpAtomicsWrapper:states[ClumpIndex(first)].geometry_bound=false;return first;
        case Op::AddCameraClump:Require(first==0x5000,"invalid GOB camera registration");states[ClumpIndex(second)].camera_registered=true;return second;
        case Op::RemoveCameraClump:Require(first==0x5000,"invalid GOB camera removal");states[ClumpIndex(second)].camera_registered=false;return second;
        default:throw std::logic_error("scene object callback is not bound");
        }
    }
    void UpdateBase(std::uint32_t object,float) override{Require(states[Index(object)].type==6,"invalid GOBAction update");}
    void UpdateInterpolator(std::uint32_t state,float seconds) override{Animation(state).Advance(seconds);}
    std::uint32_t AnimationFlag(std::uint32_t state) override{return Animation(state).Completed();}
};
SceneObjects::SceneObjects(const ArenaResources& arena,SceneMinimap& minimap,GobMemberReader read,EffectPlayback* effects):impl_(std::make_unique<Impl>(arena,minimap,std::move(read),effects)) {
    auto& s=*impl_;Require(arena.catalog.gobs.size()<=65535 && bool(s.read),"invalid scene object resources");
    s.states.resize(arena.catalog.gobs.size());s.objects.resize(s.states.size());
    for(std::size_t i=0;i<s.states.size();++i){
        const auto& source=arena.catalog.gobs[i];auto& state=s.states[i];auto& object=s.objects[i];s.loading=i;
        state.name=source.name;state.type=source.type;state.visible=source.flags[0];state.active=source.type==3?0:1;
        object.initial_effect_pending=state.type==8 && state.visible!=0;
        state.position=source.position;state.rotation=source.rotation;object.words[0x188]=source.flags[1];object.words[0x18c]=source.flags[2];object.words[0x19c]=source.flags[3];
        pal4::GobObjectBase::ApplyParameters(Impl::Handle(i),s);
        for(std::size_t a=0;a<arena.actors.size();++a)if(arena.actors[a].gob_type && arena.actors[a].name==source.name){state.actor=a;break;}
        if(state.type==6 && state.actor){
            const auto bytes=s.read(source.resource_directory,source.model+".tab");
            if(bytes.has_value()){
                const auto table=ReadAnimationTable(*bytes);
                if(!table.empty()){
                    object.initial_alias=table.front().first;object.initial_resource=table.front().second;
                    const auto& resource=object.initial_resource;Require(resource.ends_with(".anm") && resource.find_first_of("/\\") ==std::string::npos,"unsupported GOBAction animation resource");
                    const auto clip=resource.substr(0,resource.size()-4),directory=source.resource_directory,model=source.model;
                    object.playback=std::make_unique<ActorPlayback>(arena.actors[*state.actor].actor,model,[&s,directory,model](std::string_view member){
                        const auto prefix=model+"\\";Require(member.starts_with(prefix),"GOBAction message prefix mismatch");return s.read(directory,member.substr(prefix.size()));
                    });
                    if(object.playback->Select(clip))object.playback->SetRestart(true);
                    else object.playback.reset();
                }
            }
            object.words[0x258]=static_cast<std::uint32_t>(s.ReadInt("PAL4_GameObject-action-default-play",1));
            object.words[0x25c]=static_cast<std::uint32_t>(s.ReadInt("PAL4_GameObject-action-play-times",0));
            object.words[0x260]=static_cast<std::uint32_t>(s.ReadInt("PAL4_GameObject-action-holding-end",0));
            s.SetObjectState(Impl::Handle(i),state.visible);
        }
    }
    // 所有对象先形成统一集合，再运行原增删回调，供marker遍历读取。
    for(std::size_t i=0;i<s.states.size();++i)if(s.states[i].actor && s.states[i].type!=8)s.AddAnimatedObject(Impl::world,Impl::Handle(i));
    s.Refresh();
}
SceneObjects::~SceneObjects(){impl_->DetachMovement();}
void SceneObjects::BindMovement(SceneCollision& collision,CollisionEligibility eligibility){
    auto& s=*impl_;Require(!s.movement_collision && bool(eligibility),"GOB movement owner is already bound or eligibility is missing");
    for(const auto& state:s.states)if(state.actor && state.type!=8){const auto& actor=s.arena.actors[*state.actor];Require(actor.collision.has_value(),"GOB collision cache is missing; rebuild arena cache v3");ValidateObjectCollision(*actor.collision,actor.actor);}
    s.movement_collision=&collision;s.collision_eligibility=std::move(eligibility);
    try{for(std::size_t i=0;i<s.states.size();++i)if(s.states[i].actor && s.states[i].type!=8){s.states[i].movement_backend_bound=true;if(s.states[i].movement_registered)s.RegisterMovement(i);}}
    catch(...){s.DetachMovement();throw;}
}
void SceneObjects::UnbindMovement(){impl_->DetachMovement();}
void SceneObjects::ActivateInitialEffects(){
    auto& s=*impl_;Require(s.effects!=nullptr,"initial effects need a playback owner");
    for(std::size_t i=0;i<s.states.size();++i){
        auto& object=s.objects[i];if(!object.initial_effect_pending)continue;
        const auto config=ReadGobEffectParameters(s.arena.catalog.gobs[i].config);
        Require(config.has_value(),"initial GOB effect configuration is missing");
        object.initial_effect_issue=s.effects->SupportIssue(config->name);
        if(!object.initial_effect_issue.empty())continue;
        s.SetObjectState(Impl::Handle(i),s.states[i].visible);
    }
}
bool SceneObjects::SetVisible(std::string_view name,bool visible) {
    Require(name.find('\0')==std::string_view::npos,"embedded NUL in GOB name");++impl_->calls;
    if(!impl_->FindByName(Impl::manager,name))return false;
    const std::string text(name);(void)pal4::ScriptObjectVisibility::SetVisible(text.c_str(),visible,*impl_);++impl_->matched;return true;
}
bool SceneObjects::IsVisible(std::string_view name) const {
    Require(name.find('\0')==std::string_view::npos,"embedded NUL in GOB name");
    const auto object=impl_->FindByName(Impl::manager,name);
    return object!=0 && impl_->ReadWord(object+0x184u)!=0;
}
bool SceneObjects::ResetConfiguration(std::string_view name) {
    auto& s=*impl_;Require(name.find('\0')==std::string_view::npos,"embedded NUL in GOB name");
    const auto object=s.FindByName(Impl::manager,name);
    if(!object)return false;
    const auto index=s.Index(object);
    const auto type=s.states[index].type;
    if(type>10u)
        throw std::logic_error("GOB configuration reset type is not implemented: "+std::to_string(type));
    const auto previous=s.loading;
    s.loading=index;
    try {
        pal4::GobObjectBase::ApplyParameters(object,s);
    } catch (...) {
        s.loading=previous;
        throw;
    }
    s.loading=previous;
    return true;
}
bool SceneObjects::SetResearch(std::string_view name,bool enabled) {
    auto& s=*impl_;Require(name.find('\0')==std::string_view::npos,"invalid GOB research name");
    struct Runtime final:pal4::IScriptObjectResearchRuntime {
        Impl& owner;std::string_view name;
        Runtime(Impl& o,std::string_view n):owner(o),name(n){}
        std::uint32_t ReadWord(std::uint32_t address) override{Require(address==0x7004,"GOB research text handle mismatch");return 0x7100;}
        std::uint32_t EmptyStringAddress() override{return 0x7100;}
        std::uint32_t ObjectManager() override{return Impl::manager;}
        std::uint32_t FindByName(std::uint32_t manager,std::uint32_t text) override{Require(text==0x7100,"GOB research name mismatch");return owner.FindByName(manager,name);}
        void WriteWord(std::uint32_t address,std::uint32_t value) override{Require(address-Impl::Handle(owner.Index(address))==0x18c,"GOB research write offset mismatch");owner.WriteWord(address,value);}
    } runtime(s,name);
    return pal4::ScriptObjectResearch::Set(0x7000,enabled,runtime)!=0;
}
std::optional<std::size_t> SceneObjects::FindInteraction(Point3 position,float radius) {
    auto& s=*impl_;for(auto value:position)Require(std::isfinite(value),"invalid GOB interaction position");Require(std::isfinite(radius) && radius>=0,"invalid GOB interaction height");
    struct Query final:pal4::IPalGameObjectManagerVisitRuntime {
        Impl& owner;Point3 position;float radius;std::vector<std::size_t> order;
        Query(Impl& o,Point3 p,float r):owner(o),position(p),radius(r),order(o.states.size()){
            std::iota(order.begin(),order.end(),0);std::sort(order.begin(),order.end(),[&](auto a,auto b){return owner.states[a].name<owner.states[b].name;});
        }
        std::uint32_t FirstNode(std::uint32_t) override{return 0;}
        std::uint32_t EndNode(std::uint32_t) override{return static_cast<std::uint32_t>(order.size());}
        std::uint32_t NextNode(std::uint32_t n) override{return n+1;}
        std::uint32_t NodeObject(std::uint32_t n) override{return Impl::Handle(order.at(n));}
        std::uint32_t ReadWord(std::uint32_t a) override{return owner.ReadWord(a);}
        void WriteWord(std::uint32_t a,std::uint32_t value) override{owner.WriteWord(a,value);}
        std::uint32_t ClearObjectTree(std::uint32_t) override{throw std::logic_error("GOB selection cannot clear objects");}
        void ReleaseObjectResources(std::uint32_t) override{throw std::logic_error("GOB selection cannot release objects");}
        void DestroyObject(std::uint32_t,std::uint32_t) override{throw std::logic_error("GOB selection cannot destroy objects");}
        void DeactivateObject(std::uint32_t) override{throw std::logic_error("GOB selection cannot deactivate objects");}
        void ActivateObject(std::uint32_t) override{throw std::logic_error("GOB selection cannot activate objects");}
        void UpdateObject(std::uint32_t,std::uint32_t) override{throw std::logic_error("GOB selection cannot update objects");}
        void UpdateObjectSecond(std::uint32_t,std::uint32_t) override{throw std::logic_error("GOB selection cannot update objects");}
        std::string_view ObjectName(std::uint32_t o) override{return owner.states[owner.Index(o)].name;}
        bool IsSelectable(std::uint32_t o) override{return owner.Enabled(owner.Index(o));}
        bool TryPointDistance(std::uint32_t,const pal4::GameObjectPoint&,float&) override{throw std::logic_error("unexpected GOB point query");}
        std::uint32_t MaterialCount(std::uint32_t clump) override{return clump?owner.SelectableAtomics(owner.ClumpIndex(clump)):0;}
        std::int32_t ObjectType(std::uint32_t o) override{return static_cast<std::int32_t>(owner.states[owner.Index(o)].type);}
        bool TryRayDistance(std::uint32_t object,std::uint32_t first,std::uint32_t second,float& distance) override {
            Require(first==1 && std::bit_cast<float>(second)==radius,"GOB coordinate query mismatch");return owner.Within(owner.Index(object),position,radius,&distance);
        }
    } query(s,position,radius);
    const auto selected=pal4::PalGameObjectManagerVisit::FindClosestRay(Impl::manager,1,std::bit_cast<std::uint32_t>(radius),false,-1,query);
    return selected?std::optional(s.Index(selected)):std::nullopt;
}
bool SceneObjects::InteractionEnabled(std::size_t i) const{return impl_->Enabled(i);}
bool SceneObjects::InteractionInRange(std::size_t i,Point3 position,float radius) const{return impl_->Enabled(i) && impl_->Within(i,position,radius);}
std::int32_t SceneObjects::InteractionEvent(std::size_t i) const{return std::bit_cast<std::int32_t>(impl_->objects.at(i).words.at(0x1b4));}
bool SceneObjects::Interact(std::size_t index,ISceneEventScripts& scripts) {
    auto& s=*impl_;if(!s.Enabled(index))return false;
    Require(s.states[index].type<=10,"interaction for this GOB class is not bound");
    struct Runtime final:pal4::IGobNormalScriptRuntime {
        Impl& owner;ISceneEventScripts& scripts;std::size_t index;std::string declaration;
        Runtime(Impl& o,ISceneEventScripts& h,std::size_t i):owner(o),scripts(h),index(i){}
        std::uint32_t ReadWord(std::uint32_t address) override {
            if(address==Impl::Handle(index)+0x17c)return static_cast<std::uint32_t>(Entry().size());
            if(address==Impl::Handle(index)+0x178)return 1;
            return owner.ReadWord(address);
        }
        std::string_view Entry() const{return owner.arena.catalog.gobs.at(index).script;}
        std::string_view ReadCString(std::uint32_t text) override{Require(text==1,"GOB script text handle mismatch");return Entry();}
        void CleanupScriptPackages() override{}
        void InitializeScript() override{}
        std::string CurrentSceneScriptPath() override{return owner.arena.description.world;}
        bool ScriptCacheContains(std::string_view key) override {
            const auto entry=Entry();Require(!entry.empty() && entry.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")==std::string_view::npos,"invalid GOB script entry");
            Require(key==CurrentSceneScriptPath()+"["+std::string(entry)+"]","GOB script cache key mismatch");declaration="void "+std::string(entry)+"()";return scripts.Contains(CurrentSceneScriptPath(),declaration);
        }
        std::uint32_t CreateScriptInterpreter(std::string_view path,std::string_view,std::string_view entry) override{Require(path==CurrentSceneScriptPath() && entry==Entry(),"GOB script request mismatch");return 1;}
        bool AddScriptModule(std::uint32_t interpreter) override{Require(interpreter==1,"GOB interpreter mismatch");scripts.LoadWorld(CurrentSceneScriptPath());return true;}
        void PushStackModule(std::uint32_t interpreter) override{Require(interpreter==1,"GOB stack interpreter mismatch");(void)scripts.Push(CurrentSceneScriptPath(),declaration);}
        void ReleaseInterpreter(std::uint32_t) override{}
        void ShowScriptNotFound() override{throw std::runtime_error("GOB original script interpreter is missing");}
        std::uint32_t CleanupScriptPath() override{return 0;}
        std::uint32_t ConsumeResearchUse(std::uint32_t object) override{return pal4::GobObjectBase::ConsumeResearchUse(object,owner);}
    } runtime(s,scripts,index);
    (void)pal4::GobNormal::RunScriptAndConsumeUse(Impl::Handle(index),runtime);return true;
}
void SceneObjects::Advance(float seconds) {
    Require(std::isfinite(seconds) && seconds>=0,"invalid scene object elapsed");
    for(std::size_t i=0;i<impl_->states.size();++i)if(impl_->states[i].type==6 && impl_->states[i].active && impl_->objects[i].playback)
        (void)pal4::GobAction::Update(Impl::Handle(i),seconds,static_cast<pal4::IGobActionUpdateRuntime&>(*impl_));
    for(std::size_t i=0;i<impl_->states.size();++i)impl_->SyncMovement(i);
    ++impl_->updates;
}
std::span<const SceneObjectState> SceneObjects::States() const{return impl_->states;}
const ActorPlayback* SceneObjects::Playback(std::size_t index) const{return impl_->objects.at(index).playback.get();}
bool SceneObjects::Drawable(std::size_t index) const {
    const auto& state=impl_->states.at(index);
    return state.actor && (state.type==0 || state.type==6 || state.type==7) && state.visible && state.active && state.world_attached && state.geometry_bound && state.render_bound;
}
void SceneObjects::Sample(std::size_t index,std::vector<Vertex>& vertices,std::vector<Matrix>* frames) {
    const auto& state=impl_->states.at(index);Require(state.actor.has_value(),"scene object has no mesh");
    if(auto* playback=impl_->objects[index].playback.get())playback->Sample(vertices,frames);
    else SampleActorVertices(impl_->arena.actors[*state.actor].actor,-1,0,vertices,AnimationTimeMode::loop,frames);
}
void SceneObjects::WriteReport(std::ostream& out) const {
    const auto& s=*impl_;out<<"{\"calls\":"<<s.calls<<",\"matched\":"<<s.matched<<",\"logs\":"<<s.logs<<",\"updates\":"<<s.updates<<",\"last_name\":"<<std::quoted(s.last_name)<<",\"last_visible\":"<<(s.last_visible?"true":"false")<<",\"objects\":[";
    for(std::size_t i=0;i<s.states.size();++i){if(i)out<<',';const auto& state=s.states[i];
        out<<"{\"name\":"<<std::quoted(state.name)<<",\"type\":"<<state.type<<",\"visible\":"<<state.visible<<",\"active\":"<<state.active<<",\"marker\":"<<state.marker<<",\"uses\":"<<state.uses
            <<",\"disappearance_requested\":"<<(state.disappearance_requested?"true":"false")<<",\"initial_effect_pending\":"<<(s.objects[i].initial_effect_pending?"true":"false")<<",\"effect_runtime_bound\":"<<(s.objects[i].effect?"true":"false")<<",\"world\":"<<(state.world_attached?"true":"false")<<",\"movement\":"<<(state.movement_registered?"true":"false")<<",\"movement_backend_bound\":"<<(state.movement_backend_bound?"true":"false")<<",\"movement_shapes\":"<<state.movement_shapes<<",\"camera\":"<<(state.camera_registered?"true":"false")<<",\"drawable\":"<<(Drawable(i)?"true":"false")<<",\"initial_alias\":"<<std::quoted(s.objects[i].initial_alias)<<",\"initial_resource\":"<<std::quoted(s.objects[i].initial_resource)<<",\"playback\":";
        if(s.objects[i].playback)s.objects[i].playback->WriteReport(out,false);else out<<"null";
        out<<",\"initial_effect_issue\":"<<std::quoted(s.objects[i].initial_effect_issue)<<'}';
    }out<<"],\"minimap\":";s.minimap.WriteReport(out);out<<'}';
}
}
