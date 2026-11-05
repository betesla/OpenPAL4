#include "pal4/portable/game_path.h"
#include "pal4/portable/arena_source.h"
#include "pal4/portable/package.h"
#include "pal4/portable/dff_collision.h"
#include "pal4/portable/image_texture.h"
#include "pal4/portable/bsp_scene.h"
#include "pal4/portable/original_actor.h"
#include "pal4/portable/original_animation.h"
#include <algorithm>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <stdexcept>
namespace pal4::portable {
struct ArenaSource::Impl {
    std::filesystem::path game_data,cache;ArenaAssetReader development;bool original_only{};
    std::unique_ptr<std::ifstream> stream;std::unique_ptr<PackageReader> package;
    struct ReadRecord {std::string member,source;std::size_t bytes{};};
    static constexpr std::size_t record_limit=4096;
    std::deque<ReadRecord> records;
    std::uint64_t dropped_records{};
    void Record(std::string member,std::string source,std::size_t bytes){
        if(records.size()==record_limit){records.pop_front();if(dropped_records<std::numeric_limits<std::uint64_t>::max())++dropped_records;}
        records.push_back({std::move(member),std::move(source),bytes});
    }
    struct Original {std::ifstream stream;PackageReader reader;explicit Original(const std::filesystem::path& path):stream(ResolveOriginalPath(path),std::ios::binary),reader(stream){}};
    std::map<std::string,std::shared_ptr<Original>> originals;
    Impl(std::filesystem::path g,std::filesystem::path c,ArenaAssetReader d):game_data(std::move(g)),cache(std::move(c)),development(std::move(d)){}
};
ArenaSource::ArenaSource(std::filesystem::path game,std::filesystem::path cache,ArenaAssetReader dev):impl_(std::make_shared<Impl>(std::move(game),std::move(cache),std::move(dev))){}
ArenaSource::ArenaSource(std::filesystem::path game):impl_(std::make_shared<Impl>(std::move(game),std::filesystem::path{},ArenaAssetReader{})){impl_->original_only=true;}
ArenaSource::~ArenaSource()=default;
namespace {
std::string ComponentPath(std::string_view value) {
    const auto valid_char = [](unsigned char c) {
        if (c >= 0x80) return true;
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '/' || c == '\\' || c == '.' || c == ' ' ||
               c == '(' || c == ')' || c == '+' || c == '~' || c == '#' || c == '!' ||
               c == '@' || c == '$' || c == '%' || c == '&' || c == '=';
    };
    if(value.empty() || value.size()>256 || value.find("..")!=std::string_view::npos || !std::all_of(value.begin(), value.end(), valid_char))
        throw std::invalid_argument("invalid original arena member: '" + std::string(value) + "'");
    std::string result(value);std::replace(result.begin(),result.end(),'/','\\');
    if(result.front()=='\\' || result.find("\\\\")!=std::string::npos)throw std::invalid_argument("invalid original member separator: '" + std::string(value) + "'");
    return result;
}
std::pair<std::string,std::string> OriginalDirectory(std::string_view directory){
    auto path=ComponentPath(directory);std::string lower=path;std::transform(lower.begin(),lower.end(),lower.begin(),[](char c){return c>='A' && c<='Z'?static_cast<char>(c+'a'-'A'):c;});
    if(!lower.starts_with("gamedata\\"))throw std::invalid_argument("resource directory is outside gamedata");
    std::string package,prefix;
    if(lower.starts_with("gamedata\\palobject\\")){package="palobject.cpk";prefix=path.substr(19);}
    else if(lower.starts_with("gamedata\\palweapon\\")){package="palweapon.cpk";prefix=path.substr(19);}
    else if(lower.starts_with("gamedata\\palactor\\")){package="palactor.cpk";prefix=path.substr(18);}
    else if(lower.starts_with("gamedata\\palworld\\")){const auto end=path.find('\\',18);if(end==std::string::npos || end==18)throw std::invalid_argument("incomplete world directory");package="PALWorld/"+lower.substr(18,end-18)+".cpk";prefix=path.substr(end+1);}
    else throw std::invalid_argument("unsupported original resource package");
    if(!prefix.empty() && prefix.back()!='\\')prefix+='\\';
    return {package,prefix};
}
bool SafeActorComponent(std::string_view value){return !value.empty() && value.size()<=64 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==value.npos;}
std::vector<std::string> OriginalAnimationNames(PackageReader& reader,const std::string& prefix){
    std::map<std::string,std::string> names;
    for(auto file:reader.ListFiles()){
        if(file.size()<=4)continue;
        auto extension=file.substr(file.size()-4);
        for(auto& c:extension)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
        if(extension!=".anm" || !reader.Find(prefix+file))continue;
        file.resize(file.size()-4);if(!SafeActorComponent(file))throw std::runtime_error("invalid original animation filename");
        auto key=file;for(auto& c:key)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');names.emplace(key,file);
    }
    if(names.size()>128)throw std::runtime_error("too many original actor animations");
    std::vector<std::string> result;for(const auto& entry:names)result.push_back(entry.second);return result;
}
}
std::optional<std::vector<std::uint8_t>> ArenaSource::ReadSceneMember(std::string_view member) {
    auto& s=*impl_;const auto original=ComponentPath(member);
    if(!s.package){s.stream=std::make_unique<std::ifstream>(ResolveOriginalPath(s.game_data/"scenedata.cpk"),std::ios::binary);if(!*s.stream)throw std::runtime_error("cannot open original scenedata.cpk");s.package=std::make_unique<PackageReader>(*s.stream);}
    auto data=s.package->ReadIfPresent(original);if(!data)return {};
    s.Record(original,"scenedata.cpk",data->size());return data;
}
CollisionMesh ArenaSource::ReadOriginalCollision(std::string_view world,std::string_view scene) {
    const auto name=[](std::string_view value){return !value.empty() && value.size()<=64 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")==std::string_view::npos;};
    if(!name(world) || !name(scene))throw std::invalid_argument("invalid collision scene name");
    const auto base=std::string(world)+"\\"+std::string(scene)+"\\"+std::string(scene);
    auto floor=ReadSceneMember(base+"_floor.dff"),wall=ReadSceneMember(base+"_wall.dff");
    if((!floor || !wall) && scene.size()>1){
        const auto area=scene.substr(0,scene.size()-1);
        const auto area_base=std::string(world)+"\\"+std::string(area)+"\\"+std::string(area);
        if(!floor)floor=ReadSceneMember(area_base+"_floor.dff");
        if(!wall)wall=ReadSceneMember(area_base+"_wall.dff");
    }
    if(!floor || !wall)throw std::runtime_error("original collision DFF is missing: "+base);
    CollisionMesh mesh;mesh.floor=ReadDffCollision(*floor);mesh.wall=ReadDffCollision(*wall);
    if(mesh.floor.empty() || mesh.floor.size()+mesh.wall.size()>1000000)throw std::runtime_error("invalid original collision triangle count");
    return mesh;
}
ArenaDescription ArenaSource::ReadOriginalDescription(std::string_view world,std::string_view scene,std::span<const OriginalSceneDefinition> scenes) {
    const auto safe=[](std::string_view value){return !value.empty() && value.size()<=64 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==value.npos;};
    if(!safe(world) || !safe(scene))throw std::invalid_argument("invalid original scene name");
    auto upper=[](std::string value){for(auto& c:value)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');return value;};
    const auto wanted_world=upper(std::string(world)),wanted_scene=upper(std::string(scene));ArenaDescription result;bool found=false;
    for(const auto& entry:scenes)if(upper(entry.world)==wanted_world && upper(entry.scene)==wanted_scene){if(found)throw std::runtime_error("duplicate original scene definition");found=true;result.id=entry.id;result.type=entry.type;result.earthball=entry.earthball;result.world=std::string(world);result.scene=std::string(scene);result.title=entry.title;}
    if(!found && wanted_scene.size()>1){
        const auto wanted_area=wanted_scene.substr(0,wanted_scene.size()-1);
        for(const auto& entry:scenes)if(upper(entry.world)==wanted_world && upper(entry.scene)==wanted_area){found=true;result.id=entry.id;result.type=entry.type;result.earthball=entry.earthball;result.world=std::string(world);result.scene=std::string(scene);result.title=entry.title;break;}
    }
    if(!found)throw std::runtime_error("original scene definition missing");
    auto& s=*impl_;if(!s.package){s.stream=std::make_unique<std::ifstream>(ResolveOriginalPath(s.game_data/"scenedata.cpk"),std::ios::binary);if(!*s.stream)throw std::runtime_error("cannot open original scenedata.cpk");s.package=std::make_unique<PackageReader>(*s.stream);}
    std::map<std::string,std::string> candidates;
    for(auto file:s.package->ListFiles()){if(file.size()<=4 || upper(file.substr(file.size()-4))!=".CAM" || !safe(file.substr(0,file.size()-4)))continue;const auto key=upper(file);if(s.package->Find(std::string(world)+"\\"+std::string(scene)+"\\"+file))candidates.emplace(key,file);}
    if(candidates.size()>256)throw std::runtime_error("too many original cameras");
    for(const auto& entry:candidates)result.camera_files.push_back(entry.second);
    std::string lower=std::string(world);for(auto& c:lower)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');const auto package="PALWorld/"+lower+".cpk";auto& source=s.originals[package];if(!source)source=std::make_unique<Impl::Original>(s.game_data/package);
    for(const auto* kind:{"trans","clip","clipNA","sky"}){
        const auto area=scene.size()>1?scene.substr(0,scene.size()-1):scene;
        if(source->reader.Find(std::string(scene)+"\\"+std::string(scene)+"_"+kind+".dff") ||
           (area!=scene && source->reader.Find(std::string(area)+"\\"+std::string(area)+"_"+kind+".dff")))
            result.environment_files.emplace_back(kind);
    }
    result.object_collision_cache=true;result.event_collision_cache=true;return result;
}
Scene ArenaSource::ReadOriginalEnvironment(std::string_view world,std::string_view scene,std::string_view kind) {
    if(kind!="trans" && kind!="clip" && kind!="clipNA" && kind!="sky")throw std::invalid_argument("invalid environment layer");
    const auto safe=[](std::string_view value){return !value.empty() && value.size()<=64 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==value.npos;};
    if(!safe(world) || !safe(scene))throw std::invalid_argument("invalid environment scene name");
    std::string directory="gamedata\\PALWorld\\"+std::string(world)+"\\"+std::string(scene)+"\\";
    std::string filename=std::string(scene)+"_"+std::string(kind)+".dff";auto dff=ReadObjectMember(directory,filename);
    if(!dff && scene.size()>1){
        const auto area=scene.substr(0,scene.size()-1);
        directory="gamedata\\PALWorld\\"+std::string(world)+"\\"+std::string(area)+"\\";
        filename=std::string(area)+"_"+std::string(kind)+".dff";
        dff=ReadObjectMember(directory,filename);
    }
    if(!dff)throw std::runtime_error("original environment DFF missing: "+filename);
    return ReadDffEnvironment(*dff,[&](std::string_view name){return ReadOriginalTexture(directory,name);});
}
Texture ArenaSource::ReadOriginalTexture(std::string_view directory,std::string_view name) {
    auto data=ReadObjectMember(directory,std::string(name)+".dds");
    if(!data)data=ReadObjectMember(directory,std::string(name)+".png");
    if(!data)data=ReadObjectMember(directory,name);
    if(!data)return Texture{1,1,{255,255,255,255}};
    const bool dds=data->size()>=4 && (*data)[0]=='D' && (*data)[1]=='D' && (*data)[2]=='S' && (*data)[3]==' ';
    return DecodeOriginalTexture(*data,dds);
}
Scene ArenaSource::ReadOriginalWorld(std::string_view world,std::string_view scene) {
    const auto safe=[](std::string_view value){return !value.empty() && value.size()<=64 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==value.npos;};
    if(!safe(world) || !safe(scene))throw std::invalid_argument("invalid world scene name");
    std::string directory="gamedata\\PALWorld\\"+std::string(world)+"\\"+std::string(scene)+"\\";
    auto bsp=ReadObjectMember(directory,std::string(scene)+".bsp");
    auto config=ReadObjectMember(directory,std::string(scene)+"_ltMap.cfg");
    if(!bsp && scene.size()>1){
        const auto area=scene.substr(0,scene.size()-1);
        directory="gamedata\\PALWorld\\"+std::string(world)+"\\"+std::string(area)+"\\";
        bsp=ReadObjectMember(directory,std::string(area)+".bsp");
        if(!config)config=ReadObjectMember(directory,std::string(area)+"_ltMap.cfg");
    }
    if(!config){
        config=std::vector<std::uint8_t>{0,0,128,63, 0,0,128,63, 0,0,128,63, 0,0,128,63};
    }
    if(!bsp)throw std::runtime_error("original BSP or lightmap configuration missing: "+std::string(scene));
    return ReadBspScene(*bsp,*config,[&](std::string_view name){return ReadOriginalTexture(directory,name);},[&](std::string_view name)->std::optional<Texture>{
        auto data=ReadObjectMember(directory,std::string(name)+".dds");
        if(!data)data=ReadObjectMember(directory,std::string(name)+".png");
        if(!data)data=ReadObjectMember(directory,name);
        if(!data)return {};
        const bool dds=data->size()>=4 && (*data)[0]=='D' && (*data)[1]=='D' && (*data)[2]=='S' && (*data)[3]==' ';
        return DecodeOriginalTexture(*data,dds);
    });
}
Actor ArenaSource::ReadOriginalActorResource(std::string_view directory,std::string_view model,std::string_view variant,bool lazy_animations) {
    const auto safe=[](std::string_view value){return !value.empty() && value.size()<=64 && value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==value.npos;};
    if(!safe(model) || (!variant.empty() && !safe(variant)))throw std::invalid_argument("invalid actor resource name");
    Actor actor;std::vector<std::uint32_t> hierarchy;
    {
        auto dff=ReadObjectMember(directory,std::string(model)+".dff");if(!dff)throw std::runtime_error("original actor DFF missing: "+std::string(model));
        std::optional<std::vector<std::uint8_t>> replacement;if(!variant.empty())replacement=ReadObjectMember(directory,std::string(variant)+".png");
        actor=ReadOriginalActor(*dff,[&](std::string_view name){return ReadOriginalTexture(directory,name);},replacement?variant:std::string_view{},[&]{return DecodeOriginalTexture(*replacement,false);});
        hierarchy=ReadDffAnimationHierarchy(*dff);
    }
    PrepareActorTextureFiltering(actor);
    if(hierarchy.empty())return actor;
    if(lazy_animations){
        auto catalog=CreateOriginalAnimations(directory,std::move(hierarchy));
        for(const auto& name:catalog.names)actor.clips.push_back({name,0,{}});
        actor.clip_loader=[store=std::move(catalog.store)](std::string_view name){return store->Get(name);};
        return actor;
    }
    auto [package,prefix]=OriginalDirectory(directory);auto& source=impl_->originals.at(package);const auto names=OriginalAnimationNames(source->reader,prefix);
    std::uint64_t key_bytes=0;
    for(const auto& name:names){auto data=ReadObjectMember(directory,name+".anm");if(!data)throw std::runtime_error("original actor animation disappeared");auto clip=ReadOriginalAnimation(*data,name,hierarchy);for(const auto& track:clip.tracks){key_bytes+=std::uint64_t(track.keys.size())*sizeof(AnimationKey);if(key_bytes>256ull*1024*1024)throw std::runtime_error("original actor animation budget exceeded");}actor.clips.push_back(std::move(clip));}
    return actor;
}
OriginalAnimationCatalog ArenaSource::OpenOriginalAnimations(std::string_view directory,std::string_view model) {
    if(!SafeActorComponent(model))throw std::invalid_argument("invalid actor resource name");
    const auto dff=ReadObjectMember(directory,std::string(model)+".dff");
    if(!dff)throw std::runtime_error("original actor DFF missing: "+std::string(model));
    return CreateOriginalAnimations(directory,ReadDffAnimationHierarchy(*dff));
}
OriginalAnimationCatalog ArenaSource::CreateOriginalAnimations(std::string_view directory,std::vector<std::uint32_t> hierarchy) {
    if(hierarchy.empty())return {};
    auto [package,prefix]=OriginalDirectory(directory);
    auto& cached=impl_->originals[package];if(!cached)cached=std::make_shared<Impl::Original>(impl_->game_data/package);
    auto archive=cached;
    OriginalAnimationCatalog result;result.names=OriginalAnimationNames(archive->reader,prefix);
    std::weak_ptr<Impl> observer=impl_;
    result.store=std::make_shared<OriginalAnimationStore>(std::move(hierarchy),result.names,
        [archive,prefix,package,observer](std::string_view name){
            const auto member=prefix+std::string(name)+".anm";auto data=archive->reader.Read(member);
            if(auto owner=observer.lock())owner->Record(member,package,data.size());
            return data;
        });
    return result;
}
std::optional<std::vector<std::uint8_t>> ArenaSource::ReadObjectMember(std::string_view directory,std::string_view member) {
    auto [package,prefix]=OriginalDirectory(directory);auto file=ComponentPath(member);
    auto& s=*impl_;auto& source=s.originals[package];if(!source)source=std::make_shared<Impl::Original>(s.game_data/package);
    const auto original=prefix+file;auto data=source->reader.ReadIfPresent(original);if(!data)return {};
    s.Record(original,package,data->size());return data;
}
std::size_t ArenaSource::ReleaseUnusedArchives() {
    auto& archives=impl_->originals;std::size_t released=0;
    if(impl_->package || impl_->stream){impl_->package.reset();impl_->stream.reset();++released;}
    for(auto it=archives.begin();it!=archives.end();){
        if(!it->second || it->second.use_count()==1){it=archives.erase(it);++released;}
        else ++it;
    }
    return released;
}
std::vector<std::uint8_t> ArenaSource::Read(std::string_view member) {
    if(!member.starts_with("arena/") || member.size()>256 || member.find("..")!=std::string_view::npos || member.find("//")!=std::string_view::npos || member.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-/.")!=std::string_view::npos)
        throw std::invalid_argument("invalid arena resource member");
    const auto first=member.find('/',6),second=first==std::string_view::npos?first:member.find('/',first+1);
    if(first==std::string_view::npos || second==std::string_view::npos || first==6 || second==first+1 || second+1==member.size())throw std::invalid_argument("incomplete arena resource member");
    const auto suffix=std::filesystem::path(member).extension().string();auto& s=*impl_;
    std::vector<std::uint8_t> data;std::string source;
    if(suffix==".npc" || suffix==".gob" || suffix==".mst" || suffix==".pwy" || suffix==".evf" || suffix==".cam") {
        if(!s.package){
            s.stream=std::make_unique<std::ifstream>(ResolveOriginalPath(s.game_data/"scenedata.cpk"),std::ios::binary);
            if(!*s.stream)throw std::runtime_error("cannot open original scenedata.cpk");
            s.package=std::make_unique<PackageReader>(*s.stream);
        }
        std::string original(member.substr(6));std::replace(original.begin(),original.end(),'/','\\');
        data=s.package->Read(original);source="scenedata.cpk";
    } else {
        if(suffix!=".p4d" && suffix!=".p4s" && suffix!=".p4c" && suffix!=".p4a" && suffix!=".p4g")throw std::invalid_argument("unsupported arena cache member");
        if(s.original_only)throw std::runtime_error("converted arena resources are disabled in original-only mode");
        const auto path=s.cache/std::string(member);
        if(std::filesystem::exists(path)){
            std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)throw std::runtime_error("cannot open arena cache "+std::string(member));
            const auto length=input.tellg();if(length<=0 || length>384ll*1024*1024)throw std::runtime_error("invalid arena cache length "+std::string(member));
            data.resize(static_cast<std::size_t>(length));input.seekg(0);
            if(!input.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size())))throw std::runtime_error("truncated arena cache "+std::string(member));
            source="user-cache";
        } else if(s.development){data=s.development(member);source="development-assets";}
        else throw std::runtime_error("missing arena cache "+std::string(member));
    }
    s.Record(std::string(member),source,data.size());return data;
}
void ArenaSource::WriteReport(std::ostream& out) const {
    out<<"{\"reads\":[";bool first=true;
    for(const auto& r:impl_->records){if(!first)out<<',';first=false;out<<"{\"member\":"<<std::quoted(r.member)<<",\"source\":"<<std::quoted(r.source)<<",\"bytes\":"<<r.bytes<<'}';}
    out<<"],\"dropped_reads\":"<<impl_->dropped_records<<",\"cache_invalidation_implemented\":false}";
}
}
