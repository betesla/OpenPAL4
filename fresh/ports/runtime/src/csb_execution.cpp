/*
 * Typed compatibility adaptation of instruction semantics from AngelScript
 * 2.2.0 source/as_context.cpp. This is an altered implementation, not the
 * original AngelScript engine. Original notice:
 *
 * AngelCode Scripting Library
 * Copyright (c) 2003-2005 Andreas Jönsson
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you
 *    must not claim that you wrote the original software. If you use
 *    this software in a product, an acknowledgment in the product
 *    documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and
 *    must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source
 *    distribution.
 *
 * The original version of this library can be located at:
 * http://www.angelcode.com/angelscript/
 * Andreas Jönsson, andreas@angelcode.com
 */
#include "pal4/portable/csb_execution.h"
#include <bit>
#include <sstream>
#include <stdexcept>

namespace pal4::portable {
namespace {
std::string_view Declaration(int id) {
    switch(id) {
#define PAL4_GAME_FUNCTION(number,text) case number:return text;
#include "game_function_catalog.inc"
#undef PAL4_GAME_FUNCTION
    default:throw std::runtime_error("unsupported legacy system function "+std::to_string(id));
    }
}
std::string Ascii(std::string_view text) {
    for(unsigned char c:text)if(c>=128)throw std::runtime_error("GBK decoder is required for legacy script text");
    return std::string(text);
}
}
struct CsbExecution::Impl {
    struct Reference {bool global{};std::size_t index{};bool module_global{};};
    using Text=std::shared_ptr<std::string>;
    using Cell=std::variant<std::uint32_t,Reference,Text>;
    std::shared_ptr<const CsbModule> module;
    const CsbFunction* function{};
    ScriptGlobals& globals;IScriptServices& services;CsbTextDecoder decoder;
    std::vector<Cell> stack;
    std::vector<Cell> module_globals;
    std::size_t sp{},fp{},pc{};
    struct Frame {const CsbFunction* function;std::size_t pc,fp,sp;};
    std::vector<Frame> frames;
    Cell temporary{};Text object;
    std::uint64_t return_bits{};
    ScriptState state{ScriptState::prepared};ScriptPause pause{};std::string error;
    bool executing{},original_steps{},repeat_steps{};
    Impl(std::shared_ptr<const CsbModule> m,ScriptGlobals& g,IScriptServices& s,CsbTextDecoder d)
        :module(std::move(m)),globals(g),services(s),decoder(d?std::move(d):Ascii),stack(65536),module_globals(65536),sp(stack.size()),fp(sp) {}
    Cell& Top(std::size_t offset=0) {
        if(offset>=stack.size()-sp)throw std::runtime_error("legacy operand stack underflow");return stack[sp+offset];
    }
    void Push(Cell value) {if(!sp)throw std::runtime_error("legacy stack overflow");stack[--sp]=std::move(value);}
    Cell Pop(){auto value=Top();stack[sp++]=std::uint32_t{};return value;}
    static std::uint32_t Number(const Cell& value) {
        if(!std::holds_alternative<std::uint32_t>(value))throw std::runtime_error("legacy numeric slot has a reference");
        return std::get<std::uint32_t>(value);
    }
    Cell& Local(std::int32_t offset) {
        const auto index=std::int64_t(fp)-offset;
        if(index<0 || index>=static_cast<std::int64_t>(stack.size()))throw std::runtime_error("legacy local outside stack");
        return stack[static_cast<std::size_t>(index)];
    }
    Cell Read(const Reference& r) {
        if(r.module_global)return module_globals.at(r.index);
        if(r.global)return static_cast<std::uint32_t>(globals.at(r.index));return stack.at(r.index);
    }
    void Write(const Reference& r,Cell value) {
        if(r.module_global)module_globals.at(r.index)=std::move(value);
        else if(r.global)globals.at(r.index)=std::bit_cast<std::int32_t>(Number(value));else stack.at(r.index)=std::move(value);
    }
    void CleanupFailedState() noexcept {
        for(auto& cell:stack)cell=std::uint32_t{};
        temporary=std::uint32_t{};object.reset();frames.clear();
    }
    static Reference Ref(const Cell& value) {
        if(!std::holds_alternative<Reference>(value))throw std::runtime_error("legacy checked reference required");return std::get<Reference>(value);
    }
    Text String(const Cell& value) {
        const Cell selected=std::holds_alternative<Reference>(value)?Read(Ref(value)):value;
        if(!std::holds_alternative<Text>(selected) || !std::get<Text>(selected))throw std::runtime_error("legacy string reference required");return std::get<Text>(selected);
    }
    void Jump(const CsbInstruction& instruction) {
        const auto target=std::int64_t(instruction.offset)+instruction.bytes+
            std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(instruction.operand));
        for(std::size_t index=0;index<function->instructions.size();++index)
            if(function->instructions[index].offset==target){pc=index;return;}
        throw std::runtime_error("legacy jump target missing");
    }
    bool Call(int id) {
        if(id==19) {
            const auto length=Number(Pop());const auto text=String(Pop());
            if(length!=text->size())throw std::runtime_error("legacy string factory length mismatch");
            object=std::make_shared<std::string>(decoder(*text));return false;
        }
        if(id==17 || id==18) {
            const auto destination=String(Pop()),value=String(Pop());
            if(id==17)*destination=*value;else *destination+=*value;
            object=destination;
            return false;
        }
        if(id==34 || id==35) {
            const auto destination=String(Pop());
            const auto value=std::bit_cast<std::int32_t>(Number(Pop()));
            if(id==34)*destination=std::to_string(value);else *destination+=std::to_string(value);
            object=destination;
            return false;
        }
        if(id==26) {
            const auto right=String(Pop()),left=String(Pop());
            object=std::make_shared<std::string>(*left+*right);return false;
        }
        if(id==36) {
            const auto text=String(Pop());const auto value=std::bit_cast<std::int32_t>(Number(Pop()));
            object=std::make_shared<std::string>(*text+std::to_string(value));return false;
        }
        if(id==37) {
            const auto value=std::bit_cast<std::int32_t>(Number(Pop()));const auto text=String(Pop());
            object=std::make_shared<std::string>(std::to_string(value)+*text);return false;
        }
        const auto declaration=Declaration(id);const auto open=declaration.find('('),close=declaration.rfind(')');
        ScriptCall call{id,globals[0],{}};
        std::istringstream arguments(std::string(declaration.substr(open+1,close-open-1)));std::string parameter;
        while(std::getline(arguments,parameter,',')) {
            std::istringstream words(parameter);std::string type;words>>type;if(type.empty())continue;
            if(type.ends_with('&'))type.pop_back();
            if(parameter.find("out")!=std::string::npos)throw std::runtime_error("legacy writable argument requires typed service");
            if(type=="string") {
                if(id==240 && !call.arguments.empty()) {
                    const bool two_string_args=pc>=4 && function->instructions[pc-4].opcode==118;
                    if(!two_string_args) {
                        call.arguments.emplace_back(std::string{});
                        continue;
                    }
                }
                call.arguments.emplace_back(*String(Pop()));
            }
            else if(type=="int")call.arguments.emplace_back(std::bit_cast<std::int32_t>(Number(Pop())));
            else if(type=="bool")call.arguments.emplace_back((Number(Pop())&255)!=0);
            else if(type=="float")call.arguments.emplace_back(std::bit_cast<float>(Number(Pop())));
            else if(type=="double") {
                const auto lo=std::uint64_t(Number(Pop())),hi=std::uint64_t(Number(Pop()));
                call.arguments.emplace_back(std::bit_cast<double>(lo|(hi<<32)));
            }else throw std::runtime_error("unsupported legacy parameter "+type);
        }
        const auto response=services.Invoke(call);const auto result=declaration.substr(0,declaration.find(' '));
        if(result=="void") {if(!std::holds_alternative<std::monostate>(response.value))throw std::runtime_error("legacy void callback returned a value");}
        else if(result=="int")return_bits=static_cast<std::uint32_t>(std::get<std::int32_t>(response.value));
        else if(result=="bool")return_bits=std::get<bool>(response.value)?1:0;
        else if(result=="float")return_bits=std::bit_cast<std::uint32_t>(std::get<float>(response.value));
        else throw std::runtime_error("unsupported legacy callback result");
        return response.suspend;
    }
    void StepBoundary() {
        if(original_steps && !(repeat_steps && services.ImmediateMode()==1)){
            state=ScriptState::suspended;pause=ScriptPause::step;
        }
    }
    void Instruction(const CsbInstruction& i) {
        const auto u=static_cast<std::uint32_t>(i.operand);const auto s=std::bit_cast<std::int32_t>(u);
        const auto local=static_cast<std::int16_t>(u&65535);
        switch(i.opcode) {
        case 0:for(unsigned n=0;n<(u&65535);++n)(void)Pop();break;
        case 1:for(unsigned n=0;n<(u&65535);++n)Push(std::uint32_t{});break;
        case 2:Push(u);break;
        case 3:Top()=Read(Ref(Top()));break;
        case 4:Push(Local(local));break;
        case 5: {const auto ref=Ref(Pop());Write(ref,Top());break;}
        case 6: {const auto ref=Ref(Pop());Write(ref,Pop());break;}
        case 7: {const auto index=std::int64_t(fp)-local;if(index<0 || index>=static_cast<std::int64_t>(stack.size()))throw std::runtime_error("legacy frame reference outside stack");Push(Reference{false,static_cast<std::size_t>(index)});break;}
        case 8:Local(local)=Pop();break;
        case 9:std::swap(Top(),Top(1));break;
        case 10:temporary=Top();break;
        case 11:Push(temporary);break;
        case 12: {
            if(s<0 || (u&0x40000000u))throw std::runtime_error("invalid legacy script function ID");
            const auto& target=module->functions.at(u&65535);
            if(!target.object_type.empty())throw std::runtime_error("legacy object method call requires binding support");
            if(frames.size()>=256)throw std::runtime_error("legacy call depth limit exceeded");
            frames.push_back({function,pc,fp,sp});function=&target;pc=0;fp=sp;
            for(const auto& variable:function->object_variables)Local(variable.position)=std::uint32_t{};
            break;
        }
        case 13: {
            if(frames.empty()){state=ScriptState::finished;break;}
            const auto previous=frames.back();frames.pop_back();
            if((u&65535)>stack.size()-previous.sp)throw std::runtime_error("legacy return argument count exceeds stack");
            while(sp<fp)(void)Pop();function=previous.function;pc=previous.pc;fp=previous.fp;sp=previous.sp;
            for(unsigned n=0;n<(u&65535);++n)(void)Pop();break;
        }
        case 14:Jump(i);break;
        case 15:case 16:case 91:case 92:case 93:case 94: {
            const auto value=std::bit_cast<std::int32_t>(Number(Pop()));
            const bool take=i.opcode==15?value==0:i.opcode==16?value!=0:i.opcode==91?value<0:i.opcode==92?value>=0:i.opcode==93?value>0:value<=0;
            if(take)Jump(i);break;
        }
        case 17:Top()=Number(Top())==0?1u:0u;break;
        case 18:Top()=Number(Top())!=0?1u:0u;break;
        case 19:Top()=std::bit_cast<std::int32_t>(Number(Top()))<0?1u:0u;break;
        case 20:Top()=std::bit_cast<std::int32_t>(Number(Top()))>=0?1u:0u;break;
        case 21:Top()=std::bit_cast<std::int32_t>(Number(Top()))>0?1u:0u;break;
        case 22:Top()=std::bit_cast<std::int32_t>(Number(Top()))<=0?1u:0u;break;
        case 23: {const auto r=std::bit_cast<std::int32_t>(Number(Pop())),l=std::bit_cast<std::int32_t>(Number(Top()));Top()=static_cast<std::uint32_t>(l+r);break;}
        case 24: {const auto r=std::bit_cast<std::int32_t>(Number(Pop())),l=std::bit_cast<std::int32_t>(Number(Top()));Top()=static_cast<std::uint32_t>(l-r);break;}
        case 25: {const auto r=std::bit_cast<std::int32_t>(Number(Pop())),l=std::bit_cast<std::int32_t>(Number(Top()));Top()=static_cast<std::uint32_t>(l*r);break;}
        case 26: {const auto r=std::bit_cast<std::int32_t>(Number(Pop())),l=std::bit_cast<std::int32_t>(Number(Top()));if(!r)throw std::runtime_error("legacy divide by zero");Top()=static_cast<std::uint32_t>(l/r);break;}
        case 27: {const auto r=std::bit_cast<std::int32_t>(Number(Pop())),l=std::bit_cast<std::int32_t>(Number(Top()));if(!r)throw std::runtime_error("legacy divide by zero");Top()=static_cast<std::uint32_t>(l%r);break;}
        case 28:Top()=static_cast<std::uint32_t>(-std::bit_cast<std::int32_t>(Number(Top())));break;
        case 29: {const auto r=std::bit_cast<std::int32_t>(Number(Pop())),l=std::bit_cast<std::int32_t>(Number(Top()));Top()=static_cast<std::uint32_t>(l<r?-1:l>r?1:0);break;}
        case 30:case 31: {
            const auto reference=Ref(Top());
            const auto value=Number(Read(reference));
            Write(reference,value+(i.opcode==30?1u:std::uint32_t(-1)));
            break;
        }
        case 32:Top()=std::bit_cast<std::uint32_t>(static_cast<float>(std::bit_cast<std::int32_t>(Number(Top()))));break;
        case 33: {const auto r=std::bit_cast<float>(Number(Pop())),l=std::bit_cast<float>(Number(Top()));Top()=std::bit_cast<std::uint32_t>(l+r);break;}
        case 34: {const auto r=std::bit_cast<float>(Number(Pop())),l=std::bit_cast<float>(Number(Top()));Top()=std::bit_cast<std::uint32_t>(l-r);break;}
        case 35: {const auto r=std::bit_cast<float>(Number(Pop())),l=std::bit_cast<float>(Number(Top()));Top()=std::bit_cast<std::uint32_t>(l*r);break;}
        case 36: {const auto r=std::bit_cast<float>(Number(Pop())),l=std::bit_cast<float>(Number(Top()));Top()=std::bit_cast<std::uint32_t>(l/r);break;}
        case 38:Top()=std::bit_cast<std::uint32_t>(-std::bit_cast<float>(Number(Top())));break;
        case 39: {const auto r=std::bit_cast<float>(Number(Pop())),l=std::bit_cast<float>(Number(Top()));Top()=static_cast<std::uint32_t>(l<r?-1:l>r?1:0);break;}
        case 42:Top()=static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<float>(Number(Top()))));break;
        case 43:Top()=~Number(Top());break;
        case 44: {const auto r=Number(Pop()),l=Number(Top());Top()=l&r;break;}
        case 45: {const auto r=Number(Pop()),l=Number(Top());Top()=l|r;break;}
        case 46: {const auto r=Number(Pop()),l=Number(Top());Top()=l^r;break;}
        case 47: {const auto r=Number(Pop()),l=Number(Top());Top()=l<<r;break;}
        case 48: {const auto r=Number(Pop()),l=Number(Top());Top()=l>>r;break;}
        case 49: {const auto r=Number(Pop());const auto l=std::bit_cast<std::int32_t>(Number(Top()));Top()=static_cast<std::uint32_t>(l>>r);break;}
        case 50:Top()=std::bit_cast<std::uint32_t>(static_cast<float>(Number(Top())));break;
        case 51:Top()=static_cast<std::uint32_t>(std::bit_cast<float>(Number(Top())));break;
        case 52: {const auto r=Number(Pop()),l=Number(Top());Top()=static_cast<std::uint32_t>(l<r?-1:l>r?1:0);break;}
        case 53:Top()=static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(Number(Top())))));break;
        case 54:Top()=static_cast<std::uint32_t>(static_cast<std::int32_t>(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(Number(Top())))));break;
        case 55:Top()=Number(Top())&0xffu;break;
        case 56:Top()=Number(Top())&0xffffu;break;
        case 63:Push(std::uint32_t{});break;
        case 65:if(s<0)Push(Reference{true,static_cast<std::size_t>(-std::int64_t(s)-1),false});else Push(Reference{false,static_cast<std::size_t>(u&65535),true});break;
        // AngelScript 2.2 BC_SET8: reserve two 32-bit slots, low word at SP.
        case 66:Push(static_cast<std::uint32_t>(i.operand>>32));Push(u);break;
        case 85:pc+=Number(Pop());break;
        case 86:return_bits=Number(Pop());break;
        case 88:Push(static_cast<std::uint32_t>(return_bits));break;
        case 109: {
            const auto type_index=static_cast<std::size_t>(u);
            const auto constructor=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(i.operand>>32));
            if(module->used_types.at(type_index)!="string" || constructor!=-14)
                throw std::runtime_error("unsupported legacy object allocator or constructor");
            const auto destination=Ref(Top());
            auto allocated=std::make_shared<std::string>();
            Write(destination,Cell(std::move(allocated)));
            (void)Pop();
            break;
        }
        case 111:
            object=String(Local(local));Local(local)=std::uint32_t{};break;
        case 57:case 58: {
            const auto destination=Ref(Pop());
            const auto value=Number(Top());
            Write(destination,i.opcode==57?value&0xffu:value&0xffffu);
            break;
        }
        case 116:case 117: {
            const auto& source=Top();
            const auto value=std::holds_alternative<Reference>(source)?Read(Ref(source)):source;
            const auto bits=Number(value);
            Top()=i.opcode==116?bits&0xffu:bits&0xffffu;
            break;
        }
        case 90: {
            const auto& text=module->strings.at(u&65535);Push(std::make_shared<std::string>(text));Push(static_cast<std::uint32_t>(text.size()));break;
        }
        case 95:case 96: {const auto value=Number(Top());const auto left=std::bit_cast<std::int32_t>(value);Top()=static_cast<std::uint32_t>(i.opcode==95?(left<s?-1:left>s?1:0):(value<u?-1:value>u?1:0));break;}
        case 97:
            if(s>=0 || s==(-2147483647-1))throw std::runtime_error("invalid legacy system function ID");
            if(Call(-s)){state=ScriptState::suspended;pause=ScriptPause::service;}else StepBoundary();
            break;
        case 99:if(s<0)Push(static_cast<std::uint32_t>(globals.at(static_cast<std::size_t>(-std::int64_t(s)-1))));else Push(module_globals.at(static_cast<std::size_t>(u&65535)));break;
        case 100:if(s<0)globals.at(static_cast<std::size_t>(-std::int64_t(s)-1))=std::bit_cast<std::int32_t>(Number(Pop()));else module_globals.at(static_cast<std::size_t>(u&65535))=Pop();break;
        case 101:Top()=static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(Number(Top()))+s);break;
        case 102:Top()=static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(Number(Top()))-s);break;
        case 103: {const auto l=std::bit_cast<float>(Number(Top())),r=std::bit_cast<float>(u);Top()=static_cast<std::uint32_t>(l<r?-1:l>r?1:0);break;}
        case 104:Top()=std::bit_cast<std::uint32_t>(std::bit_cast<float>(Number(Top()))+std::bit_cast<float>(u));break;
        case 105:Top()=std::bit_cast<std::uint32_t>(std::bit_cast<float>(Number(Top()))-std::bit_cast<float>(u));break;
        case 106:Top()=static_cast<std::uint32_t>(std::bit_cast<std::int32_t>(Number(Top()))*s);break;
        case 107:Top()=std::bit_cast<std::uint32_t>(std::bit_cast<float>(Number(Top()))*std::bit_cast<float>(u));break;
        case 108:
            // ExecuteStep(0) consumes the cue before yielding. The original
            // executor decides whether to repeat from mode at frame entry.
            StepBoundary();
            break;
        case 110: {if(module->used_types.at(u)!="string")throw std::runtime_error("unsupported legacy object release");Write(Ref(Pop()),std::uint32_t{});break;}
        case 112:Local(local)=object;object.reset();break;
        case 115: {const auto& cell=Top();if((std::holds_alternative<std::uint32_t>(cell) && Number(cell)==0) || (std::holds_alternative<Text>(cell) && !std::get<Text>(cell)))throw std::runtime_error("legacy null reference");break;}
        case 118: {const auto offset=Number(Top(u&65535));Top(u&65535)=Local(static_cast<std::int32_t>(offset));break;}
        default:throw std::runtime_error("legacy opcode not yet supported: "+std::to_string(i.opcode));
        }
    }
};
CsbExecution::CsbExecution(std::shared_ptr<const CsbModule> module,std::string_view entry,ScriptGlobals& globals,IScriptServices& services,CsbTextDecoder decoder) {
    if(!module)throw std::invalid_argument("legacy module is required");
    impl_=std::make_unique<Impl>(std::move(module),globals,services,std::move(decoder));auto& self=*impl_;
    self.function=self.module->Find(entry);
    if(!self.function || !self.function->parameters.empty() || self.function->result.token!=69)
        throw std::invalid_argument("legacy void entry without parameters required");
    if(!self.module->structures.empty() || !self.module->imports.empty())
        throw std::runtime_error("legacy module types/globals/imports require initialization support");
    if(!self.module->initialize.instructions.empty()){
        const auto* saved_fn=self.function;
        self.function=&self.module->initialize;self.pc=0;
        while(self.pc<self.function->instructions.size() && self.state==ScriptState::prepared){
            const auto& ins=self.function->instructions[self.pc++];
            if(ins.opcode==13)break;
            self.Instruction(ins);
        }
        self.function=saved_fn;self.pc=0;self.state=ScriptState::prepared;self.pause=ScriptPause::none;
    }
}
CsbExecution::~CsbExecution()=default;
ScriptState CsbExecution::Execute(std::uint32_t budget,ScriptRunMode mode) {
    auto& self=*impl_;if(self.executing || !budget || (self.state!=ScriptState::prepared && self.state!=ScriptState::suspended))throw std::logic_error("legacy execution requires inactive prepared/suspended context and budget");
    struct Guard {bool& running;explicit Guard(bool& flag):running(flag){running=true;}~Guard(){running=false;}} guard(self.executing);
    self.state=ScriptState::prepared;self.pause=ScriptPause::none;
    self.original_steps=mode==ScriptRunMode::original_steps;self.repeat_steps=self.services.ImmediateMode()==1;
    try {
        while(budget-- && self.state==ScriptState::prepared) {
            if(self.pc>=self.function->instructions.size())throw std::runtime_error("legacy execution fell outside function");
            const auto& instruction=self.function->instructions[self.pc++];self.Instruction(instruction);
        }
        if(self.state==ScriptState::prepared){self.state=ScriptState::suspended;self.pause=ScriptPause::budget;}
    }catch(const std::exception& error){self.state=ScriptState::failed;self.error=self.function->name+" at instruction "+std::to_string(self.pc?self.pc-1:0)+": "+error.what();self.CleanupFailedState();}
    return self.state;
}
ScriptState CsbExecution::State() const{return impl_->state;}
ScriptPause CsbExecution::PauseReason() const{return impl_->pause;}
const std::string& CsbExecution::Error() const{return impl_->error;}
} // namespace pal4::portable
