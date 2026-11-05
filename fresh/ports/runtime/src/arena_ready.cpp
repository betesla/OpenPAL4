#include "pal4/portable/arena_ready.h"
#include "pal4/game_arena_ready_handler.h"
#include <array>
#include <ostream>
#include <stdexcept>
#include <utility>
namespace pal4::portable {
struct ArenaReady::Impl final:pal4::IGameArenaReadyRuntime {
    std::string world,scene;PlayerRoster& players;ActorController& controller;
    IControllerResetServices& input;IArenaReadyServices& services;
    std::array<std::uint32_t,3> clocks{};
    std::uint32_t ready{},restoring{},calls{},restore_calls{},input_calls{},pet_calls{},grid_calls{};bool completed{};
    Impl(std::string w,std::string s,PlayerRoster& p,ActorController& c,IControllerResetServices& i,IArenaReadyServices& v)
        :world(std::move(w)),scene(std::move(s)),players(p),controller(c),input(i),services(v){}
    void RequireBoundPlayer() const {
        if(!players.Prepared() || !controller.Initialized() || !controller.State().actor ||
            *controller.State().actor!=players.Leader())throw std::logic_error("arena ready requires bound player/controller");
    }
    std::uint32_t PlayerHandle() const{return 0x10000+players.Leader()*0xb14;}
    std::uint32_t ActorControl() override{return 0x1000;}
    std::uint32_t ArenaManager() override{return 0x2000;}
    std::uint32_t GameState() override{return 0x3000;}
    std::uint32_t ReadWord(std::uint32_t address) override {
        if(address==0x1000)return controller.State().enabled;
        if(address==0x1004)return controller.State().actor?PlayerHandle():0;
        if(address==0x20c4)return ready;
        if(address==0x21bc)return restoring;
        if(address==0x3574)return clocks[2];
        throw std::logic_error("invalid arena ready read");
    }
    void WriteWord(std::uint32_t address,std::uint32_t value) override {
        if(address==0x20c4)ready=value;
        else if(address==0x21bc)restoring=value;
        else if(address>=0x356c && address<=0x3574 && (address-0x356c)%4==0)clocks[(address-0x356c)/4]=value;
        else throw std::logic_error("invalid arena ready write");
    }
    std::string_view ReadText(std::uint32_t address) override {
        if(address==0x2008)return world;
        if(address==0x20e0)return scene;
        throw std::logic_error("invalid arena ready text");
    }
    std::uint32_t GameDatabase() override{return 0x4000;}
    std::span<const pal4::ArenaReadyHintRecord> HintRecords(std::uint32_t member) override{if(member!=0x4034)throw std::logic_error("invalid arena hint database");return services.HintRecords();}
    std::uint32_t CreateHintWindow() override{return services.CreateHintWindow();}
    void ShowHint(std::uint32_t window,std::string_view text,float x,float y,float duration,std::uint32_t first,std::uint32_t second) override{services.ShowHint(window,text,x,y,duration,first,second);}
    std::int32_t ReleaseHintMatches() override{return 0;}
    void ProcessActorInput(std::uint32_t actor) override{if(actor!=0x1000)throw std::logic_error("invalid ready actor");controller.ProcessInput(false,input);++input_calls;}
    void ProcessActorInputAndClearVector(std::uint32_t actor) override{if(actor!=0x1000)throw std::logic_error("invalid ready actor");controller.ProcessInput(true,input);++input_calls;}
    std::uint32_t InputManager() override{return 1;}
    void InitializeInputState(std::uint32_t handle) override{if(handle!=1)throw std::logic_error("invalid ready input");input.ClearPlatformInput();}
    std::uint32_t PetManager() override{return 2;}
    void InitializePetData(std::uint32_t handle) override{if(handle!=2)throw std::logic_error("invalid ready pet");services.InitializePetStoredData();++pet_calls;}
    std::uint32_t MinimapManager() override{return 3;}
    void UpdateGrid(std::uint32_t handle,std::uint32_t position,std::uint32_t rotation) override {
        if(handle!=3 || position!=PlayerHandle()+0xac || rotation!=PlayerHandle()+0xa0)throw std::logic_error("invalid ready grid");
        const auto& player=players.Slots()[players.Leader()];services.UpdateGrid(player.position,player.rotation);++grid_calls;
    }
    float LeaderY() override{return players.Slots()[players.Leader()].position[1];}
    float LeaderZ() override{return players.Slots()[players.Leader()].position[2];}
    void SetMinimapTexture(std::uint32_t) override{}
    std::uint32_t Milliseconds() override{return services.Milliseconds();}
};
ArenaReady::ArenaReady(std::string w,std::string s,PlayerRoster& p,ActorController& c,IControllerResetServices& i,IArenaReadyServices& v,bool restoring)
    :impl_(std::make_unique<Impl>(std::move(w),std::move(s),p,c,i,v)){impl_->restoring=restoring;}
ArenaReady::~ArenaReady()=default;
void ArenaReady::Run() {
    impl_->RequireBoundPlayer();
    ++impl_->calls;impl_->completed=false;(void)pal4::GameArenaReadyHandler::Run(*impl_);impl_->completed=impl_->ready!=0;
}
void ArenaReady::Restore() {
    ++impl_->restore_calls;
    // The original false branch only reads the arena restore flag. In
    // particular, it does not require a player or touch readiness/clocks.
    const bool run=impl_->restoring!=0;
    if(run){impl_->RequireBoundPlayer();++impl_->calls;impl_->completed=false;}
    (void)pal4::GameArenaReadyHandler::Restore(*impl_);
    if(run)impl_->completed=impl_->ready!=0;
}
void ArenaReady::WriteReport(std::ostream& output) const {
    const auto& s=*impl_;
    output<<"{\"calls\":"<<s.calls<<",\"completed\":"<<(s.completed?"true":"false")<<",\"ready\":"<<s.ready
        <<",\"restoring\":"<<s.restoring<<",\"restore_calls\":"<<s.restore_calls<<",\"input_calls\":"<<s.input_calls<<",\"pet_calls\":"<<s.pet_calls<<",\"grid_calls\":"<<s.grid_calls
        <<",\"timestamps\":["<<s.clocks[0]<<','<<s.clocks[1]<<','<<s.clocks[2]<<"]}";
}
}
