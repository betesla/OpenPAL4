#include "pal4/portable/player.h"
#include "pal4/resource_rotation_angle.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <ostream>
#include <iomanip>
#include <stdexcept>
#include <unordered_set>

namespace pal4::portable {
namespace {
void Require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
std::uint32_t Word(std::istream& input) {
    unsigned char bytes[4]{};
    Require(bool(input.read(reinterpret_cast<char*>(bytes),4)), "truncated UpgradeData resource");
    return bytes[0] | (std::uint32_t(bytes[1])<<8) | (std::uint32_t(bytes[2])<<16) | (std::uint32_t(bytes[3])<<24);
}
struct NumericRow {
    std::array<std::string,39> names;
    std::array<std::uint32_t,39> types{}, values{};
    std::uint32_t Get(std::string_view name, unsigned type) const {
        for (std::size_t i=0;i<names.size();++i) if (names[i]==name) {
            Require(types[i]==type,"incorrect UpgradeData field type"); return values[i];
        }
        throw std::runtime_error("missing UpgradeData field "+std::string(name));
    }
    std::uint32_t Number(std::string_view name) const { return Get(name,1); }
    float Real(std::string_view name) const {
        const auto value=std::bit_cast<float>(Get(name,3));
        Require(std::isfinite(value),"non-finite UpgradeData value"); return value;
    }
};
template<class Row> PlayerStats Stats(const Row& row) {
    PlayerStats result;
    constexpr std::array core={"CA_MAX_HP","CA_ADDITIONAL_RAGE","CA_MAX_MP","CA_PHYSICAL","CA_TOUGHNESS","CA_SPEED","CA_LUCKY","CA_WILL"};
    constexpr std::array rates={"CA_ADDITIONAL_CRITICAL","CA_FEND_OFF","CA_ADDITIONAL_HITTING","CA_PAY_FOR_SPARE"};
    constexpr std::array channels={"PHYSICAL","WATER","FIRE","THUNDER","AIR","EARTH"};
    constexpr std::array elements={"EFE_WATER","EFE_FIRE","EFE_THUNDER","EFE_AIR","EFE_EARTH"};
    for (std::size_t i=0;i<core.size();++i) result.core[i]=row.Number(core[i]);
    for (std::size_t i=0;i<rates.size();++i) result.rates[i]=row.Real(rates[i]);
    for (std::size_t i=0;i<channels.size();++i) {
        const auto prefix=std::string("CA_ATTACK_")+channels[i];
        result.attack[i]=row.Number(prefix+"_ADDITIONAL");
        result.react[i]=row.Real(prefix+"_REACT"); result.extract[i]=row.Real(prefix+"_EXTRACT");
    }
    for (std::size_t i=0;i<elements.size();++i) result.elements[i]=row.Number(elements[i]);
    return result;
}
std::int32_t Signed(std::uint32_t value) { return std::bit_cast<std::int32_t>(value); }
std::int32_t Health(std::int32_t value, std::uint32_t maximum) {
    return value>Signed(maximum)?Signed(maximum):(value<0?0:value);
}
}
std::vector<UpgradeRecord> ReadUpgrades(std::istream& input) {
    Require(Word(input)==0x50553450 && Word(input)==1,"unsupported UpgradeData resource");
    const auto count=Word(input); Require(count>0 && count<=4096 && Word(input)==39,"invalid UpgradeData counts");
    NumericRow row; std::unordered_set<std::string> names;
    for (std::size_t i=0;i<row.names.size();++i) {
        const auto size=Word(input); Require(size>0 && size<=64,"invalid UpgradeData field name length");
        row.names[i].resize(size); Require(bool(input.read(row.names[i].data(),size)),"truncated UpgradeData schema");
        Require(row.names[i].find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")==std::string::npos,
                "invalid UpgradeData field name");
        Require(names.insert(row.names[i]).second,"duplicate UpgradeData field");
        row.types[i]=Word(input); Require(row.types[i]==1 || row.types[i]==3,"invalid UpgradeData field type");
    }
    std::vector<UpgradeRecord> records; records.reserve(count); std::unordered_set<std::uint32_t> ids;
    for (std::uint32_t i=0;i<count;++i) {
        for (auto& value:row.values) value=Word(input);
        UpgradeRecord record{row.Number("ID"),row.Number("ROLE_ID"),row.Number("CA_LEVEL"),row.Number("CA_EXPERIENCE"),Stats(row)};
        Require(record.role_id>=1001 && record.role_id<=1004 && record.level>=1 && record.level<=99 && Signed(record.experience)>=0,
                "invalid UpgradeData role/level/experience");
        Require(ids.insert(record.id).second,"duplicate UpgradeData ID"); records.push_back(record);
    }
    Require(input.peek()==std::char_traits<char>::eof() && !input.bad(),"trailing UpgradeData bytes");
    return records;
}
PlayerProgress InitialPlayerProgress(const RoleRecord& role, std::span<const UpgradeRecord> upgrades) {
    PlayerProgress result; result.base=Stats(role); result.level=1;
    const auto id=role.Number("ROLE_ID");
    const auto found=std::find_if(upgrades.begin(),upgrades.end(),[&](const auto& r){return r.role_id==id && r.level==1;});
    Require(found!=upgrades.end(),"missing initial player UpgradeData");
    // 57DB70保留原首字/类型；581690另行恢复五灵字段。本结构不存虚表。
    const auto elements=result.base.elements; result.base=found->stats; result.base.elements=elements;
    result.effective=result.base;
    // 一级的前一级经验下界为0，上界为该级CA_EXPERIENCE-1。
    const auto upper=Signed(found->experience-1u);
    if (result.experience>upper) result.experience=upper;
    result.hp=Health(Signed(result.effective.core[0]),result.effective.core[0]);
    result.mp=Health(Signed(result.effective.core[2]),result.effective.core[2]);
    result.dead=result.hp<=0;
    return result;
}
void PlayerRoster::Clear() noexcept { slots_={}; world_.clear();scene_.clear();leader_=0; prepared_=false; }
void PlayerRoster::SetPartyOrder(std::uint32_t player,std::uint32_t order) {
    Require(prepared_ && player<slots_.size() && order<slots_.size(),"invalid prepared party order");
    slots_[player].script_index=order;
}
void PlayerRoster::Prepare(const RoleCatalog& roles, std::span<const UpgradeRecord> upgrades, const ActorLoader& load) {
    Clear(); std::array<PlayerSlot,4> next;
    constexpr std::array names={"Player-TIANHE","Player-LINSHA","Player-MENGLI","Player-ZIYING"};
    for (std::uint32_t index=0;index<next.size();++index) {
        auto& slot=next[index]; slot.name=names[index]; slot.role=roles[index];
        Require(slot.role.Number("ROLE_ID")==1001+index,"invalid player Role ordering");
        slot.actor=std::make_unique<Actor>(load(slot.role.Text("RA_MODEL")));
        Require(!slot.actor->frames.empty() && !slot.actor->mesh.vertices.empty(),"empty player actor resource");
        slot.script_index=index;
    }
    for (auto& slot:next) slot.progress=InitialPlayerProgress(slot.role,upgrades);
    next[0].inventory_enabled=true;
    slots_=std::move(next); prepared_=true;
}
void WritePlayerReport(std::ostream& output, const PlayerRoster& roster) {
    output<<"{\"prepared\":"<<(roster.Prepared()?"true":"false")<<",\"leader\":"<<roster.Leader()<<",\"gameplay_initialized\":false,\"players\":[";
    if (roster.Prepared()) for (std::size_t i=0;i<roster.Slots().size();++i) {
        const auto& slot=roster.Slots()[i]; const auto& p=slot.progress;
        if (i) output<<',';
        output<<"{\"index\":"<<i<<",\"name\":\""<<slot.name<<"\",\"role_id\":"<<slot.role.Number("ROLE_ID")<<",\"model\":\""<<slot.role.Text("RA_MODEL")
            <<"\",\"frames\":"<<slot.actor->frames.size()<<",\"vertices\":"<<slot.actor->mesh.vertices.size()
            <<",\"script_index\":"<<slot.script_index<<",\"active\":"<<(slot.active?"true":"false")
            <<",\"script_priority\":"<<std::bit_cast<std::int32_t>(slot.script_index)<<",\"auto_seek\":"<<(slot.auto_seek?"true":"false")
            <<",\"inventory_enabled\":"<<(slot.inventory_enabled?"true":"false")<<",\"level\":"<<p.level<<",\"experience\":"<<p.experience
            <<",\"hp\":"<<p.hp<<",\"mp\":"<<p.mp<<",\"rage\":"<<p.rage<<",\"stats_bits\":[";
        bool first=true;
        const auto words=[&](const auto& values) { for (auto value:values) {if(!first)output<<',';first=false;output<<value;} };
        const auto floats=[&](const auto& values) { for(float value:values) {if(!first)output<<',';first=false;output<<std::bit_cast<std::uint32_t>(value);} };
        words(p.base.core);floats(p.base.rates);words(p.base.attack);floats(p.base.react);floats(p.base.extract);words(p.base.elements);
        output<<"],\"effective_stats_bits\":[";first=true;
        words(p.effective.core);floats(p.effective.rates);words(p.effective.attack);floats(p.effective.react);floats(p.effective.extract);words(p.effective.elements);
        output<<"],\"position\":[";
        for (std::size_t n=0;n<slot.position.size();++n) {if(n)output<<',';output<<slot.position[n];}
        output<<"],\"rotation\":[";
        for (std::size_t n=0;n<slot.rotation.size();++n) {if(n)output<<',';output<<slot.rotation[n];}
        output<<"],\"equipment\":[";
        for (std::size_t n=0;n<slot.equipment.size();++n) {if(n)output<<',';output<<slot.equipment[n];}
        output<<"],\"bind_sphere\":";
        if(const auto sphere=ActorBindSphere(*slot.actor)) {
            output<<std::setprecision(9)<<'[';for(unsigned n=0;n<4;++n){if(n)output<<',';output<<(*sphere)[n];}output<<']';
        } else output<<"null";
        output<<'}';
    }
    output<<"]}";Require(bool(output),"cannot write player report");
}
void PlayerRoster::EnterScene(std::string_view world,std::string_view scene,std::uint32_t index) {
    Require(prepared_ && index<slots_.size(),"player scene binding requires a prepared player");
    auto& selected=slots_[index];bool found=false;
    for(const auto& clip:selected.actor->clips)if(clip.name=="C01")found=true;
    Require(found,"initial player C01 animation is missing");
    world_=world;scene_=scene;
    for(auto& slot:slots_){slot.active=false;slot.world.clear();slot.scene.clear();if(slot.animation.empty())slot.animation="C01";}
    selected.world=world;selected.scene=scene;selected.animation="C01";selected.active=true;
}
void PlayerRoster::SelectAction(std::uint32_t index,std::string_view action) {
    Require(prepared_ && index<slots_.size(),"player action requires prepared slot");
    auto& player=slots_[index];
    const auto same=[](std::string_view a,std::string_view b){
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i){
            const auto fold=[](char c){return c>='A' && c<='Z'?static_cast<char>(c+'a'-'A'):c;};
            if(fold(a[i])!=fold(b[i]))return false;
        }
        return true;
    };
    const auto it=std::find_if(player.actor->clips.begin(),player.actor->clips.end(),[&](const auto& clip){return same(clip.name,action);});
    Require(it!=player.actor->clips.end(),"player action is missing");
    player.animation=it->name;
}
void PlayerRoster::SetPosition(std::uint32_t index,std::array<float,3> position) {
    Require(prepared_ && index<slots_.size(),"player position requires prepared slot");
    for(float v:position)Require(std::isfinite(v),"non-finite player position");
    slots_[index].position=position;
}
void PlayerRoster::SetRotation(std::uint32_t index,std::array<float,3> rotation) {
    Require(prepared_ && index<slots_.size(),"player rotation requires prepared slot");
    for(float value:rotation)Require(std::isfinite(value),"non-finite player rotation");
    for(auto& value:rotation)value=pal4::NormalizeResourceRotationAngle(value);
    slots_[index].rotation=rotation;
}
void PlayerRoster::SetFullHp() {
    Require(prepared_,"full HP requires prepared players");
    for(auto& slot:slots_)if(slot.inventory_enabled){
        slot.progress.hp=Health(Signed(slot.progress.effective.core[0]),slot.progress.effective.core[0]);
        slot.progress.dead=slot.progress.hp<=0;
    }
}
void PlayerRoster::SetFullMp() {
    Require(prepared_,"full MP requires prepared players");
    for(auto& slot:slots_)if(slot.inventory_enabled){
        slot.progress.mp=Health(Signed(slot.progress.effective.core[2]),slot.progress.effective.core[2]);
    }
}
void PlayerRoster::SetHealth(std::uint32_t index,std::int32_t hp,std::int32_t mp) {
    Require(prepared_ && index<slots_.size(),"player health update requires prepared slot");
    auto& p=slots_[index].progress;
    p.hp=Health(hp,p.effective.core[0]);
    p.mp=Health(mp,p.effective.core[2]);
    p.dead=p.hp<=0;
}
void PlayerRoster::SetFullRage() {
    Require(prepared_,"full rage requires prepared players");
    for(auto& slot:slots_)if(slot.inventory_enabled)slot.progress.rage=100;
}
void PlayerRoster::TakeARest() {
    Require(prepared_,"player rest requires prepared players");
    SetFullHp();
    SetFullMp();
}
void PlayerRoster::SetLevel(std::int32_t index,std::int32_t level) {
    Require(prepared_ && index>=0 && static_cast<std::size_t>(index)<slots_.size() && level>=1 && level<=99,"invalid player level update");
    slots_[static_cast<std::size_t>(index)].progress.level=level;
}
std::int32_t PlayerRoster::GetLevel(std::int32_t index) const {
    Require(prepared_ && index>=0 && static_cast<std::size_t>(index)<slots_.size(),"invalid player level query");
    return slots_[static_cast<std::size_t>(index)].progress.level;
}
void PlayerRoster::AddSkill(std::int32_t index,std::uint32_t skill_id) {
    Require(prepared_ && index>=0 && static_cast<std::size_t>(index)<slots_.size(),"invalid player skill target");
    auto& skills=slots_[static_cast<std::size_t>(index)].skills;
    if(std::find(skills.begin(),skills.end(),skill_id)==skills.end())skills.push_back(skill_id);
}
void PlayerRoster::AddFavor(std::int32_t first_index,std::int32_t second_index,std::int32_t delta) {
    Require(prepared_,"player favor update requires prepared players");
    if(first_index<0 || static_cast<std::size_t>(first_index)>=slots_.size())return;
    if(second_index<0 || static_cast<std::size_t>(second_index)>=slots_.size())return;
    auto& cell=slots_[static_cast<std::size_t>(first_index)].favor[static_cast<std::size_t>(second_index)];
    cell=std::max<std::int32_t>(0,cell+delta);
}
void PlayerRoster::SetVisible(std::int32_t index,bool visible) {
    Require(prepared_ && index>=0 && static_cast<std::size_t>(index)<slots_.size(),"invalid player visibility target");
    slots_[static_cast<std::size_t>(index)].visible=visible;
}
} // namespace pal4::portable
