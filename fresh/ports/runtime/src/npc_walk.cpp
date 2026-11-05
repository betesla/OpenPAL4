#include "pal4/portable/npc_walk.h"
#include "pal4/portable/scene_npcs.h"
#include "pal4/portable/surface_motion.h"
#include "pal4/script_player_walk.h"
#include "pal4/base_object_motion.h"
#include "pal4/game_vector_math.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
namespace {
void Require(bool v,const char* message){if(!v)throw std::logic_error(message);}
void Json(std::ostream& out,Point3 p){out<<'['<<p[0]<<','<<p[1]<<','<<p[2]<<']';}
const pal4::GameMathTables& Math(){static const auto t=[]{pal4::GameMathTables value;(void)value.CalculateSquareRootTable();return value;}();return t;}
}
struct NpcWalks::Impl final:pal4::IScriptPlayerWalkRuntime,pal4::IBaseObjectMotionRuntime {
    struct Handler {pal4::PlayerWalkWaitState state;ScriptToken token;std::string name;bool wait{},closed{};};
    SceneNpcs& npcs;const Scene& world;
    CollisionSurface surface;std::vector<std::unique_ptr<SurfaceMotion>> motions;
    std::vector<unsigned> status;std::vector<float> speeds;unsigned current{},registrations{},calls{},end_calls{},completed{},blocking_completed{},frames{},moved_frames{};
    float elapsed{};Point3 requested{},projected{};ScriptToken request_token{};
    std::vector<NpcWalkCompletion> completions;
    std::unique_ptr<Handler> pending;std::vector<std::unique_ptr<Handler>> handlers;
    Impl(SceneNpcs& n,const CollisionMesh& collision,const Scene& scene):npcs(n),world(scene),surface(collision.floor),motions(n.States().size()),status(n.States().size()),speeds(n.States().size()){}
    unsigned Index(unsigned actor) const{Require(actor>=0x100000 && (actor-0x100000)%0x1000==0 && (actor-0x100000)/0x1000<npcs.States().size(),"invalid NPC walk handle");return (actor-0x100000)/0x1000;}
    unsigned PlayerManager() override{throw std::logic_error("player walk is not bound to NPC owner");}
    unsigned PlayerByIndex(unsigned,int) override{throw std::logic_error("player walk is not bound to NPC owner");}
    unsigned CurrentPlayer(unsigned) override{throw std::logic_error("player walk is not bound to NPC owner");}
    std::string_view ActorName(unsigned actor) override{return npcs.States()[Index(actor)].name;}
    unsigned NpcManager() override{return 0x9000;}
    unsigned NpcByName(unsigned owner,std::string_view name) override{Require(owner==0x9000,"invalid NPC walk manager");const auto slots=npcs.States();for(unsigned i=0;i<slots.size();++i)if(slots[i].name==name)return 0x100000+i*0x1000;return 0;}
    pal4::PlayerWalkPoint ReadPosition(unsigned actor) override{const auto p=npcs.States()[Index(actor)].position;return {p[0],p[1],p[2]};}
    pal4::PlayerWalkPoint ConvertPoint(const pal4::PlayerWalkPoint& p) override{return p;}
    unsigned ArenaManager() override{return 0x2000;}
    unsigned PlanRoute(unsigned source,pal4::PlayerWalkPoint& target) override{Require(source==0x2194,"invalid walk route source");auto p=Point3{target.x,target.y,target.z};const auto face=surface.Snap(p);projected=p;target={p[0],p[1],p[2]};return face;}
    SurfaceMotion& Motion(unsigned movement){const auto i=Index(movement-pal4::ScriptPlayerWalk::kMovementOffset);Require(bool(motions[i]),"walk collision actor is not initialized");return *motions[i];}
    bool StartMovement(unsigned movement,const pal4::PlayerWalkPoint& from,const pal4::PlayerWalkPoint& to,unsigned face,unsigned mode) override {
        Require(mode==0,"alternate movement mode is not bound");const auto i=Index(movement-pal4::ScriptPlayerWalk::kMovementOffset);current=i;
        if(!motions[i]){const float radius=npcs.MovementRadius(i);motions[i]=std::make_unique<SurfaceMotion>(surface,Point3{from.x,from.y,from.z},radius,[this](Point3 a,Point3 b){return WorldSegmentBlocked(world,a,b);});}
        return motions[i]->Start({from.x,from.y,from.z},{to.x,to.y,to.z},face);
    }
    void SetMovementSpeed(unsigned movement,float speed) override{Motion(movement).SetSpeed(speed);speeds[Index(movement-pal4::ScriptPlayerWalk::kMovementOffset)]=speed;}
    void SetMovementStatus(unsigned actor,unsigned value) override{status[Index(actor)]=value;}
    std::string_view ActionName(unsigned actor) override {auto* playback=npcs.Playback(Index(actor));Require(playback!=nullptr,"NPC movement requires animation owner");return playback->Action();}
    int SetAction(unsigned actor,std::string_view action) override{auto* playback=npcs.Playback(Index(actor));Require(playback!=nullptr,"NPC movement requires animation owner");return playback->Select(action)?1:0;}
    pal4::PlayerWalkWaitState* Allocate(unsigned size) override{Require(size==sizeof(pal4::PlayerWalkWaitState) && !pending,"invalid walk handler allocation");pending=std::make_unique<Handler>();pending->token=request_token;return &pending->state;}
    unsigned ScriptExecutor() override{return 0x3000;}
    void CheckPending(const pal4::PlayerWalkWaitState& s){Require(pending && &pending->state==&s,"invalid pending walk handler");}
    void Construct(pal4::PlayerWalkWaitState& s,std::uint8_t wait,unsigned queue) override{CheckPending(s);Require(wait<=1 && queue==0x3054,"invalid walk wait queue");pending->wait=wait!=0;}
    void AssignName(pal4::PlayerWalkWaitState& s,std::string_view name) override{CheckPending(s);pending->name=name;}
    void CleanupWeaponEffects(unsigned actor) override{npcs.CleanupWeaponEffects(Index(actor));}
    void Flush(pal4::PlayerWalkWaitState& s) override{CheckPending(s);}
    void AddHandler(unsigned queue,pal4::PlayerWalkWaitState* s) override{Require(s!=nullptr,"null walk handler");CheckPending(*s);Require(queue==0x3054 && s->vtable_address==pal4::ScriptPlayerWalk::kVtable && s->phase==1,"invalid walk handler");for(const auto& old:handlers)if(old->name==pending->name && old->wait)completions.push_back({old->token,"NPC walk replaced"});std::erase_if(handlers,[&](const auto& old){return old->name==pending->name;});handlers.push_back(std::move(pending));++registrations;}
    unsigned CleanupName() override{return 0;}
    unsigned ShowPathError() override{npcs.SetMotionPosition(current,projected);return 0;}
    bool MovementActive(unsigned movement) override{const auto i=Index(movement-pal4::ScriptPlayerWalk::kMovementOffset);return motions[i] && motions[i]->Active();}
    void StopMovement(unsigned movement) override{Motion(movement).Stop();}
    void Close(pal4::PlayerWalkWaitState& s) override{for(auto& h:handlers)if(&h->state==&s){h->closed=true;return;}throw std::logic_error("walk handler is not registered");}
    void NormalizeAndClamp(pal4::BaseObjectVector3& p) override{pal4::GameVector3 v{p.x,p.y,p.z};pal4::GameVectorMath::NormalizeAndClamp(v,Math());p={v[0],v[1],v[2]};}
    double AngleFromCosine(float cosine) override{return Math().ApproximateArccosine(cosine);}
    void SetPose(pal4::BaseObjectState&,const pal4::BaseObjectVector3& p,unsigned mode) override{Require(mode==0,"invalid motion pose mode");npcs.SetMotionRotation(current,{p.x,p.y,p.z});}
    unsigned SetDirection(pal4::BaseObjectState&,const pal4::BaseObjectVector3& p) override{npcs.SetMotionPosition(current,{p.x,p.y,p.z});++moved_frames;return 1;}
    void Apply(unsigned index) {
        current=index;pal4::BaseObjectState base;const auto p=motions[index]->Position(),v=motions[index]->Velocity(),rotation=npcs.States()[index].rotation;
        base.Write(0x604,status[index]);for(unsigned axis=0;axis<3;++axis){base.Write(0x570+axis*4,std::bit_cast<unsigned>(p[axis]));base.Write(0x57c+axis*4,std::bit_cast<unsigned>(v[axis]));base.Write(0xa0+axis*4,std::bit_cast<unsigned>(rotation[axis]));}
        (void)pal4::BaseObjectMotion::ApplyTransform(base,0x100000+index*0x1000,*this);
    }
};
NpcWalks::NpcWalks(SceneNpcs& n,const CollisionMesh& collision,const Scene& world):impl_(std::make_unique<Impl>(n,collision,world)){n.BindScriptMotion();}
NpcWalks::~NpcWalks(){impl_->npcs.UnbindScriptMotion();}
ScriptResponse NpcWalks::Start(ScriptToken token,std::string_view name,Point3 target,bool run,bool wait){
    Require(name.find('\0')==std::string_view::npos,"NUL in NPC walk name");for(float v:target)Require(std::isfinite(v),"invalid NPC walk target");
    auto& s=*impl_;++s.calls;s.requested=target;s.request_token=token;const auto actor=s.NpcByName(0x9000,name);
    if(!actor)return {};
    Require(s.npcs.HasAction(s.Index(actor),run?"C03":"C02") || s.npcs.HasAction(s.Index(actor),"Z02"),"original NPC movement clip is missing from cache");
    const auto before=s.registrations;if(run)(void)pal4::ScriptPlayerWalk(s).RunNpc(name,{target[0],target[1],target[2]},wait);else (void)pal4::ScriptPlayerWalk(s).WalkNpc(name,{target[0],target[1],target[2]},wait);
    return {{},wait && s.registrations>before};
}
ScriptResponse NpcWalks::End(ScriptToken token,std::string_view name){auto& s=*impl_;++s.end_calls;s.request_token=token;const auto before=s.registrations;(void)pal4::ScriptPlayerWalk(s).EndMoveNpc(name);return {{},s.registrations>before};}
std::vector<NpcWalkCompletion> NpcWalks::Advance(float elapsed){
    Require(std::isfinite(elapsed) && elapsed>=0,"invalid walk frame time");auto& s=*impl_;++s.frames;
    for(unsigned i=0;i<s.motions.size();++i)if(s.npcs.States()[i].active && s.motions[i] && s.motions[i]->Active()){s.motions[i]->Advance(elapsed);s.Apply(i);s.elapsed+=elapsed;}
    auto result=std::move(s.completions);s.completions.clear();
    for(auto& h:s.handlers){if(!h->closed)pal4::ScriptPlayerWalk(s).Update(h->state);if(h->closed){++s.completed;if(h->wait){++s.blocking_completed;result.push_back({h->token,{}});}}}
    std::erase_if(s.handlers,[](auto& h){return h->closed;});return result;
}
bool NpcWalks::Requested() const{return impl_->calls!=0 || impl_->end_calls!=0;}
void NpcWalks::WriteReport(std::ostream& out) const {
    const auto& s=*impl_;out<<std::setprecision(9)<<"{\"calls\":"<<s.calls<<",\"end_calls\":"<<s.end_calls<<",\"completed\":"<<s.completed<<",\"blocking_completed\":"<<s.blocking_completed<<",\"handlers\":"<<s.handlers.size()<<",\"frames\":"<<s.frames<<",\"moved_frames\":"<<s.moved_frames<<",\"elapsed\":"<<s.elapsed<<",\"linked_edges\":"<<s.surface.LinkedEdges()<<",\"requested\":";Json(out,s.requested);out<<",\"projected\":";Json(out,s.projected);out<<",\"npcs\":[";
    for(unsigned i=0;i<s.npcs.States().size();++i){if(i)out<<',';out<<"{\"position\":";Json(out,s.npcs.States()[i].position);out<<",\"rotation\":";Json(out,s.npcs.States()[i].rotation);out<<",\"motion\":";if(!s.motions[i]){out<<"null}";continue;}const auto& m=*s.motions[i];out<<"{\"active\":"<<(m.Active()?"true":"false")<<",\"status\":"<<m.Status()<<",\"face\":"<<m.Face()<<",\"position\":";Json(out,m.Position());out<<",\"speed\":"<<s.speeds[i]<<",\"velocity\":";Json(out,m.Velocity());out<<",\"path\":[";const auto route=m.Path();for(std::size_t j=0;j<route.size();++j){if(j)out<<',';out<<"{\"position\":";Json(out,route[j].position);out<<",\"face\":"<<route[j].face<<'}';}out<<"]}}";}out<<"]}";
}
}
