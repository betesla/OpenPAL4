#include "pal4/portable/game_path.h"
#include "pal4/portable/chapter_host.h"
#include "pal4/portable/original_database.h"
#include "pal4/portable/original_player_data.h"
#include "pal4/portable/dff_collision.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <sstream>
namespace pal4::portable {
using Clock=std::chrono::steady_clock;
namespace {
const char* StateName(ScriptState state){switch(state){case ScriptState::idle:return "idle";case ScriptState::prepared:return "prepared";case ScriptState::suspended:return "suspended";case ScriptState::finished:return "finished";case ScriptState::failed:return "failed";}return "unknown";}
void Error(ChapterHost& host,const std::exception& error){host.ready=false;host.error=error.what();host.trace<<"error="<<error.what()<<std::endl;}
}
void ReleaseEnvironmentGraphics(ChapterHost& session){
    for(auto& layer:session.environment_graphics)layer.gpu->Release();
    session.environment_graphics.clear();session.environment_draws=session.environment_triangles=session.sky_triangles=0;
}
void UploadEnvironmentGraphics(ChapterHost& session){
    ReleaseEnvironmentGraphics(session);if(!session.arena)return;
    for(const auto& source:session.arena->environment){
        ChapterHost::SequenceGraphic layer;layer.mesh=source.mesh;
        layer.gpu=std::make_unique<GlesRenderer>();layer.gpu->Upload(layer.mesh);
        session.environment_graphics.push_back(std::move(layer));
    }
}
void DrawEnvironmentGraphics(ChapterHost& session,const Matrix& projection,bool sky){
    if(!sky)session.environment_draws=session.environment_triangles=session.sky_triangles=0;
    if(!session.arena)return;
    const auto identity=IdentityEffectMatrix();
    for(std::size_t i=0;i<session.environment_graphics.size();++i){
        if((session.arena->environment[i].kind=="sky")!=sky)continue;
        auto& layer=session.environment_graphics[i];
        const auto triangles=sky?layer.gpu->DrawSky(layer.mesh,projection):layer.gpu->DrawScene(layer.mesh,projection,identity);
        session.environment_draws+=static_cast<unsigned>(layer.mesh.draws.size());
        session.environment_triangles+=triangles;if(sky)session.sky_triangles+=triangles;
    }
}
void ReleaseMonsterGraphics(ChapterHost& session) {
    if(session.monster_graphics)session.monster_graphics->Release();
    session.monster_graphics.reset();
}
void UploadMonsterGraphics(ChapterHost& session) {
    if(!session.monster_scene_runtime)return;
    if(!session.distance)throw std::logic_error("monster graphics require the shared distance owner");
    if(!session.monster_graphics)
        session.monster_graphics=std::make_unique<MonsterSceneGraphics>(session.monster_scene_runtime->Monsters(),*session.distance);
    session.monster_graphics->Upload();
}
void ReleaseNpcGraphics(ChapterHost& session) {
    for(auto& npc:session.npc_graphics)npc.gpu->Release();
    session.npc_graphics.clear();session.npc_draws=session.npc_triangles=0;
}
void UploadNpcGraphics(ChapterHost& session) {
    ReleaseNpcGraphics(session);if(!session.scene_npcs)return;
    const auto states=session.scene_npcs->States();
    for(std::size_t i=0;i<states.size();++i){
        ChapterHost::ObjectGraphics graphics;graphics.index=i;graphics.mesh=session.arena->actors[states[i].actor].actor.mesh;
        session.scene_npcs->Sample(i,graphics.mesh.vertices,&graphics.frames);graphics.gpu=std::make_unique<GlesRenderer>();graphics.gpu->Upload(graphics.mesh);
        session.npc_graphics.push_back(std::move(graphics));
    }
}
std::optional<std::vector<std::uint8_t>> ReadActorMember(ChapterHost& session,std::string_view name) {
    if(!session.actor_package) {
        session.actor_package_stream=std::make_unique<std::ifstream>(ResolveOriginalPath(session.game_data+"/palactor.cpk"),std::ios::binary);
        if(!*session.actor_package_stream)throw std::runtime_error("cannot open original palactor.cpk");
        session.actor_package=std::make_unique<PackageReader>(*session.actor_package_stream);
    }
    if(!session.actor_package->Find(name))return {};
    auto bytes=session.actor_package->Read(name);session.trace<<"original actor member="<<name<<" bytes="<<bytes.size()<<std::endl;return bytes;
}
std::optional<std::vector<std::uint8_t>> ReadUiMember(ChapterHost& session,std::string_view member) {
    if(!session.ui_package){
        session.ui_package_stream=std::make_unique<std::ifstream>(ResolveOriginalPath(session.game_data+"/ui.cpk"),std::ios::binary);
        if(!*session.ui_package_stream)throw std::runtime_error("cannot open user ui.cpk");
        session.ui_package=std::make_unique<PackageReader>(*session.ui_package_stream);
    }
    std::string key(member);std::replace(key.begin(),key.end(),'/','\\');
    if(!session.ui_package->Find(key))return {};
    auto bytes=session.ui_package->Read(key);session.trace<<"original UI member="<<key<<" bytes="<<bytes.size()<<std::endl;return bytes;
}
void ReleaseObjectGraphics(ChapterHost& session) {
    for(auto& object:session.object_graphics)if(object.gpu)object.gpu->Release();
    session.object_graphics.clear();session.object_draws=session.object_triangles=0;
}
void UploadObjectGraphics(ChapterHost& session) {
    ReleaseObjectGraphics(session);
    if(!session.scene_objects)return;
    const auto states=session.scene_objects->States();
    for(std::size_t i=0;i<states.size();++i)if(states[i].actor && (states[i].type==0 || states[i].type==6 || states[i].type==7)){
        ChapterHost::ObjectGraphics graphics;graphics.index=i;graphics.mesh=session.arena->actors[*states[i].actor].actor.mesh;
        session.scene_objects->Sample(i,graphics.mesh.vertices,&graphics.frames);graphics.gpu=std::make_unique<GlesRenderer>();graphics.gpu->Upload(graphics.mesh);
        session.object_graphics.push_back(std::move(graphics));
    }
}
void SelectGameAnimation(ChapterHost& session,std::uint32_t index,std::string_view action) {
    if(session.controller.State().actor!=index)throw std::logic_error("animation controller is not bound to player");
    const auto& actor=*session.players.Slots()[index].actor;bool found=false;
    for(std::size_t i=0;i<actor.clips.size();++i)if(actor.clips[i].name==action){session.clip=static_cast<int>(i);found=true;}
    if(!found)throw std::logic_error("selected player animation is missing");
    session.animation_start=Clock::now();
}
class PlacementServices final:public IPlayerPlacementServices {
public:
    explicit PlacementServices(ChapterHost& owner):session(owner){}
    void FollowPlayer(std::uint32_t index) override {
        if(!session.camera_follow || session.controller.State().actor!=index)throw std::logic_error("placement camera follow owner is unavailable");
        session.camera_follow->FollowPlayer();
    }
    void UpdateFloorAttributes(unsigned child,const FloorHit& hit) override {
        if(child || !session.arena || !session.floor_attributes)throw std::logic_error("placement floor owner is unavailable");
        session.floor_attributes->Update(session.arena->collision.floor.at(hit.triangle));
    }
    void UpdateGrid(Point3 position,Point3 rotation) override {
        if(!session.minimap)throw std::logic_error("placement minimap owner is not bound");
        session.minimap->UpdatePlayer(position,rotation);
        if(session.capture_frame_traces){
            std::ofstream report(session.directory+"/minimap-player-probe.json",std::ios::trunc);session.minimap->WriteReport(report);
            std::ofstream placement(session.directory+"/minimap-placement-probe.json",std::ios::trunc);session.minimap->WriteReport(placement);
        }
    }
    void SelectAnimation(std::uint32_t player,std::string_view action) override {SelectGameAnimation(session,player,action);}
private:ChapterHost& session;
};
class ResetServices final:public IControllerResetServices {
public:
    explicit ResetServices(ChapterHost& owner):session(owner){}
    void SetDefaultCursor() override{session.scene_ui.cursor="default.cur";}
    void ClearPlatformInput() override{
        session.input_x=session.input_y=0;session.camera_yaw_input=session.camera_pitch_input=0;
        session.requested_action.clear();
#if PAL4_MODERN_GAMEPAD
        if(session.modern_movement_active && session.controller.Initialized()){
            session.controller.SetMovementMode(session.controller.State().input.mode);
            session.controller.SetAnalogMovementResult(false);
        }
        session.modern_movement_active=false;
#endif
        if(session.player_actions && session.controller.State().actor)session.player_actions->SetTaskSpeed(*session.controller.State().actor,1);
    }
    void HideInteractionUi() override{session.scene_ui.HideInteraction();}
    void SetMonsterPursuitStopped(bool stopped) override {
        if(!session.arena || !session.arena->monsters_verified_empty)throw std::logic_error("populated monster control is not bound");
        session.arena->monster_pursuit_stopped=stopped;
    }
private:ChapterHost& session;
};
class ReadyServices final:public IArenaReadyServices {
public:
    explicit ReadyServices(ChapterHost& owner):session(owner){
        names.reserve(session.original_scenes.size());hints.reserve(session.original_scenes.size());
        for(const auto& scene:session.original_scenes){
            auto upper=[](std::string text){for(auto& c:text)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');return text;};
            names.push_back({upper(scene.world),upper(scene.scene)});
            hints.push_back({names.back()[0],names.back()[1],scene.title});
        }
    }
    std::span<const pal4::ArenaReadyHintRecord> HintRecords() override{return hints;}
    std::uint32_t CreateHintWindow() override{if(!session.interaction_hints)throw std::logic_error("arena hint owner is missing");return session.interaction_hints->CreateHintWindow();}
    void ShowHint(std::uint32_t window,std::string_view text,float x,float y,float duration,std::uint32_t first,std::uint32_t second) override{
        if(!session.interaction_hints)throw std::logic_error("arena hint owner is missing");
        session.interaction_hints->InitializeHintWindow(window,text,x,y,duration,first,second);
    }
    void InitializePetStoredData() override {
        // PetManager::InitializeStoredPetData (0x43E2E0): source==0 returns.
        // No pet load service has completed in this runtime. Future loading
        // must populate this owner before enabling the nonempty branch.
        if(session.pet_stored_source)throw std::logic_error("stored pet initialization is not bound");
    }
    void UpdateGrid(Point3 position,Point3 rotation) override {
        if(!session.minimap)throw std::logic_error("arena ready minimap owner is not bound");
        session.minimap->UpdatePlayer(position,rotation);
        std::ofstream report(session.directory+"/minimap-player-probe.json",std::ios::trunc);session.minimap->WriteReport(report);
    }
    std::uint32_t Milliseconds() override {
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    }
private:ChapterHost& session;std::vector<std::array<std::string,2>> names;std::vector<pal4::ArenaReadyHintRecord> hints;
};
void StopGame(ChapterHost& session) {
    session.movement_input_gate.Reset();
    session.inventory_services.reset();session.inventory.reset();session.property_catalog.clear();session.item_messages.clear();
    session.scene_interaction.reset();session.free_movement.reset();session.scene_events.reset();session.event_scripts.reset();session.interaction_hints.reset();session.interaction_text.clear();session.input_confirm=session.confirm_pending=session.confirm_suppressed=false;session.free_idle={};session.free_control_active=false;
    session.scene_effect_frames.close();session.scene_effect_capture_remaining=0;
    session.gob_effect_preview.reset();session.effect_playback.reset();session.playback_frames.close();session.playback_capture_remaining=0;session.playback_action.clear();
    for(auto& [token,graphics]:session.playback_graphics){(void)token;for(auto& object:graphics)if(object.gpu)object.gpu->Release();}session.playback_graphics.clear();
    session.effect_sequence.reset();session.sequence_frames.close();session.sequence_capture_remaining=0;
    for(auto& object:session.sequence_graphics)if(object.gpu)object.gpu->Release();
    session.sequence_graphics.clear();
    session.particle_preview.reset();session.particle_frames.close();session.particle_capture_remaining=0;
    if(session.particle_counts.instances)throw std::logic_error("particle preview owner was not released");
    session.effect_preview_tasks.reset();session.effect_task_frames.close();session.effect_preview.reset();session.effect_preview_textures.reset();session.effect_preview_documents.reset();session.effect_preview_package.reset();session.effect_preview_stream.reset();session.effect_preview_triangles=0;
    if(session.effect_preview_counts.cylinders)throw std::logic_error("effect preview owners were not released");
    if(session.game)session.game->Clear();
    if(session.arena_skill_state){session.arena_skill_state->Close();session.arena_skill_state.reset();}
    if(session.staged_monster_scene_runtime){session.staged_monster_scene_runtime->Close();session.staged_monster_scene_runtime.reset();}
    ReleaseMonsterGraphics(session);
    if(session.monster_scene_runtime){session.monster_scene_runtime->Close();session.monster_scene_runtime.reset();}
    if(session.game_media)session.game_media->Cancel();
    session.tutorial.reset();session.tutorial_catalog.clear();session.tutorial_presentation.reset();session.tutorial_presentation_id=0;
    session.dialog.reset();session.dialog_assets.reset();session.ui_package.reset();session.ui_package_stream.reset();session.dialog_frames.close();session.dialog_revision=0;session.dialog_clear_pending=true;
    session.time_script.reset();
    session.game.reset();session.game_script_bytes.Clear();session.game_modules.clear();session.time_script_modules.clear();session.game_effect.reset();session.game_media.reset();session.game_settings.reset();session.game_arena.reset();session.sound_services.reset();session.game_services.reset();session.game_frames.close();
    session.game_placement.reset();session.placement_services.reset();session.game_movement.reset();
    session.player_rotation.reset();session.player_notifications.reset();
    session.arena_ready.reset();session.ready_input.reset();session.ready_services.reset();session.pet_stored_source=0;
    session.system_ui_permissions.reset();session.game_entries={};
    session.player_walks.reset();session.walk_frames.close();session.npc_action_frames.close();session.npc_walk_frames.close();
    session.player_equipment.reset();session.player_turns.reset();session.player_actions.reset();session.actor_package.reset();session.actor_package_stream.reset();session.action_frames.close();
    for(auto& instance:session.player_distance)instance.reset();
    session.player_render_clip.Reset();session.player_pose_frames.clear();
    session.npc_walks.reset();session.npc_actions.reset();ReleaseNpcGraphics(session);session.scene_npcs.reset();
    ReleaseObjectGraphics(session);session.scene_objects.reset();session.scene_collision.reset();
    session.object_distance.clear();session.npc_distance.clear();session.distance.reset();session.minimap.reset();
    ReleaseEnvironmentGraphics(session);session.arena.reset();session.game_modules.clear();session.scene_initializers=0;
    session.camera_follow.reset();session.controller.Shutdown();session.players.Clear();
    session.camera_tracks.reset();session.camera_frames.close();session.game_camera.reset();
    session.arena_source.reset();
    session.floor_attributes.reset();session.temperature_floor_selection=4;session.minimap_absence_verified=false;
    session.scene_ui={};
    ++session.media_generation;session.media_cancel_pending=true;session.game_recorded_calls=0;
    session.requested_new_game=false;session.game_reported=false;session.game_frame=0;session.game_globals.fill(0);
}
TutorialView ChapterTextWindowView(const ChapterHost& session){
    if(session.interaction_hints && session.interaction_hints->View().visible){
        const auto& hint=session.interaction_hints->View();TutorialView view;
        view.generation=hint.generation;view.id=hint.id;view.revision=hint.revision;view.visible=true;view.source=hint.text;return view;
    }
    if(session.tutorial)return session.tutorial->View();
    TutorialView view;view.generation=session.media_generation;return view;
}
bool CloseChapterTextWindow(ChapterHost& session,std::uint64_t generation,std::uint64_t id){
    const auto view=ChapterTextWindowView(session);
    if(!view.visible || view.generation!=generation || view.id!=id)return false;
    session.confirm_suppressed=session.input_confirm;session.confirm_pending=false;
    if(session.interaction_hints && session.interaction_hints->Close(generation,id))return true;
    return session.tutorial && session.tutorial->Close(generation,id);
}
void SetChapterInteractionInput(ChapterHost& session,bool pressed){
    if(!pressed)session.confirm_suppressed=false;
    if(pressed && ((session.tutorial && session.tutorial->View().visible) || (session.dialog && session.dialog->View().visible)))session.confirm_suppressed=true;
    if(pressed && !session.input_confirm && !session.confirm_suppressed)session.confirm_pending=true;
    session.input_confirm=pressed;
}
const TutorialPresentation* ChapterTextWindowPresentation(ChapterHost& session){
    const auto view=ChapterTextWindowView(session);if(!view.visible)return nullptr;
    if(!session.tutorial_presentation || session.tutorial_presentation_id!=view.id){
        if(!session.dialog_assets)throw std::logic_error("text window assets owner is missing");
        if(session.interaction_hints && session.interaction_hints->View().visible){const auto& hint=session.interaction_hints->View();session.tutorial_presentation=PrepareHintPresentation(*session.dialog_assets,view.source,hint.x,hint.y);}
        else session.tutorial_presentation=PrepareTutorialPresentation(*session.dialog_assets,view.source);
        session.tutorial_presentation_id=view.id;
    }
    return &*session.tutorial_presentation;
}
namespace {
void EnsureInventory(ChapterHost& session){
    if(session.inventory_services)return;
    if(!session.interaction_hints || !session.sound_services)throw std::logic_error("inventory UI/audio services are missing");
    session.inventory=std::make_unique<Inventory>(session.property_catalog);
    session.inventory_services=std::make_unique<InventoryServices>(*session.inventory,session.property_catalog,[&session](std::uint32_t key)->std::optional<std::string>{
        const auto found=session.item_messages.find(key);return found==session.item_messages.end()?std::nullopt:std::optional(found->second);
    },[&session](std::string_view text,float x,float y,float duration){session.interaction_hints->ShowMessage(text,x,y,duration);},
       [&session](std::string_view name,std::uint32_t repeats){(void)session.sound_services->PlayUiSound(name,repeats);});
}
}
void EnsureChapterInventory(ChapterHost& session){EnsureInventory(session);}
void BeginNewGame(ChapterHost& session) {
    StopGame(session);
    session.game_clock.Reset();
    session.game_entries.InitializeSceneUiDefaults();
    auto database=ReadOriginalDatabasePackage(session.game_data);
    auto player_data=ReadOriginalPlayerData(database);
    session.roles=std::move(player_data.roles);session.upgrades=std::move(player_data.upgrades);session.equipment_catalog=std::move(player_data.equipment);
    session.original_scenes=std::move(database.scenes);
    session.property_catalog=std::move(database.properties);
    session.tutorial_catalog=std::move(database.tutorials);
    session.help_topics=std::move(database.help_topics);
    session.help_tips=std::move(database.help_tips);
    session.interaction_text=std::move(database.game_strings);
    session.item_messages=session.interaction_text;
    session.scene_effect_frames.open(session.directory+"/scene-effect-frames.jsonl",std::ios::trunc);if(!session.scene_effect_frames)throw std::runtime_error("cannot write scene effect frames");
    session.arena_source=std::make_shared<ArenaSource>(session.game_data);
    session.game_services=std::make_unique<GameServices>(session.trace);
    session.arena_skill_state=std::make_unique<ArenaSkillState>();
    session.game_services->hide_skill_object=[&session] {
        if(!session.arena_skill_state)throw std::logic_error("arena skill owner is unavailable");
        (void)session.arena_skill_state->HideSkillObject();
    };
    session.game_services->get_object_visible=[&session](std::string_view name) {
        if(!session.scene_objects)throw std::logic_error("object visibility query requires a loaded scene");
        return session.scene_objects->IsVisible(name);
    };
    session.game_services->reset_object=[&session](std::string_view name) {
        if(!session.scene_objects)throw std::logic_error("GOB reset requires a loaded scene");
        (void)session.scene_objects->ResetConfiguration(name);
    };
    const auto inventory_change=[&session](std::uint32_t id,std::int32_t count,bool hint,bool adding){
        EnsureInventory(session);
        if(adding)session.inventory_services->Add(id,count,hint);else session.inventory_services->Remove(id,count,hint);
        std::ofstream report(session.directory+"/inventory-probe.json",std::ios::trunc);session.inventory->WriteReport(report);
    };
    session.game_services->add_property=[inventory_change](std::uint32_t id,std::int32_t count,bool hint){inventory_change(id,count,hint,true);};
    session.game_services->remove_property=[inventory_change](std::uint32_t id,std::int32_t count,bool hint){inventory_change(id,count,hint,false);};
    session.distance=std::make_unique<DistanceOptimization>();
    session.game_services->set_camera_distance=[&session](bool enabled){session.distance->SetEnabled(enabled);session.trace<<"Atomic distance optimization enabled="<<enabled<<std::endl;};
    session.game_camera=std::make_unique<GameCameraSystem>();
    session.camera_follow=std::make_unique<CameraFollow>(*session.game_camera,
        [&session,radii=std::array<std::optional<float>,4>{}]() mutable ->std::optional<CameraFollowTarget> {
            if(!session.players.Prepared() || !session.controller.State().actor)return {};
            const auto index=*session.controller.State().actor;const auto& player=session.players.Slots().at(index);
            if(!radii[index]){const auto sphere=ActorBindSphere(*player.actor);if(!sphere)throw std::runtime_error("camera follow requires original actor bounds");radii[index]=(*sphere)[3];}
            return CameraFollowTarget{player.position,player.rotation,*radii[index],player.auto_seek};
        },[&session](bool enabled){session.players.SetAutoSeek(enabled);});
    const auto report_camera=[&session] {
        std::ofstream out(session.directory+"/camera-follow-probe.json",std::ios::trunc);
        out<<"{\"follow\":";session.camera_follow->WriteReport(out);out<<",\"camera\":";WriteGameCameraReport(out,*session.game_camera);out<<'}';
    };
    session.game_services->set_camera_mode=[&session,report_camera](unsigned mode,bool sync){session.camera_follow->SetMode(mode,sync);report_camera();};
    session.game_services->flush_camera_tail_yaw=[&session,report_camera]{session.camera_follow->FlushTailYaw();report_camera();};
    session.game_services->seek_camera_player=[&session,report_camera]{session.camera_follow->SeekToPlayer();report_camera();};
    session.camera_tracks=std::make_unique<CameraTracks>(*session.game_camera,[&session](std::string_view member)->std::optional<std::vector<std::uint8_t>> {
        std::ifstream input(session.game_data+"/scenedata.cpk",std::ios::binary);
        if(!input)throw std::runtime_error("cannot open original scene camera package");
        PackageReader package(input);if(!package.Find(member))return {};
        auto bytes=package.Read(member);session.trace<<"original camera member="<<member<<" bytes="<<bytes.size()<<std::endl;return bytes;
    });
    session.camera_frames.open(session.directory+"/camera-frames.jsonl",std::ios::trunc);
    if(!session.camera_frames)throw std::runtime_error("cannot open camera frame evidence");
    std::remove((session.directory+"/camera-track-probe.json").c_str());
    session.game_services->prepare_camera=[&session](const ScriptRequest& request) {
        if(!session.arena)throw std::logic_error("camera prepare requires active arena");
        return session.camera_tracks->Prepare(session.arena->description.world,session.arena->description.scene,std::get<std::string>(request.call.arguments[0]));
    };
    session.game_services->run_camera=[&session](const ScriptRequest& request) {
        return session.camera_tracks->RunSingle(request.token,std::get<std::string>(request.call.arguments[0]),std::get<bool>(request.call.arguments[1]));
    };
    session.game_services->wait_camera=[&session](const ScriptRequest& request){return session.camera_tracks->WaitCurrent(request.token);};
    session.player_equipment=std::make_unique<PlayerEquipment>(session.players,session.equipment_catalog);
    std::remove((session.directory+"/player-equipment-probe.json").c_str());
    session.game_services->add_equipment=[&session](std::int32_t player,std::uint32_t equipment) {
        session.player_equipment->Add(player,equipment);
        std::ofstream output(session.directory+"/player-equipment-probe.json",std::ios::trunc);session.player_equipment->WriteReport(output);
    };
    session.player_actions=std::make_unique<PlayerActions>(session.players,[&session](std::string_view name)->std::optional<std::vector<std::uint8_t>> {
        return ReadActorMember(session,name);
    });
    session.npc_action_frames.open(session.directory+"/npc-action-frames.jsonl",std::ios::trunc);
    if(!session.npc_action_frames)throw std::runtime_error("cannot open NPC action evidence");
    session.game_services->npc_action=[&session](const ScriptRequest& request){
        if(!session.npc_actions)throw std::logic_error("NPC action requires a loaded scene");
        const auto& a=request.call.arguments;
        ScriptResponse result;
        if(request.call.system_id==273){
            (void)std::get<bool>(a[1]); // 原处理器不读取注册的bool；结束等待始终阻塞。
            result=session.npc_actions->End(request.token,std::get<std::string>(a[0]));
        }else result=session.npc_actions->Start(request.token,std::get<std::string>(a[0]),std::get<std::string>(a[1]),std::get<std::int32_t>(a[2]),std::get<bool>(a[3]));
        std::ofstream report(session.directory+"/npc-action-probe.json",std::ios::trunc);session.npc_actions->WriteReport(report);return result;
    };
    session.npc_walk_frames.open(session.directory+"/npc-walk-frames.jsonl",std::ios::trunc);
    if(!session.npc_walk_frames)throw std::runtime_error("cannot open NPC movement evidence");
    session.game_services->npc_walk=[&session](const ScriptRequest& request){
        if(!session.npc_walks)throw std::logic_error("NPC movement requires a loaded scene");
        const auto& a=request.call.arguments;
        const auto result=session.npc_walks->Start(request.token,std::get<std::string>(a[0]),{std::get<float>(a[1]),std::get<float>(a[2]),std::get<float>(a[3])},request.call.system_id==270,std::get<bool>(a[4]));
        session.npc_walk_frames<<"{\"event\":\"start\",\"walk\":";session.npc_walks->WriteReport(session.npc_walk_frames);session.npc_walk_frames<<"}\n"<<std::flush;
        return result;
    };
    session.player_turns=std::make_unique<PlayerTurns>(session.players,*session.player_actions);
    session.walk_frames.open(session.directory+"/player-walk-frames.jsonl",std::ios::trunc);
    if(!session.walk_frames)throw std::runtime_error("cannot open player walk frame evidence");
    session.game_services->player_walk=[&session](const ScriptRequest& request){
        if(!session.player_walks)throw std::logic_error("player walk requires a loaded scene");
        const auto& a=request.call.arguments;
        const auto result=request.call.system_id==267?session.player_walks->End(request.token,std::get<std::int32_t>(a[0])):
            session.player_walks->Start(request.token,std::get<std::int32_t>(a[0]),{std::get<float>(a[1]),std::get<float>(a[2]),std::get<float>(a[3])},std::get<bool>(a[4]));
        std::ofstream report(session.directory+"/player-walk-probe.json",std::ios::trunc);session.player_walks->WriteReport(report);return result;
    };
    session.game_services->player_turn=[&session](const ScriptRequest& request){const auto& a=request.call.arguments;return session.player_turns->Start(request.token,std::get<std::int32_t>(a[0]),std::get<float>(a[1]),std::get<bool>(a[2]));};
    session.action_frames.open(session.directory+"/player-action-frames.jsonl",std::ios::trunc);
    if(!session.action_frames)throw std::runtime_error("cannot open player action frame evidence");
    session.game_services->player_action=[&session](const ScriptRequest& request) {
        const auto& args=request.call.arguments;
        ScriptResponse result;
        if(request.call.system_id==118)session.player_actions->Unhold(std::get<std::int32_t>(args[0]));
        else if(request.call.system_id==246)result=session.player_actions->End(request.token,std::get<std::int32_t>(args[0]));
        else result=session.player_actions->Start(request.token,std::get<std::int32_t>(args[0]),std::get<std::string>(args[1]),std::get<std::int32_t>(args[2]),std::get<bool>(args[3]));
        std::ofstream output(session.directory+"/player-action-probe.json",std::ios::trunc);session.player_actions->WriteReport(output,false);return result;
    };
    session.system_ui_permissions=std::make_unique<SystemUiPermissions>(session.game_entries);
    const auto report_permissions=[&session] {
        std::ofstream output(session.directory+"/system-ui-permissions-probe.json",std::ios::trunc);session.system_ui_permissions->WriteReport(output);
    };
    session.game_services->grant_magic=[&session,report_permissions](bool first,bool second) {
        session.system_ui_permissions->GrantMagic(first,second);report_permissions();
    };
    session.game_services->grant_system_ui=[&session,report_permissions](std::int32_t index,bool enabled) {
        session.system_ui_permissions->Grant(index,enabled);report_permissions();
    };
    session.player_notifications=std::make_unique<PlayerNotifications>();
    session.player_rotation=std::make_unique<PlayerRotation>(session.players,*session.player_notifications);
    session.game_services->set_angle=[&session](float angle) {
        session.player_rotation->SetCurrentAngle(angle);
        std::ofstream output(session.directory+"/player-rotation-probe.json",std::ios::trunc);
        session.player_rotation->WriteReport(output);
        std::ofstream notifications(session.directory+"/player-notifications-probe.json",std::ios::trunc);
        session.player_notifications->WriteReport(notifications);
    };
    session.game_services->ready_arena=[&session](bool restore) {
        if(!session.arena_ready)throw std::logic_error("arena ready requires loaded scene");
        const auto report=[&session] {
            std::ofstream output(session.directory+"/arena-ready-probe.json",std::ios::trunc);session.arena_ready->WriteReport(output);
        };
        try{if(restore)session.arena_ready->Restore();else session.arena_ready->Run();}catch(...){report();throw;}report();
    };
    session.game_services->object_visible=[&session](std::string_view name,bool visible){
        if(!session.scene_objects)throw std::logic_error("object visibility requires a loaded scene");
        try{(void)session.scene_objects->SetVisible(name,visible);}catch(...){std::ofstream output(session.directory+"/scene-objects-probe.json",std::ios::trunc);session.scene_objects->WriteReport(output);throw;}
        std::ofstream output(session.directory+"/scene-objects-probe.json",std::ios::trunc);session.scene_objects->WriteReport(output);
        if(session.capture_frame_traces){session.scene_effect_capture_remaining=300;session.scene_effect_frames<<"{\"event\":\"visibility\",\"name\":"<<std::quoted(std::string(name))<<",\"visible\":"<<(visible?"true":"false")<<",\"playback\":";session.effect_playback->WriteReport(session.scene_effect_frames);session.scene_effect_frames<<"}\n"<<std::flush;}
    };
    session.game_services->npc_visible=[&session](std::string_view name,bool visible){
        if(!session.scene_npcs)throw std::logic_error("NPC visibility requires a loaded scene");
        const auto report=[&session]{std::ofstream output(session.directory+"/scene-npcs-probe.json",std::ios::trunc);session.scene_npcs->WriteReport(output,false);};
        try{(void)session.scene_npcs->SetVisible(name,visible);}catch(...){report();throw;}report();
    };
    session.floor_attributes=std::make_unique<FloorAttributes>();
    for(unsigned i=0;i<pal4::kTemperatureFloorRgba.size();++i)
        session.temperature_floor_colors[i]=FloorPixel8888(pal4::kTemperatureFloorRgba[i]);
    session.temperature_floor_colors[4]=session.floor_attributes->CurrentColor();
    for(auto pixel:session.temperature_floor_colors)
        session.floor_attributes->AddCallback(pixel,[&session](std::uint32_t observed) {
            (void)pal4::SelectTemperatureFloorColor(session.temperature_floor_colors,observed,session.temperature_floor_selection);
        });
    session.game_services->set_camera_collision=[&session](bool enabled) {
        session.game_camera->SetCollision(enabled);
        std::ofstream report(session.directory+"/game-camera-probe.json",std::ios::trunc);
        WriteGameCameraReport(report,*session.game_camera);
    };
    const auto report_players=[&session] {
        std::ofstream report(session.directory+"/player-control-probe.json",std::ios::trunc);
        WritePlayerReport(report,session.players);
    };
    session.game_services->set_leader=[&session,report_players](std::int32_t index) {
        session.players.SetLeader(index,[&session](std::uint32_t selected_index) {
            if(!session.arena)throw std::logic_error("leader requires an active arena");
            session.controller.SelectActor(selected_index,session.arena->description.type);
            const auto& selected=session.players.Slots()[selected_index];session.instance_scene=selected.actor->mesh;
            session.clip=-1;
            for(std::size_t i=0;i<selected.actor->clips.size();++i)if(selected.actor->clips[i].name==selected.animation)session.clip=static_cast<int>(i);
            session.player_render_clip.Sample(*selected.actor,session.clip,0,session.instance_scene.vertices);
            session.instance_renderer.Upload(session.instance_scene);session.animation_start=Clock::now();
        });
        report_players();
    };
    session.game_services->get_leader=[&session]() -> std::int32_t {
        if(!session.players.Prepared())throw std::logic_error("leader query requires prepared players");
        return static_cast<std::int32_t>(session.players.Leader());
    };
    session.game_services->add_team_member=[&session,report_players](std::int32_t index,bool) {
        session.players.SetInTeam(index,true);report_players();
    };
    session.game_services->remove_team_member=[&session,report_players](std::int32_t index,bool) {
        session.players.SetInTeam(index,false);report_players();
    };
    session.game_services->set_auto_seek=[&session,report_players](bool enabled) {
        session.players.SetAutoSeek(enabled);report_players();
    };
    session.game_services->lock_player=[&session] {
        ResetServices services(session);
        LockPlayer(session.players,session.controller,session.scene_ui,services,[&session](std::uint32_t index,std::string_view action) {
            SelectGameAnimation(session,index,action);
        });
        std::ofstream report(session.directory+"/player-lock-probe.json",std::ios::trunc);
        report<<"{\"controller\":";WriteControllerReport(report,session.controller);
        report<<",\"ui\":";WriteSceneUiReport(report,session.scene_ui);
        report<<",\"monsters_verified_empty\":"<<(session.arena->monsters_verified_empty?"true":"false")
            <<",\"monster_pursuit_stopped\":"<<(session.arena->monster_pursuit_stopped?"true":"false")<<'}';
    };
    session.game_services->object_research=[&session](std::string_view name,bool enabled) {
        if(!session.scene_objects)throw std::logic_error("GOB research requires an active scene");
        (void)session.scene_objects->SetResearch(name,enabled);
    };
    session.game_services->event_visible=[&session](std::string_view name,bool visible) {
        if(!session.scene_events)throw std::logic_error("EVF visibility requires complete event cache");
        (void)session.scene_events->SetVisible(name,visible);
        std::ofstream report(session.directory+"/scene-events-probe.json",std::ios::trunc);session.scene_events->WriteReport(report);
    };
    session.game_services->unlock_player=[&session] {
        if(!session.placement_services)throw std::logic_error("player unlock placement owner is unavailable");
        ResetServices services(session);
        UnlockPlayer(session.players,session.controller,session.scene_ui,services,[&session](Point3 position,Point3 rotation) {
            session.placement_services->UpdateGrid(position,rotation);
        });
        std::ofstream report(session.directory+"/player-unlock-probe.json",std::ios::trunc);
        report<<"{\"controller\":";WriteControllerReport(report,session.controller);
        report<<",\"ui\":";WriteSceneUiReport(report,session.scene_ui);report<<'}';
    };
    session.game_services->set_position=[&session](Point3 position) {
        if(!session.game_placement)throw std::logic_error("player placement requires an active arena");
        const auto report=[&session] {
            std::ofstream output(session.directory+"/player-placement-probe.json",std::ios::trunc);session.game_placement->WriteReport(output);
            std::ofstream floor_output(session.directory+"/floor-attributes-probe.json",std::ios::trunc);
            session.floor_attributes->WriteReport(floor_output);
        };
        try{session.game_placement->SetCurrent(position);}catch(...){report();throw;}
        report();
    };
    session.game_services->get_current_x=[&session]() -> float {
        if(!session.game_placement)throw std::logic_error("player placement requires an active arena");
        return session.game_placement->CurrentX();
    };
    session.game_services->get_current_y=[&session]() -> float {
        if(!session.game_placement)throw std::logic_error("player placement requires an active arena");
        return session.game_placement->CurrentY();
    };
    session.game_services->get_current_z=[&session]() -> float {
        if(!session.game_placement)throw std::logic_error("player placement requires an active arena");
        return session.game_placement->CurrentZ();
    };
    session.game_services->set_full_hp=[&session,report_players] {
        session.players.SetFullHp();report_players();
    };
    session.game_services->set_full_mp=[&session,report_players] {
        session.players.SetFullMp();report_players();
    };
    session.game_services->set_full_rage=[&session,report_players] {
        session.players.SetFullRage();report_players();
    };
    session.game_services->player_take_rest=[&session,report_players] {
        session.players.TakeARest();report_players();
    };
    session.game_services->set_player_level=[&session,report_players](std::int32_t index,std::int32_t level) {
        session.players.SetLevel(index,level);report_players();
    };
    session.game_services->get_player_level=[&session](std::int32_t index) -> std::int32_t {
        return session.players.GetLevel(index);
    };
    session.game_services->add_player_skill=[&session,report_players](std::int32_t index,std::uint32_t skill_id,bool) {
        session.players.AddSkill(index,skill_id);report_players();
    };
    session.game_services->add_player_favor=[&session,report_players](std::int32_t first_index,std::int32_t second_index,std::int32_t delta) {
        session.players.AddFavor(first_index,second_index,delta);report_players();
    };
    session.game_services->set_player_visible=[&session,report_players](std::int32_t index,bool visible) {
        session.players.SetVisible(index,visible);report_players();
    };
    session.game_services->set_current_player_visible=[&session,report_players](bool visible) {
        session.players.SetVisible(static_cast<std::int32_t>(session.players.Leader()),visible);report_players();
    };
    session.game_services->set_player_position=[&session,report_players](std::int32_t index,Point3 pos) {
        session.players.SetPosition(static_cast<std::uint32_t>(index),pos);report_players();
    };
    session.game_services->check_property=[&session](std::uint32_t id,std::int32_t required) -> bool {
        if(!session.inventory)throw std::logic_error("inventory query requires initialized inventory");
        return required<=0 || session.inventory->Count(id)>=static_cast<std::uint32_t>(required);
    };
    session.game_services->get_property_count=[&session](std::uint32_t id) -> std::int32_t {
        if(!session.inventory)throw std::logic_error("inventory query requires initialized inventory");
        return static_cast<std::int32_t>(session.inventory->Count(id));
    };
    session.game_services->get_money=[&session]() -> std::int32_t {
        if(!session.inventory)throw std::logic_error("money query requires initialized inventory");
        return session.inventory->Money();
    };
    session.game_services->add_money=[&session](std::int32_t amount,bool) {
        if(!session.inventory)throw std::logic_error("money transaction requires initialized inventory");
        session.inventory->AdjustMoney(amount);
    };
    session.game_services->pay_money=[&session](std::int32_t amount,bool) {
        if(!session.inventory)throw std::logic_error("money transaction requires initialized inventory");
        session.inventory->AdjustMoney(-amount);
    };
    session.game_services->get_arena_name=[&session]() -> std::string {
        if(!session.arena)throw std::logic_error("arena name query requires an active arena");
        return session.arena->description.scene;
    };
    session.game_services->get_arena_area=[&session]() -> std::string {
        if(!session.arena)throw std::logic_error("arena area query requires an active arena");
        return session.arena->description.world;
    };
    std::remove((session.directory+"/players-probe.json").c_str());
    std::remove((session.directory+"/player-control-probe.json").c_str());
    std::remove((session.directory+"/player-lock-probe.json").c_str());
    std::remove((session.directory+"/player-placement-probe.json").c_str());
    std::remove((session.directory+"/controller-probe.json").c_str());
    std::remove((session.directory+"/arena-probe.json").c_str());
    session.game_services->prepare_players=[&session] {
        session.players.Prepare(session.roles,session.upgrades,[&session](std::string_view model) {
            return session.arena_source->ReadOriginalActorResource("gamedata\\PALActor\\"+std::string(model)+"\\",model,{},true);
        });
        for(std::size_t i=0;i<session.player_distance.size();++i)session.player_distance[i]=std::make_unique<AtomicDistanceInstance>(*session.players.Slots()[i].actor,*session.distance);
        std::ofstream report(session.directory+"/players-probe.json",std::ios::trunc);
        WritePlayerReport(report,session.players);
        session.controller.Shutdown();
        session.controller.Initialize(session.width,session.height,[&session] {
            return session.arena_source->ReadOriginalActorResource("gamedata\\PALObject\\JDumy\\","JDumy");
        });
        std::ofstream control_report(session.directory+"/controller-probe.json",std::ios::trunc);
        WriteControllerReport(control_report,session.controller);
        session.trace<<"NewGame initialized four players and idle controller with original JDumy; no active scene actor"<<std::endl;
    };
    session.sound_services=std::make_unique<SoundServices>(*session.game_services,session.media_generation,[&session](std::string_view path) {
        constexpr std::string_view prefix="gamedata\\PALSound\\";
        if(!path.starts_with(prefix))throw std::invalid_argument("invalid original sound path");
        const auto filename=session.game_data+"/PALSound/"+std::string(path.substr(prefix.size()));
        std::ifstream input(ResolveOriginalPath(filename),std::ios::binary|std::ios::ate);
        if(!input)throw std::runtime_error("cannot open original sound: "+filename);
        const auto size=input.tellg();if(size<=0 || size>64*1024*1024)throw std::runtime_error("invalid original sound length");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));input.seekg(0);
        if(!input.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("truncated original sound file");
        session.trace<<"original sound member="<<path.substr(prefix.size())<<" bytes="<<bytes.size()<<std::endl;return bytes;
    });
    session.interaction_hints=std::make_unique<InteractionHints>(session.media_generation,[&session](std::uint32_t key)->std::optional<std::string>{
        const auto found=session.interaction_text.find(key);return found==session.interaction_text.end()?std::nullopt:std::optional(found->second);
    },[&session](std::string_view name,std::uint32_t repeat){(void)session.sound_services->PlayUiSound(name,repeat);});
    session.dialog_assets=std::make_unique<DialogAssets>([&session](std::string_view member)->std::optional<std::vector<std::uint8_t>> {
        return ReadUiMember(session,member);
    });
    session.dialog=std::make_unique<DialogServices>(*session.sound_services,*session.dialog_assets,session.media_generation);
    session.dialog->Prepare();
    session.dialog_frames.open(session.directory+"/dialog-frames.jsonl",std::ios::trunc);
    std::remove((session.directory+"/dialog-probe.json").c_str());
    session.game_arena=std::make_unique<ArenaServices>(*session.dialog);
    if(session.tutorial_ui_available){
        session.tutorial=std::make_unique<TutorialServices>(*session.game_arena,session.tutorial_catalog,session.media_generation,session.settings.tutorials_enabled,[&session](std::uint32_t key){
            if(!session.game_settings)throw std::logic_error("tutorial settings owner is missing");
            session.game_settings->SaveTutorial(key);session.trace<<"tutorial settings saved key="<<key<<std::endl;
        });
    }
    IScriptContextServices& tutorial_next=session.tutorial?static_cast<IScriptContextServices&>(*session.tutorial):static_cast<IScriptContextServices&>(*session.game_arena);
    session.game_settings=std::make_unique<SettingsServices>(tutorial_next,session.settings,*session.settings_store);
    session.game_media=std::make_unique<MediaServices>(*session.game_settings,session.media_generation);
    session.game_effect=std::make_unique<ScreenEffectServices>(*session.game_media);
    session.game=std::make_unique<ScriptStack>(session.game_globals);
    session.time_script=std::make_unique<TimeScript>([&session](std::string_view callback) {
        if(!session.arena || !session.game)throw std::logic_error("time script requires an active arena");
        const auto& world=session.arena->description.world;
        const auto* source=session.game_script_bytes.Find(world);
        if(!source)throw std::logic_error("current world CSB is not loaded");
        const auto module="TimeScript["+std::string(callback)+"]";
        if(!session.game_modules.contains(module)) {
            session.game->LoadCsb(module,*source,DecodeGbk);
            session.game_modules.insert(module);
            session.time_script_modules.insert(module);
        }
        const auto declaration="void "+std::string(callback)+"()";
        while(session.game->State()==ScriptState::finished && session.game->Depth()) {
            session.game->PopFinished();session.game_reported=false;
        }
        if(const auto context=PushTimeScriptCallback(*session.game,module,declaration,*session.game_effect)) {
            session.trace<<"time script callback="<<module<<" entry="<<declaration
                <<" context="<<*context<<std::endl;
        }
    });
    session.game_services->start_time_script=[&session](float delay,std::string_view callback) {
        if(!session.time_script)throw std::logic_error("time script owner is unavailable");
        session.time_script->Start(delay,callback);
    };
    session.game_services->terminate_time_script=[&session] {
        if(!session.time_script)throw std::logic_error("time script owner is unavailable");
        session.time_script->Terminate();
    };
    {
        std::ifstream input(ResolveOriginalPath(session.game_data+"/script.cpk"),std::ios::binary);
        if(!input)throw std::runtime_error("原始脚本包缺失："+session.game_data+"/script.cpk");
        PackageReader package(input);const auto bytes=package.Read("script.csb");session.game->LoadCsb("script",bytes,DecodeGbk);
        session.trace<<"original CSB loaded module=script member=script.csb bytes="<<bytes.size()<<std::endl;
    }
    session.game_modules.insert("script");
    if(!session.game->Push("script","void NewGame()",*session.game_effect))throw std::runtime_error("NewGame entry missing");
    session.game_frames.open(session.directory+"/game-frames.jsonl",std::ios::trunc);
    if(!session.game_frames)throw std::runtime_error("cannot write game frame evidence");
    session.input_x=session.input_y=0;
    session.trace<<"NewGame started with screen effects and platform media; remaining services fail explicitly"<<std::endl;
}
void EnsureEffectResources(ChapterHost& session);
namespace {
void LoadWorldScript(ChapterHost& session,std::string_view world) {
    const auto& bytes=session.game_script_bytes.Load(world,[&session](std::string_view name) {
        std::ifstream input(ResolveOriginalPath(session.game_data+"/script.cpk"),std::ios::binary);
        if(!input)throw std::runtime_error("cannot open original script.cpk");
        PackageReader package(input);return package.Read(std::string(name)+".csb");
    });
    if(session.game_modules.contains(std::string(world)))return;
    session.game->LoadCsb(world,bytes,DecodeGbk);
    session.trace<<"original CSB loaded module="<<world<<" bytes="<<bytes.size()<<std::endl;
    session.game_modules.insert(std::string(world));
}
class EventScripts final:public ISceneEventScripts {
    ChapterHost& session;
public:
    explicit EventScripts(ChapterHost& host):session(host){}
    bool Contains(std::string_view world,std::string_view declaration) override{return session.game->ContainsActive(world,declaration);}
    void LoadWorld(std::string_view world) override{LoadWorldScript(session,world);}
    std::int32_t Push(std::string_view world,std::string_view declaration) override {
        if(session.game->State()==ScriptState::failed)return -1;
        while(session.game->State()==ScriptState::finished)session.game->PopFinished();
        const auto context=session.game->Push(world,declaration,*session.game_effect);
        session.trace<<"EVF push world="<<world<<" entry="<<declaration<<" context="<<context.value_or(0)<<std::endl;
        session.game_reported=false;return context?0:-6;
    }
};
void RetireInactiveWorldModules(ChapterHost& session) {
    if(!session.game || !session.arena)return;
    for(auto it=session.game_modules.begin();it!=session.game_modules.end();) {
        const auto& module=*it;
        const bool world_module=std::any_of(session.original_scenes.begin(),session.original_scenes.end(),
            [&](const auto& scene){return scene.world==module;});
        if(!world_module || module==session.arena->description.world ||
            session.game->HasActiveModule(module)) {
            ++it;continue;
        }
        session.game->UnloadModule(module);
        it=session.game_modules.erase(it);
    }
}
}
void AdvanceArenaLoad(ChapterHost& session) {
    if(!session.game_arena)return;
    const auto command=session.game_arena->TakeCommand();if(!command)return;
    session.movement_input_gate.Arm(session.input_x,session.input_y);session.input_x=session.input_y=0;
    bool scene_committed=false;
    const auto present_loading=[&](std::optional<float> progress){
        session.loading_visible=progress.has_value();
        if(progress)session.loading_progress=*progress;
        if(session.present_loading){session.present_loading(progress);if(progress)++session.loading_presentations;}
    };
    try {
        present_loading(0.f);
        if(!command->resource.empty())throw std::runtime_error("arena resource variant is not implemented");
        auto next=std::make_unique<ArenaResources>(LoadArenaResources(command->world,command->scene,[&](std::string_view name) {
            return session.arena_source->Read(name);
        },[&] {return session.arena_source->ReadOriginalCollision(command->world,command->scene);},true,[&](const SceneGob& gob){
            auto dff=session.arena_source->ReadObjectMember(gob.resource_directory,gob.model+".dff");
            if(!dff)throw std::runtime_error("original object collision DFF is missing: "+gob.model);
            return ReadDffObjectCollision(*dff);
        },[&]{return session.arena_source->ReadOriginalDescription(command->world,command->scene,session.original_scenes);},
          [&](std::string_view kind){return session.arena_source->ReadOriginalEnvironment(command->world,command->scene,kind);},
          [&]{return session.arena_source->ReadOriginalWorld(command->world,command->scene);},
          [&](const SceneNpc& npc){return session.arena_source->ReadOriginalActorResource("gamedata\\PALActor\\"+npc.resource+"\\",npc.resource,{},true);},
          [&](const SceneGob& gob){return session.arena_source->ReadOriginalActorResource(gob.resource_directory,gob.model,gob.variant,true);}));
        present_loading(.4f); // The complete original resource phase has returned.
        PrepareArenaGameplayOwners(session,*next,command->generation);
        LoadWorldScript(session,command->world);
        ReleaseEnvironmentGraphics(session);
        session.renderer.Upload(next->world);
        session.instance_renderer.Release();session.scene={};
        session.actor.reset();session.level.reset();session.movement.reset();session.floor.reset();session.requested_action.clear();
        session.collision={};session.camera.Fit(next->world);
        if(next->description.type==2)throw std::runtime_error("arena without player binding is not implemented");
        session.players.EnterScene(command->world,command->scene);
        session.controller.SelectActor(0,next->description.type);
        const auto& selected=session.players.Slots()[0];session.instance_scene=selected.actor->mesh;
        session.clip=-1;
        for(std::size_t i=0;i<selected.actor->clips.size();++i)if(selected.actor->clips[i].name==selected.animation)session.clip=static_cast<int>(i);
        session.player_render_clip.Sample(*selected.actor,session.clip,0,session.instance_scene.vertices);session.instance_renderer.Upload(session.instance_scene);
        session.animation_start=Clock::now();
        std::ofstream binding_report(session.directory+"/arena-controller-probe.json",std::ios::trunc);WriteControllerReport(binding_report,session.controller);
        std::ofstream report(session.directory+"/arena-probe.json",std::ios::trunc);WriteArenaReport(report,*next);
        // Placement borrows movement, which borrows arena geometry. Release
        // those owners before replacing the arena, including failure paths.
        session.scene_interaction.reset();session.free_movement.reset();session.scene_events.reset();session.free_idle={};session.free_control_active=false;
        session.player_walks.reset();session.game_placement.reset();session.placement_services.reset();session.game_movement.reset();
        session.arena_ready.reset();session.ready_input.reset();session.ready_services.reset();
        session.npc_walks.reset();session.npc_actions.reset();ReleaseNpcGraphics(session);session.scene_npcs.reset();
        ReleaseObjectGraphics(session);session.scene_objects.reset();session.scene_collision.reset();
        session.object_distance.clear();session.npc_distance.clear();
        if(session.effect_playback)session.effect_playback->Clear();
        for(auto& [token,graphics]:session.playback_graphics){(void)token;for(auto& object:graphics)if(object.gpu)object.gpu->Release();}session.playback_graphics.clear();
        ReleaseMonsterGraphics(session);
        session.minimap.reset();session.arena=std::move(next);scene_committed=true;
        if(session.monster_scene_runtime){session.monster_scene_runtime->Close();session.monster_scene_runtime.reset();}
        UploadEnvironmentGraphics(session);
        session.minimap=std::make_unique<SceneMinimap>(command->world,command->scene,[&session](auto member){return session.arena_source->ReadSceneMember(member);},[&session](auto member){return ReadUiMember(session,member);});
        session.minimap->SetSceneType(session.arena->description.type);
        session.monster_scene_runtime=std::move(session.staged_monster_scene_runtime);
        UploadMonsterGraphics(session);
        EnsureEffectResources(session);
        if(!session.effect_playback)session.effect_playback=std::make_unique<EffectPlayback>([&session](std::string_view effect){return ReadEffectPlan(*session.effect_preview_documents->Load(effect));},*session.effect_preview_textures,session.particle_random,session.particle_counts,session.effect_preview_counts);
        session.scene_objects=std::make_unique<SceneObjects>(*session.arena,*session.minimap,[&session](auto directory,auto member){return session.arena_source->ReadObjectMember(directory,member);},session.effect_playback.get());
        session.scene_objects->ActivateInitialEffects();
        session.distance->SetEnabled(true);
        for(const auto& state:session.scene_objects->States()) {
            std::unique_ptr<AtomicDistanceInstance> instance;
            if(state.actor && (state.type==0 || state.type==6 || state.type==7))instance=std::make_unique<AtomicDistanceInstance>(session.arena->actors[*state.actor].actor,*session.distance);
            session.object_distance.push_back(std::move(instance));
        }
        UploadObjectGraphics(session);
        session.scene_npcs=std::make_unique<SceneNpcs>(*session.arena,[&session](auto member){return ReadActorMember(session,member);});
        session.npc_actions=std::make_unique<NpcActions>(*session.scene_npcs);
        session.npc_walks=std::make_unique<NpcWalks>(*session.scene_npcs,session.arena->collision,session.arena->world);
        for(const auto& state:session.scene_npcs->States())session.npc_distance.push_back(std::make_unique<AtomicDistanceInstance>(session.arena->actors[state.actor].actor,*session.distance));
        UploadNpcGraphics(session);
        {std::ofstream output(session.directory+"/scene-npcs-probe.json",std::ios::trunc);session.scene_npcs->WriteReport(output,false);}
        {std::ofstream output(session.directory+"/scene-objects-probe.json",std::ios::trunc);session.scene_objects->WriteReport(output);}
        {std::ofstream output(session.directory+"/arena-source-probe.json",std::ios::trunc);session.arena_source->WriteReport(output);}
        session.minimap_absence_verified=false;
        {
            const auto& world=command->world;const auto& scene=command->scene;
            std::string world_lower=world;
            for(auto& c:world_lower)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
            std::ifstream scene_input(session.game_data+"/scenedata.cpk",std::ios::binary);
            std::ifstream world_input(session.game_data+"/PALWorld/"+world_lower+".cpk",std::ios::binary);
            if(!scene_input || !world_input)throw std::runtime_error("cannot open original scene/world packages; check files and directory access");
            PackageReader scenes(scene_input),worlds(world_input);
            // A real known member guards against a wrong prefix or empty index.
            if(!scenes.Find(world+"\\"+scene+"\\"+scene+"_floor.dff") || !worlds.Find(scene+"\\"+scene+".bsp"))
                throw std::runtime_error("original floor/world package member is missing");
            if(scenes.Find(world+"\\"+scene+"\\"+scene+"_intri.dff"))
                throw std::runtime_error("original intri collision exists but its loader is not bound");
            session.minimap_absence_verified=!scenes.Find(world+"\\"+scene+"\\MiniMap.df") && !worlds.Find(scene+"\\MiniMap.df");
            session.trace<<"minimap original package absence verified="<<session.minimap_absence_verified<<std::endl;
        }
        session.scene_collision=std::make_unique<SceneCollision>([&session](auto resource,bool entering,auto previous){
            if(!session.scene_events)throw std::logic_error("EVF collision owner is not bound");
            session.scene_events->OnCollision(resource,entering,previous);
        });
        const auto& walls=session.arena->collision.wall;
        for(std::size_t first=0;first<walls.size();) {
            auto end=first+1;
            while(end<walls.size() && walls[end].atomic==walls[first].atomic)++end;
            session.scene_collision->Add(CollisionGroup::wall,std::span(walls).subspan(first,end-first));
            first=end;
        }
        if(session.arena->description.object_collision_cache) {
            session.scene_objects->BindMovement(*session.scene_collision,[&session](std::size_t object,std::size_t atomic) {
                const auto& instance=session.object_distance.at(object);
                if(!instance){
                    const auto type=session.scene_objects->States()[object].type;
                    if(type==3 || type==5)return true;
                    throw std::logic_error("GOB collision distance owner is missing");
                }
                return instance->CollisionAllowed(atomic);
            });
            session.trace<<"scene collision bound: original walls and complete GOB cache"<<std::endl;
        } else session.trace<<"legacy arena cache: GOB collision unavailable; free movement remains disabled"<<std::endl;
        if(session.arena->description.event_collision_cache) {
            if(!session.event_scripts)session.event_scripts=std::make_unique<EventScripts>(session);
            session.scene_events=std::make_unique<SceneEvents>(*session.arena,*session.scene_collision,*session.event_scripts);
            session.scene_events->ActivateAll();
            std::ofstream output(session.directory+"/scene-events-probe.json",std::ios::trunc);session.scene_events->WriteReport(output);
        }
        {std::ofstream output(session.directory+"/scene-collision-probe.json",std::ios::trunc);session.scene_collision->WriteReport(output);}
        {std::ofstream output(session.directory+"/scene-objects-probe.json",std::ios::trunc);session.scene_objects->WriteReport(output);}
        session.game_movement=std::make_unique<SceneMovement>(session.arena->collision.floor);
        session.placement_services=std::make_unique<PlacementServices>(session);
        session.game_placement=std::make_unique<PlayerPlacement>(session.players,*session.game_movement,*session.placement_services,*session.player_notifications);
        if(session.arena->description.object_collision_cache && session.arena->description.event_collision_cache && session.arena->description.type<=1)
            session.free_movement=std::make_unique<PlayerFreeMovement>(session.players,session.controller,*session.player_actions,
                *session.game_movement,*session.scene_collision,*session.placement_services,[&session]{
                    if(!session.game_camera)throw std::logic_error("free movement camera is missing");
                    (void)session.game_camera->Pose();
                });
        session.ready_input=std::make_unique<ResetServices>(session);session.ready_services=std::make_unique<ReadyServices>(session);
        session.arena_ready=std::make_unique<ArenaReady>(command->world,command->scene,session.players,session.controller,*session.ready_input,*session.ready_services);
        session.player_walks=std::make_unique<PlayerWalks>(session.players,*session.player_actions,session.arena->collision,session.arena->world,[&session](unsigned index,Point3 position,Point3 rotation){
            if(index!=session.players.Leader())return;
            if(session.players.Slots()[index].auto_seek)session.placement_services->FollowPlayer(index);
            const auto sphere=ActorBindSphere(*session.players.Slots()[index].actor);if(!sphere)throw std::logic_error("walking player bounds are missing");
            const auto range=(*sphere)[3]*2;
            if(!session.game_movement->SelectAndUpdate(position,range*.35f,range*2.53f))throw std::logic_error("walking player has no movement surface");
            const auto hit=session.game_movement->LastHit();if(!hit || session.game_movement->ActiveChild()!=0)throw std::logic_error("walking secondary floor is not bound");
            session.floor_attributes->Update(session.arena->collision.floor.at(hit->triangle));session.minimap->UpdatePlayer(position,rotation);
        });
        const auto entry="void "+command->world+"_"+command->scene+"_init()";
        if(!session.game->Push(command->world,entry,*session.game_effect))throw std::runtime_error("missing or duplicate scene initializer "+entry);
        if(session.scene_events && session.interaction_hints){
            session.scene_interaction=std::make_unique<SceneInteraction>(*session.arena,*session.scene_npcs,*session.scene_objects,
                *session.scene_events,*session.event_scripts,*session.interaction_hints,[&session](std::size_t npc,std::size_t atomic){
                    const auto& distance=session.npc_distance.at(npc);
                    if(!distance)throw std::logic_error("NPC interaction distance owner is missing");
                    return distance->CollisionAllowed(atomic);
                });
            session.scene_interaction->Reset(session.input_confirm);session.confirm_pending=false;
        }
        present_loading(1.f);
        present_loading(std::nullopt);
        if(!session.game->Complete(command->token))throw std::runtime_error("arena caller wait disappeared");
        if(!session.game_arena->Finish(command->token))throw std::runtime_error("arena command disappeared");
        session.arena_source->ReleaseUnusedArchives();
        ++session.scene_initializers;
        session.game_script_bytes.RetainOnly(session.arena->description.world);
        session.trace<<"Loaded original arena resources "<<command->world<<'/'<<command->scene<<"; queued "<<entry<<"; GOB/NPC draw owners loaded; EVF movement="<<bool(session.scene_events)<<"; interaction controller="<<bool(session.scene_interaction)<<std::endl;
    } catch(const std::exception& error) {
        try{present_loading(std::nullopt);}catch(const std::exception& cleanup){session.trace<<"loading UI cleanup failed: "<<cleanup.what()<<std::endl;}
        if(session.staged_monster_scene_runtime){session.staged_monster_scene_runtime->Close();session.staged_monster_scene_runtime.reset();}
        if(scene_committed)ReleaseMonsterGraphics(session);
        if(scene_committed && session.monster_scene_runtime){session.monster_scene_runtime->Close();session.monster_scene_runtime.reset();}
        session.arena_source->ReleaseUnusedArchives();
        session.game_arena->Finish(command->token);
        if(session.arena)session.game_script_bytes.RetainOnly(session.arena->description.world);
        else session.game_script_bytes.Clear();
        if(!session.game->Complete(command->token,error.what())){session.game->Clear();Error(session,error);}
        session.trace<<"Arena load failed: "<<error.what()<<std::endl;
    }
}
void StageMonsterSceneRuntime(ChapterHost& session,std::uint64_t generation,
    std::span<const SceneMonster> monsters,std::span<const ScenePath> paths) {
    if(!generation || !session.arena_source)throw std::logic_error("monster scene staging requires a live arena source and generation");
    if(session.staged_monster_scene_runtime)throw std::logic_error("previous monster scene staging owner is still live");
    session.staged_monster_scene_runtime=std::make_unique<MonsterSceneRuntime>(generation,
        session.arena_source,std::span<const SceneMonster>(monsters),std::span<const ScenePath>(paths),
        // Resource staging cannot advance behavior until the real host RNG is bound.
        []() -> std::uint32_t {throw std::logic_error("monster scene random source is not bound");});
}
void PrepareArenaGameplayOwners(ChapterHost& session,const ArenaResources& arena,std::uint64_t generation) {
    StageMonsterSceneRuntime(session,generation,arena.catalog.monsters,arena.catalog.paths);
    try {
        ValidateArenaGameplaySupport(arena);
    } catch(...) {
        session.staged_monster_scene_runtime->Close();
        session.staged_monster_scene_runtime.reset();
        throw;
    }
}
void EnsureEffectResources(ChapterHost& session){
    if(session.effect_preview_documents)return;
    session.effect_preview_stream=std::make_unique<std::ifstream>(ResolveOriginalPath(session.game_data+"/effect.cpk"),std::ios::binary);
    if(!*session.effect_preview_stream)throw std::runtime_error("user original effect.cpk is missing");
    session.effect_preview_package=std::make_unique<PackageReader>(*session.effect_preview_stream);
    const EffectMemberReader read=[&session](auto member)->std::optional<std::vector<std::uint8_t>>{if(!session.effect_preview_package->Find(member))return {};return session.effect_preview_package->Read(member);};
    session.effect_preview_documents=std::make_unique<EffectDocuments>(read);session.effect_preview_textures=std::make_unique<EffectTextures>(read);
}
void InitializeChapterHost(ChapterHost& host){
    host.settings_store=std::make_unique<FileSettingsStore>(host.directory+"/pal4-port.set");host.settings=host.settings_store->Load();
}
ChapterFrame AdvanceChapterHost(ChapterHost& session,float step){
    if(!std::isfinite(step) || step<0)throw std::logic_error("invalid chapter frame delta");
    (void)session.game_clock.Advance(step);
    if(session.scene_ui.game_state==10 || session.scene_ui.game_state==2){++session.modal_render_frames;return {};}
    ++session.scene_update_frames;
    if(session.interaction_hints)session.interaction_hints->Advance(step);
        if(session.effect_playback){
            session.effect_playback->Advance(step);
            if(session.playback_capture_remaining){session.playback_frames<<std::setprecision(9)<<"{\"delta\":"<<step<<",\"action\":"<<std::quoted(session.playback_action)<<",\"context_generation\":"<<session.context_generation<<",\"playback\":";session.effect_playback->WriteReport(session.playback_frames);session.playback_frames<<"}\n"<<std::flush;--session.playback_capture_remaining;}
            session.playback_action.clear();
        }
        if(session.scene_objects)session.scene_objects->Advance(step);
        if(session.scene_npcs)session.scene_npcs->Advance(step);
        if(session.npc_walks && session.game){
            for(const auto& completion:session.npc_walks->Advance(step)){
                if(!session.game->Complete(completion.token,completion.error))throw std::logic_error("NPC movement completion lost script wait");
                session.trace<<"NPC walk completed context="<<completion.token.context<<" sequence="<<completion.token.sequence<<std::endl;
            }
            if(session.capture_frame_traces && session.npc_walks->Requested()){
                session.npc_walk_frames<<std::setprecision(9)<<"{\"event\":\"frame\",\"delta\":"<<step<<",\"walk\":";session.npc_walks->WriteReport(session.npc_walk_frames);session.npc_walk_frames<<",\"scene\":";session.scene_npcs->WriteReport(session.npc_walk_frames,false);session.npc_walk_frames<<"}\n"<<std::flush;
            }
        }
        if(session.npc_actions && session.game)for(const auto& completion:session.npc_actions->Advance(step)){
            if(!session.game->Complete(completion.token,completion.error))throw std::logic_error("NPC action completion lost its script wait");
            session.trace<<"NPC action completed context="<<completion.token.context<<" sequence="<<completion.token.sequence<<std::endl;
        }
        if(session.dialog && session.game){
            session.dialog->Advance(step);
            if(const auto completion=session.dialog->TakeCompletion()){
                if(!session.game->Complete(*completion))throw std::logic_error("dialog completion lost its script wait");
                session.trace<<"dialog completed context="<<completion->context<<" sequence="<<completion->sequence<<std::endl;
            }
        }
        bool camera_frame=false;
        if(session.camera_tracks && session.game) {
            camera_frame=session.camera_tracks->Playing() || session.camera_tracks->Waiting();
            for(const auto& completion:session.camera_tracks->Advance(step)) {
                if(!session.game->Complete(completion.token,completion.error))throw std::logic_error("camera completion lost its script wait");
                session.trace<<"camera completed context="<<completion.token.context<<" sequence="<<completion.token.sequence<<std::endl;
            }
        }
        if(session.player_turns && session.game)for(const auto& completion:session.player_turns->Advance(step)){
            if(!session.game->Complete(completion.token,completion.error))throw std::logic_error("turn completion lost its script wait");
            session.trace<<"turn completed context="<<completion.token.context<<" sequence="<<completion.token.sequence<<std::endl;
        }
        bool action_completed=false;
        if(session.player_actions && session.game)for(const auto& completion:session.player_actions->Advance(step)) {
            if(!session.game->Complete(completion.token,completion.error))throw std::logic_error("player action completion lost its script wait");
            action_completed=true;session.trace<<"player action completed context="<<completion.token.context<<" sequence="<<completion.token.sequence<<std::endl;
        }
        if(session.player_walks && session.game){
            for(const auto& completion:session.player_walks->Advance(step)){
                if(!session.game->Complete(completion.token,completion.error))throw std::logic_error("walk completion lost its script wait");
                session.trace<<"walk completed context="<<completion.token.context<<" sequence="<<completion.token.sequence<<std::endl;
            }
            if(session.capture_frame_traces && session.player_walks->Requested() && !session.game_reported){session.walk_frames<<"{\"context_generation\":"<<session.context_generation<<",\"script_state\":\""<<StateName(session.game->State())<<"\",\"walk\":";session.player_walks->WriteReport(session.walk_frames);session.walk_frames<<"}\n"<<std::flush;}
        }
        if(session.game && session.tutorial){if(const auto token=session.tutorial->Advance()){
            if(!session.game->Complete(*token))throw std::logic_error("tutorial completion lost its script wait");
            session.trace<<"tutorial completed context="<<token->context<<" sequence="<<token->sequence<<std::endl;
        }}
        if(session.game && session.game->State()==ScriptState::finished && session.game->Depth()>1) {
            session.game->PopFinished();session.game_reported=false;
        }
        if(session.time_script && session.game) {
            auto state=session.game->State();
            const bool failed=state==ScriptState::failed;
            const bool actor_control_active=session.controller.State().enabled && !failed;
            const bool script_executor_busy=state==ScriptState::prepared || state==ScriptState::suspended || failed;
            session.time_script->Advance(step,actor_control_active,script_executor_busy);
        }
        if(session.game && (session.game->State()==ScriptState::prepared || session.game->State()==ScriptState::suspended)) {
            for(const auto token:session.game_effect->Advance(step))session.game->Complete(token);
            if(session.game->Tick(step,10000,ScriptRunMode::original_steps)==ScriptState::failed)session.trace<<"NewGame stopped: "<<session.game->Error()<<std::endl;
            AdvanceArenaLoad(session);
        }
        if(session.game)for(auto it=session.time_script_modules.begin();it!=session.time_script_modules.end();) {
            if(session.game->HasActiveModule(*it)){++it;continue;}
            session.game->UnloadModule(*it);session.game_modules.erase(*it);it=session.time_script_modules.erase(it);
        }
        RetireInactiveWorldModules(session);

    AdvanceChapterFreeControl(session,step);
    if(session.camera_follow)session.camera_follow->Advance();
    return {camera_frame,action_completed};
}
}
