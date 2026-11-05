#pragma once
#include "pal4/portable/script_stack.h"
#include "pal4/portable/world.h"
#include <bit>
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
// 公共脚本服务路由；平台只连接已验证的游戏模块与音画输入接口。
struct GameServices final:IScriptContextServices {
    std::ostream& trace;
    unsigned calls{},last_service{};
    std::function<void()> prepare_players;
    std::function<void(std::uint32_t,std::int32_t,bool)> add_property,remove_property;
    std::function<void(bool)> set_camera_collision;
    std::function<void(std::uint32_t,bool)> set_camera_mode;
    std::function<void()> seek_camera_player,flush_camera_tail_yaw;
    std::function<void(bool)> set_camera_distance;
    std::function<void(std::int32_t)> set_leader;
    std::function<std::int32_t()> get_leader;
    std::function<void(std::int32_t,bool)> add_team_member,remove_team_member;
    std::function<void(bool)> set_auto_seek;
    std::function<void()> lock_player,unlock_player;
    std::function<void(Point3)> set_position;
    std::function<float()> get_current_x,get_current_y,get_current_z;
    std::function<void(float)> set_angle;
    std::function<void(bool)> ready_arena;
    std::function<bool(std::string_view)> get_object_visible;
    std::function<void(std::string_view)> reset_object;
    std::function<void(std::string_view,bool)> object_visible,npc_visible,event_visible,object_research;
    std::function<void(bool,bool)> grant_magic;
    std::function<void(std::int32_t,bool)> grant_system_ui;
    std::function<ScriptResponse(const ScriptRequest&)> player_action,player_turn,player_walk,npc_action,npc_walk;
    std::function<void(float,std::string_view)> start_time_script;
    std::function<void()> terminate_time_script;
    std::function<void()> hide_skill_object;
    std::function<void()> set_full_hp,set_full_mp,set_full_rage,player_take_rest;
    std::function<std::string()> get_arena_name,get_arena_area;
    std::function<std::int32_t(std::string_view,std::string_view)> come_from_here;
    std::function<std::int32_t(std::int32_t,std::int32_t)> get_random_number;
    std::function<bool(std::uint32_t,std::int32_t)> check_property;
    std::function<std::int32_t(std::uint32_t)> get_property_count;
    std::function<std::int32_t()> get_money;
    std::function<void(std::int32_t,bool)> add_money,pay_money;
    std::function<void(std::int32_t,std::int32_t)> set_player_level;
    std::function<std::int32_t(std::int32_t)> get_player_level;
    std::function<void(std::int32_t,std::uint32_t,bool)> add_player_skill;
    std::function<void(std::int32_t,std::int32_t,std::int32_t)> add_player_favor;
    std::function<void(std::int32_t,bool)> set_player_visible;
    std::function<void(bool)> set_current_player_visible;
    std::function<void(std::int32_t,Point3)> set_player_position;
    std::function<void(std::int32_t,std::uint32_t)> add_equipment;
    std::function<void(std::int32_t,std::int32_t)> add_combat_monster;
    std::function<void(bool,std::int32_t,std::int32_t,std::int32_t)> config_combat_param;
    std::function<void(std::string_view)> config_combat_bgm,config_combat_camera,config_combat_ground_camera;
    std::function<void(std::int32_t)> config_combat_vip_monster;
    std::function<ScriptResponse(const ScriptRequest&)> start_combat;
    std::function<ScriptResponse(const ScriptRequest&)> prepare_camera,run_camera,wait_camera;
    std::function<ScriptResponse(const ScriptRequest&)> fallback;
    explicit GameServices(std::ostream& output):trace(output) {}
    void OnCall(const ScriptRequest& request) override {
        ++calls;last_service=static_cast<unsigned>(request.call.system_id);
        trace<<"game "<<EncodeScriptCall(request.call)<<std::flush;
    }
    ScriptResponse Invoke(const ScriptRequest& request) override {
        if(request.call.system_id==98 || request.call.system_id==99){
            if(request.call.arguments.size()!=3)throw std::runtime_error("invalid inventory change arguments");
            const auto& callback=request.call.system_id==98?add_property:remove_property;
            if(!callback)throw std::runtime_error("inventory change is not bound");
            callback(std::bit_cast<std::uint32_t>(std::get<std::int32_t>(request.call.arguments[0])),std::get<std::int32_t>(request.call.arguments[1]),std::get<bool>(request.call.arguments[2]));return {};
        }
        if(request.call.system_id==81){if(!set_camera_mode || request.call.arguments.size()!=2)throw std::runtime_error("invalid camera mode binding");set_camera_mode(std::bit_cast<std::uint32_t>(std::get<std::int32_t>(request.call.arguments[0])),std::get<bool>(request.call.arguments[1]));return {};}
        if(request.call.system_id==91){if(!flush_camera_tail_yaw || !request.call.arguments.empty())throw std::runtime_error("invalid camera tail yaw binding");flush_camera_tail_yaw();return {};}
        if(request.call.system_id==216){if(!start_time_script || request.call.arguments.size()!=2)throw std::runtime_error("invalid time script binding");start_time_script(std::get<float>(request.call.arguments[0]),std::get<std::string>(request.call.arguments[1]));return {};}
        if(request.call.system_id==217){if(!terminate_time_script || !request.call.arguments.empty())throw std::runtime_error("invalid time script termination binding");terminate_time_script();return {};}
        if(request.call.system_id==180){if(!hide_skill_object || !request.call.arguments.empty())throw std::runtime_error("invalid arena skill cleanup binding");hide_skill_object();return {};}
        if(request.call.system_id==104){if(!seek_camera_player || !request.call.arguments.empty())throw std::runtime_error("invalid camera seek binding");seek_camera_player();return {};}
        if(request.call.system_id==269 || request.call.system_id==270){if(!npc_walk || request.call.arguments.size()!=5)throw std::runtime_error("invalid NPC movement binding");return npc_walk(request);}
        if(request.call.system_id==272 || request.call.system_id==273){if(!npc_action || request.call.arguments.size()!=(request.call.system_id==272?4u:2u))throw std::runtime_error("invalid NPC action binding");return npc_action(request);}
        if(request.call.system_id==257 || request.call.system_id==258 || request.call.system_id==267){if(!player_walk || request.call.arguments.size()!=(request.call.system_id==267?1u:5u))throw std::runtime_error("invalid player walk binding");return player_walk(request);}
        if(request.call.system_id==120){if(!set_camera_distance || request.call.arguments.size()!=1)throw std::runtime_error("invalid distance optimization binding");set_camera_distance(std::get<bool>(request.call.arguments[0]));return {};}
        if(request.call.system_id==76){if(!npc_visible || request.call.arguments.size()!=2)throw std::runtime_error("invalid NPC visibility binding");npc_visible(std::get<std::string>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};}
        if(request.call.system_id==122){if(!object_research || request.call.arguments.size()!=2)throw std::runtime_error("invalid GOB research binding");object_research(std::get<std::string>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};}
        if(request.call.system_id==149){if(!event_visible || request.call.arguments.size()!=2)throw std::runtime_error("invalid EVF visibility binding");event_visible(std::get<std::string>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};}
        if(request.call.system_id==154){if(!get_object_visible || request.call.arguments.size()!=1)throw std::runtime_error("invalid object visibility query binding");return {get_object_visible(std::get<std::string>(request.call.arguments[0])),false};}
        if(request.call.system_id==210){if(!reset_object || request.call.arguments.size()!=1)throw std::runtime_error("invalid GOB reset binding");reset_object(std::get<std::string>(request.call.arguments[0]));return {};}
        if(request.call.system_id==97){if(!object_visible || request.call.arguments.size()!=2)throw std::runtime_error("invalid object visibility binding");object_visible(std::get<std::string>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};}
        if(request.call.system_id==255){if(!player_turn || request.call.arguments.size()!=3)throw std::runtime_error("invalid player turn binding");return player_turn(request);}
        if(request.call.system_id==287){if(!prepare_camera || request.call.arguments.size()!=1)throw std::runtime_error("invalid camera prepare binding");return prepare_camera(request);}
        if(request.call.system_id==288){if(!run_camera || request.call.arguments.size()!=2)throw std::runtime_error("invalid camera sequence binding");return run_camera(request);}
        if(request.call.system_id==290){if(!wait_camera || !request.call.arguments.empty())throw std::runtime_error("invalid camera wait binding");return wait_camera(request);}
        if(request.call.system_id==109) {
            if(request.call.arguments.size()!=2 || !add_equipment)throw std::runtime_error("invalid player equipment binding");
            add_equipment(std::get<std::int32_t>(request.call.arguments[0]),std::bit_cast<std::uint32_t>(std::get<std::int32_t>(request.call.arguments[1])));return {};
        }
        if(request.call.system_id==245 || request.call.system_id==118 || request.call.system_id==246) {
            if(request.call.arguments.size()!=(request.call.system_id==245?4u:1u) || !player_action)throw std::runtime_error("invalid player action binding");
            return player_action(request);
        }
        if(request.call.system_id==160) {
            if(request.call.arguments.size()!=2 || !grant_magic)throw std::runtime_error("invalid magic permission binding");
            grant_magic(std::get<bool>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==157) {
            if(request.call.arguments.size()!=2 || !grant_system_ui)throw std::runtime_error("invalid system UI permission binding");
            grant_system_ui(std::get<std::int32_t>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==66 || request.call.system_id==67) {
            if(!request.call.arguments.empty() || !ready_arena)throw std::runtime_error("invalid arena ready binding");
            ready_arena(request.call.system_id==67);return {};
        }
        if(request.call.system_id==253) {
            if(request.call.arguments.size()!=1 || !set_angle)throw std::runtime_error("invalid current player rotation binding");
            set_angle(std::get<float>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==250) {
            if(request.call.arguments.size()!=3 || !set_position)throw std::runtime_error("invalid player position binding");
            set_position({std::get<float>(request.call.arguments[0]),std::get<float>(request.call.arguments[1]),std::get<float>(request.call.arguments[2])});return {};
        }
        if(request.call.system_id==75) {
            if(!request.call.arguments.empty() || !unlock_player)throw std::runtime_error("invalid player unlock binding");
            unlock_player();return {};
        }
        if(request.call.system_id==74) {
            if(!request.call.arguments.empty() || !lock_player)throw std::runtime_error("invalid player lock binding");
            lock_player();return {};
        }
        if(request.call.system_id==100 || request.call.system_id==101) {
            if(request.call.arguments.size()!=2)throw std::runtime_error("invalid party membership arguments");
            const auto& callback=request.call.system_id==100?add_team_member:remove_team_member;
            if(!callback)throw std::runtime_error("party membership is not bound");
            callback(std::get<std::int32_t>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==107) {
            if(!request.call.arguments.empty() || !get_leader)throw std::runtime_error("invalid leader query binding");
            return {get_leader(),false};
        }
        if(request.call.system_id==191 || request.call.system_id==192 || request.call.system_id==193) {
            const auto& callback=request.call.system_id==191?get_current_x:request.call.system_id==192?get_current_y:get_current_z;
            if(!request.call.arguments.empty() || !callback)throw std::runtime_error("invalid current player position query binding");
            return {callback(),false};
        }
        if(request.call.system_id==114 || request.call.system_id==115) {
            const auto& callback=request.call.system_id==114?set_full_hp:set_full_mp;
            if(!request.call.arguments.empty() || !callback)throw std::runtime_error("invalid full HP/MP binding");
            callback();return {};
        }
        if(request.call.system_id==194 || request.call.system_id==195) {
            const auto& callback=request.call.system_id==194?get_arena_name:get_arena_area;
            if(!request.call.arguments.empty() || !callback)throw std::runtime_error("invalid arena name/area query binding");
            return {callback(),false};
        }
        if(request.call.system_id==92) {
            if(request.call.arguments.size()!=2 || !add_combat_monster)throw std::runtime_error("invalid add combat monster binding");
            add_combat_monster(std::get<std::int32_t>(request.call.arguments[0]),std::get<std::int32_t>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==93) {
            if(request.call.arguments.size()!=4 || !config_combat_param)throw std::runtime_error("invalid combat param binding");
            config_combat_param(std::get<bool>(request.call.arguments[0]),std::get<std::int32_t>(request.call.arguments[1]),std::get<std::int32_t>(request.call.arguments[2]),std::get<std::int32_t>(request.call.arguments[3]));return {};
        }
        if(request.call.system_id==94 || request.call.system_id==95 || request.call.system_id==218) {
            const auto& callback=request.call.system_id==94?config_combat_bgm:request.call.system_id==95?config_combat_camera:config_combat_ground_camera;
            if(request.call.arguments.size()!=1 || !callback)throw std::runtime_error("invalid combat text config binding");
            callback(std::get<std::string>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==230) {
            if(request.call.arguments.size()!=1 || !config_combat_vip_monster)throw std::runtime_error("invalid combat VIP monster binding");
            config_combat_vip_monster(std::get<std::int32_t>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==96) {
            if(request.call.arguments.size()!=1 || !start_combat)throw std::runtime_error("invalid start combat binding");
            return start_combat(request);
        }
        if(request.call.system_id==69) {
            if(request.call.arguments.size()!=2 || !come_from_here)throw std::runtime_error("invalid arena come-from query binding");
            return {come_from_here(std::get<std::string>(request.call.arguments[0]),std::get<std::string>(request.call.arguments[1])),false};
        }
        if(request.call.system_id==71) {
            if(request.call.arguments.size()!=2 || !set_player_visible)throw std::runtime_error("invalid player visibility binding");
            set_player_visible(std::get<std::int32_t>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==89) {
            if(request.call.arguments.size()!=2 || !get_random_number)throw std::runtime_error("invalid random number query binding");
            return {get_random_number(std::get<std::int32_t>(request.call.arguments[0]),std::get<std::int32_t>(request.call.arguments[1])),false};
        }
        if(request.call.system_id==108) {
            if(request.call.arguments.size()!=2 || !set_player_level)throw std::runtime_error("invalid player level update binding");
            set_player_level(std::get<std::int32_t>(request.call.arguments[0]),std::get<std::int32_t>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==113) {
            if(request.call.arguments.size()!=1 || !set_current_player_visible)throw std::runtime_error("invalid current player visibility binding");
            set_current_player_visible(std::get<bool>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==116) {
            if(request.call.arguments.size()!=1 || !get_player_level)throw std::runtime_error("invalid player level query binding");
            return {get_player_level(std::get<std::int32_t>(request.call.arguments[0])),false};
        }
        if(request.call.system_id==145) {
            if(request.call.arguments.size()!=3 || !add_player_skill)throw std::runtime_error("invalid player skill acquisition binding");
            add_player_skill(std::get<std::int32_t>(request.call.arguments[0]),std::bit_cast<std::uint32_t>(std::get<std::int32_t>(request.call.arguments[1])),std::get<bool>(request.call.arguments[2]));return {};
        }
        if(request.call.system_id==156) {
            if(request.call.arguments.size()!=2 || !check_property)throw std::runtime_error("invalid pack property query binding");
            return {check_property(std::bit_cast<std::uint32_t>(std::get<std::int32_t>(request.call.arguments[0])),std::get<std::int32_t>(request.call.arguments[1])),false};
        }
        if(request.call.system_id==178 || request.call.system_id==179) {
            const auto& callback=request.call.system_id==178?add_money:pay_money;
            if(request.call.arguments.size()!=2 || !callback)throw std::runtime_error("invalid money transaction binding");
            callback(std::get<std::int32_t>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));return {};
        }
        if(request.call.system_id==190) {
            if(request.call.arguments.size()!=1 || !get_property_count)throw std::runtime_error("invalid property count query binding");
            return {get_property_count(std::bit_cast<std::uint32_t>(std::get<std::int32_t>(request.call.arguments[0]))),false};
        }
        if(request.call.system_id==199) {
            if(!request.call.arguments.empty() || !player_take_rest)throw std::runtime_error("invalid player rest binding");
            player_take_rest();return {};
        }
        if(request.call.system_id==205) {
            if(!request.call.arguments.empty() || !get_money)throw std::runtime_error("invalid money query binding");
            return {get_money(),false};
        }
        if(request.call.system_id==219) {
            if(request.call.arguments.size()!=3 || !add_player_favor)throw std::runtime_error("invalid player favor update binding");
            add_player_favor(std::get<std::int32_t>(request.call.arguments[0]),std::get<std::int32_t>(request.call.arguments[1]),std::get<std::int32_t>(request.call.arguments[2]));return {};
        }
        if(request.call.system_id==235) {
            if(!request.call.arguments.empty() || !set_full_rage)throw std::runtime_error("invalid full rage binding");
            set_full_rage();return {};
        }
        if(request.call.system_id==249) {
            if(request.call.arguments.size()!=4 || !set_player_position)throw std::runtime_error("invalid indexed player position binding");
            set_player_position(std::get<std::int32_t>(request.call.arguments[0]),{std::get<float>(request.call.arguments[1]),std::get<float>(request.call.arguments[2]),std::get<float>(request.call.arguments[3])});return {};
        }
        if(request.call.system_id==70) {
            if(request.call.arguments.size()!=1 || !set_leader)throw std::runtime_error("invalid leader binding");
            set_leader(std::get<std::int32_t>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==105) {
            if(request.call.arguments.size()!=1 || !set_auto_seek)throw std::runtime_error("invalid camera follow binding");
            set_auto_seek(std::get<bool>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==103) {
            if(request.call.arguments.size()!=1 || !set_camera_collision)throw std::runtime_error("invalid camera collision binding");
            set_camera_collision(std::get<bool>(request.call.arguments[0]));return {};
        }
        if(request.call.system_id==57) {
            if(!request.call.arguments.empty() || !prepare_players)throw std::runtime_error("invalid NewGame player binding");
            prepare_players();
            return {};
        }
        if(fallback)return fallback(request);
        throw std::runtime_error("unbound game service "+std::to_string(request.call.system_id));
    }
};
}
