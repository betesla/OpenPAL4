#include "pal4/portable/effect_playback.h"
#include "pal4/effect_handle.h"
#include "pal4/effect_system_active_frame.h"
#include "pal4/effect_system_group_update.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>
namespace pal4::portable {
namespace {
void Require(bool v,const char* message){if(!v)throw std::logic_error(message);}
constexpr unsigned active_head=0x20,pending_head=0x40,inline_head=0x60;
constexpr unsigned slot_base=0x100000,active_base=0x10000000,pending_base=0x20000000;
unsigned NewGroup(){static std::atomic<unsigned> next{0x1000};const auto value=next.fetch_add(0x100);Require(value<0xf0000,"effect group token limit");return value;}
}
struct EffectPlayback::Impl final:IEffectHandleContentRuntime,IEffectSystemActiveFrameRuntime {
    enum class Status {Acquired,Active,Pending,Cached};
    struct Slot {std::string name;bool clip{};std::unique_ptr<EffectDialog> dialog;Status status=Status::Acquired;unsigned xml_id{};bool has_id{},has_name{};};
    struct Active {unsigned effect{},repeats{};};
    PlanLoader load;EffectTextures& textures;ParticleRandom& random;ParticleCounts& particles;EffectObjectCounts& cylinders;
    const unsigned group=NewGroup();unsigned next_id{};bool scope{},creating_clip{};
    std::vector<Slot> slots;std::map<unsigned,Active> active;std::map<unsigned,float> pending;
    unsigned created{},started{},finished{},released{},returned{},reused{};
    explicit Impl(PlanLoader p,EffectTextures& t,ParticleRandom& r,ParticleCounts& pc,EffectObjectCounts& cc):load(std::move(p)),textures(t),random(r),particles(pc),cylinders(cc){Require(bool(load),"effect plan loader is missing");}
    static unsigned Effect(std::size_t index){return slot_base+static_cast<unsigned>(index)*0x1000;}
    std::size_t Index(unsigned e) const{Require(e>=slot_base && (e-slot_base)%0x1000==0 && (e-slot_base)/0x1000<slots.size(),"invalid effect Dialog token");const auto i=(e-slot_base)/0x1000;Require(bool(slots[i].dialog),"released effect Dialog token");return i;}
    Slot& Get(unsigned e){return slots.at(Index(e));}const Slot& Get(unsigned e) const{return slots.at(Index(e));}
    static unsigned ActiveNode(unsigned id){Require(id<0x100000,"effect ID token limit");return active_base+id*32;}
    unsigned ActiveId(unsigned n) const{Require(n>=active_base && (n-active_base)%32==0 && active.contains((n-active_base)/32),"invalid active effect record");return (n-active_base)/32;}
    unsigned PendingNode(unsigned e) const{return pending_base+static_cast<unsigned>(Index(e))*32;}
    unsigned PendingEffect(unsigned n) const{Require(n>=pending_base && (n-pending_base)%32==0,"invalid pending effect record");const auto e=Effect((n-pending_base)/32);Require(pending.contains(e),"missing pending effect record");return e;}
    void CheckGroup(unsigned g) const{Require(g==group,"effect group mismatch");}
    unsigned FirstActive() const{return active.empty()?active_head:ActiveNode(active.begin()->first);}
    unsigned FirstPending() const{return pending.empty()?pending_head:PendingNode(pending.begin()->first);}
    unsigned InitializeSystem() override{return group;}
    unsigned RegisterSystem() override{return group;}
    bool Validate(unsigned s,unsigned owner) override{return s==group && owner==group;}
    bool ContainsOwner(unsigned s,unsigned owner) override{return Validate(s,owner);}
    unsigned FindElement(unsigned tree,const EffectSystemHandleWords& h) override{Require(tree==group+12 && h.system==group,"invalid active effect lookup");const auto i=active.lower_bound(h.id);return i==active.end()?active_head:ActiveNode(i->first);}
    unsigned ReadWord(unsigned a) override{
        if(a==group+8)return next_id;
        if(a==group+16)return active_head;
        if(a==group+32)return pending_head;
        if(a==group+216)return inline_head;
        if(a==active_head)return FirstActive();
        if(a==pending_head)return FirstPending();
        if(a==inline_head)return inline_head;
        if(a>=active_base && a<pending_base){const auto node=a&~31u,id=ActiveId(node);const auto& record=active.at(id);switch(a-node){case 12:return id;case 16:return record.effect;case 20:return record.repeats;default:throw std::logic_error("unsupported active effect field");}}
        if(a>=pending_base){const auto node=a&~31u,e=PendingEffect(node);Require(a-node==12,"unsupported pending effect field");return e;}
        Require(a>=slot_base,"unsupported effect group field");const auto e=slot_base+((a-slot_base)/0x1000)*0x1000;const auto& slot=Get(e);
        switch(a-e){case 8:return slot.dialog->HasTasks()?1u:0u;case 12:return group;case 172:return e+0x100;default:throw std::logic_error("unsupported Dialog field");}
    }
    std::uint8_t ReadByte(unsigned a) override{if(a==group+4)return 1;Require(a>=slot_base,"unsupported effect byte");const auto e=slot_base+((a-slot_base)/0x1000)*0x1000;Require(a-e==80,"unsupported Dialog byte");return Get(e).dialog->Prepared();}
    void WriteWord(unsigned a,unsigned v) override{
        if(a==group+8){Require(v==next_id+1 && v<0x100000,"effect ID limit");next_id=v;return;}
        if(a>=active_base && a<pending_base){auto& record=active.at(ActiveId(a&~31u));if((a&31u)==20){record.repeats=v;return;}Require((a&31u)==16 && v==0,"unsupported active effect write");record.effect=0;return;}
        Require(a>=slot_base,"unsupported effect write");const auto e=slot_base+((a-slot_base)/0x1000)*0x1000;(void)Get(e);Require(a-e==12 && v==group,"invalid effect owner write");
    }
    std::string ReadNameAttribute(unsigned a) override{Require(a>=20,"invalid effect attributes");return Get(a-20).name;}
    void ResetDialogRuntimeState(unsigned e) override{Get(e).dialog->Reset();}
    void DispatchOwnerRelease(unsigned owner,EffectSystemHandleWords h,unsigned mode) override{CheckGroup(owner);Require(h.system==group && active.contains(h.id) && mode==0,"invalid effect release notification");++released;}
    void ReturnEffect(unsigned pool,unsigned e,unsigned mode) override{Require(pool==group+84,"invalid effect pool");ReturnEffect(e,mode);}
    void ReturnEffect(unsigned e,unsigned mode) override{
        auto& slot=Get(e);Require(mode<=1 && slot.status!=Status::Cached,"invalid effect return");
        if(mode){slot.dialog.reset();return;}
        slot.dialog->ReturnToCache();slot.status=Status::Cached;++returned;
    }
    unsigned EraseActiveNode(unsigned tree,unsigned node) override{Require(tree==group+12,"invalid active tree");const auto id=ActiveId(node);const auto next=active.erase(active.find(id));return next==active.end()?active_head:ActiveNode(next->first);}
    void EraseEntry(unsigned tree,unsigned node) override{(void)EraseActiveNode(tree,node);}
    void ProcessEffectList(unsigned owner) override{CheckGroup(owner);(void)EffectSystem::ProcessEffectList(group,static_cast<IEffectSystemActiveFrameRuntime&>(*this));}
    unsigned SharedNil(unsigned) override{throw std::logic_error("legacy effect tree storage is unavailable");}
    unsigned RemoveInactiveInline(unsigned,unsigned) override{throw std::logic_error("inline effect class is not bound");}
    unsigned NextActiveNode(unsigned tree,unsigned node) override{Require(tree==group+12,"invalid active traversal");const auto i=active.upper_bound(ActiveId(node));return i==active.end()?active_head:ActiveNode(i->first);}
    unsigned PendingNil() override{throw std::logic_error("legacy pending tree storage is unavailable");}
    unsigned Predecessor(unsigned) override{throw std::logic_error("legacy pending predecessor is unavailable");}
    void InsertPending(unsigned,unsigned,unsigned,unsigned,float) override{throw std::logic_error("legacy pending insertion is unavailable");}
    void QueuePendingCompletion(unsigned owner,unsigned e,float time) override{CheckGroup(owner);Require(std::isfinite(time)&&time>=0,"invalid effect completion duration");auto& slot=Get(e);Require(slot.status==Status::Active && !pending.contains(e),"effect already pending");pending.emplace(e,time);slot.status=Status::Pending;}
    unsigned FrameEffect(unsigned f) const{Require(f>=0x100,"invalid effect frame");const auto e=f-0x100;(void)Get(e);return e;}
    std::array<float,16> FrameTransform(unsigned f) override{return Get(FrameEffect(f)).dialog->WorldTransform();}
    float StopDialogPlayback(unsigned e,bool detach) override{return static_cast<float>(Get(e).dialog->Stop(detach));}
    void SetFrameIdentity(unsigned f) override{Get(FrameEffect(f)).dialog->RestoreDetachedTransform(IdentityEffectMatrix());}
    void ApplyFrameTransform(unsigned f,const std::array<float,16>& m,unsigned mode) override{Require(mode==0,"unsupported effect frame composition");Get(FrameEffect(f)).dialog->RestoreDetachedTransform(m);}
    void UpdateFrameObjects(unsigned f) override{(void)FrameEffect(f);} // typed帧写入时已向子对象传播。
    void UpdatePlayback(unsigned e,double dt) override{Get(e).dialog->AdvanceTasks(static_cast<float>(dt));}
    void StartPlayback(unsigned e) override{Require(Get(e).dialog->Prepare(),"effect playback restart failed");}
    void PausePlayback(unsigned e) override{Require(Get(e).dialog->BeginWaiting(),"effect waiting restart failed");}
    void ResetDialog(unsigned e) override{Get(e).dialog->Reset();}
    EffectSystemHandle CopyHandle(unsigned id,unsigned owner) override{CheckGroup(owner);EffectSystemHandleWords words;(void)EffectHandle::CopyData(words,{id,owner},owner);return {words.id,words.system};}
    void ResetTemporaryHandle(EffectSystemHandle& h) override{EffectSystemHandleWords words{h.id,h.system};EffectHandle::Reset(words);h={words.id,words.system};}
    void NotifyStarted(unsigned owner,const EffectSystemHandle& h,std::int32_t) override{CheckGroup(owner);Require(h.system==group && active.contains(h.id),"invalid effect start notification");++started;}
    void NotifyFinished(unsigned owner,const EffectSystemHandle& h,std::int32_t) override{CheckGroup(owner);Require(h.system==group && active.contains(h.id),"invalid effect finish notification");++finished;}
    unsigned FindXmlAttribute(unsigned xml,std::string_view key) override{const auto e=xml-20;const auto& slot=Get(e);if(key=="ID")return slot.has_id?e+0x200:0;if(key=="NAME")return slot.has_name?e+0x204:0;throw std::logic_error("unsupported effect runtime attribute");}
    void SetXmlInt(unsigned attr,unsigned id) override{auto& slot=Get(attr-0x200);slot.xml_id=id;slot.has_id=true;}
    void AddXmlInt(unsigned xml,std::string_view key,unsigned id) override{Require(key=="ID","invalid effect ID attribute");auto& slot=Get(xml-20);slot.xml_id=id;slot.has_id=true;}
    struct Secondary final:IEffectSystemSecondaryTimerRuntime {
        Impl& s;explicit Secondary(Impl& owner):s(owner){}
        std::uint8_t ReadByte(unsigned a) override{return s.ReadByte(a);}
        unsigned ReadWord(unsigned a) override{return s.ReadWord(a);}
        float ReadFloat(unsigned a) override{Require((a&31u)==16,"invalid pending duration field");return s.pending.at(s.PendingEffect(a-16));}
        void WriteFloat(unsigned a,float v) override{Require((a&31u)==16 && std::isfinite(v),"invalid pending duration write");s.pending.at(s.PendingEffect(a-16))=v;}
        unsigned SharedNil(unsigned) override{throw std::logic_error("legacy pending traversal is unavailable");}
        unsigned NextPendingNode(unsigned tree,unsigned n) override{Require(tree==s.group+28,"invalid pending traversal");const auto i=s.pending.upper_bound(s.PendingEffect(n));return i==s.pending.end()?pending_head:s.PendingNode(i->first);}
        void ResetDialog(unsigned e) override{s.ResetDialog(e);}
        void ReturnEffect(unsigned e,unsigned mode) override{s.ReturnEffect(e,mode);}
        unsigned EraseNode(unsigned tree,unsigned n) override{Require(tree==s.group+28,"invalid pending erase");const auto e=s.PendingEffect(n);const auto next=s.pending.erase(s.pending.find(e));return next==s.pending.end()?pending_head:s.PendingNode(next->first);}
    };
    struct Creation final:IEffectSystemCreateRuntime {
        Impl& s;unsigned acquired{};std::optional<unsigned> inserted;explicit Creation(Impl& owner):s(owner){}
        ~Creation(){if(acquired){if(inserted)s.active.erase(*inserted);auto& slot=s.Get(acquired);slot.dialog->Reset();slot.dialog.reset();}}
        std::uint8_t ReadByte(unsigned a) override{return s.ReadByte(a);}
        unsigned ReadWord(unsigned a) override{return s.ReadWord(a);}
        void WriteWord(unsigned a,unsigned v) override{s.WriteWord(a,v);}
        void WriteByte(unsigned a,std::uint8_t v) override{const auto e=a-83;const auto& slot=s.Get(e);Require((v!=0)==slot.clip,"effect distance clipping mismatch");}
        void BeginScriptScope() override{Require(!s.scope,"nested effect creation scope");s.scope=true;}
        void EndScriptScope() override{s.scope=false;}
        bool UppercaseNames() override{return true;}
        unsigned AcquireEffect(std::string_view name) override{
            for(std::size_t i=0;i<s.slots.size();++i){auto& slot=s.slots[i];if(slot.dialog && slot.status==Status::Cached && slot.name==name && slot.clip==s.creating_clip){acquired=Effect(i);slot.status=Status::Acquired;slot.has_name=slot.has_id=false;slot.dialog->SetTransform({0,1,1},{0,0,0},{1,1,1});++s.reused;return acquired;}}
            Require(s.slots.size()<1024,"effect instance capacity limit");auto plan=s.load(name);plan.distance_clip=s.creating_clip;
            Slot slot;slot.name=name;slot.clip=s.creating_clip;slot.dialog=std::make_unique<EffectDialog>(std::move(plan),s.textures,s.random,s.particles,s.cylinders);slot.dialog->SetTransform({0,1,1},{0,0,0},{1,1,1});
            s.slots.push_back(std::move(slot));acquired=Effect(s.slots.size()-1);return acquired;
        }
        void ReturnEffect(unsigned e,std::uint8_t mode) override{s.Get(e).dialog->Reset();s.ReturnEffect(e,unsigned(mode));if(e==acquired)acquired=0;}
        unsigned FindXmlAttribute(unsigned x,std::string_view k) override{return s.FindXmlAttribute(x,k);}
        void SetXmlString(unsigned a,std::string_view v) override{auto& slot=s.Get(a-0x204);Require(slot.name==v,"effect name mismatch");slot.has_name=true;}
        void AddXmlString(unsigned x,std::string_view k,std::string_view v) override{Require(k=="NAME","unsupported effect name attribute");auto& slot=s.Get(x-20);Require(slot.name==v,"effect name mismatch");slot.has_name=true;}
        void SetXmlInt(unsigned a,unsigned v) override{s.SetXmlInt(a,v);}
        void AddXmlInt(unsigned x,std::string_view k,unsigned v) override{s.AddXmlInt(x,k,v);}
        void SetPosition(unsigned e,unsigned p,std::uint8_t mode) override{Require(p==1 && mode==0,"unsupported creation position");s.Get(e).dialog->SetTransform({0,1,1},{0,0,0},{1,1,1});}
        bool StartPlayback(unsigned e) override{return s.Get(e).dialog->Prepare();}
        bool PausePlayback(unsigned e) override{return s.Get(e).dialog->BeginWaiting();}
        void InsertEffect(unsigned tree,unsigned id,unsigned e,unsigned n) override{Require(tree==s.group+12 && id<0x100000 && !s.active.contains(id),"duplicate effect ID");s.active.emplace(id,Active{e,n});inserted=id;s.Get(e).status=Status::Active;}
        EffectSystemHandle CopyHandle(unsigned id,unsigned owner) override{return s.CopyHandle(id,owner);}
        void ResetTemporaryHandle(EffectSystemHandle& h) override{s.ResetTemporaryHandle(h);}
        void NotifyCreated(unsigned owner,const EffectSystemHandle& h,unsigned p,unsigned) override{s.CheckGroup(owner);Require(h.system==owner && s.active.contains(h.id) && p==1,"invalid effect creation notification");++s.created;}
    };
    Handle Create(std::string_view name,std::int32_t repeats,bool clip){
        Require(!name.empty() && name.size()<=128 && name.find_first_of("/\\\0",0,3)==std::string_view::npos,"invalid effect resource name");Require(repeats>=-1,"unsupported effect repeat count");Require(!scope,"nested effect request");creating_clip=clip;Creation request(*this);
        const auto handle=EffectSystem::CreateEffectObjectWithoutPause(group,name,1,std::bit_cast<unsigned>(repeats),clip?1:0,request);request.acquired=0;return {handle.id,handle.system};
    }
};
EffectPlayback::EffectPlayback(PlanLoader p,EffectTextures& t,ParticleRandom& r,ParticleCounts& pc,EffectObjectCounts& cc):impl_(std::make_unique<Impl>(std::move(p),t,r,pc,cc)){}
EffectPlayback::~EffectPlayback()=default;
EffectPlayback::Handle EffectPlayback::Create(std::string_view n,std::int32_t repeats,bool clip){return impl_->Create(n,repeats,clip);}
std::string EffectPlayback::SupportIssue(std::string_view name) const{
    if(name.empty() || name.size()>128 || name.find_first_of("/\\\0",0,3)!=std::string_view::npos)return "invalid effect resource name";
    std::string canonical(name);for(auto& c:canonical)if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
    try {
        const auto plan=impl_->load(canonical);
        if(plan.camera_start!=1.f)return "camera probability";
        for(const auto& id:plan.object_ids){
            const auto& node=plan.templates.at(id);const auto* attr=node.FindAttribute("Attr_Class_Name");
            const auto* type=attr?std::get_if<std::string>(&attr->value):nullptr;
            if(!type)throw std::logic_error("effect template class is missing");
            if(*type=="RtkPrtStdAtomicObject"){
                const auto* bb=node.FindAttribute("Attr_Prt_Is_Billboard");
                if(bb && std::bit_cast<std::int32_t>(bb->ToInt(0))>0)return "particle collection path";
                continue;
            }
            if(*type=="CylinderObject")continue;
            const auto* dummy=node.FindAttribute("Attr_Dumy");
            if(*type=="ClumpObject" && dummy && std::bit_cast<std::int32_t>(dummy->ToInt(0))>0)continue;
            return "object class: "+*type;
        }
        for(std::size_t i=0;i<plan.task_ids.size();++i){
            const auto& target=plan.templates.at(plan.object_ids.at(plan.task_ids[i]));
            const auto* type=target.FindAttribute("Attr_Class_Name");const auto* dummy=target.FindAttribute("Attr_Dumy");
            if(type && std::get<std::string>(type->value)=="ClumpObject" && dummy && std::bit_cast<std::int32_t>(dummy->ToInt(0))>0)continue;
            const auto* attr=plan.task_parameters[i].FindAttribute("Attr_Class_Name");
            const auto* task=attr?std::get_if<std::string>(&attr->value):nullptr;
            if(!task)throw std::logic_error("effect task class is missing");
            if(*task!="ShowobjTask" && *task!="HideobjTask" && *task!="ScaleTask" && *task!="ChangeColorTask" && *task!="DecressTask")return "task class: "+*task;
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
bool EffectPlayback::Contains(Handle h) const{return EffectHandle::DialogNeedsUpdate(h,*impl_);}
const EffectDialog* EffectPlayback::Dialog(Handle h) const{if(!Contains(h))return nullptr;return impl_->Get(impl_->active.at(h.id).effect).dialog.get();}
void EffectPlayback::Attach(Handle h,const EffectMatrix& m){Require(Contains(h),"stale effect handle");impl_->Get(impl_->active.at(h.id).effect).dialog->SetParentTransform(m);}
void EffectPlayback::SetTransform(Handle h,Point3 p,Point3 r,Point3 s){Require(Contains(h),"stale effect handle");impl_->Get(impl_->active.at(h.id).effect).dialog->SetTransform(p,r,s);}
bool EffectPlayback::BeginWaiting(Handle h){Require(Contains(h),"stale effect handle");return impl_->Get(impl_->active.at(h.id).effect).dialog->BeginWaiting();}
void EffectPlayback::Stop(Handle& h){EffectHandle::UpdateContent(h,*impl_);}
void EffectPlayback::Release(Handle& h,bool force){EffectHandle::Release(h,force,*impl_);}
void EffectPlayback::Advance(float dt){Require(std::isfinite(dt)&&dt>=0,"invalid effect playback delta");auto& s=*impl_;EffectSystem::UpdateActiveEffects(s.group,dt,s);Impl::Secondary secondary(s);EffectSystem::UpdateSecondaryTimers(s.group,dt,secondary);for(auto& slot:s.slots)if(slot.dialog && slot.status!=Impl::Status::Cached)slot.dialog->AdvanceObjects(dt);}
void EffectPlayback::Clear(){auto& s=*impl_;while(!s.active.empty()){Handle h{s.active.begin()->first,s.group};EffectHandle::Release(h,true,s);}for(const auto& [e,time]:s.pending){(void)time;s.Get(e).dialog->Reset();}s.pending.clear();s.slots.clear();}
void EffectPlayback::Visit(const std::function<void(unsigned,const EffectDialog&)>& visitor) const{Require(bool(visitor),"missing effect visitor");for(std::size_t i=0;i<impl_->slots.size();++i){const auto& slot=impl_->slots[i];if(slot.dialog && slot.status!=Impl::Status::Cached)visitor(Impl::Effect(i),*slot.dialog);}}
std::size_t EffectPlayback::ActiveCount() const{return impl_->active.size();}
std::size_t EffectPlayback::PendingCount() const{return impl_->pending.size();}
std::size_t EffectPlayback::CachedCount() const{std::size_t count=0;for(const auto& slot:impl_->slots)if(slot.dialog && slot.status==Impl::Status::Cached)++count;return count;}
void EffectPlayback::WriteReport(std::ostream& out) const{const auto& s=*impl_;out<<std::setprecision(9)<<"{\"active\":"<<ActiveCount()<<",\"pending\":"<<PendingCount()<<",\"cached\":"<<CachedCount()<<",\"created\":"<<s.created<<",\"started\":"<<s.started<<",\"finished\":"<<s.finished<<",\"released\":"<<s.released<<",\"returned\":"<<s.returned<<",\"reused\":"<<s.reused<<",\"pending_seconds\":[";bool first=true;for(const auto& [e,t]:s.pending){(void)e;if(!first)out<<',';first=false;out<<t;}out<<"],\"dialogs\":[";first=true;Visit([&](unsigned e,const EffectDialog& d){if(!first)out<<',';first=false;out<<"{\"token\":"<<e<<",\"queued\":"<<d.QueuedObjects()<<",\"has_tasks\":"<<(d.HasTasks()?"true":"false")<<",\"waiting\":"<<(d.Waiting()?"true":"false")<<",\"sequence\":";d.Sequence().WriteReport(out);out<<'}';});out<<"]}";}
}
