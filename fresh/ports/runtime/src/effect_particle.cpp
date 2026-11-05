#include "pal4/portable/effect_particle.h"
#include "pal4/portable/effect_transform.h"
#include "pal4/portable/world.h"
#include "pal4/particle_visual_object.h"
#include "pal4/rtk_prt_std_atomic_configuration.h"
#include "pal4/rtk_prt_std_atomic_control.h"
#include "pal4/rtk_prt_std_atomic_properties.h"
#include "pal4/rtk_prt_std_atomic_task_facets.h"
#include "pal4/xml_float_to_int.h"
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <stdexcept>
namespace pal4::portable {
namespace {
using namespace pal4;
void Require(bool v,const char* message){if(!v)throw std::logic_error(message);}
[[noreturn]] void Unsupported(){throw std::logic_error("particle collection/skin service is not bound");}
}
struct EffectParticle::Impl final:IRtkPrtStdAtomicConfigurationRuntime,IRtkPrtStdAtomicObjectRuntime,
    IParticleVisualRuntime,IParticleTankRuntime,IParticleFrameTransformRuntime,IParticleVisualLifecycleRuntime,
    IParticleCollectionRuntime,IParticleCollectionStateRuntime,IParticleEmissionRuntime,
    IParticleSimulationRuntime,IParticleTankUpdateRuntime,IRtkPrtStdAtomicUpdateRuntime {
    static constexpr unsigned resource=0x1000,tank=0x2000,plugin=0x3000,geometry=0x4000,
        materials=0x5000,material=0x6000,frame=0x7000,matrix_id=0x7100,system=0x8000,heap_start=0x10000000;
    BinaryXmlNode document;EffectTextures& textures;ParticleRandom& random;ParticleCounts& counts;
    RtkPrtStdAtomicObjectState object{};ParticleVisualState state{};
    std::shared_ptr<const EffectTexture> texture;bool allocated{},constructed{},counted{},tank_live{},frame_live{};
    unsigned next=heap_start,flags{},active_count{},src{},dst{},alpha{},held_texture{},locked{};
    std::size_t heap_bytes{};std::map<unsigned,std::vector<std::byte>> heap;
    std::map<unsigned,unsigned> streams;std::array<float,3> rotation{},scale{1,1,1},position{};
    std::array<float,16> matrix=InstanceMatrix({0,0,0},0);double seconds{};unsigned updates{};
    EffectMatrix parent=IdentityEffectMatrix();
    Impl(const BinaryXmlNode& d,EffectTextures& t,ParticleRandom& r,ParticleCounts& c):document(d),textures(t),random(r),counts(c){
        Require(document.name=="RtkPrtStdAtomicObject","wrong particle class");
        (void)RtkPrtStdAtomicObject::Construct(object,*this);counted=true;++counts.created;
    }
    ~Impl() override{
        if(constructed)(void)ParticleVisualObject::Destruct(state,*this);
        if(counted)RtkPrtStdAtomicObject::Destroy(object,*this);
    }
    void Load(){object.words[4]=system;Require(RtkPrtStdAtomicConfiguration::Load(object,1,*this),"original particle configuration failed");}
    struct Facets final:IRtkPrtStdAtomicPropertiesRuntime,IRtkPrtStdAtomicTaskFacetRuntime {
        Impl& owner;explicit Facets(Impl& o):owner(o){}
        unsigned ReadWord(unsigned a) override{return owner.ReadWord(a);}
        float ReadFloat(unsigned a) override{return owner.ReadFloat(a);}
        std::uint8_t ReadByte(unsigned a) override{return owner.ReadByte(a);}
        void WriteWord(unsigned a,unsigned v) override{owner.WriteWord(a,v);}
        void WriteFloat(unsigned a,float v) override{owner.WriteFloat(a,v);}
        void WriteByte(unsigned a,std::uint8_t v) override{owner.WriteByte(a,v);}
        std::uint8_t UpdateFrame(unsigned r) override{owner.UpdateFrame(r);return 1;}
        int FloatToInt(double v) override{if(!std::isfinite(v)||v<-2147483648.0||v>=2147483648.0)return std::numeric_limits<int>::min();return static_cast<int>(v);}
    } facets{*this};
    struct Deactivation final:IParticleVisualUpdateRuntime {
        Impl& owner;explicit Deactivation(Impl& o):owner(o){}
        IParticleSkinRuntime& SkinRuntime() override{Unsupported();}
        IParticleTankUpdateRuntime& TankUpdateRuntime() override{Unsupported();}
        IParticleVisualRenderRuntime& VisualRenderRuntime() override{Unsupported();}
        IParticleEmissionRuntime& EmissionRuntime() override{Unsupported();}
        IParticleRenderStateRuntime& RenderStateRuntime() override{Unsupported();}
        void PrepareRender(double) override{Unsupported();}
        void RenderCollectionElement(unsigned,unsigned,ParticleVisualState&) override{Unsupported();}
        unsigned ReadExternalWord(unsigned a) override{return owner.ReadWord(a);}
        std::uint8_t ReadExternalByte(unsigned a) override{return owner.ReadByte(a);}
        void WriteExternalWord(unsigned a,unsigned v) override{owner.WriteWord(a,v);}
        void WriteExternalByte(unsigned a,std::uint8_t v) override{owner.WriteByte(a,v);}
    } deactivation{*this};
    struct ResetRuntime final:IRtkPrtStdAtomicResetRuntime {
        Impl& owner;explicit ResetRuntime(Impl& o):owner(o){}
        void ResetPlaybackFacet(unsigned f) override{Require(f==0x9014,"invalid particle reset facet");(void)RtkPrtStdAtomicControl::DisablePlayback(owner.object);}
        unsigned ReadWord(unsigned a) override{return owner.ReadWord(a);}
        void WriteWord(unsigned a,unsigned v) override{owner.WriteWord(a,v);}
        void WriteFloat(unsigned a,float v) override{owner.WriteFloat(a,v);}
        void UpdateFrame(unsigned r) override{owner.UpdateFrame(r);}
        void DeactivateResource(unsigned r) override{owner.CheckResource(r);(void)ParticleVisualObject::Deactivate(owner.state,owner.deactivation);}
    } reset_runtime{*this};
    unsigned SystemInstance() override{return system;}
    unsigned CurrentTime() override{return random.Time();}void SeedRandom(unsigned seed) override{random.Seed(seed);}
    int Random() override{const auto n=random.Next15();Require(n>=0 && n<=32767,"invalid particle random15 sample");return n;}
    unsigned AllocateResource(unsigned bytes) override{Require(!allocated && bytes==sizeof(state),"invalid particle resource allocation");allocated=true;return resource;}
    unsigned ConstructResource(unsigned r) override{CheckResource(r);(void)ParticleVisualObject::Construct(state,0,*this);constructed=true;return r;}
    void DestroyResource(unsigned r) override{CheckResource(r);if(constructed){(void)ParticleVisualObject::Destruct(state,*this);constructed=false;}}
    void FreeResource(unsigned r) override{CheckResource(r);allocated=false;}
    void CheckResource(unsigned r) const{Require(allocated && r==resource,"invalid particle resource owner");}
    const BinaryXmlAttribute* Attribute(unsigned d,std::string_view key){Require(d==1,"invalid particle XML handle");return document.FindAttribute(key);}
    int ReadInt(unsigned d,std::string_view key,int fallback) override{const auto* a=Attribute(d,key);if(!a)return fallback;if(const auto* f=std::get_if<float>(&a->value))return XmlFloatToInt(std::bit_cast<unsigned>(*f));Require(std::holds_alternative<std::int32_t>(a->value)||std::holds_alternative<std::int8_t>(a->value),"particle XML text-number conversion is not bound");return std::bit_cast<int>(a->ToInt(std::bit_cast<unsigned>(fallback)));}
    float ReadFloat(unsigned d,std::string_view key,float fallback) override{const auto* a=Attribute(d,key);if(!a)return fallback;Require(std::holds_alternative<float>(a->value)||std::holds_alternative<std::int32_t>(a->value)||std::holds_alternative<std::int8_t>(a->value),"particle XML text-number conversion is not bound");const auto v=a->ToFloat(fallback);Require(std::isfinite(v),"nonfinite particle configuration");return v;}
    std::string ReadString(unsigned d,std::string_view key,std::string_view fallback) override{const auto* a=Attribute(d,key);if(!a)return std::string(fallback);const auto* s=std::get_if<std::string>(&a->value);Require(s!=nullptr,"particle XML string conversion is not bound");return *s;}
    unsigned FindTexture(std::string_view path) override{Require(!held_texture,"particle texture reference already held");texture=textures.Load(path);held_texture=1;return 1;}
    unsigned LoadTexture(std::string_view path) override{return FindTexture(path);}
    void DestroyTexture(unsigned t) override{Require(t==1 && held_texture==1,"particle texture reference mismatch");held_texture=0;}
    void UpdateFrame(unsigned r) override{CheckResource(r);(void)ParticleVisualObject::UpdateFrameTransform(state,*this);}
    void SetProperties(unsigned r,bool mode) override{CheckResource(r);(void)ParticleVisualObject::SetProperties(state,mode);}
    bool InitializeResource(unsigned r,bool point,unsigned t,unsigned emitter,float x,float y,float z,bool axis) override{
        CheckResource(r);if(state.ReadWord(124)!=0)state.WriteWord(124,0);
        Require(state.ReadWord(200)>0 && state.ReadWord(200)<=4096,"particle capacity limit");
        Require(state.ReadWord(260)<=4096 && state.ReadWord(264)<=4096,"particle emission limit");
        float life = std::bit_cast<float>(state.ReadWord(276));
        float life_bias = std::bit_cast<float>(state.ReadWord(280));
        Require(std::isfinite(life) && std::isfinite(life_bias) && life >= 0.0f,"invalid particle lifetime range");
        if (life <= 0.0f) {
            life = 0.001f;
            state.WriteWord(276, std::bit_cast<unsigned>(life));
        }
        if (std::abs(life_bias) >= life) {
            life_bias = std::copysign(life * 0.999f, life_bias);
            state.WriteWord(280, std::bit_cast<unsigned>(life_bias));
        }
        return ParticleVisualObject::Initialize(state,point,t,emitter,std::bit_cast<unsigned>(x),std::bit_cast<unsigned>(y),std::bit_cast<unsigned>(z),axis,*this)!=0;
    }
    void SetCollectionState(unsigned r,bool mode) override{CheckResource(r);(void)ParticleVisualObject::SetCollectionEnabled(state,mode,*this);}
    void SetElementEnabled(unsigned,std::uint8_t) override{Unsupported();}
    // PTank/材质仅映射本适配器实际拥有的语义属性，不分配或仿造SDK对象布局。
    unsigned CreateTank(unsigned capacity,unsigned format,unsigned data) override{Require(!tank_live && capacity==state.ReadWord(200) && format==0x100000a7 && data==1,"invalid particle tank creation");tank_live=true;return tank;}
    unsigned PluginOffset() override{return 0x80;}
    unsigned CreateFrame() override{Require(!frame_live,"particle frame already exists");frame_live=true;return frame;}
    void SetAtomicFrame(unsigned t,unsigned f) override{Require(t==tank && f==frame && tank_live && frame_live,"invalid particle frame binding");}
    void SetMaterialTexture(unsigned m,unsigned t) override{Require(m==material && t==1 && texture,"invalid particle material texture");}
    void SetFrameIdentity(unsigned f) override{SetIdentity(f);}void UpdateFrameObjects(unsigned f) override{(void)UpdateObjects(f);}
    IParticleFrameTransformRuntime& FrameTransformRuntime() override{return *this;}
    void CheckFrame(unsigned f) const{Require(f==frame && frame_live,"invalid particle frame");}
    void SetIdentity(unsigned f) override{CheckFrame(f);scale={1,1,1};rotation={};position={};}
    void Scale(unsigned f,ParticleVisualPoint v,unsigned combine) override{CheckFrame(f);Require(combine==0,"unsupported particle scale composition");scale={v.x,v.y,v.z};}
    void Rotate(unsigned f,ParticleVisualPoint axis,float degrees,unsigned combine) override{CheckFrame(f);Require(combine==2,"unsupported particle rotation composition");if(axis.x==1 && axis.y==0 && axis.z==0)rotation[0]=degrees;else if(axis.x==0 && axis.y==1 && axis.z==0)rotation[1]=degrees;else {Require(axis.x==0 && axis.y==0 && axis.z==1,"invalid particle rotation axis");rotation[2]=degrees;}}
    void Translate(unsigned f,ParticleVisualPoint v,unsigned combine) override{CheckFrame(f);Require(combine==2,"unsupported particle translation composition");position={v.x,v.y,v.z};}
    std::uint8_t UpdateObjects(unsigned f) override{CheckFrame(f);auto local=InstanceMatrix(position,rotation);for(unsigned col=0;col<3;++col)for(unsigned row=0;row<3;++row)local[col*4+row]*=scale[col];matrix=MultiplyEffectMatrices(parent,local);return 1;}
    unsigned ObjectTransform(unsigned f) override{CheckFrame(f);return matrix_id;}
    ParticleVisualPoint TransformVector(unsigned m,ParticleVisualPoint p) override{Require(m==matrix_id,"invalid particle matrix");return {matrix[0]*p.x+matrix[4]*p.y+matrix[8]*p.z,matrix[1]*p.x+matrix[5]*p.y+matrix[9]*p.z,matrix[2]*p.x+matrix[6]*p.y+matrix[10]*p.z};}
    ParticleVisualPoint TransformPosition(unsigned m,ParticleVisualPoint p) override{auto v=TransformVector(m,p);v.x+=matrix[12];v.y+=matrix[13];v.z+=matrix[14];return v;}
    ParticleVisualPoint Normalize(ParticleVisualPoint p) override{const float length=std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z);if(length>0){p.x/=length;p.y/=length;p.z/=length;}return p;}
    unsigned Allocate(unsigned bytes) override{Require(bytes>0 && bytes<=65536 && heap_bytes+bytes<=4u*1024*1024 && next<0xf0000000,"particle memory budget");const auto a=next;next+=0x10000;heap.emplace(a,std::vector<std::byte>(bytes));heap_bytes+=bytes;return a;}
    void FreeBuffer(unsigned a) override{if(!a)return;const auto f=heap.find(a);Require(f!=heap.end(),"invalid particle buffer free");heap_bytes-=f->second.size();heap.erase(f);}
    int Free(unsigned a) override{FreeBuffer(a);return 0;}
    std::span<std::byte> Memory(unsigned a,std::size_t bytes){auto f=heap.upper_bound(a);Require(f!=heap.begin(),"unknown particle buffer");--f;const auto offset=a-f->first;Require(offset<=f->second.size() && bytes<=f->second.size()-offset,"particle buffer bounds");return std::span(f->second).subspan(offset,bytes);}
    unsigned ReadWord(unsigned a) override{
        if(a>=resource && a<=resource+sizeof(state)-4){Require((a-resource)%4==0,"unaligned particle resource field");return state.ReadWord(a-resource);}
        if(a==system+164)return counts.instances;
        if(a==system+48)return 0; // 此无骨骼PTank分支不消费相机参数。
        if(a==tank+0x80)return plugin;
        if(a==tank+24)return geometry;
        if(a==geometry+32)return materials;
        if(a==materials)return material;
        if(a==plugin+64)return flags;
        if(a==plugin+4)return active_count;
        if(a==frame+4){CheckFrame(frame);return 0;}
        auto bytes=Memory(a,4);unsigned v;std::memcpy(&v,bytes.data(),4);return v;
    }
    void WriteWord(unsigned a,unsigned v) override{
        if(a>=resource && a<=resource+sizeof(state)-4){Require((a-resource)%4==0,"unaligned particle resource field");state.WriteWord(a-resource,v);return;}
        if(a==system+164){counts.instances=v;return;}if(a==plugin+64){flags=v;return;}if(a==plugin+4){Require(v<=state.ReadWord(200),"particle stream capacity overflow");active_count=v;return;}
        if(a==plugin+164){src=v;return;}if(a==plugin+168){dst=v;return;}if(a==plugin+172){alpha=v;return;}
        auto bytes=Memory(a,4);std::memcpy(bytes.data(),&v,4);
    }
    std::uint8_t ReadByte(unsigned a) override{if(a>=resource && a<resource+sizeof(state))return state.ReadByte(a-resource);return std::to_integer<std::uint8_t>(Memory(a,1)[0]);}
    void WriteByte(unsigned a,std::uint8_t v) override{if(a>=resource && a<resource+sizeof(state)){state.WriteByte(a-resource,v);return;}Memory(a,1)[0]=std::byte(v);}
    float ReadFloat(unsigned a) override{return std::bit_cast<float>(ReadWord(a));}
    void WriteFloat(unsigned a,float v) override{WriteWord(a,std::bit_cast<unsigned>(v));}
    unsigned ReadExternalWord(unsigned a) override{return ReadWord(a);}
    void RemoveFrameChild(unsigned) override{throw std::logic_error("particle parent attachment is not bound");}
    int DestroyAtomic(unsigned a) override{Require(a==tank && tank_live,"invalid particle tank destruction");tank_live=false;for(const auto& [_,address]:streams)FreeBuffer(address);streams.clear();return 1;}
    int DestroyFrame(unsigned f) override{CheckFrame(f);frame_live=false;return 1;}
    void DeinitializeElement(unsigned) override{Unsupported();}
    IParticleTankRuntime& TankRuntime() override{return *this;}IParticleCollectionRuntime& CollectionRuntime() override{return *this;}
    void AddFrameChild(unsigned,unsigned) override{Unsupported();}
    void InitializeFromAsset(unsigned,unsigned,std::array<float,2>,bool,std::uint8_t,unsigned) override{Unsupported();}
    void InitializeFromObject(unsigned,std::array<unsigned,3>,unsigned,std::array<float,2>,bool,std::uint8_t,unsigned) override{Unsupported();}
    void AttachElementFrame(unsigned,unsigned) override{Unsupported();}
    ParticleTankLock Lock(unsigned t,unsigned stream,unsigned access) override{
        Require(t==tank && tank_live && access==0x40000000,"invalid particle stream lock");unsigned stride;
        switch(stream){case 1:stride=12;break;case 2:stride=4;break;case 4:stride=8;break;case 128:stride=16;break;case 32:stride=4;break;default:throw std::logic_error("unsupported particle stream");}
        Require((locked&stream)==0,"particle stream already locked");locked|=stream;
        if(!streams.contains(stream))streams.emplace(stream,Allocate(state.ReadWord(200)*stride));
        return {streams.at(stream),stride};
    }
    int Unlock(unsigned t) override{Require(t==tank && locked==0xa7,"incomplete particle stream upload");locked=0;return 1;}
    void Advance(double delta,bool skip){
        Require(allocated && state.ReadByte(0) && state.ReadWord(124)==0 && !state.ReadByte(55),"particle tank update prerequisites");
        if(!skip && state.ReadByte(1))(void)ParticleVisualObject::EmitTankParticles(state,delta,*this);
        (void)ParticleVisualObject::SimulateParticles(state,delta,*this);(void)ParticleVisualObject::RenderTank(state,*this);seconds+=delta;++updates;
    }
    std::uint8_t UpdateState(unsigned r,double delta,unsigned argument,std::uint8_t skip) override{CheckResource(r);Require(argument==0,"particle external update context is not bound");Advance(delta,skip!=0);return 1;}
    std::vector<ParticleVertexData> Snapshot(){
        Require(!locked,"particle snapshot during stream update");std::vector<ParticleVertexData> output;
        if(streams.empty())return output;
        for(unsigned i=0;i<active_count;++i){ParticleVertexData p;const auto pos=streams.at(1)+i*12,col=streams.at(2)+i*4,size=streams.at(4)+i*8,uv=streams.at(128)+i*16;
            for(unsigned c=0;c<3;++c)p.position[c]=ReadFloat(pos+c*4);
            for(unsigned c=0;c<4;++c){p.color[c]=ReadByte(col+c);p.uv[c]=ReadFloat(uv+c*4);}
            for(unsigned c=0;c<2;++c)p.size[c]=ReadFloat(size+c*4);
            p.rotation=ReadFloat(streams.at(32)+i*4);output.push_back(p);}
        return output;
    }
};
EffectParticle::EffectParticle(const BinaryXmlNode& d,EffectTextures& t,ParticleRandom& r,ParticleCounts& c):impl_(std::make_unique<Impl>(d,t,r,c)){impl_->Load();}
EffectParticle::~EffectParticle()=default;
void EffectParticle::Advance(double elapsed,bool skip){Require(std::isfinite(elapsed) && elapsed>=0,"invalid particle delta");const auto original=impl_->object.words[2];if(skip)impl_->object.words[2]|=0x01000000;try{(void)RtkPrtStdAtomicControl::Advance(impl_->object,elapsed,*impl_);}catch(...){impl_->object.words[2]=original;throw;}impl_->object.words[2]=original;}
void EffectParticle::SetEmission(bool enabled){impl_->state.WriteByte(1,enabled);}
void EffectParticle::Show(bool visible){if(visible)(void)RtkPrtStdAtomicControl::EnablePlayback(impl_->object);else (void)RtkPrtStdAtomicControl::DisablePlayback(impl_->object);}
bool EffectParticle::Visible() const{return (impl_->object.words[1]&0xff)!=0 && (impl_->object.words[1]>>24)!=0;}
void EffectParticle::Reset(){(void)RtkPrtStdAtomicControl::Reset(impl_->object,0x9000,impl_->reset_runtime);}
void EffectParticle::PreRender(){(void)RtkPrtStdAtomicObject::Update(impl_->object,*impl_);}
void EffectParticle::SetTimedMode(bool enabled){impl_->object.words[2]=(impl_->object.words[2]&0x00ffffff)|(unsigned(enabled)<<24);}
std::array<float,3> EffectParticle::Scale() const{std::array<unsigned,3> bits{};(void)RtkPrtStdAtomicProperties::GetScale(impl_->object,bits,impl_->facets);return std::bit_cast<std::array<float,3>>(bits);}
void EffectParticle::SetScale(std::array<float,3> scale,bool relative){for(float f:scale)Require(std::isfinite(f),"nonfinite particle scale");(void)RtkPrtStdAtomicProperties::SetScale(impl_->object,scale,relative,impl_->facets);}
std::array<std::uint8_t,4> EffectParticle::Color() const{std::array<std::uint8_t,4> color{};const std::array<std::uint8_t*,4> output{&color[0],&color[1],&color[2],&color[3]};(void)RtkPrtStdAtomicProperties::GetColor(impl_->object,output,impl_->facets);return color;}
void EffectParticle::SetColor(std::array<std::uint8_t,4> color){(void)RtkPrtStdAtomicProperties::SetColor(impl_->object,color,impl_->facets);}
void EffectParticle::SetColorMode(std::uint8_t mode){(void)RtkPrtStdAtomicProperties::SetResourceMode(impl_->object,mode,impl_->facets);}
void EffectParticle::StopEmission(){(void)RtkPrtStdAtomicTaskFacets::DisableResourceFlag(impl_->object,impl_->facets);}
void EffectParticle::SetEmissionTime(float seconds,bool increase){Require(std::isfinite(seconds) && seconds>0,"invalid particle emission interval");(void)RtkPrtStdAtomicTaskFacets::SetEmissionTime(impl_->object,seconds,increase,0,impl_->facets);}
std::vector<ParticleVertexData> EffectParticle::Snapshot() const{return impl_->Snapshot();}
const EffectTexture& EffectParticle::Texture() const{Require(bool(impl_->texture),"particle texture is missing");return *impl_->texture;}
unsigned EffectParticle::SourceBlend() const{return impl_->src;}
const std::array<float,16>& EffectParticle::Transform() const{return impl_->matrix;}
unsigned EffectParticle::DestinationBlend() const{return impl_->dst;}
unsigned EffectParticle::Capacity() const{return impl_->state.ReadWord(200);}
double EffectParticle::Duration() const{return RtkPrtStdAtomicObject::GetDuration(impl_->object,*impl_);}
void EffectParticle::SetParentTransform(const std::array<float,16>& parent){ValidateEffectMatrix(parent);impl_->parent=parent;(void)impl_->UpdateObjects(Impl::frame);}
void EffectParticle::WriteReport(std::ostream& out) const{
    const auto& s=*impl_;out<<std::setprecision(9)<<"{\"seconds\":"<<s.seconds<<",\"updates\":"<<s.updates<<",\"capacity\":"<<s.state.ReadWord(200)<<",\"slots\":"<<s.state.ReadWord(204)<<",\"live\":"<<s.state.ReadWord(208)<<",\"source_blend\":"<<s.src<<",\"destination_blend\":"<<s.dst<<",\"texture\":"<<std::quoted(s.texture->member)<<",\"rgba_crc32\":"<<s.texture->rgba_crc32<<",\"heap_bytes\":"<<s.heap_bytes<<",\"matrix\":[";for(unsigned i=0;i<16;++i){if(i)out<<',';out<<s.matrix[i];}out<<"],\"particles\":[";
    const auto snapshot=Snapshot();for(std::size_t i=0;i<snapshot.size();++i){if(i)out<<',';const auto& p=snapshot[i];out<<"{\"position\":["<<p.position[0]<<','<<p.position[1]<<','<<p.position[2]<<"],\"color\":["<<unsigned(p.color[0])<<','<<unsigned(p.color[1])<<','<<unsigned(p.color[2])<<','<<unsigned(p.color[3])<<"],\"size\":["<<p.size[0]<<','<<p.size[1]<<"],\"rotation\":"<<p.rotation<<'}';}out<<"],\"visible\":"<<(Visible()?"true":"false")<<",\"running\":"<<((s.object.words[1]>>16)&255)<<",\"timed\":"<<(s.object.words[2]>>24)<<",\"emitting\":"<<unsigned(s.state.ReadByte(1))<<",\"decreasing\":"<<unsigned(s.state.ReadByte(5))<<",\"emission_rate\":"<<s.state.ReadWord(8)<<",\"emission_limit\":"<<s.state.ReadWord(12)<<'}';
}
}
