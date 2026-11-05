#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace pal4::portable::rw {
inline void Need(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
struct Chunk {std::uint32_t id;std::size_t begin,end;};
class Input {
    std::span<const std::uint8_t> data_;
public:
    explicit Input(std::span<const std::uint8_t> data):data_(data){}
    std::size_t Size()const{return data_.size();}
    std::uint8_t U8(std::size_t p)const{Need(p<data_.size(),"truncated RenderWare byte");return data_[p];}
    std::uint32_t U32(std::size_t p)const{Need(p<=data_.size() && data_.size()-p>=4,"truncated RenderWare word");return data_[p]|(std::uint32_t(data_[p+1])<<8)|(std::uint32_t(data_[p+2])<<16)|(std::uint32_t(data_[p+3])<<24);}
    std::uint16_t U16(std::size_t p)const{Need(p<=data_.size() && data_.size()-p>=2,"truncated RenderWare halfword");return data_[p]|(std::uint16_t(data_[p+1])<<8);}
    double Float(std::size_t p)const{const auto f=std::bit_cast<float>(U32(p));return std::isfinite(f)?f:0.0;}
    std::string Text(std::size_t p,std::size_t end)const{Need(p<=end && end<=data_.size(),"invalid RenderWare string range");std::string s;while(p<end && data_[p])s.push_back(static_cast<char>(data_[p++]));return s;}
    std::string Raw(std::size_t p,std::size_t end)const{Need(p<=end && end<=data_.size(),"invalid RenderWare data range");return std::string(reinterpret_cast<const char*>(data_.data()+p),end-p);}
    Chunk ReadChunk(std::size_t p,std::size_t limit)const{Need(p<=limit && limit-p>=12,"truncated RenderWare chunk");const auto size=U32(p+4);Need(size<=limit-p-12,"RenderWare chunk outside parent");return {U32(p),p+12,p+12+size};}
    std::vector<Chunk> Children(Chunk parent)const{std::vector<Chunk> result;for(auto p=parent.begin;p<parent.end;){auto c=ReadChunk(p,parent.end);result.push_back(c);p=c.end;}return result;}
};
inline Chunk First(const std::vector<Chunk>& chunks,unsigned id){for(auto c:chunks)if(c.id==id)return c;throw std::runtime_error("required RenderWare chunk missing");}
inline void Range(std::size_t p,std::size_t n,std::size_t end){Need(p<=end && n<=end-p,"truncated RenderWare structure");}
struct Material {std::string texture,lightmap;std::array<std::uint8_t,4> color{255,255,255,255};std::array<std::string,8> uv_animations{};};
inline bool NameCharacter(char c){return (c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-' || c=='.' || c=='/' || c=='\\';}
inline Material ReadMaterial(const Input& in,Chunk material){
    const auto parts=in.Children(material);const auto s=First(parts,1);Range(s.begin,28,s.end);Material result;const auto color=in.U32(s.begin+4);for(unsigned i=0;i<4;++i)result.color[i]=static_cast<std::uint8_t>(color>>(i*8));
    for(auto part:parts){if(part.id==6 && in.U32(s.begin+12)){for(auto child:in.Children(part))if(child.id==2){result.texture=in.Text(child.begin,child.end);break;}}
        if(part.id==3)for(auto ext:in.Children(part))if(ext.id==0x120){const auto raw=in.Raw(ext.begin,ext.end);const auto pos=raw.find("LightingMap");if(pos!=raw.npos){auto start=pos;while(start && NameCharacter(raw[start-1]))--start;auto end=pos+11;while(end<raw.size() && NameCharacter(raw[end]))++end;result.lightmap=raw.substr(start,end-start);}}}
    // RpMaterial UV animation plugin: nested STRUCT, slot mask, then one
    // fixed 32-byte dictionary name for each occupied slot (librw readUVAnim).
    for(auto part:parts)if(part.id==3)for(auto ext:in.Children(part))if(ext.id==0x135){
        const auto data=First(in.Children(ext),1);Range(data.begin,4,data.end);
        const auto mask=in.U32(data.begin);Need((mask&~255u)==0,"invalid material UV slot mask");
        auto p=data.begin+4;
        for(unsigned i=0;i<8;++i)if(mask&(1u<<i)){
            Range(p,32,data.end);auto name=in.Text(p,p+32);Need(!name.empty()&&name.size()<32,"invalid material UV animation name");
            result.uv_animations[i]=std::move(name);p+=32;
        }
        Need(p==data.end,"extra material UV animation data");
    }
    return result;
}
inline std::vector<Material> ReadMaterials(const Input& in,Chunk chunk){
    const auto children=in.Children(chunk);const auto header=First(children,1);Range(header.begin,4,header.end);const auto count=in.U32(header.begin);Need(count<=65536 && header.end-header.begin==4+std::size_t(count)*4,"invalid RenderWare material table");
    std::vector<Material> result;result.reserve(count);std::size_t next=0;
    for(unsigned i=0;i<count;++i){const auto ref=std::bit_cast<std::int32_t>(in.U32(header.begin+4+i*4));if(ref>=0){Need(std::size_t(ref)<result.size(),"invalid RenderWare material reference");result.push_back(result[ref]);}else{Need(ref==-1,"invalid RenderWare material reference");while(next<children.size() && children[next].id!=7)++next;Need(next<children.size(),"missing RenderWare material");result.push_back(ReadMaterial(in,children[next++]));}}
    while(next<children.size()){Need(children[next].id!=7,"extra RenderWare material");++next;}return result;
}
}
