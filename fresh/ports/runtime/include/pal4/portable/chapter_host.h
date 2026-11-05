#pragma once
#include "pal4/portable/tutorial.h"
#include "pal4/portable/tutorial_assets.h"
#include "pal4/portable/gles_renderer.h"
#include "pal4/portable/scene_multisample.h"
#include "pal4/portable/actor.h"
#include "pal4/portable/atomic_distance.h"
#include "pal4/portable/world.h"
#include "pal4/portable/movement.h"
#include "pal4/portable/script.h"
#include "pal4/portable/script_stack.h"
#include "pal4/portable/game_services.h"
#include "pal4/portable/time_script.h"
#include "pal4/portable/script_module_cache.h"
#include "pal4/portable/screen_effect.h"
#include "pal4/portable/gles_overlay.h"
#include "pal4/portable/media.h"
#include "pal4/portable/sound.h"
#include "pal4/portable/gbk.h"
#include "pal4/portable/dialog.h"
#include "pal4/portable/settings.h"
#include "pal4/portable/original_database.h"
#include "pal4/portable/role.h"
#include "pal4/portable/player.h"
#include "pal4/portable/controller.h"
#include "pal4/portable/arena.h"
#include "pal4/portable/arena_skill_state.h"
#include "pal4/portable/arena_source.h"
#include "pal4/portable/scene_objects.h"
#include "pal4/portable/scene_events.h"
#include "pal4/portable/scene_collision.h"
#include "pal4/portable/player_free_movement.h"
#include "pal4/portable/movement_input_gate.h"
#include "pal4/actor_control_idle.h"
#include "pal4/portable/scene_npcs.h"
#include "pal4/portable/monster_scene_runtime.h"
#include "pal4/portable/monster_scene_graphics.h"
#include "pal4/portable/scene_interaction.h"
#include "pal4/portable/interaction_hints.h"
#include "pal4/portable/inventory_services.h"
#include "pal4/portable/npc_action.h"
#include "pal4/portable/npc_walk.h"
#include "pal4/portable/effect_cylinder.h"
#include "pal4/portable/effect_particle.h"
#include "pal4/portable/effect_sequence.h"
#include "pal4/portable/effect_playback.h"
#include "pal4/portable/gob_effect_instance.h"
#include <random>
#include "pal4/portable/arena_services.h"
#include "pal4/portable/package.h"
#include "pal4/portable/game_camera.h"
#include "pal4/portable/camera_follow.h"
#include "pal4/portable/player_lock.h"
#include "pal4/portable/player_placement.h"
#include "pal4/portable/player_rotation.h"
#include "pal4/portable/arena_ready.h"
#include "pal4/portable/system_ui.h"
#include "pal4/portable/game_clock.h"
#include "pal4/portable/player_action.h"
#include "pal4/portable/player_turn.h"
#include "pal4/portable/player_walk.h"
#include "pal4/portable/player_equipment.h"
#include "pal4/portable/camera_tracks.h"
#include "pal4/portable/floor_attributes.h"
#include "pal4/temperature_floor_selection.h"
#include "pal4/random_number_generator.h"
#include <chrono>
#include <fstream>
#include <set>
#include <map>
#include <stdexcept>
namespace pal4::portable {
using ChapterClock=std::chrono::steady_clock;
struct PreviewParticleRandom final:ParticleRandom {
    std::mt19937 engine;
    unsigned Time() override{return 1234;}
    void Seed(unsigned n) override{engine.seed(n);}
    int Next15() override{return static_cast<int>(engine() & 32767);}
};
struct ChapterHost {
    std::function<std::unique_ptr<std::istream>(std::string_view)> open_cache;
    std::string directory, name, requested_scene, error, gl_version;
    std::string game_data;
    std::string previous_world, previous_scene;
    bool allow_populated_monsters{false};
    pal4::RandomNumberGenerator host_rng;
    EffectObjectCounts effect_preview_counts;
    std::unique_ptr<std::ifstream> effect_preview_stream;
    std::unique_ptr<PackageReader> effect_preview_package;
    std::unique_ptr<EffectDocuments> effect_preview_documents;
    std::unique_ptr<EffectTextures> effect_preview_textures;
    ParticleCounts particle_counts;
    PreviewParticleRandom particle_random;
    std::unique_ptr<EffectParticle> particle_preview;
    std::unique_ptr<EffectSequence> effect_sequence;
    struct SequenceGraphic {std::unique_ptr<GlesRenderer> gpu;Scene mesh;};
    std::vector<SequenceGraphic> sequence_graphics;
    std::unique_ptr<EffectPlayback> effect_playback;
    std::unique_ptr<GobEffectInstance> gob_effect_preview;
    std::map<unsigned,std::vector<SequenceGraphic>> playback_graphics;
    std::ofstream playback_frames;
    unsigned playback_capture_remaining{};
    std::string playback_action;
    std::ofstream scene_effect_frames;
    unsigned scene_effect_capture_remaining{};
    std::ofstream sequence_frames;
    unsigned sequence_capture_remaining{};
    std::ofstream particle_frames;
    unsigned particle_capture_remaining{};
    std::unique_ptr<EffectCylinder> effect_preview;
    std::unique_ptr<EffectTasks> effect_preview_tasks;
    std::ofstream effect_task_frames;
    unsigned effect_preview_triangles{};
    std::unique_ptr<GameCameraSystem> game_camera;
    std::unique_ptr<CameraFollow> camera_follow;
    std::unique_ptr<DistanceOptimization> distance;
    std::array<std::unique_ptr<AtomicDistanceInstance>,4> player_distance;
    std::vector<std::unique_ptr<AtomicDistanceInstance>> object_distance,npc_distance;
    std::vector<Matrix> player_pose_frames;
    std::unique_ptr<CameraTracks> camera_tracks;
    std::ofstream camera_frames;
    std::ofstream trace;
    bool capture_frame_traces{true}; // 平台可关闭高频JSONL；原脚本调用与错误日志仍保留。
    Scene scene;
    Scene instance_scene;
    std::optional<LevelDefinition> level;
    CollisionMesh collision;
    std::optional<FloorHit> floor;
    std::optional<MovementState> movement;
    MovementConfig movement_config;
    float input_x{},input_y{};MovementInputGate movement_input_gate;
    float camera_yaw_input{},camera_pitch_input{};
    bool modern_movement_active{};
    bool input_confirm{},confirm_pending{},confirm_suppressed{};
    ChapterClock::time_point movement_frame=ChapterClock::now();
    std::optional<ScriptContract> script_probe;
    std::optional<ScriptContract> chapter_script_probe;
    std::optional<ChapterStackContract> chapter_stack_probe;
    ScriptGlobals game_globals{};
    Settings settings;
    RoleCatalog roles;
    std::vector<UpgradeRecord> upgrades;
    EquipmentCatalog equipment_catalog;
    std::unique_ptr<PlayerEquipment> player_equipment;
    PlayerRoster players;
    ActorController controller;
    SceneUi scene_ui;
    std::unique_ptr<FloorAttributes> floor_attributes;
    std::array<std::uint32_t,5> temperature_floor_colors{};
    std::uint32_t temperature_floor_selection{4};
    bool minimap_absence_verified{};
    std::unique_ptr<SceneMovement> game_movement;
    std::unique_ptr<IPlayerPlacementServices> placement_services;
    std::unique_ptr<PlayerPlacement> game_placement;
    std::unique_ptr<PlayerNotifications> player_notifications;
    std::unique_ptr<PlayerRotation> player_rotation;
    std::unique_ptr<IControllerResetServices> ready_input;
    std::unique_ptr<IArenaReadyServices> ready_services;
    std::unique_ptr<ArenaReady> arena_ready;
    std::uint32_t pet_stored_source{};
    GameDataEntries game_entries;
    GameClock game_clock;
    std::unique_ptr<SystemUiPermissions> system_ui_permissions;
    std::unique_ptr<PlayerActions> player_actions;
    std::unique_ptr<PlayerTurns> player_turns;
    std::unique_ptr<PlayerWalks> player_walks;
    std::ofstream walk_frames;
    std::unique_ptr<std::ifstream> actor_package_stream;
    std::unique_ptr<PackageReader> actor_package;
    std::ofstream action_frames;
    std::unique_ptr<FileSettingsStore> settings_store;
    std::unique_ptr<GameServices> game_services;
    std::unique_ptr<ArenaSkillState> arena_skill_state;
    std::unique_ptr<TimeScript> time_script;
    ScriptModuleCache game_script_bytes;
    std::set<std::string> time_script_modules;
    std::unique_ptr<SettingsServices> game_settings;
    std::unique_ptr<ArenaServices> game_arena;
    std::unique_ptr<ArenaResources> arena;
    std::unique_ptr<MonsterSceneRuntime> monster_scene_runtime;
    std::unique_ptr<MonsterSceneRuntime> staged_monster_scene_runtime;
    std::unique_ptr<MonsterSceneGraphics> monster_graphics;
    // Synchronous loading presentation only; nullopt closes the UI. Callbacks
    // must not pump input, advance game/script/RNG clocks or retain UI owners.
    std::function<void(std::optional<float>)> present_loading;
    bool loading_visible{};
    float loading_progress{};
    std::uint64_t loading_presentations{};
    std::shared_ptr<ArenaSource> arena_source;
    std::unique_ptr<SceneMinimap> minimap;
    // Declaration order keeps collision and distance owners alive during GOB teardown.
    std::vector<OriginalSceneDefinition> original_scenes;
    PropertyCatalog property_catalog;
    TextCatalog item_messages;
    std::unique_ptr<Inventory> inventory;
    std::unique_ptr<InventoryServices> inventory_services;
    TextCatalog interaction_text;
    std::unique_ptr<InteractionHints> interaction_hints;
    std::unique_ptr<ISceneEventScripts> event_scripts;
    std::unique_ptr<SceneCollision> scene_collision;
    std::unique_ptr<SceneEvents> scene_events;
    std::unique_ptr<SceneObjects> scene_objects;
    std::unique_ptr<PlayerFreeMovement> free_movement;
    pal4::ActorControlIdleState free_idle;
    bool free_control_active{};
    std::unique_ptr<SceneNpcs> scene_npcs;
    std::unique_ptr<SceneInteraction> scene_interaction;
    std::unique_ptr<NpcActions> npc_actions;
    std::unique_ptr<NpcWalks> npc_walks;
    std::ofstream npc_walk_frames;
    std::ofstream npc_action_frames;
    struct ObjectGraphics {std::size_t index{};Scene mesh;std::vector<Matrix> frames;std::unique_ptr<GlesRenderer> gpu;};
    std::vector<SequenceGraphic> environment_graphics;
    unsigned environment_draws{},environment_triangles{},sky_triangles{};
    std::vector<ObjectGraphics> object_graphics;
    std::vector<ObjectGraphics> npc_graphics;
    unsigned object_draws{},object_triangles{};
    unsigned npc_draws{},npc_triangles{};
    std::set<std::string> game_modules;
    unsigned scene_initializers{};
    std::unique_ptr<MediaServices> game_media;
    std::unique_ptr<SoundServices> sound_services;
    std::unique_ptr<std::ifstream> ui_package_stream;
    std::unique_ptr<PackageReader> ui_package;
    std::unique_ptr<DialogAssets> dialog_assets;
    std::unique_ptr<DialogServices> dialog;
    bool tutorial_ui_available{};
    TutorialCatalog tutorial_catalog;
    std::vector<OriginalHelpTopic> help_topics;
    std::vector<OriginalHelpTip> help_tips;
    std::uint64_t scene_update_frames{},modal_render_frames{};
    std::unique_ptr<TutorialServices> tutorial;
    std::optional<TutorialPresentation> tutorial_presentation;
    std::uint64_t tutorial_presentation_id{};
    std::uint64_t dialog_revision{};
    bool dialog_clear_pending{};
    std::ofstream dialog_frames;
    std::unique_ptr<ScreenEffectServices> game_effect;
    std::unique_ptr<ScriptStack> game;
    bool requested_new_game{},game_reported{},overlay_verified{};
    unsigned game_frame{};
    unsigned game_recorded_calls{};
    std::uint64_t media_generation{};
    bool media_cancel_pending{};
    std::ofstream game_frames;
    std::unique_ptr<Actor> actor;
    ActorClipLease player_render_clip;
    int clip{-1};
    std::string requested_action;
    float animation_seconds{};
    double pose_checksum{};
    ChapterClock::time_point animation_start=ChapterClock::now();
    OrbitCamera camera;
    SceneMultisample scene_multisample;
    // Match the original single-sample path; smoothing is an explicit option.
    int scene_samples_requested{0};
    GlesRenderer renderer;
    GlesRenderer instance_renderer;
    GlesOverlay overlay;
    int width{},height{};
    unsigned frames{},gl_error{},context_generation{};
    float fps{};
    bool ready{},depth_verified{};
    const Scene& DisplayedScene() const { return arena ? arena->world : scene; }
    const CollisionMesh& DisplayedCollision() const { return arena ? arena->collision : collision; }
    ChapterClock::time_point report=ChapterClock::now();
    ~ChapterHost() { scene_multisample.Abandon(); for(auto& layer:environment_graphics)layer.gpu->Abandon();for(auto& [token,graphics]:playback_graphics){(void)token;for(auto& object:graphics)if(object.gpu)object.gpu->Abandon();}for(auto& object:sequence_graphics)if(object.gpu)object.gpu->Abandon();for(auto& object:object_graphics)object.gpu->Abandon();for(auto& npc:npc_graphics)npc.gpu->Abandon();renderer.Abandon(); instance_renderer.Abandon();overlay.Abandon(); }
};
// 平台提供缓存流、媒体和输入；新游戏/场景/角色/脚本连接共用此宿主。
template<class T> T ReadAsset(ChapterHost& host,const std::string& member,T(*reader)(std::istream&)) {
    if(!host.open_cache)throw std::logic_error("cache reader is not bound");
    auto input=host.open_cache(member);if(!input || !*input)throw std::runtime_error("missing user cache "+member);
    return reader(*input);
}
void InitializeChapterHost(ChapterHost&);
void EnsureChapterInventory(ChapterHost&);
void StopGame(ChapterHost&);
void BeginNewGame(ChapterHost&);
TutorialView ChapterTextWindowView(const ChapterHost&);
bool CloseChapterTextWindow(ChapterHost&,std::uint64_t generation,std::uint64_t id);
const TutorialPresentation* ChapterTextWindowPresentation(ChapterHost&);
void SetChapterInteractionInput(ChapterHost&,bool pressed);
void AdvanceArenaLoad(ChapterHost&);
void StageMonsterSceneRuntime(ChapterHost&,std::uint64_t generation,
    std::span<const SceneMonster> monsters,std::span<const ScenePath> paths);
void PrepareArenaGameplayOwners(ChapterHost&,const ArenaResources&,std::uint64_t generation);
void EnsureEffectResources(ChapterHost&);
void ReleaseEnvironmentGraphics(ChapterHost&);
void UploadEnvironmentGraphics(ChapterHost&);
void DrawEnvironmentGraphics(ChapterHost&,const Matrix&,bool sky);
void ReleaseMonsterGraphics(ChapterHost&);
void UploadMonsterGraphics(ChapterHost&);
void ReleaseNpcGraphics(ChapterHost&);
void UploadNpcGraphics(ChapterHost&);
void ReleaseObjectGraphics(ChapterHost&);
void UploadObjectGraphics(ChapterHost&);
void SelectGameAnimation(ChapterHost&,std::uint32_t,std::string_view);
std::optional<std::vector<std::uint8_t>> ReadActorMember(ChapterHost&,std::string_view);
std::optional<std::vector<std::uint8_t>> ReadUiMember(ChapterHost&,std::string_view);
std::vector<std::uint8_t> ReadBytes(std::istream&);
struct ChapterFrame {bool camera_frame{},action_completed{};};
void AdvanceChapterFreeControl(ChapterHost&,float elapsed);
ChapterFrame AdvanceChapterHost(ChapterHost&,float elapsed);
}
