#include "pal4/portable/player_walk.h"
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
struct PlayerWalks::Impl final:pal4::IScriptPlayerWalkRuntime,pal4::IBaseObjectMotionRuntime {
    struct Handler {pal4::PlayerWalkWaitState state;ScriptToken token;std::string name;bool wait{},closed{};};
    PlayerRoster& players;PlayerActions& actions;const Scene& world;PlayerWalkMoved moved;
    CollisionSurface surface;std::array<std::unique_ptr<SurfaceMotion>,4> motions;
    std::array<unsigned,4> status{};unsigned current{},calls{},end_calls{},completed{},blocking_completed{},frames{},moved_frames{};
    float elapsed{};Point3 requested{},projected{};ScriptToken request_token{};
    std::unique_ptr<Handler> pending;std::vector<std::unique_ptr<Handler>> handlers;
    Impl(PlayerRoster& p,PlayerActions& a,const CollisionMesh& collision,const Scene& scene,PlayerWalkMoved m):players(p),actions(a),world(scene),moved(std::move(m)),surface(collision.floor){Require(bool(moved),"walk movement services are not bound");}
    unsigned Index(unsigned actor) const{Require(actor>=0x10000 && (actor-0x10000)%0xb14==0 && (actor-0x10000)/0xb14<4 && players.Prepared(),"invalid walk actor");return (actor-0x10000)/0xb14;}
    unsigned PlayerManager() override{return 0x8000;}
    unsigned PlayerByIndex(unsigned manager,int index) override{Require(manager==0x8000,"invalid walk player manager");return players.Prepared() && index>=0 && index<4?0x10000+static_cast<unsigned>(index)*0xb14:0;}
    unsigned CurrentPlayer(unsigned manager) override{return PlayerByIndex(manager,static_cast<int>(players.Leader()));}
    std::string_view ActorName(unsigned actor) override{return players.Slots()[Index(actor)].name;}
    unsigned NpcManager() override{throw std::logic_error("NPC walk is not bound");}
    unsigned NpcByName(unsigned,std::string_view) override{throw std::logic_error("NPC walk is not bound");}
    pal4::PlayerWalkPoint ReadPosition(unsigned actor) override{const auto p=players.Slots()[Index(actor)].position;return {p[0],p[1],p[2]};}
    pal4::PlayerWalkPoint ConvertPoint(const pal4::PlayerWalkPoint& p) override{return p;}
    unsigned ArenaManager() override{return 0x2000;}
    unsigned PlanRoute(unsigned source,pal4::PlayerWalkPoint& target) override{Require(source==0x2194,"invalid walk route source");auto p=Point3{target.x,target.y,target.z};const auto face=surface.Snap(p);projected=p;target={p[0],p[1],p[2]};return face;}
    SurfaceMotion& Motion(unsigned movement){const auto i=Index(movement-pal4::ScriptPlayerWalk::kMovementOffset);Require(bool(motions[i]),"walk collision actor is not initialized");return *motions[i];}
    bool StartMovement(unsigned movement,const pal4::PlayerWalkPoint& from,const pal4::PlayerWalkPoint& to,unsigned face,unsigned mode) override {
        Require(mode==0,"alternate movement mode is not bound");const auto i=Index(movement-pal4::ScriptPlayerWalk::kMovementOffset);current=i;
        if(!motions[i]){const auto sphere=ActorBindSphere(*players.Slots()[i].actor);Require(sphere.has_value(),"walk actor radius is missing");motions[i]=std::make_unique<SurfaceMotion>(surface,Point3{from.x,from.y,from.z},(*sphere)[3],[this](Point3 a,Point3 b){return WorldSegmentBlocked(world,a,b);});}
        return motions[i]->Start({from.x,from.y,from.z},{to.x,to.y,to.z},face);
    }
    void SetMovementSpeed(unsigned movement,float speed) override{Motion(movement).SetSpeed(speed);}
    void SetMovementStatus(unsigned actor,unsigned value) override{status[Index(actor)]=value;}
    std::string_view ActionName(unsigned actor) override {const auto i=Index(actor);if(const auto* playback=actions.Playback(i))return playback->Action();return players.Slots()[i].animation;}
    int SetAction(unsigned actor,std::string_view action) override{return actions.SelectTaskAction(Index(actor),action)?1:0;}
    pal4::PlayerWalkWaitState* Allocate(unsigned size) override{Require(size==sizeof(pal4::PlayerWalkWaitState) && !pending,"invalid walk handler allocation");pending=std::make_unique<Handler>();pending->token=request_token;return &pending->state;}
    unsigned ScriptExecutor() override{return 0x3000;}
    void CheckPending(const pal4::PlayerWalkWaitState& s){Require(pending && &pending->state==&s,"invalid pending walk handler");}
    void Construct(pal4::PlayerWalkWaitState& s,std::uint8_t wait,unsigned queue) override{CheckPending(s);Require(wait<=1 && queue==0x3054,"invalid walk wait queue");pending->wait=wait!=0;}
    void AssignName(pal4::PlayerWalkWaitState& s,std::string_view name) override{CheckPending(s);pending->name=name;}
    void CleanupWeaponEffects(unsigned actor) override{actions.CleanupTaskWeaponEffects(Index(actor));}
    void Flush(pal4::PlayerWalkWaitState& s) override{CheckPending(s);}
    void AddHandler(unsigned queue,pal4::PlayerWalkWaitState* s) override{Require(s!=nullptr,"null walk handler");CheckPending(*s);Require(queue==0x3054 && s->vtable_address==pal4::ScriptPlayerWalk::kVtable && s->phase==1,"invalid walk handler");handlers.push_back(std::move(pending));}
    unsigned CleanupName() override{return 0;}
    unsigned ShowPathError() override{players.SetPosition(current,projected);moved(current,players.Slots()[current].position,players.Slots()[current].rotation);return 0;}
    bool MovementActive(unsigned movement) override{const auto i=Index(movement-pal4::ScriptPlayerWalk::kMovementOffset);return motions[i] && motions[i]->Active();}
    void StopMovement(unsigned movement) override{Motion(movement).Stop();}
    void Close(pal4::PlayerWalkWaitState& s) override{for(auto& h:handlers)if(&h->state==&s){h->closed=true;return;}throw std::logic_error("walk handler is not registered");}
    void NormalizeAndClamp(pal4::BaseObjectVector3& p) override{pal4::GameVector3 v{p.x,p.y,p.z};pal4::GameVectorMath::NormalizeAndClamp(v,Math());p={v[0],v[1],v[2]};}
    double AngleFromCosine(float cosine) override{return Math().ApproximateArccosine(cosine);}
    void SetPose(pal4::BaseObjectState&,const pal4::BaseObjectVector3& p,unsigned mode) override{Require(mode==0,"invalid motion pose mode");players.SetRotation(current,{p.x,p.y,p.z});}
    unsigned SetDirection(pal4::BaseObjectState&,const pal4::BaseObjectVector3& p) override{players.SetPosition(current,{p.x,p.y,p.z});moved(current,players.Slots()[current].position,players.Slots()[current].rotation);++moved_frames;return 1;}
    void Apply(unsigned index) {
        current=index;pal4::BaseObjectState base;const auto p=motions[index]->Position(),v=motions[index]->Velocity(),rotation=players.Slots()[index].rotation;
        base.Write(0x604,status[index]);for(unsigned axis=0;axis<3;++axis){base.Write(0x570+axis*4,std::bit_cast<unsigned>(p[axis]));base.Write(0x57c+axis*4,std::bit_cast<unsigned>(v[axis]));base.Write(0xa0+axis*4,std::bit_cast<unsigned>(rotation[axis]));}
        (void)pal4::BaseObjectMotion::ApplyTransform(base,0x10000+index*0xb14,*this);
    }
};
PlayerWalks::PlayerWalks(PlayerRoster& p,PlayerActions& a,const CollisionMesh& collision,const Scene& world,PlayerWalkMoved m):impl_(std::make_unique<Impl>(p,a,collision,world,std::move(m))){}
PlayerWalks::~PlayerWalks()=default;
ScriptResponse PlayerWalks::Start(ScriptToken token,int player,Point3 target,bool wait,bool run){for(float v:target)Require(std::isfinite(v),"invalid walk target");auto& s=*impl_;++s.calls;s.requested=target;s.request_token=token;const auto count=s.handlers.size();pal4::ScriptPlayerWalk runtime(s);if(run)(void)runtime.RunTo(player,{target[0],target[1],target[2]},wait);else(void)runtime.WalkTo(player,{target[0],target[1],target[2]},wait);return {{},wait && s.handlers.size()>count};}
ScriptResponse PlayerWalks::End(ScriptToken token,int player){auto& s=*impl_;++s.end_calls;s.request_token=token;const auto count=s.handlers.size();(void)pal4::ScriptPlayerWalk(s).EndMoveIndexed(player);return {{},s.handlers.size()>count};}
std::vector<PlayerWalkCompletion> PlayerWalks::Advance(float elapsed){
    Require(std::isfinite(elapsed) && elapsed>=0,"invalid walk frame time");auto& s=*impl_;++s.frames;
    for(unsigned i=0;i<s.motions.size();++i)if(s.motions[i] && s.motions[i]->Active()){s.motions[i]->Advance(elapsed);s.Apply(i);s.elapsed+=elapsed;}
    std::vector<PlayerWalkCompletion> result;
    for(auto& h:s.handlers){if(!h->closed)pal4::ScriptPlayerWalk(s).Update(h->state);if(h->closed){++s.completed;if(h->wait){++s.blocking_completed;result.push_back({h->token,{}});}}}
    std::erase_if(s.handlers,[](auto& h){return h->closed;});return result;
}
bool PlayerWalks::Requested() const{return impl_->calls!=0;}
bool PlayerWalks::Active() const{for(std::size_t i=0;i<impl_->motions.size();++i)if(impl_->motions[i] && impl_->motions[i]->Active())return true;return false;}
void PlayerWalks::WriteReport(std::ostream& out) const {
    const auto& s=*impl_;out<<std::setprecision(9)<<"{\"calls\":"<<s.calls<<",\"end_calls\":"<<s.end_calls<<",\"completed\":"<<s.completed<<",\"blocking_completed\":"<<s.blocking_completed<<",\"handlers\":"<<s.handlers.size()<<",\"frames\":"<<s.frames<<",\"moved_frames\":"<<s.moved_frames<<",\"elapsed\":"<<s.elapsed<<",\"linked_edges\":"<<s.surface.LinkedEdges()<<",\"requested\":";Json(out,s.requested);out<<",\"projected\":";Json(out,s.projected);out<<",\"players\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';out<<"{\"position\":";Json(out,s.players.Slots()[i].position);out<<",\"rotation\":";Json(out,s.players.Slots()[i].rotation);out<<",\"motion\":";if(!s.motions[i]){out<<"null}";continue;}const auto& m=*s.motions[i];out<<"{\"active\":"<<(m.Active()?"true":"false")<<",\"status\":"<<m.Status()<<",\"face\":"<<m.Face()<<",\"position\":";Json(out,m.Position());out<<",\"path\":[";const auto route=m.Path();for(std::size_t j=0;j<route.size();++j){if(j)out<<',';out<<"{\"position\":";Json(out,route[j].position);out<<",\"face\":"<<route[j].face<<'}';}out<<"]}}";}out<<"]}";
}
}
