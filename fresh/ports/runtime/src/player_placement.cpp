#include "pal4/portable/player_placement.h"
#include "pal4/script_player_position.h"
#include <algorithm>
#include <ostream>
#include <iomanip>
#include <stdexcept>
namespace pal4::portable {
namespace {[[noreturn]] void Missing(){throw std::logic_error("unbound indexed/NPC player placement service");}}
struct PlayerPlacement::Impl final:pal4::IScriptPlayerPositionRuntime {
    PlayerRoster& players;SceneMovement& movement;IPlayerPlacementServices& services;
    PlayerNotifications& notifications;
    Point3 requested{};float above{},below{};unsigned calls{};
    bool floor_updated{},grid_updated{},completed{};
    std::string phase{"idle"};
    Impl(PlayerRoster& p,SceneMovement& m,IPlayerPlacementServices& s,PlayerNotifications& n):players(p),movement(m),services(s),notifications(n){}
    std::uint32_t CurrentHandle() const{return 0x10000+players.Leader()*0xb14;}
    void RequireCurrent(std::uint32_t handle) const{if(handle!=CurrentHandle())throw std::logic_error("invalid placement player handle");}
    float CurrentX() const {
        if(!players.Prepared())throw std::logic_error("placement player manager is unavailable");
        return players.Slots().at(players.Leader()).position[0];
    }
    float CurrentY() override {
        if(!players.Prepared())throw std::logic_error("placement player manager is unavailable");
        return players.Slots().at(players.Leader()).position[1];
    }
    float CurrentZ() override {
        if(!players.Prepared())throw std::logic_error("placement player manager is unavailable");
        return players.Slots().at(players.Leader()).position[2];
    }
    std::uint32_t PlayerManager() override{return 0x8000;}
    std::uint32_t CurrentPlayer(std::uint32_t manager) override {
        if(manager!=0x8000 || !players.Prepared())throw std::logic_error("placement player manager is unavailable");
        return CurrentHandle();
    }
    std::uint32_t PlayerByIndex(std::uint32_t,std::int32_t) override{Missing();}
    void SetPosition(std::uint32_t actor,const pal4::PlayerPosition& position) override {
        RequireCurrent(actor);phase="position";players.SetPosition(players.Leader(),{position.x,position.y,position.z});
        if(players.Slots()[players.Leader()].auto_seek)services.FollowPlayer(players.Leader());
    }
    std::string_view ActorName(std::uint32_t actor) override{RequireCurrent(actor);return players.Slots()[players.Leader()].name;}
    std::uint32_t NpcManager() override{Missing();}
    std::uint32_t NpcByName(std::uint32_t,std::string_view) override{Missing();}
    void SetNpcPosition(std::uint32_t,const pal4::PlayerPosition&,std::uint32_t) override{Missing();}
    std::int32_t SetNpcAction(std::uint32_t,std::string_view) override{Missing();}
    float MovementProperty(std::uint32_t actor) override {
        RequireCurrent(actor);phase="radius";
        const auto sphere=ActorBindSphere(*players.Slots()[players.Leader()].actor);
        if(!sphere)throw std::logic_error("original actor bounds are missing from cache");
        return (*sphere)[3]*2.0f;
    }
    std::uint32_t MovementAdapter() override{return 0x20000;}
    void ValidateMovement(std::uint32_t adapter,std::uint32_t position) const {
        if(adapter!=0x20000 || position!=CurrentHandle()+172)throw std::logic_error("invalid placement movement handle");
    }
    void SelectAndUpdate(std::uint32_t adapter,std::uint32_t position,float first,float second,std::uint32_t mode) override {
        ValidateMovement(adapter,position);if(mode!=1)throw std::logic_error("invalid placement select mode");
        phase="movement";above=first;below=second;
        (void)movement.SelectAndUpdate(players.Slots()[players.Leader()].position,above,below);
    }
    bool CheckActive(std::uint32_t adapter,std::uint32_t position,float first,float second,std::uint32_t mode) override {
        ValidateMovement(adapter,position);if(mode)throw std::logic_error("invalid placement active mode");
        return movement.CheckActive(players.Slots()[players.Leader()].position,first,second);
    }
    void UpdateFloorColor() override {
        phase="floor_attributes";const auto hit=movement.LastHit();
        if(!hit)throw std::logic_error("placement lost active floor hit");
        services.UpdateFloorAttributes(movement.ActiveChild(),*hit);floor_updated=true;
    }
    void UpdateGrid(std::uint32_t position,std::uint32_t rotation) override {
        if(position!=CurrentHandle()+172 || rotation!=CurrentHandle()+160)throw std::logic_error("invalid placement grid handles");
        phase="grid";const auto& player=players.Slots()[players.Leader()];services.UpdateGrid(player.position,player.rotation);grid_updated=true;
    }
    void SetAction(std::uint32_t actor,std::string_view action) override {
        RequireCurrent(actor);phase="animation";players.SelectAction(players.Leader(),action);services.SelectAnimation(players.Leader(),action);
    }
    pal4::PlayerPositionWaitState* Allocate(std::uint32_t size) override {phase="wait";return notifications.Allocate(size);}
    std::uint32_t ScriptExecutor() override{return PlayerNotifications::executor;}
    void Construct(pal4::PlayerPositionWaitState& state,std::uint32_t mode,std::uint32_t queue) override{notifications.Construct(state,mode,queue);}
    void AssignName(pal4::PlayerPositionWaitState& state,std::string_view name) override{notifications.AssignName(state,name,1,pal4::ScriptPlayerPosition::kVtable);}
    void AddHandler(std::uint32_t queue,pal4::PlayerPositionWaitState* state) override{notifications.Add(queue,state);}
    std::uint32_t CleanupName() override{return 1;}
};
PlayerPlacement::PlayerPlacement(PlayerRoster& p,SceneMovement& m,IPlayerPlacementServices& s,PlayerNotifications& n):impl_(std::make_unique<Impl>(p,m,s,n)){}
PlayerPlacement::~PlayerPlacement()=default;
void PlayerPlacement::SetCurrent(Point3 position) {
    ++impl_->calls;impl_->requested=position;impl_->floor_updated=impl_->grid_updated=impl_->completed=false;
    pal4::ScriptPlayerPosition script(*impl_);(void)script.SetCurrent({position[0],position[1],position[2]});
    impl_->completed=true;impl_->phase="submitted";
}
float PlayerPlacement::CurrentX() const { return impl_->CurrentX(); }
float PlayerPlacement::CurrentY() const { return impl_->CurrentY(); }
float PlayerPlacement::CurrentZ() const { return impl_->CurrentZ(); }
void PlayerPlacement::ProcessWaitHandlers(){impl_->notifications.Process();}
void PlayerPlacement::WriteReport(std::ostream& output) const {
    const auto& s=*impl_;const auto& player=s.players.Slots()[s.players.Leader()];const auto hit=s.movement.LastHit();
    output<<std::setprecision(9)<<"{\"calls\":"<<s.calls<<",\"phase\":\""<<s.phase<<"\",\"completed\":"<<(s.completed?"true":"false")
        <<",\"above\":"<<s.above<<",\"below\":"<<s.below<<",\"active_child\":"<<s.movement.ActiveChild()
        <<",\"floor_updated\":"<<(s.floor_updated?"true":"false")<<",\"grid_updated\":"<<(s.grid_updated?"true":"false")
        <<",\"waits\":"<<s.notifications.Count(1)<<",\"requested\":["<<s.requested[0]<<','<<s.requested[1]<<','<<s.requested[2]
        <<"],\"position\":["<<player.position[0]<<','<<player.position[1]<<','<<player.position[2]<<"],\"rotation\":["<<player.rotation[0]<<','<<player.rotation[1]<<','<<player.rotation[2]<<"],\"floor\":";
    if(hit)output<<"{\"y\":"<<hit->position[1]<<",\"triangle\":"<<hit->triangle<<'}';else output<<"null";
    output<<'}';if(!output)throw std::runtime_error("cannot write player placement report");
}
}
