#include "pal4/portable/effect_document.h"
#include "pal4/gob_effect.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <stdexcept>
#include <type_traits>
namespace pal4::portable {
namespace {
const BinaryXmlNode* Child(const BinaryXmlNode& root,std::string_view name){
    for(const auto& n:root.children)if(n.name==name)return &n;
    return nullptr;
}
struct ConfigReader final:pal4::IGobEffectConfigValueRuntime {
    const BinaryXmlNode& node;GobEffectParameters result;
    explicit ConfigReader(const BinaryXmlNode& n):node(n){}
    std::string ReadString(std::string_view key,std::string_view fallback) override {
        if(const auto* attr=node.FindAttribute(key))if(const auto* text=std::get_if<std::string>(&attr->value))return *text;
        return std::string(fallback);
    }
    int ReadInt(std::string_view key,int fallback) override {
        const auto* attr=node.FindAttribute(key);return attr?std::bit_cast<int>(attr->ToInt(std::bit_cast<unsigned>(fallback))):fallback;
    }
    void AssignEffectName(unsigned address,std::string_view name) override {
        if(address!=pal4::GobEffect::kStringOffset)throw std::logic_error("invalid effect config string slot");
        result.name=name;
    }
    void WriteWord(unsigned address,unsigned value) override {
        switch(address){case 0x1f0:result.times=value;break;case 0x1f4:result.saver=value;break;case 0x1f8:result.distance_clip=value;break;default:throw std::logic_error("invalid effect config word slot");}
    }
};
// 文档报告保留所有节点与类型；只用于诊断，不将未知对象当成已支持播放。
void JsonText(std::ostream& out,std::string_view text){
    constexpr char hex[]="0123456789abcdef";out<<'"';
    // 原文档可能含CP936注释；诊断JSON逐字节转义。
    for(unsigned char c:text){if(c=='"' || c=='\\')out<<'\\'<<char(c);else if(c<32 || c>=127)out<<"\\u00"<<hex[c>>4]<<hex[c&15];else out<<char(c);}out<<'"';
}
void JsonNode(std::ostream& out,const BinaryXmlNode& node){
    out<<"{\"name\":";JsonText(out,node.name);out<<",\"attributes\":[";
    for(std::size_t i=0;i<node.attributes.size();++i){if(i)out<<',';const auto& a=node.attributes[i];out<<"{\"name\":";JsonText(out,a.name);out<<",\"type\":"<<a.type<<",\"value\":";
        std::visit([&](const auto& v){using T=std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<T,std::monostate>)out<<"null";
            else if constexpr(std::is_same_v<T,std::string>)JsonText(out,v);
            else if constexpr(std::is_same_v<T,std::vector<std::uint8_t>>){out<<'[';for(std::size_t n=0;n<v.size();++n){if(n)out<<',';out<<unsigned(v[n]);}out<<']';}
            else if constexpr(std::is_same_v<T,float>){if(std::isfinite(v))out<<std::setprecision(9)<<v;else out<<"null";out<<",\"bits\":"<<std::bit_cast<unsigned>(v);}
            else out<<int(v);
        },a.value);out<<'}';
    }
    out<<"],\"children\":[";for(std::size_t i=0;i<node.children.size();++i){if(i)out<<',';JsonNode(out,node.children[i]);}out<<"]}";
}
}
std::optional<GobEffectParameters> ReadGobEffectParameters(const BinaryXmlConfig& document){
    const auto& root=document.Root();const auto* parameters=root.name=="PAL4-GameObject-Parameters"?&root:Child(root,"PAL4-GameObject-Parameters");
    if(!parameters)return {};
    const auto* effect=Child(*parameters,"PAL4_GameObject-effect");if(!effect)return {};
    ConfigReader values(*effect);pal4::GobEffect::ApplyConfigValues(0,values);return values.result;
}
EffectDocuments::EffectDocuments(EffectMemberReader reader):read_(std::move(reader)){if(!read_)throw std::invalid_argument("effect package reader is not bound");}
std::size_t EffectDocuments::Size() const{std::size_t count=0;for(const auto& entry:documents_)if(!entry.second.expired())++count;return count;}
std::shared_ptr<const EffectDocument> EffectDocuments::Load(std::string_view name){
    const auto valid_char=[](unsigned char c){
        if(c>=0x80)return true;
        return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'||c==' '||c=='('||c==')'||c=='+'||c=='~';
    };
    if(name.empty() || name.size()>255 || !std::all_of(name.begin(),name.end(),valid_char))throw std::invalid_argument("invalid effect document name");
    for(auto i=documents_.begin();i!=documents_.end();)if(i->second.expired())i=documents_.erase(i);else ++i;
    if(const auto i=documents_.find(std::string(name));i!=documents_.end())if(auto existing=i->second.lock())return existing;
    auto document=std::make_shared<EffectDocument>();document->name=name;document->member="script\\"+std::string(name)+".eft";
    auto bytes=read_(document->member);if(!bytes)throw std::runtime_error("original effect document is missing: "+document->member);
    document->source_bytes=bytes->size();
    if(!document->xml.Parse(*bytes))throw std::runtime_error("original effect document is invalid: "+document->member);
    documents_.emplace(std::string(name),document);return document;
}
void WriteEffectDocumentReport(std::ostream& output,const EffectDocument& document){
    output<<"{\"name\":";JsonText(output,document.name);output<<",\"member\":";JsonText(output,document.member);output<<",\"bytes\":"<<document.source_bytes<<",\"document\":";JsonNode(output,document.xml.Root());output<<",\"playback_bound\":false}";
    if(!output)throw std::runtime_error("cannot write effect document report");
}
}
