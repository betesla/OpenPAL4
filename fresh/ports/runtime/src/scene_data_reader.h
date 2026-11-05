#pragma once
#include "pal4/binary_xml_config.h"
#include <array>
#include <bit>
#include <cmath>
#include <istream>
#include <stdexcept>

namespace pal4::portable::detail {
inline void Require(bool valid,const char* message) {if(!valid)throw std::runtime_error(message);}
inline void ValidateXml(const BinaryXmlNode& node) {
    for(const auto& attribute:node.attributes) {
        Require(attribute.type<=4,"unsupported scene XML attribute type");
        if(const auto* value=std::get_if<float>(&attribute.value))Require(std::isfinite(*value),"non-finite scene XML value");
    }
    for(const auto& child:node.children)ValidateXml(child);
}
class Reader {
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_{};
public:
    explicit Reader(std::istream& input) {
        char chunk[65536];
        while(input.read(chunk,sizeof chunk) || input.gcount()) {
            Require(bytes_.size()+static_cast<std::size_t>(input.gcount())<=32u*1024*1024,"scene data exceeds limit");
            bytes_.insert(bytes_.end(),chunk,chunk+input.gcount());
        }
        Require(input.eof() && !input.bad(),"cannot read scene data");
    }
    std::size_t Remaining() const {return bytes_.size()-offset_;}
    std::uint32_t Word() {
        Require(Remaining()>=4,"truncated scene data");
        const auto* b=bytes_.data()+offset_;offset_+=4;
        return b[0] | (std::uint32_t(b[1])<<8) | (std::uint32_t(b[2])<<16) | (std::uint32_t(b[3])<<24);
    }
    std::uint8_t Byte() {Require(Remaining()>=1,"truncated scene byte");return bytes_[offset_++];}
    std::uint32_t Peek() {const auto value=Word();offset_-=4;return value;}
    std::uint32_t Count(std::uint32_t limit=10000) {const auto count=Word();Require(count<=limit,"invalid scene record count");return count;}
    std::string Text() {
        const auto length=Word();if(length==0)return {};
        Require(length<=4096 && length<=Remaining(),"invalid scene string length");
        Require(bytes_[offset_+length-1]==0,"unterminated scene string");
        std::string result(reinterpret_cast<const char*>(bytes_.data()+offset_),length-1);offset_+=length;
        Require(result.find('\0')==std::string::npos,"embedded NUL in scene string");return result;
    }
    void Point(std::array<float,3>& point) {for(auto& x:point){x=std::bit_cast<float>(Word());Require(std::isfinite(x),"non-finite scene transform");}}
    bool HasXmlAt(std::size_t skip_bytes) const {
        if(Remaining()<skip_bytes+8)return false;
        BinaryXmlConfig probe;std::size_t consumed{};
        return probe.ParsePrefix(std::span<const std::uint8_t>(bytes_).subspan(offset_+skip_bytes),consumed) && consumed>0;
    }
    BinaryXmlConfig Xml() {
        BinaryXmlConfig result;std::size_t consumed{};
        Require(result.ParsePrefix(std::span<const std::uint8_t>(bytes_).subspan(offset_),consumed) && consumed>0,"invalid scene XML");
        ValidateXml(result.Root());offset_+=consumed;return result;
    }
    std::vector<std::uint8_t> OptionalClump(std::vector<std::uint8_t>& normalized) {
        normalized.clear();
        std::vector<std::pair<std::size_t,std::uint32_t>> patches;
        if(Remaining()<12 || Peek()!=16)return {};
        const auto start=offset_;(void)Word();const auto size=Word();(void)Word();
        const auto body=offset_;
        const auto skip=[&](std::uint32_t expected) {
            Require(Word()==expected,"unexpected event clump child");const auto length=Word();(void)Word();
            Require(length<=Remaining(),"truncated event clump child");offset_+=length;return length;
        };
        Require(Word()==1,"missing event clump struct");const auto structure=Word();(void)Word();
        Require(structure>=12 && structure<=Remaining(),"invalid event clump struct");
        const auto atomics=Count();const auto lights=Word(),cameras=Word();
        Require(!lights && !cameras,"unsupported event clump lights/cameras");offset_+=structure-12;
        skip(14);skip(26);
        std::size_t omitted{};
        for(std::uint32_t i=0;i<atomics;++i) {
            Require(Word()==20,"missing event atomic");const auto atomic_size=Word();(void)Word();const auto atomic_body=offset_;
            skip(1);
            Require(Word()==3,"missing event atomic extension");const auto extension_size=Word();(void)Word();
            const auto extension_body=offset_;
            std::uint32_t declared_consumed=0;
            std::uint32_t missing=0;
            while(declared_consumed<extension_size) {
                Require(Remaining()>=12 && extension_size-declared_consumed>=12,"truncated event atomic extension");
                const auto plugin_pos=offset_;
                const auto id=Word(),declared=Word();(void)Word();
                Require(declared<=extension_size-declared_consumed-12,"truncated event atomic extension");
                declared_consumed+=12u+declared;
                if(id>=0x2101u && id<=0x2104u && (Remaining()<declared || (Remaining()>=12 && (Peek()==3u || Peek()==20u || (Peek()>=0x2101u && Peek()<=0x2104u))))) {
                    missing+=declared;
                    patches.emplace_back(plugin_pos+4,0u);
                } else {
                    Require(declared<=Remaining(),"truncated event atomic extension");
                    offset_+=declared;
                }
            }
            const auto physical_size=static_cast<std::uint32_t>(offset_-extension_body);
            if(missing>0) {
                patches.emplace_back(atomic_body-8,atomic_size-missing);
                patches.emplace_back(extension_body-8,physical_size);
            }
            Require(offset_-atomic_body+missing==atomic_size,"event atomic size mismatch");omitted+=missing;
        }
        skip(3);
        Require(offset_-body+omitted==size,"event clump size mismatch");
        normalized.assign(bytes_.begin()+start,bytes_.begin()+offset_);
        patches.emplace_back(start+4,size-static_cast<std::uint32_t>(omitted));
        for(const auto& [absolute,value]:patches) {
            Require(absolute>=start && absolute-start+4<=normalized.size(),"invalid event normalization offset");
            for(unsigned i=0;i<4;++i)normalized[absolute-start+i]=static_cast<std::uint8_t>(value>>(i*8));
        }
        return {bytes_.begin()+start,bytes_.begin()+offset_};
    }
    void End() {Require(!Remaining(),"trailing scene data bytes");}
};
}
