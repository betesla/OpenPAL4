#include "pal4/portable/scene_data.h"
#include "pal4/portable/effect_document.h"
#include "pal4/monster_info_manager.h"
#include "scene_data_reader.h"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <ostream>

namespace pal4::portable {
namespace {
using detail::Require;
using detail::Reader;
// 句柄只标识本次读取的字段/数组；不包含宿主指针或私有STL布局。
class MonsterReader final:public pal4::NpcInfoRuntimeStub {
    Reader& input_;SceneMonster& record_;std::uint32_t array_{};
    static constexpr std::uint32_t base=0x100,array=0x10000;
    static void Stream(std::uint32_t stream){Require(stream==1,"invalid monster stream handle");}
public:
    MonsterReader(Reader& input,SceneMonster& record):input_(input),record_(record){}
    void ReadStreamString(std::uint32_t stream,std::uint32_t field) override {
        Stream(stream);if(field==base)record_.name=input_.Text();
        else if(field==base+12)record_.resource=input_.Text();
        else throw std::logic_error("invalid monster string field");
    }
    void ReadStreamData(std::uint32_t stream,std::uint32_t field,std::uint32_t size) override {
        Stream(stream);
        if(field==base+24 && size==12){input_.Point(record_.position);return;}
        if(field==base+36 && size==12){input_.Point(record_.rotation);return;}
        if(field>=base+48 && field<=base+64 && (field-base)%4==0 && size==4){
            record_.fields[(field-base-48)/4]=input_.Word();return;
        }
        if(array_ && field>=array && (field-array)%4==0 && size==4 && (field-array)/4<record_.values.size()){
            record_.values[(field-array)/4]=input_.Word();return;
        }
        throw std::logic_error("invalid monster data field");
    }
    std::uint32_t ReadWord(std::uint32_t field) override {
        if(field==base+64)return record_.fields[4];
        if(field==base+68)return array_;
        if(field==base+72)return 2;
        if(field==base+76)return 3;
        throw std::logic_error("invalid monster word field");
    }
    void WriteWord(std::uint32_t field,std::uint32_t value) override {
        Require(field==base+68 && (value==0 || value==array),"invalid monster array handle");array_=value;
    }
    std::uint32_t Allocate(std::uint32_t size) override {
        Require(!array_ && size && size%4==0 && size<=input_.Remaining(),"invalid monster value array size");
        record_.values.resize(size/4);return array;
    }
    void Free(std::uint32_t handle) override {Require(handle==array_,"invalid monster array release");record_.values.clear();array_=0;}
    std::uint32_t StreamKind(std::uint32_t stream) override {Stream(stream);return 1;}
    std::uint32_t StreamLocale(std::uint32_t stream) override {Stream(stream);return 1;}
    void CheckXml(std::uint32_t locale,std::uint32_t document) override {
        Require(locale==1,"invalid monster XML adapter");
        if(document==2)record_.behavior=input_.Xml();
        else if(document==3)record_.cache=input_.Xml();
        else throw std::logic_error("invalid monster XML handle");
    }
    void Read(){pal4::MonsterInfoRecord::Read(base,1,*this);}
};
void Unique(std::unordered_set<std::string>& names,const std::string& name) {
    Require(!name.empty() && names.insert(name).second,"empty or duplicate scene record name");
}
}
std::vector<SceneGob> ReadSceneGobs(std::istream& input) {
    Reader r(input);const auto count=r.Count();std::vector<std::uint32_t> types(count);
    for(auto& type:types)type=r.Word();
    std::vector<SceneGob> records;records.reserve(count);std::unordered_set<std::string> names;
    for(auto type:types) {
        SceneGob record;record.type=type;record.name=r.Text();Unique(names,record.name);
        record.resource_directory=r.Text();record.model=r.Text();record.variant=r.Text();
        r.Point(record.position);r.Point(record.rotation);record.script=r.Text();
        for(auto& flag:record.flags)flag=r.Word();
        record.config=r.Xml();records.push_back(std::move(record));
    }
    r.End();return records;
}
std::vector<SceneNpc> ReadSceneNpcs(std::istream& input) {
    Reader r(input);const auto count=r.Count();std::vector<SceneNpc> records;records.reserve(count);std::unordered_set<std::string> names;
    for(std::uint32_t i=0;i<count;++i) {
        SceneNpc record;record.name=r.Text();Unique(names,record.name);record.resource=r.Text();record.script=r.Text();
        r.Point(record.position);r.Point(record.rotation);for(auto& flag:record.flags)flag=r.Word();
        record.behavior=r.Xml();record.cache=r.Xml();records.push_back(std::move(record));
    }
    r.End();return records;
}
std::optional<std::string> SceneNpc::DefaultAction() const {
    for(const auto& child:cache.Root().children)if(child.name=="NPCINFO_BufferCache_Attr") {
        const auto* attribute=child.FindAttribute("NPCINFO_BufferCache_Attr_defaultAct");
        if(attribute)if(const auto* action=std::get_if<std::string>(&attribute->value))return *action;
        return "C01";
    }
    return {};
}
std::vector<SceneMonster> ReadSceneMonsters(std::istream& input) {
    Reader r(input);const auto count=r.Count();std::vector<SceneMonster> records;records.reserve(count);std::unordered_set<std::string> names;
    for(std::uint32_t i=0;i<count;++i){
        SceneMonster record;MonsterReader reader(r,record);reader.Read();Unique(names,record.name);
        Require(!record.resource.empty(),"empty monster resource");
        Require(record.fields[4]<=0x7fffffffu && record.fields[4]==record.values.size(),"invalid monster value count");
        records.push_back(std::move(record));
    }
    r.End();return records;
}
std::vector<SceneEvent> ReadSceneEvents(std::istream& input) {
    Reader r(input);const auto count=r.Count();std::vector<SceneEvent> records;records.reserve(count);std::unordered_set<std::string> names;
    for(std::uint32_t i=0;i<count;++i) {
        SceneEvent record;record.name=r.Text();Unique(names,record.name);record.shape=r.Word();record.effect_type=r.Word();
        record.effect=r.Text();for(auto& field:record.effect_fields)field=r.Word();
        record.world=r.Text();record.scene=r.Text();record.script=r.Text();
        const auto properties=r.Count(100000);Require(std::uint64_t(properties)*80<=r.Remaining(),"truncated scene event properties");
        record.properties.resize(properties);for(auto& property:record.properties)for(auto& word:property)word=r.Word();
        for(auto& flag:record.geometry_flags)flag=r.Word();
        record.clump=r.OptionalClump(record.normalized_clump);records.push_back(std::move(record));
    }
    r.End();return records;
}
std::vector<SceneCamera> ReadSceneCameras(std::istream& input) {
    Reader r(input);const auto count=r.Count();std::vector<SceneCamera> records;records.reserve(count);std::unordered_set<std::string> names;
    for(std::uint32_t i=0;i<count;++i) {
        SceneCamera record;record.name=r.Text();Unique(names,record.name);
        for(std::size_t j=0;j<11;++j)record.values[j]=r.Word();
        if(r.HasXmlAt(8)){record.values[11]=r.Word();record.values[12]=r.Word();record.config=r.Xml();}
        records.push_back(std::move(record));
    }
    r.End();return records;
}
void WriteSceneCatalogReport(std::ostream& output,const SceneCatalog& catalog) {
    const auto hex=[&](std::string_view value) {
        constexpr char digits[]="0123456789abcdef";
        output<<'"';for(unsigned char c:value)output<<digits[c>>4]<<digits[c&15];output<<'"';
    };
    const auto words=[&](const auto& values){output<<'[';bool first=true;for(auto value:values){if(!first)output<<',';first=false;output<<value;}output<<']';};
    const auto point=[&](const auto& values){output<<'[';bool first=true;for(float value:values){if(!first)output<<',';first=false;output<<std::bit_cast<std::uint32_t>(value);}output<<']';};
    output<<"{\"gobs\":[";
    for(std::size_t i=0;i<catalog.gobs.size();++i) {
        if(i)output<<',';
        const auto& g=catalog.gobs[i];output<<"{\"type\":"<<g.type<<",\"name_hex\":";hex(g.name);
        output<<",\"directory_hex\":";hex(g.resource_directory);output<<",\"model_hex\":";hex(g.model);output<<",\"variant_hex\":";hex(g.variant);
        output<<",\"script_hex\":";hex(g.script);output<<",\"position_bits\":";point(g.position);output<<",\"rotation_bits\":";point(g.rotation);
        output<<",\"flags\":";words(g.flags);output<<",\"config_attributes\":"<<g.config.Root().attributes.size();
        if(g.type==8){
            output<<",\"effect\":";const auto effect=ReadGobEffectParameters(g.config);
            if(effect){output<<"{\"name_hex\":";hex(effect->name);output<<",\"times\":"<<effect->times<<",\"saver\":"<<effect->saver<<",\"distance_clip\":"<<effect->distance_clip<<'}';}
            else output<<"null";
        }
        output<<'}';
    }
    output<<"],\"npcs\":[";
    for(std::size_t i=0;i<catalog.npcs.size();++i) {
        if(i)output<<',';
        const auto& n=catalog.npcs[i];output<<"{\"name_hex\":";hex(n.name);output<<",\"resource_hex\":";hex(n.resource);
        output<<",\"default_action_hex\":";hex(n.DefaultAction().value_or(""));
        output<<",\"position_bits\":";point(n.position);output<<",\"rotation_bits\":";point(n.rotation);output<<",\"flags\":";words(n.flags);output<<'}';
    }
    output<<"],\"monsters\":[";
    for(std::size_t i=0;i<catalog.monsters.size();++i){
        if(i)output<<',';
        const auto& m=catalog.monsters[i];output<<"{\"name_hex\":";hex(m.name);output<<",\"resource_hex\":";hex(m.resource);
        output<<",\"position_bits\":";point(m.position);output<<",\"rotation_bits\":";point(m.rotation);
        output<<",\"fields\":";words(m.fields);output<<",\"values\":";words(m.values);
        const auto behavior=BinaryXmlConfig::Serialize(m.behavior.Root()),cache=BinaryXmlConfig::Serialize(m.cache.Root());
        Require(bool(behavior) && bool(cache),"cannot serialize monster XML report");
        output<<",\"behavior_hex\":";hex(std::string_view(reinterpret_cast<const char*>(behavior->data()),behavior->size()));
        output<<",\"cache_hex\":";hex(std::string_view(reinterpret_cast<const char*>(cache->data()),cache->size()));output<<'}';
    }
    output<<"],\"paths\":[";
    for(std::size_t i=0;i<catalog.paths.size();++i){
        if(i)output<<',';
        const auto& path=catalog.paths[i];output<<"{\"name_hex\":";hex(path.name);
        output<<",\"grid_marker\":"<<path.grid_marker<<",\"radius_bits\":"<<path.radius_bits<<",\"loop_byte\":"<<unsigned(path.loop_byte)<<",\"point_bits\":[";
        for(std::size_t j=0;j<path.points.size();++j){if(j)output<<',';point(path.points[j]);}
        output<<"]}";
    }
    output<<"],\"events\":[";
    for(std::size_t i=0;i<catalog.events.size();++i) {
        if(i)output<<',';
        const auto& e=catalog.events[i];output<<"{\"name_hex\":";hex(e.name);output<<",\"shape\":"<<e.shape<<",\"effect_type\":"<<e.effect_type<<",\"script_hex\":";hex(e.script);
        output<<",\"effect_hex\":";hex(e.effect);output<<",\"world_hex\":";hex(e.world);output<<",\"scene_hex\":";hex(e.scene);
        output<<",\"property_count\":"<<e.properties.size()<<",\"property_words\":[";
        for(std::size_t j=0;j<e.properties.size();++j){if(j)output<<',';words(e.properties[j]);}
        output<<"],\"clump_bytes\":"<<e.clump.size()<<",\"geometry_flags\":";words(e.geometry_flags);output<<'}';
    }
    output<<"],\"cameras\":[";
    for(std::size_t i=0;i<catalog.cameras.size();++i) {
        if(i)output<<',';
        const auto& [name,records]=catalog.cameras[i];output<<"{\"file_hex\":";hex(name);output<<",\"records\":[";
        for(std::size_t j=0;j<records.size();++j){if(j)output<<',';output<<"{\"name_hex\":";hex(records[j].name);output<<",\"values\":";words(records[j].values);output<<'}';}
        output<<"]}";
    }
    output<<"],\"gameplay_initialized\":false}";Require(bool(output),"cannot write scene catalog report");
}
} // namespace pal4::portable
