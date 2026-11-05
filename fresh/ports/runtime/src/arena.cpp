#include "pal4/portable/arena.h"
#include "pal4/portable/dff_collision.h"
#include <sstream>
#include <stdexcept>
#include <ostream>

namespace pal4::portable {
namespace {
void Require(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
std::uint32_t Word(std::istream& input){unsigned char b[4]{};Require(bool(input.read(reinterpret_cast<char*>(b),4)),"truncated arena description");return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24);}
std::string Text(std::istream& input){const auto n=Word(input);Require(n>0 && n<=4096,"invalid arena text length");std::string s(n,'\0');Require(bool(input.read(s.data(),n)) && s.back()==0,"invalid arena text");s.pop_back();Require(s.find('\0')==std::string::npos,"embedded arena NUL");return s;}
bool Name(std::string_view s){return !s.empty() && s.size()<=64 && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_- ()+~.")==std::string_view::npos;}
template<class T> T Asset(const ArenaAssetReader& read,const std::string& name,T(*parse)(std::istream&),bool optional_if_missing=false) {
    std::vector<std::uint8_t> bytes;
    try {
        bytes=read(name);
    } catch(const std::exception& error) {
        if(optional_if_missing && std::string_view(error.what()).starts_with("package member missing:"))return T{};
        throw;
    }
    Require(!bytes.empty() && bytes.size()<=384u*1024*1024,"invalid arena asset length");
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    try{return parse(input);}catch(const std::exception& error){throw std::runtime_error(name+": "+error.what());}
}
}
ArenaDescription ReadArenaDescription(std::istream& input) {
    Require(Word(input)==0x44413450,"unsupported arena description");
    const auto version=Word(input);Require(version>=1 && version<=3,"unsupported arena description version");
    ArenaDescription result;result.id=Word(input);result.type=Word(input);result.earthball=Word(input);
    result.world=Text(input);result.scene=Text(input);result.title=Text(input);Require(Name(result.world) && Name(result.scene),"invalid arena identity");
    const auto count=Word(input);Require(count<=256,"too many arena camera files");
    for(std::uint32_t i=0;i<count;++i){auto file=Text(input);Require(file.ends_with(".cam") && Name(std::string_view(file).substr(0,file.size()-4)),"invalid arena camera name");result.camera_files.push_back(std::move(file));}
    if(version>=2){const auto layers=Word(input);Require(layers<=4,"too many environment layers");
        for(unsigned i=0;i<layers;++i){auto kind=Text(input);Require(kind=="trans" || kind=="clip" || kind=="clipNA" || kind=="sky","invalid environment layer");
            for(const auto& previous:result.environment_files)Require(previous!=kind,"duplicate environment layer");
            result.environment_files.push_back(std::move(kind));}}
    if(version>=3){const auto flags=Word(input);Require((flags&~3u)==0,"unknown arena collision capability");result.object_collision_cache=(flags&1)!=0;result.event_collision_cache=(flags&2)!=0;}
    Require(input.peek()==std::char_traits<char>::eof(),"trailing arena description bytes");return result;
}
ArenaResources LoadArenaResources(std::string_view world,std::string_view scene,const ArenaAssetReader& read,const std::function<CollisionMesh()>& original_collision,bool original_events,const std::function<ObjectCollisionGeometry(const SceneGob&)>& original_object,const std::function<ArenaDescription()>& original_description,const std::function<Scene(std::string_view)>& original_environment,const std::function<Scene()>& original_world,const std::function<Actor(const SceneNpc&)>& original_npc,const std::function<Actor(const SceneGob&)>& original_gob) {
    Require(Name(world) && Name(scene),"invalid arena request name");
    const auto base="arena/"+std::string(world)+"/"+std::string(scene)+"/";
    ArenaResources result;result.description=original_description?original_description():Asset(read,base+"arena.p4d",ReadArenaDescription);
    Require(result.description.world==world && result.description.scene==scene,"arena request/description mismatch");
    result.world=original_world?original_world():Asset(read,base+"world.p4s",ReadScene);result.collision=original_collision?original_collision():Asset(read,base+"collision.p4c",ReadCollision);
    for(const auto& kind:result.description.environment_files)
        {auto mesh=original_environment?original_environment(kind):Asset(read,base+"environment/"+kind+".p4s",ReadScene);
        mesh.saturation=result.world.saturation;mesh.brightness=result.world.brightness;
        result.environment.push_back({kind,std::move(mesh)});}
    result.catalog.npcs=Asset(read,base+"npcInfo.npc",ReadSceneNpcs,true);
    result.catalog.gobs=Asset(read,base+"GameObjs.gob",ReadSceneGobs,true);
    result.catalog.monsters=Asset(read,base+"mstInfo.mst",ReadSceneMonsters,true);
    result.monsters_verified_empty=result.catalog.monsters.empty();
    result.catalog.events=Asset(read,base+std::string(scene)+".evf",ReadSceneEvents,true);
    if(result.description.event_collision_cache) {
        result.event_collisions.resize(result.catalog.events.size());
        for(std::size_t i=0;i<result.catalog.events.size();++i)if(!result.catalog.events[i].clump.empty()) {
            auto geometry=original_events?ReadDffEventCollision(result.catalog.events[i].normalized_clump):Asset(read,base+"events/event_"+std::to_string(i)+".p4g",ReadObjectCollision);
            for(const auto& atomic:geometry.atomics)Require(atomic.frame==0,"event collision cache must use world coordinates");
            result.event_collisions[i]=std::move(geometry);
        }
    }
    result.catalog.paths=Asset(read,base+"pathway.pwy",ReadScenePaths,true);
    for(const auto& camera:result.description.camera_files)
        result.catalog.cameras.emplace_back(camera,Asset(read,base+camera,ReadSceneCameras));
    for(const auto& npc:result.catalog.npcs) {
        Require(Name(npc.name) && Name(npc.resource),"invalid arena NPC resource");
        ArenaActor actor;actor.name=npc.name;actor.animation=npc.DefaultAction().value_or("");actor.position=npc.position;actor.rotation=npc.rotation;actor.state=npc.flags[0];
        actor.actor=original_npc?original_npc(npc):Asset(read,base+"entities/"+npc.name+".p4a",ReadActor);
        if(!actor.animation.empty()) {
            const auto ieq=[](std::string_view a,std::string_view b){
                if(a.size()!=b.size())return false;
                for(std::size_t i=0;i<a.size();++i){
                    char ca=a[i]>='a'&&a[i]<='z'?char(a[i]-32):a[i];
                    char cb=b[i]>='a'&&b[i]<='z'?char(b[i]-32):b[i];
                    if(ca!=cb)return false;
                }
                return true;
            };
            bool found=false;
            for(const auto& clip:actor.actor.clips)if(ieq(clip.name,actor.animation)){actor.animation=clip.name;found=true;break;}
            if(!found){
                for(const auto& clip:actor.actor.clips)if(ieq(clip.name,"C01")){actor.animation=clip.name;found=true;break;}
            }
            if(!found){
                actor.animation=actor.actor.clips.empty()?std::string{}:actor.actor.clips.front().name;
            }
        }
        result.actors.push_back(std::move(actor));
    }
    for(const auto& gob:result.catalog.gobs) {
        // 特效实体保留完整配置，渲染绑定将在特效服务中完成。
        if(gob.type==8 || gob.model.empty())continue;
        Require(gob.type<=10 && Name(gob.name),"unsupported arena GOB actor type");
        ArenaActor actor;actor.gob_type=gob.type;actor.name=gob.name;actor.position=gob.position;actor.rotation=gob.rotation;actor.state=gob.flags[0];
        if(const auto* scale=gob.config.Root().FindAttribute("PAL4-GameObject-object-scale"))actor.scale=scale->ToFloat(1);
        actor.actor=original_gob?original_gob(gob):Asset(read,base+"entities/"+gob.name+".p4a",ReadActor);
        PrepareActorInstanceRoot(actor.actor);
        if(result.description.object_collision_cache){actor.collision=original_object?original_object(gob):Asset(read,base+"entities/"+gob.name+".p4g",ReadObjectCollision);ValidateObjectCollision(*actor.collision,actor.actor);}
        result.actors.push_back(std::move(actor));
    }
    return result;
}
void ValidateArenaGameplaySupport(const ArenaResources& arena) {
    Require(arena.monsters_verified_empty && arena.catalog.monsters.empty(),"populated monster scene owner is not implemented");
    Require(arena.catalog.paths.empty(),"populated pathway behavior binding is not implemented");
}
void WriteArenaReport(std::ostream& output,const ArenaResources& arena) {
    std::size_t event_atomics{},event_triangles{};
    for(const auto& geometry:arena.event_collisions)if(geometry)for(const auto& atomic:geometry->atomics){++event_atomics;event_triangles+=atomic.triangles.size();}
    output<<"{\"world\":\""<<arena.description.world<<"\",\"scene\":\""<<arena.description.scene<<"\",\"scene_type\":"<<arena.description.type
        <<",\"world_triangles\":"<<arena.world.vertices.size()/3<<",\"floor_triangles\":"<<arena.collision.floor.size()<<",\"wall_triangles\":"<<arena.collision.wall.size()
        <<",\"event_collision_cache\":"<<(arena.description.event_collision_cache?"true":"false")<<",\"event_collision_atomics\":"<<event_atomics<<",\"event_collision_triangles\":"<<event_triangles
        <<",\"object_collision_cache\":"<<(arena.description.object_collision_cache?"true":"false")<<",\"actor_resources\":"<<arena.actors.size()<<",\"entity_rendering_bound\":false,\"event_dispatch_bound\":false,\"gameplay_initialized\":false,\"actors\":[";
    for(std::size_t i=0;i<arena.actors.size();++i){if(i)output<<',';const auto& actor=arena.actors[i];
        output<<"{\"name\":\""<<actor.name<<"\",\"frames\":"<<actor.actor.frames.size()<<",\"vertices\":"<<actor.actor.mesh.vertices.size()<<",\"collision_atomics\":"<<(actor.collision?actor.collision->atomics.size():0)<<",\"gob_type\":";
        if(actor.gob_type)output<<*actor.gob_type;else output<<"null";
        // Report the sampled DFF hierarchy before the scene instance transform.
        // This is observational: do not normalize root frames to match a screenshot.
        std::vector<Vertex> vertices;std::vector<Matrix> frames;
        SampleActorVertices(actor.actor,-1,0,vertices,AnimationTimeMode::clamp,&frames);
        output<<",\"root_frames\":[";bool first_root=true;
        for(std::size_t f=0;f<actor.actor.frames.size();++f){
            if(actor.actor.frames[f].parent>=0)continue;
            if(!first_root)output<<',';
            first_root=false;output<<"{\"frame\":"<<f<<",\"matrix\":[";
            for(std::size_t n=0;n<16;++n){if(n)output<<',';output<<actor.actor.frames[f].local[n];}
            output<<"]}";
        }
        output<<"],\"bind_atomic_frames\":[";
        for(std::size_t a=0;a<actor.actor.atomic_spheres.size();++a){
            if(a)output<<',';
            const auto frame=actor.actor.atomic_spheres[a].frame;
            output<<"{\"frame\":"<<frame<<",\"matrix\":[";
            for(std::size_t n=0;n<16;++n){if(n)output<<',';output<<frames.at(frame)[n];}
            output<<"]}";
        }
        output<<"]}";}
    output<<"],\"environment\":[";
    for(std::size_t i=0;i<arena.environment.size();++i){if(i)output<<',';const auto& layer=arena.environment[i];output<<"{\"kind\":"<<std::quoted(layer.kind)<<",\"triangles\":"<<layer.mesh.vertices.size()/3<<",\"draws\":"<<layer.mesh.draws.size()<<'}';}
    output<<"],\"catalog\":";
    WriteSceneCatalogReport(output,arena.catalog);output<<'}';Require(bool(output),"cannot write arena report");
}
} // namespace pal4::portable
