#include "pal4/portable/player.h"
#include "pal4/portable/player_lock.h"
#include "pal4/portable/player_placement.h"
#include "pal4/portable/player_rotation.h"
#include "pal4/portable/arena_ready.h"
#include "pal4/portable/player_action.h"
#include "pal4/portable/player_turn.h"
#include "pal4/portable/player_walk.h"
#include "pal4/portable/player_free_movement.h"
#include "pal4/portable/monster_player_view.h"
#include <cmath>
#include "pal4/portable/player_equipment.h"
#include <bit>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace pal4::portable;
void Check(bool value,const std::source_location location=std::source_location::current()) { if(!value)throw std::runtime_error("player contract failed at line "+std::to_string(location.line())); }
void Word(std::string& b,std::uint32_t value) { for(unsigned n=0;n<32;n+=8)b+=static_cast<char>(value>>n); }
void Text(std::string& b,std::string_view value) {Word(b,static_cast<std::uint32_t>(value.size()));b+=value;}
using Field=std::pair<std::string,unsigned>;
std::vector<Field> Fields() {
    std::vector<Field> fields{{"ID",1},{"ROLE_ID",1},{"CA_LEVEL",1},{"CA_EXPERIENCE",1}};
    for(const auto* n:{"CA_MAX_HP","CA_ADDITIONAL_RAGE","CA_MAX_MP","CA_PHYSICAL","CA_TOUGHNESS","CA_SPEED","CA_LUCKY","CA_WILL",
                      "EFE_WATER","EFE_FIRE","EFE_THUNDER","EFE_AIR","EFE_EARTH"})fields.emplace_back(n,1);
    for(const auto* suffix:{"ADDITIONAL","EXTRACT","REACT"})for(const auto* channel:{"PHYSICAL","WATER","FIRE","THUNDER","AIR","EARTH"})
        fields.emplace_back(std::string("CA_ATTACK_")+channel+"_"+suffix,std::string_view(suffix)=="ADDITIONAL"?1:3);
    for(const auto* n:{"CA_ADDITIONAL_CRITICAL","CA_FEND_OFF","CA_ADDITIONAL_HITTING","CA_PAY_FOR_SPARE"})fields.emplace_back(n,3);
    return fields;
}
std::string GrowthFixture() {
    const auto fields=Fields();Check(fields.size()==39);
    std::string b="P4UP";Word(b,1);Word(b,4);Word(b,39);
    for(const auto& [name,type]:fields){Text(b,name);Word(b,type);}
    for(unsigned i=0;i<4;++i)for(const auto& [name,type]:fields){
        const unsigned value=name=="ID"?i+1:name=="ROLE_ID"?1001+i:name=="CA_LEVEL"?1:name=="CA_EXPERIENCE"?54:
            name=="CA_MAX_HP"?210+i:name=="CA_MAX_MP"?30+i:name=="EFE_WATER"?7:type==3?0x3e4ccccdu:0;
        Word(b,value);
    }
    return b;
}
std::string EquipmentFixture() {
    auto fields=Fields();fields[1]={"TYPE",1};fields[2]={"EA_SKILL_ID",1};fields[3]={"EA_NAME",2};
    while(fields.size()<58)fields.emplace_back("extra_"+std::to_string(fields.size()),1);
    std::string b="P4EQ";Word(b,1);Word(b,3);Word(b,58);
    for(auto c:{0u,0u,0u,1u,2u,3u,4u})Word(b,c);
    for(const auto& [name,type]:fields){Text(b,name);Word(b,type);}
    for(unsigned i=0;i<3;++i)for(const auto& [name,type]:fields){
        if(type==2){Text(b,"equipment");continue;}
        Word(b,name=="ID"?3500+i:name=="TYPE"?(i==2?6:4):name=="CA_TOUGHNESS"?5:name=="CA_PHYSICAL"?10:type==3?0x3e800000:0);
    }
    return b;
}
RoleCatalog RolesFixture() {
    std::string b="P4RL";Word(b,1);Word(b,4);Word(b,50);
    for(unsigned i=0;i<4;++i){
        Word(b,i);Text(b,"ROLE_ID");Word(b,1);Word(b,1001+i);
        for(const auto* n:{"RA_NAME","RA_ICON","RA_MODEL","RA_TEXTURE","SOUND_WOUNDED1","SOUND_WOUNDED2","SOUND_WOUNDED3"}) {
            Text(b,n);Word(b,2);Text(b,"101");
        }
        const auto fields=Fields();
        for(std::size_t n=4;n<fields.size();++n){Text(b,fields[n].first);Word(b,fields[n].second);
            Word(b,fields[n].first=="EFE_WATER"?11:0);}
        for(const auto* n:{"RACE","ePT_TIANHE_FAVOR","ePT_LINSHA_FAVOR","ePT_MENGLI_FAVOR","ePT_ZIYING_FAVOR","PHYSICAL_ATK_TARGET","PHYSICAL_ATK_RANGE"})
            {Text(b,n);Word(b,1);Word(b,0);}
    }
    std::istringstream input(b);return ReadRoles(input);
}
std::vector<UpgradeRecord> Parse(const std::string& bytes) {std::istringstream input(bytes);return ReadUpgrades(input);}
void Reject(const std::string& bytes) {bool failed=false;try{(void)Parse(bytes);}catch(const std::exception&){failed=true;}Check(failed);}
Actor FixtureActor() {
    const Matrix identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    Actor result;result.frames.push_back({-1,identity});result.palettes.push_back({0,identity});
    result.mesh.vertices.resize(3);result.influences.resize(3,{{0,0,0,0},{1,0,0,0}});
    result.clips.push_back({"C01",1,{}});result.clips.push_back({"C02",1,{}});
    result.clips.push_back({"J08",2,{{0,{{0,{0,0,0,1},{0,0,0}},{2,{0,0,0,1},{2,0,0}}}}}});return result;
}
std::optional<std::vector<std::uint8_t>> ActionMessage(std::string_view path) {
    if(!path.ends_with("J08.amf"))return {};
    constexpr std::string_view signature="_pal4__Actor__Action_Message_File__(__SoftStar_sh__)__v_0_1_";
    std::string bytes;Word(bytes,static_cast<unsigned>(signature.size()));bytes+=signature;Word(bytes,1);
    const auto offset=bytes.size();bytes.resize(offset+332);bytes.replace(offset,3,"101");bytes.replace(offset+64,3,"J08");bytes.replace(offset+128,4,"HOLD");
    for(unsigned b=0;b<4;++b)bytes[offset+192+b]=static_cast<char>(std::bit_cast<unsigned>(1.f)>>(b*8));
    return std::vector<std::uint8_t>(bytes.begin(),bytes.end());
}
struct ActionHost final:IScriptContextServices {
    PlayerActions& actions;PlayerTurns* turns{};
    explicit ActionHost(PlayerActions& value):actions(value){}
    ScriptResponse Invoke(const ScriptRequest& request) override {
        if(request.call.system_id==255){Check(turns!=nullptr);return turns->Start(request.token,std::get<std::int32_t>(request.call.arguments[0]),std::get<float>(request.call.arguments[1]),std::get<bool>(request.call.arguments[2]));}
        if(request.call.system_id==118){actions.Unhold(std::get<std::int32_t>(request.call.arguments[0]));return {};}
        if(request.call.system_id==246)return actions.End(request.token,std::get<std::int32_t>(request.call.arguments[0]));
        Check(request.call.system_id==245);
        return actions.Start(request.token,std::get<std::int32_t>(request.call.arguments[0]),std::get<std::string>(request.call.arguments[1]),std::get<std::int32_t>(request.call.arguments[2]),std::get<bool>(request.call.arguments[3]));
    }
};
struct LockServices final:IControllerResetServices {
    SceneUi& ui;unsigned inputs{},stops{};bool allow_clear{};
    explicit LockServices(SceneUi& value):ui(value){}
    void SetDefaultCursor() override{ui.cursor="default.cur";}
    void ClearPlatformInput() override{++inputs;}
    void HideInteractionUi() override{ui.HideInteraction();}
    void SetMonsterPursuitStopped(bool value) override{Check(value || allow_clear);++stops;}
};
struct ReadyServices final:IArenaReadyServices {
    unsigned pets{},grids{},shown{};bool reject_pet{},window_available{true};std::string hint;
    std::vector<pal4::ArenaReadyHintRecord> records{{"Q01","N01","hut"},{"Q01","N01","second"}};
    void InitializePetStoredData() override{if(reject_pet)throw std::logic_error("active pet unavailable");++pets;}
    void UpdateGrid(Point3 position,Point3 rotation) override{Check(position==Point3{0,1,0} && rotation==Point3{12,0,34});++grids;}
    std::span<const pal4::ArenaReadyHintRecord> HintRecords() override{return records;}
    std::uint32_t CreateHintWindow() override{return window_available?1:0;}
    void ShowHint(std::uint32_t window,std::string_view text,float x,float y,float duration,std::uint32_t first,std::uint32_t second) override{
        Check(window==1 && x==.5f && y==.15f && duration==1 && first==0 && second==1);hint=text;++shown;
    }
    std::uint32_t Milliseconds() override{return 123456;}
};
struct FreeMovementServices final:IPlayerPlacementServices {
    PlayerRoster& players;std::string order;
    explicit FreeMovementServices(PlayerRoster& p):players(p){}
    void FollowPlayer(std::uint32_t index) override{Check(index==0 && players.Slots()[index].position[1]==0);order+='P';}
    void UpdateFloorAttributes(unsigned child,const FloorHit& hit) override{Check(child==0 && hit.position[1]==0);order+='F';}
    void UpdateGrid(Point3,Point3) override{order+='G';}
    void SelectAnimation(std::uint32_t index,std::string_view action) override{Check(index==0 && players.Slots()[index].animation==action);order+='A';}
};
struct PlacementServices final:IPlayerPlacementServices {
    unsigned followed{},floors{},grids{},animations{};bool reject_floor{};
    void FollowPlayer(std::uint32_t index) override{Check(index==1);++followed;}
    void UpdateFloorAttributes(unsigned child,const FloorHit& hit) override {
        Check(child==0 && hit.position[1]==0);if(reject_floor)throw std::runtime_error("unbound floor attributes");++floors;
    }
    void UpdateGrid(Point3 position,Point3 rotation) override{Check(position[0]==0 && rotation[1]==180);++grids;}
    void SelectAnimation(std::uint32_t index,std::string_view action) override{Check(index==1 && action=="C01");++animations;}
};
}
int main(int argc,char** argv) {
    try {
        if(argc==5 || argc==6){
            std::ifstream roles_file(argv[1],std::ios::binary),growth_file(argv[2],std::ios::binary);
            const auto roles=ReadRoles(roles_file);const auto growth=ReadUpgrades(growth_file);
            PlayerRoster roster;roster.Prepare(roles,growth,[&](std::string_view model){
                std::ifstream actor(std::filesystem::path(argv[3])/("actor_"+std::string(model)+".p4a"),std::ios::binary);return ReadActor(actor);
            });
            std::ofstream report(argv[4]);
            if(argc==6){std::ifstream equipment_file(argv[5],std::ios::binary);const auto catalog=ReadEquipment(equipment_file);
                PlayerEquipment equipment(roster,catalog);equipment.Add(0,3586);equipment.Add(0,3501);equipment.WriteReport(report);
            }else WritePlayerReport(report,roster);
            return 0;
        }
        const auto bytes=GrowthFixture();auto growth=Parse(bytes);const auto roles=RolesFixture();
        const auto p=InitialPlayerProgress(roles[0],growth);
        Check(p.level==1 && p.experience==0 && p.hp==210 && p.mp==30 && p.rage==0 && !p.dead);
        Check(p.base.elements[0]==11 && p.effective.elements[0]==11 && p.base.rates[0]==.2f);
        auto duplicate=growth;duplicate.insert(duplicate.begin(),growth[0]);duplicate[0].stats.core[0]=321;
        Check(InitialPlayerProgress(roles[0],duplicate).hp==321);
        bool failed=false;try{(void)InitialPlayerProgress(roles[0],{});}catch(const std::exception&){failed=true;}Check(failed);
        for(std::size_t size=0;size<bytes.size();++size)Reject(bytes.substr(0,size));
        Reject(bytes+"x");auto broken=bytes;broken[4]=2;Reject(broken);
        broken=bytes;broken[12]=38;Reject(broken);
        broken=bytes;const auto name=broken.find("CA_MAX_HP");broken.replace(name,9,"CA_MAX_MP");Reject(broken);
        broken=bytes;const auto f=broken.find("CA_ADDITIONAL_CRITICAL");broken[f+std::string_view("CA_ADDITIONAL_CRITICAL").size()]=1;Reject(broken);
        broken=bytes;const auto first_float=broken.size()-4*39*4+23*4;
        broken[first_float]=0;broken[first_float+1]=0;broken[first_float+2]=char(0xc0);broken[first_float+3]=char(0x7f);Reject(broken);
        PlayerRoster roster;unsigned loaded{};
        roster.Prepare(roles,growth,[&](std::string_view model){Check(model=="101");++loaded;return FixtureActor();});
        Check(roster.Prepared() && loaded==4 && roster.Slots()[0].inventory_enabled && !roster.Slots()[1].inventory_enabled);
        Check(roster.Slots()[3].progress.hp==213 && roster.Slots()[3].script_index==3 && !roster.Slots()[3].active);
        roster.SetPartyOrder(3,1);Check(roster.Slots()[3].script_index==1 && roster.Slots()[3].progress.hp==213 && roster.Leader()==0);
        bool invalid_order=false;try{roster.SetPartyOrder(4,0);}catch(const std::exception&){invalid_order=true;}Check(invalid_order);
        invalid_order=false;try{roster.SetPartyOrder(3,4);}catch(const std::exception&){invalid_order=true;}Check(invalid_order && roster.Slots()[3].script_index==1);
        roster.SetPartyOrder(3,3);
        roster.EnterScene("Q01","N01");Check(roster.Slots()[0].active && roster.Slots()[0].world=="Q01" && roster.Slots()[0].animation=="C01");
        roster.EnterScene("Q01","Q01",1);Check(!roster.Slots()[0].active && roster.Slots()[1].active && roster.Slots()[1].scene=="Q01");
        roster.EnterScene("Q01","N01");
        unsigned bindings=0;
        const auto bind=[&](unsigned index){++bindings;Check(index==roster.Leader());};
        roster.SetLeader(0,{});Check(bindings==0 && roster.Leader()==0);
        roster.SetAutoSeek(false);for(const auto& slot:roster.Slots())Check(!slot.auto_seek);
        roster.SetAutoSeek(true);Check(roster.Slots()[0].auto_seek && !roster.Slots()[1].auto_seek);
        roster.SetLeader(1,bind);Check(roster.Leader()==1 && bindings==0 && roster.Slots()[0].active);
        roster.SetAutoSeek(true);Check(!roster.Slots()[0].auto_seek && roster.Slots()[1].auto_seek);
        roster.SetLeader(0,bind);Check(roster.Leader()==0 && bindings==1 && roster.Slots()[0].auto_seek);
        // Arrange a second party member with distinct transform/priorities.
        auto& first=const_cast<PlayerSlot&>(roster.Slots()[0]);
        auto& second=const_cast<PlayerSlot&>(roster.Slots()[1]);
        roster.SetInTeam(1,true);Check(second.inventory_enabled && first.script_index==0 && second.script_index==1);
        roster.SetInTeam(1,false);Check(!second.inventory_enabled && first.script_index==0);
        roster.SetInTeam(-1,true);roster.SetInTeam(4,true);
        roster.SetInTeam(1,true);Check(second.inventory_enabled);
        roster.SetHealth(0,0,5);Check(first.progress.hp==0 && first.progress.mp==5 && first.progress.dead);
        roster.TakeARest();
        Check(first.progress.hp==210 && first.progress.mp==30 && !first.progress.dead);
        roster.SetFullRage();
        Check(first.progress.rage==100 && second.progress.rage==100);
        roster.SetLevel(0,15);Check(roster.GetLevel(0)==15);
        roster.AddSkill(0,5101);roster.AddSkill(0,5101);Check(first.skills.size()==1 && first.skills[0]==5101);
        roster.AddFavor(0,1,25);Check(first.favor[1]==25);
        roster.AddFavor(0,1,-40);Check(first.favor[1]==0);
        roster.SetVisible(0,false);Check(!first.visible);
        roster.SetVisible(0,true);Check(first.visible);
        first.position={7,8,9};first.rotation={1,180,3};first.script_index=2;
        second.script_index=9;
        roster.SetLeader(1,bind);
        Check(bindings==2 && !first.active && second.active && second.world=="Q01" && second.scene=="N01");
        Check(second.position==first.position && second.rotation==first.rotation && second.animation=="C01");
        Check(!first.auto_seek && second.auto_seek && first.script_index==9 && second.script_index==2);
        roster.SetLeader(1,{});Check(bindings==2 && second.script_index==2);
        failed=false;try{roster.SetLeader(-1,bind);}catch(const std::exception&){failed=true;}Check(failed && roster.Leader()==1);
        failed=false;try{roster.SetLeader(4,bind);}catch(const std::exception&){failed=true;}Check(failed && roster.Leader()==1);
        ActorController control;control.Initialize(1280,720,FixtureActor);control.SelectActor(1,1);
        SceneUi ui;ui.hud_visible=ui.jump_hint_visible=ui.progress_visible=true;ui.cursor="other.cur";
        LockServices lock_services(ui);unsigned selections{};
        LockPlayer(roster,control,ui,lock_services,[&](auto index,auto action){Check(index==1 && action=="C01");++selections;});
        Check(selections==1 && control.State().reset_count==1 && !control.State().enabled && lock_services.stops==1 && lock_services.inputs==1);
        Check(!ui.hud_visible && !ui.jump_hint_visible && !ui.progress_visible && ui.scene_events==1 && ui.cursor=="default.cur");
        Check(second.animation=="C01");
        LockPlayer(roster,control,ui,lock_services,[&](auto,auto){++selections;});Check(selections==2 && control.State().reset_count==2 && ui.scene_events==2);
        lock_services.allow_clear=true;unsigned unlock_grids{};
        UnlockPlayer(roster,control,ui,lock_services,[&](Point3 position,Point3 rotation){
            Check(control.State().enabled && ui.hud_visible && ui.scene_events==3);
            Check(position==second.position && rotation==second.rotation);++unlock_grids;
        });
        Check(unlock_grids==1 && control.State().reset_count==3 && selections==2);
        LockPlayer(roster,control,ui,lock_services,[&](auto,auto){++selections;});
        Check(!control.State().enabled && !ui.hud_visible);
        ActorController no_actor;no_actor.Initialize(1280,720,FixtureActor);
        SceneUi unbound_ui;LockServices unbound_services(unbound_ui);unbound_services.allow_clear=true;
        UnlockPlayer(roster,no_actor,unbound_ui,unbound_services,[&](auto,auto){++unlock_grids;});
        Check(no_actor.State().enabled && unbound_ui.hud_visible && unlock_grids==1);
        second.actor->atomic_spheres={{0,{0,0,0,5}}};
        CollisionTriangle plane{{Point3{-10,0,-10},Point3{10,0,-10},Point3{0,0,10}},0,0,0};
        SceneMovement scene_movement(std::span<const CollisionTriangle>(&plane,1));PlacementServices placement_services;
        PlayerNotifications notifications;PlayerPlacement placement(roster,scene_movement,placement_services,notifications);
        placement.SetCurrent({0,1,0});Check(second.position==Point3{0,1,0});
        Check(placement.CurrentX()==0 && placement.CurrentY()==1 && placement.CurrentZ()==0);
        Check(placement_services.followed==1 && placement_services.floors==1 && placement_services.grids==1 && placement_services.animations==1);
        std::ostringstream placement_report;placement.WriteReport(placement_report);
        Check(placement_report.str().find("\"completed\":true")!=std::string::npos && placement_report.str().find("\"waits\":1")!=std::string::npos);
        placement.SetCurrent({0,1,0});std::ostringstream replaced_report;placement.WriteReport(replaced_report);
        Check(replaced_report.str().find("\"waits\":1")!=std::string::npos);
        PlayerRotation rotation(roster,notifications);
        roster.SetRotation(1,{12,20,34});
        rotation.SetCurrentAngle(180);
        Check(second.rotation==Point3{12,180,34} && second.position==Point3{0,1,0});
        Check(notifications.Count(1)==1 && notifications.Count(2)==1);
        rotation.SetCurrentAngle(450);Check(second.rotation==Point3{12,90,34} && notifications.Count(2)==1);
        rotation.SetCurrentAngle(360);Check(second.rotation[1]==360);
        rotation.SetCurrentAngle(-360);Check(second.rotation[1]==360);
        rotation.SetCurrentAngle(-370);Check(second.rotation[1]==350);
        rotation.SetCurrentAngle(720);Check(second.rotation[1]==0);
        std::ostringstream notify_report;notifications.WriteReport(notify_report);
        Check(notify_report.str().find("Player-Movment")!=std::string::npos && notify_report.str().find("\"phase\":2,\"closed\":true,\"mode\":0")!=std::string::npos);
        failed=false;try{rotation.SetCurrentAngle(std::numeric_limits<float>::infinity());}catch(const std::invalid_argument&){failed=true;}
        Check(failed && second.rotation[1]==0);
        placement.ProcessWaitHandlers();std::ostringstream cleared_report;placement.WriteReport(cleared_report);
        Check(cleared_report.str().find("\"waits\":0")!=std::string::npos && notifications.Count(2)==0);
        ReadyServices ready_services;ArenaReady ready("Q01","N01",roster,control,lock_services,ready_services);
        {
            PlayerRoster unloaded;ActorController unbound;
            ArenaReady not_restoring("Q01","Q01",unloaded,unbound,lock_services,ready_services);
            not_restoring.Restore(); // Original false branch needs no player/controller.
            ArenaReady restoring("Q01","Q01",unloaded,unbound,lock_services,ready_services,true);
            failed=false;try{restoring.Restore();}catch(const std::logic_error&){failed=true;}Check(failed);
            Check(ready_services.pets==0 && ready_services.grids==0);
        }
        const auto resets=control.State().reset_count;const auto hidden=ui.interaction_resets;const auto inputs=lock_services.inputs;
        ready.Restore();
        Check(ready_services.pets==0 && ready_services.grids==0 && ui.interaction_resets==hidden && lock_services.inputs==inputs);
        std::ostringstream untouched_report;ready.WriteReport(untouched_report);
        Check(untouched_report.str().find("\"calls\":0")!=std::string::npos && untouched_report.str().find("[0,0,0]")!=std::string::npos);
        ready.Run();Check(ready_services.pets==1 && ready_services.grids==1 && ui.interaction_resets==hidden+1 && lock_services.inputs==inputs+1);
        Check(!control.State().enabled && control.State().reset_count==resets && !ui.hud_visible);
        std::ostringstream ready_report;ready.WriteReport(ready_report);
        Check(ready_report.str().find("\"ready\":1")!=std::string::npos && ready_report.str().find("[123456,123456,123456]")!=std::string::npos);
        {
            ArenaReady restoring("Q01","N01",roster,control,lock_services,ready_services,true);
            restoring.Restore();
            Check(ready_services.pets==2 && ready_services.grids==2);
            std::ostringstream restored_report;restoring.WriteReport(restored_report);
            Check(restored_report.str().find("\"restoring\":0")!=std::string::npos && restored_report.str().find("\"ready\":1")!=std::string::npos);
            restoring.Restore();
            Check(ready_services.pets==2 && ready_services.grids==2);
        }
        ready_services.reject_pet=true;ArenaReady rejected_ready("Q01","N01",roster,control,lock_services,ready_services);
        failed=false;try{rejected_ready.Run();}catch(const std::logic_error&){failed=true;}Check(failed);
        std::ostringstream rejected_report;rejected_ready.WriteReport(rejected_report);
        Check(rejected_report.str().find("\"ready\":0")!=std::string::npos && rejected_report.str().find("[0,0,0]")!=std::string::npos);
        {
            ArenaReady restoring("Q01","N01",roster,control,lock_services,ready_services,true);
            failed=false;try{restoring.Restore();}catch(const std::logic_error&){failed=true;}Check(failed);
            std::ostringstream failed_restore;restoring.WriteReport(failed_restore);
            Check(failed_restore.str().find("\"restoring\":1")!=std::string::npos && failed_restore.str().find("[0,0,0]")!=std::string::npos);
            ready_services.reject_pet=false;restoring.Restore();
            Check(ready_services.pets==3 && ready_services.grids==3);
            ready_services.reject_pet=true;
        }
        lock_services.allow_clear=true;control.ResetAndSync(true,lock_services);
        ready_services.reject_pet=false;
        ArenaReady hint_ready("Q01","N01",roster,control,lock_services,ready_services);
        hint_ready.Run();Check(ready_services.shown==1 && ready_services.hint=="<dc0><df2>hut</df2></dc0><br/>");
        ArenaReady unmatched_ready("Q01","OTHER",roster,control,lock_services,ready_services);
        unmatched_ready.Run();Check(ready_services.shown==1);
        ready_services.window_available=false;const auto before_pets=ready_services.pets;
        ArenaReady unavailable_ready("Q01","N01",roster,control,lock_services,ready_services);
        unavailable_ready.Run();Check(ready_services.pets==before_pets && ready_services.shown==1);
        std::ostringstream unavailable_report;unavailable_ready.WriteReport(unavailable_report);Check(unavailable_report.str().find("\"completed\":false")!=std::string::npos);
        ready_services.window_available=true;
        control.ResetAndSync(false,lock_services);
        {
            PlayerActions actions(roster,ActionMessage);ActionHost host(actions);ScriptGlobals globals{};ScriptStack scripts(globals);
            scripts.Compile("action","void Run(){giPlayerDoAction(1,\"J08\",-1,true);global_mvar=7;}");
            Check(scripts.Push("action","void Run()",host).has_value() && scripts.Tick(0)==ScriptState::suspended && globals[0]==0);
            const auto token=*scripts.Pending();Check(actions.Advance(.75f).empty() && scripts.Tick(100)==ScriptState::suspended && globals[0]==0);
            const auto done=actions.Advance(.25f);Check(done.size()==1 && done[0].token==token && done[0].error.empty());
            Check(scripts.Complete(done[0].token) && scripts.Tick(0)==ScriptState::finished && globals[0]==7 && !scripts.Complete(done[0].token));
            Check(actions.Playback(1)->MessageName()=="hold" && actions.Playback(1)->Time()==1);
            Check(actions.Advance(5).empty() && actions.Playback(1)->Time()==1);
            Check(actions.Start({20,1},1,"J08",-1,true).suspend && actions.Start({21,1},1,"J08",-1,true).suspend);
            const auto replaced=actions.Advance(0);Check(replaced.size()==1 && replaced[0].token.context==20 && !replaced[0].error.empty());
            const auto second_done=actions.Advance(1);Check(second_done.size()==1 && second_done[0].token.context==21);
            Check(!actions.Start({22,1},1,"J08",-1,false).suspend && actions.Advance(1).empty());
        }
        {
            PlayerActions actions(roster,ActionMessage);ActionHost host(actions);ScriptGlobals globals{};ScriptStack scripts(globals);
            scripts.Compile("ending","void Run(){giPlayerDoAction(1,\"J08\",-1,true);global_mvar=1;giPlayerUnHoldAct(1);giPlayerEndAction(1);global_mvar=2;}");
            Check(scripts.Push("ending","void Run()",host).has_value() && scripts.Tick(0)==ScriptState::suspended);
            const auto hold=actions.Advance(1);Check(hold.size()==1 && actions.Playback(1)->Time()==1);
            Check(scripts.Complete(hold[0].token) && scripts.Tick(0)==ScriptState::suspended && globals[0]==1);
            Check(actions.Playback(1)->Action()=="J08" && actions.Playback(1)->Time()==1);
            Check(actions.Advance(.5f).empty() && actions.Playback(1)->Time()==1.5f && scripts.Tick(20)==ScriptState::suspended);
            const auto end=actions.Advance(.5f);Check(end.size()==1 && end[0].token!=hold[0].token && end[0].error.empty());
            Check(actions.Playback(1)->Action()=="C01" && actions.Playback(1)->Time()==0);
            Check(scripts.Complete(end[0].token) && scripts.Tick(0)==ScriptState::finished && globals[0]==2 && !scripts.Complete(end[0].token));
            Check(actions.Advance(0).empty());
            std::ostringstream report;actions.WriteReport(report,false);
            Check(report.str().find("\"unhold_calls\":1")!=std::string::npos && report.str().find("\"completed_end_handlers\":1")!=std::string::npos && report.str().find("\"end_observed_complete\":true")!=std::string::npos);
            actions.Unhold(-1);Check(!actions.End({99,1},4).suspend);
            Check(!actions.Start({100,1},1,"J08",1,false).suspend);
            Check(actions.Advance(1).empty() && actions.Playback(1)->Time()==1 && actions.Playback(1)->MessageName().empty());
            Check(actions.Advance(1).empty() && actions.Playback(1)->Completed());
            Check(actions.Advance(0).empty() && actions.Playback(1)->Action()=="C01");
        }
        {
            PlayerActions actions(roster,ActionMessage);PlayerTurns turns(roster,actions);ActionHost host(actions);host.turns=&turns;
            ScriptGlobals globals{};ScriptStack scripts(globals);roster.SetRotation(1,{3,180,4});
            scripts.Compile("turn","void Run(){giPlayerSetDir(1,297,false);global_mvar=1;giWait(.1);giPlayerSetDir(1,186,true);global_mvar=2;}");
            Check(scripts.Push("turn","void Run()",host).has_value() && scripts.Tick(0)==ScriptState::suspended && globals[0]==1);
            Check(actions.Playback(1)->Action()=="C02" && roster.Slots()[1].rotation[1]==180);
            Check(turns.Advance(.1f).empty() && roster.Slots()[1].rotation[1]>180 && roster.Slots()[1].rotation[1]<297);
            Check(turns.Advance(1).empty() && roster.Slots()[1].rotation[1]==297);
            Check(actions.Playback(1)->Action()=="C02");actions.Advance(.25f);Check(actions.Playback(1)->Action()=="C01");
            Check(scripts.Tick(.1)==ScriptState::suspended && globals[0]==1);
            Check(turns.Advance(.05f).empty());actions.Advance(.05f);Check(globals[0]==1);
            auto done=turns.Advance(1);Check(done.size()==1 && roster.Slots()[1].rotation[1]==186);
            Check(scripts.Complete(done[0].token) && scripts.Tick(0)==ScriptState::finished && globals[0]==2 && !scripts.Complete(done[0].token));
            Check(turns.Advance(0).empty());actions.Advance(.25f);Check(actions.Playback(1)->Action()=="C01");
            Check(turns.Start({90,1},1,186,true).suspend);done=turns.Advance(0);Check(done.size()==1 && done[0].token==ScriptToken{90,1});
            Check(!turns.Start({91,1},-1,0,true).suspend && !turns.Start({92,1},4,0,true).suspend);
        }
        {
            const auto bytes=EquipmentFixture();std::istringstream input(bytes);auto catalog=ReadEquipment(input);
            Check(catalog.definitions.size()==3 && catalog.definitions.at(3500).fields.size()==58);
            for(std::size_t n=0;n<bytes.size();++n){bool rejected=false;try{std::istringstream cut(bytes.substr(0,n));(void)ReadEquipment(cut);}catch(const std::exception&){rejected=true;}Check(rejected);}
            auto bad=bytes+"x";bool rejected=false;try{std::istringstream trailing(bad);(void)ReadEquipment(trailing);}catch(const std::exception&){rejected=true;}Check(rejected);
            PlayerEquipment equipment(roster,catalog);const auto base=second.progress.base;
            equipment.Add(1,3500);Check(second.equipment[2]==1 && equipment.Instances()[0].definition==3500);
            Check(second.progress.effective.core[3]==base.core[3]+10 && second.progress.effective.core[4]==base.core[4]+5);
            Check(second.progress.effective.rates[0]==base.rates[0]+.25f && second.progress.hp==211 && second.progress.mp==31);
            equipment.Add(1,3500);Check(second.equipment[2]==2 && equipment.Instances().size()==2);
            Check(second.progress.effective.core[4]==base.core[4]+5);
            equipment.Add(1,3502);equipment.Add(1,3502);equipment.Add(1,3502);
            Check(second.equipment[4]==5 && second.equipment[5]==4 && second.progress.effective.core[4]==base.core[4]+15);
            equipment.Add(-1,3500);equipment.Add(4,3500);equipment.Add(1,9999);Check(equipment.Instances().size()==5);
            catalog.definitions.at(3501).skill=99;rejected=false;try{equipment.Add(1,3501);}catch(const std::exception&){rejected=true;}Check(rejected && equipment.Instances().size()==5);
            for(unsigned n=0;n<4;++n)equipment.Add(0,3500);
            std::ostringstream report;equipment.WriteReport(report);Check(report.str().find("\"recent\":[4,5,6,7,8,9]")!=std::string::npos);
            Check(roster.Slots()[0].equipment[2]==9 && second.equipment[2]==2);
            // 定义变化不能回写已创建的实例；新实例才使用新字段。
            catalog.definitions.at(3500).stats.core[0]=std::bit_cast<std::uint32_t>(-100);
            catalog.definitions.at(3500).stats.core[2]=std::bit_cast<std::uint32_t>(-10);
            first.progress.rage=150;equipment.Add(0,3500);
            Check(equipment.Instances()[1].snapshot.stats.core[0]==0 && equipment.Instances().back().snapshot.stats.core[0]==std::bit_cast<std::uint32_t>(-100));
            Check(first.progress.hp==110 && first.progress.mp==20 && first.progress.rage==100 && !first.progress.dead);
        }
        {
            PlayerRoster walking;walking.Prepare(roles,growth,[](auto){auto a=FixtureActor();a.atomic_spheres={{0,{0,0,0,1}}};return a;});
            walking.EnterScene("Q01","Q01");walking.SetPosition(0,{0,0,0});
            PlayerActions actions(walking,[](auto)->std::optional<std::vector<std::uint8_t>>{return {};});
            CollisionMesh collision;collision.floor={{{Point3{0,0,100},Point3{-100,0,-100},Point3{100,0,-100}},0,0,0}};Scene world;
            unsigned updates{};PlayerWalks walks(walking,actions,collision,world,[&](unsigned index,Point3 position,Point3 rotation){Check(index==0 && std::isfinite(position[0]) && std::isfinite(rotation[1]));++updates;});
            Check(!walks.Active());
            Check(!walks.Start({1,1},0,{20,0,0},false).suspend && actions.Playback(0)->Action()=="C02");
            Check(walks.Active());
            Check(walks.End({1,2},0).suspend);std::vector<PlayerWalkCompletion> done;
            for(unsigned i=0;i<100 && done.empty();++i){(void)actions.Advance(.05f);done=walks.Advance(.05f);}
            Check(done.size()==1 && done[0].token==ScriptToken{1,2} && done[0].error.empty() && updates>5 && !walks.Active() && walks.Requested());
            Check(std::abs(walking.Slots()[0].position[0]-20)<.01f && std::abs(walking.Slots()[0].rotation[1]-90)<.01f && actions.Playback(0)->Action()=="C01");
            Check(walks.Start({1,3},0,{0,0,0},true).suspend);done.clear();
            for(unsigned i=0;i<100 && done.empty();++i)done=walks.Advance(.05f);
            Check(done.size()==1 && done[0].token==ScriptToken{1,3} && std::abs(walking.Slots()[0].position[0])<.01f);
            Check(!walks.Start({1,4},-1,{0,0,0},true).suspend);
            const auto inactive=walking.Slots()[1].position;
            Check(walks.Start({1,5},1,{20,0,0},true).suspend);
            for(unsigned i=0;i<5;++i)Check(walks.Advance(.05f).empty());
            Check(walking.Slots()[1].position==inactive);
        }
        {
            PlayerRoster players;ActorController controller;MonsterPlayerView view(players,controller);
            Check(!view.ControlledActor().address);
            players.Prepare(roles,growth,[](auto){auto actor=FixtureActor();actor.atomic_spheres={{0,{2,3,4,5}}};return actor;});
            players.EnterScene("Q01","N01");players.SetPosition(0,{10,20,30});players.SetRotation(0,{0,90,0});
            controller.Initialize(1280,720,FixtureActor);controller.SelectActor(0,1);
            const auto snapshot=view.ControlledActor();Check(snapshot.address==MonsterPlayerView::Token(0) && snapshot.active && snapshot.position==Point3{10,20,30});
            const auto sphere=ActorBindSphere(*players.Slots()[0].actor);Check(sphere && snapshot.half_height==(*sphere)[3]);
            const auto matrix=InstanceMatrix(players.Slots()[0].position,players.Slots()[0].rotation);
            for(unsigned axis=0;axis<3;++axis)Check(std::abs(snapshot.collision_position[axis]-(matrix[axis]*(*sphere)[0]+matrix[4+axis]*(*sphere)[1]+matrix[8+axis]*(*sphere)[2]+matrix[12+axis]))<.0001f);
            const auto target=view.Target(snapshot.address);Check(target.active && target.position==snapshot.position);
            Check(!view.Target(MonsterPlayerView::Token(1)).active);
            bool rejected=false;try{(void)view.Target(snapshot.address+1);}catch(const std::invalid_argument&){rejected=true;}Check(rejected);
            players.EnterScene("Q01","N01",1);controller.SelectActor(1,1);
            Check(view.ControlledActor().address==MonsterPlayerView::Token(1) && view.ControlledActor().active && !view.Target(snapshot.address).active);
            controller.Shutdown();Check(!view.ControlledActor().address);
        }
        {
            PlayerRoster free_players;free_players.Prepare(roles,growth,[](auto){auto actor=FixtureActor();actor.clips.push_back({"C03",1,{}});actor.atomic_spheres={{0,{0,0,0,5}}};return actor;});
            free_players.EnterScene("Q01","N01");free_players.SetPosition(0,{0,1,-30});
            ActorController free_control;free_control.Initialize(1280,720,FixtureActor);free_control.SelectActor(0,1);
            SceneUi free_ui;LockServices reset(free_ui);reset.allow_clear=true;free_control.ResetAndSync(true,reset);
            PlayerActions free_actions(free_players,ActionMessage);
            CollisionTriangle a{{Point3{-200,0,-200},Point3{200,0,-200},Point3{-200,0,200}},0,0,0};
            CollisionTriangle b{{Point3{200,0,-200},Point3{200,0,200},Point3{-200,0,200}},0,0,1};
            const std::array floor_geometry{a,b};SceneMovement floor(floor_geometry);
            CollisionTriangle wall_a{{Point3{-200,-100,0},Point3{200,-100,0},Point3{-200,100,0}},1,0,0};
            CollisionTriangle wall_b{{Point3{200,-100,0},Point3{200,100,0},Point3{-200,100,0}},1,0,1};
            const std::array wall_geometry{wall_a,wall_b};SceneCollision collision;collision.Add(CollisionGroup::wall,wall_geometry);
            FreeMovementServices services(free_players);
            PlayerFreeMovement movement(free_players,free_control,free_actions,floor,collision,services,[&]{services.order+='C';});
            FreeControlInput input;input.forward=true;ControlCameraBasis camera{{1,0,0},{0,0,1}};bool moved=false;
            const auto dispatch=[&](Point3 direction,float elapsed){moved=movement.Advance(direction,elapsed);};
            Check(free_control.UpdateFreeInput(input,camera,free_players.Slots()[0].position,.05f,dispatch) && moved);
            Check(services.order=="CAPF" && free_actions.Playback(0)->Action()=="C03");
            Check(std::abs(free_players.Slots()[0].position[2]+21.505f)<.001f && free_players.Slots()[0].position[1]==0);
            input={};Check(!free_control.UpdateFreeInput(input,camera,free_players.Slots()[0].position,.05f,dispatch));
            const auto approach_position=free_players.Slots()[0].position;
            input.right=true;services.order.clear();
            Check(free_control.UpdateFreeInput(input,camera,free_players.Slots()[0].position,.05f,dispatch) && moved);
            // 原前视线段会在本帧清零朝墙位移，但位置提交后仍更新朝向。
            Check(services.order=="CPF" && free_players.Slots()[0].position==approach_position);
            Check(std::abs(free_players.Slots()[0].rotation[1]-90)<.01f);
            const auto wall_position=free_players.Slots()[0].position;services.order.clear();Check(movement.Advance({1,0,0},.05f));
            Check(free_players.Slots()[0].position[0]>wall_position[0]+8 && std::abs(free_players.Slots()[0].position[2]-wall_position[2])<.001f);
            free_players.SetPosition(0,{500,0,500});const auto before=free_players.Slots()[0].position,rotation=free_players.Slots()[0].rotation;
            services.order.clear();Check(!movement.Advance({-1,0,0},.05f));
            Check(services.order=="C" && free_players.Slots()[0].position==before && free_players.Slots()[0].rotation==rotation);
            free_control.ResetAndSync(false,reset);services.order.clear();Check(!movement.Advance({1,0,0},.05f) && services.order.empty());
            free_control.ResetAndSync(true,reset);free_control.SetMovementMode(0);free_players.SetPosition(0,{0,0,-30});free_players.SetRotation(0,{0,0,0});
            services.order.clear();Check(movement.Advance({0,0,1},.05f));
            Check(services.order=="CAPF" && free_actions.Playback(0)->Action()=="C02" && std::abs(free_players.Slots()[0].position[2]+26.602f)<.001f);
            std::ostringstream report;movement.WriteReport(report);Check(report.str().find("\"rejected\":1")!=std::string::npos && report.str().find("\"disabled\":1")!=std::string::npos);
            free_control.SelectActor(0,0);free_control.ResetAndSync(true,reset);free_control.SetMovementMode(1);
            free_players.SetPosition(0,{0,0,-30});free_players.SetRotation(0,{0,0,0});services.order.clear();
            input={};input.forward=true;
            Check(free_control.UpdateEventInput(input,.05f,[&](auto command,auto elapsed){Check(movement.AdvanceDiscrete(command,elapsed,true));}));
            Check(services.order=="CAPGF" && std::abs(free_players.Slots()[0].position[2]+21.505f)<.001f);
            std::ostringstream discrete_report;movement.WriteReport(discrete_report);const auto diagnostic=discrete_report.str();
            Check(diagnostic.find("\"actor_height\":10")!=std::string::npos && diagnostic.find("\"requested_delta\":[0,0,")!=std::string::npos && diagnostic.find("\"resolved_delta\":[0,0,")!=std::string::npos);
            const auto distance_at=diagnostic.find("\"distance\":");Check(distance_at!=std::string::npos && std::stod(diagnostic.substr(distance_at+11))>8.4);
            Check(movement.AdvanceDiscrete(1,.05f,true));std::ostringstream blocked_report;movement.WriteReport(blocked_report);
            Check(blocked_report.str().find("\"resolved_delta\":[0,0,0]")!=std::string::npos);
            services.order.clear();input={};input.right=true;
            Check(free_control.UpdateEventInput(input,.05f,[&](auto command,auto elapsed){Check(movement.AdvanceDiscrete(command,elapsed,false));}));
            Check(services.order=="CAG" && std::abs(free_players.Slots()[0].rotation[1]-6.345f)<.001f);
            std::ostringstream turn_report;movement.WriteReport(turn_report);Check(turn_report.str().find("\"requested_delta\":[0,0,0],\"resolved_delta\":[0,0,0]")!=std::string::npos);
            free_players.SetPosition(0,{500,0,500});const auto outside=free_players.Slots()[0].position;services.order.clear();
            Check(movement.AdvanceDiscrete(1,.05f,true) && free_players.Slots()[0].position==outside && services.order=="CA");
            free_actions.SetTaskSpeed(0,1.4f);const auto time=free_actions.Playback(0)->Time();(void)free_actions.Advance(.1f);
            Check(std::abs(free_actions.Playback(0)->Time()-time-.14f)<.001f);
            free_actions.SetTaskSpeed(0,1);bool bad_speed=false;try{free_actions.SetTaskSpeed(0,-1);}catch(const std::invalid_argument&){bad_speed=true;}Check(bad_speed);
            free_players.SetPosition(0,{0,0,-30});free_players.SetRotation(0,{0,0,0});
            free_actions.SelectTaskAction(0,"C01");free_control.SetAnalogMovementTuning(1,1.25f);services.order.clear();
            Check(free_control.State().input.event_mode==1 && movement.AdvanceModern({1,0,0},.02f));
            Check(free_control.State().input.result==1);
            Check(services.order=="CAPFG" && std::abs(free_players.Slots()[0].position[2]+25.7525f)<.001f);
            Check(std::abs(free_players.Slots()[0].rotation[1]-90)<.01f);
            bool legacy_rejected=false;try{(void)movement.Advance({1,0,0},.02f);}catch(const std::logic_error&){legacy_rejected=true;}Check(legacy_rejected);
            free_control.ResetAndSync(false,reset);const auto modern_stopped=free_players.Slots()[0].position;
            Check(!movement.AdvanceModern({1,0,0},.02f) && free_players.Slots()[0].position==modern_stopped);
            Check(free_control.State().input.result==0);
        }
        placement_services.reject_floor=true;failed=false;
        try{placement.SetCurrent({0,2,0});}catch(const std::runtime_error&){failed=true;}Check(failed && second.position==Point3{0,2,0});
        std::ostringstream failed_report;placement.WriteReport(failed_report);
        Check(failed_report.str().find("\"phase\":\"floor_attributes\"")!=std::string::npos && failed_report.str().find("\"completed\":false")!=std::string::npos);
        loaded=0;failed=false;
        try{roster.Prepare(roles,growth,[&](std::string_view){if(++loaded==3)throw std::runtime_error("load failure");return FixtureActor();});}
        catch(const std::exception&){failed=true;}
        Check(failed && !roster.Prepared());for(const auto& slot:roster.Slots())Check(!slot.actor);
        failed=false;try{roster.Prepare(roles,{},[](std::string_view){return FixtureActor();});}
        catch(const std::exception&){failed=true;}Check(failed && !roster.Prepared());
        roster.Prepare(roles,growth,[](std::string_view){return FixtureActor();});roster.Clear();Check(!roster.Prepared());
        std::cout<<"player initialization data checks passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
