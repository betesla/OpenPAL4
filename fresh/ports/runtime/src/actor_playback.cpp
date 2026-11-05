#include "pal4/portable/actor_playback.h"
#include "pal4/game_object_animation_switch.h"
#include "pal4/game_object_animation_event_loader.h"
#include "pal4/game_object_animation_event_dispatcher.h"
#include "pal4/animation_string_table_collection.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <type_traits>
namespace pal4::portable {
namespace {
[[noreturn]] void Missing(){throw std::logic_error("animation blend/next-action endpoint is not bound");}
void Check(bool value,const char* error){if(!value)throw std::logic_error(error);}
bool Same(std::string_view a,std::string_view b) {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i) {
        const auto fold=[](char c){return c>='A' && c<='Z'?static_cast<char>(c+'a'-'A'):c;};
        if(fold(a[i])!=fold(b[i]))return false;
    }return true;
}
std::uint32_t Word(const pal4::AnimationEventData& bytes,unsigned offset) {std::uint32_t value;std::memcpy(&value,bytes.data()+offset,4);return value;}
std::string_view Text(const pal4::AnimationEventData& bytes,unsigned offset) {
    const auto* begin=reinterpret_cast<const char*>(bytes.data()+offset);
    const auto* end=std::find(begin,begin+64,'\0');Check(end!=begin+64,"unterminated animation message");return {begin,static_cast<std::size_t>(end-begin)};
}
}
struct ActorPlayback::Impl final:pal4::IGameObjectInterpolatorRuntime,pal4::IGameObjectAnimationSwitchRuntime,
    pal4::IGameObjectAnimationEventDispatchRuntime,pal4::IGameObjectAnimationEventRuntime {
    struct Messages {bool loaded{};std::string member;std::size_t source_bytes{};std::vector<std::uint8_t> source;std::vector<pal4::AnimationEventData> events;std::vector<pal4::AnimationStringTableData> tables;};
    const Actor& actor;std::string model;ActorMemberReader read;ActorMessageCallback message_callback;bool retain_source{};ActorClipLoader clip_loader;std::vector<std::shared_ptr<const AnimationClip>> loaded_clips;unsigned fallback_calls{};
    pal4::GameObjectEmbeddedAnimationState state;
    std::span<std::uint32_t> bound_image;std::array<std::uint32_t,31> published{},previous_image{};
    std::uint64_t binding{},next_binding{1};unsigned operations{};std::uint32_t bound_clump{};
    bool ImageIntact() const{return bound_image.empty() || std::equal(bound_image.begin(),bound_image.end(),published.begin());}
    void ReleaseInactiveMessages() noexcept {
        const auto active=state.words[3];
        for(std::size_t i=0;i<messages.size();++i){
            const auto token=records+static_cast<unsigned>(i)*0x44u;
            const bool retained=token==active || (clip>=0 && i==static_cast<std::size_t>(clip)) ||
                (state.words[15] && (blend_clips[0]==static_cast<int>(i) || blend_clips[1]==static_cast<int>(i)));
            if(!retained){messages[i]=Messages{};loaded_clips[i].reset();}
        }
    }
    void PublishImage() noexcept {
        if(bound_image.empty())return;
        for(unsigned i=1;i<state.words.size();++i)if(i!=20)bound_image[i]=state.words[i];
        bound_image[1]=state.words[1]?bound_clump:0;
        std::copy(bound_image.begin(),bound_image.end(),published.begin());
    }
    template<class F>decltype(auto) Run(F operation){
        Check(ImageIntact(),"owned animation image was modified externally");
        ++operations;struct Guard{Impl& owner;~Guard(){if(!--owner.operations)owner.ReleaseInactiveMessages();}} guard{*this};
        try {
            if constexpr(std::is_void_v<decltype(operation())>){operation();Check(ImageIntact(),"animation callback overwrote the owned image");PublishImage();return;}
            else{auto result=operation();Check(ImageIntact(),"animation callback overwrote the owned image");PublishImage();return result;}
        }catch(...){if(ImageIntact())PublishImage();throw;}
    }
    std::vector<Messages> messages;
    std::optional<std::vector<std::uint8_t>> stream;std::size_t cursor{},loading{};
    int clip{-1};std::array<int,2> blend_clips{-1,-1};float blend_weight{};bool blend_pose{};unsigned blend_frames{};float time{};unsigned frames{},holds{};bool pose_dirty{true};std::vector<Vertex> pose;std::vector<Matrix> pose_frames;
    static constexpr unsigned records=0x10000,events=0x20000,hold=0x50000,fallback=0x50001,slot=0x60000;
    Impl(const Actor& a,std::string m,ActorMemberReader r,ActorMessageCallback callback,bool retain,ActorClipLoader loader):actor(a),model(std::move(m)),read(std::move(r)),message_callback(std::move(callback)),retain_source(retain),clip_loader(std::move(loader)),loaded_clips(a.clips.size()),messages(a.clips.size()) {
        Check(!model.empty() && model.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos,"invalid actor message model");
        Check(bool(read) && !actor.clips.empty(),"actor playback requires clips and reader");
        state.words[1]=1;state.words[2]=records;state.words[4]=4;state.words[5]=5;state.words[12]=std::bit_cast<unsigned>(1.f);
        // IDA668E04 initializes the default callback to null. Only an owner
        // that actually supplies a fallback may register one (66A17D).
        state.words[11]=message_callback?fallback:0;
    }
    void LoadClip(std::size_t index){
        if(!clip_loader || loaded_clips.at(index))return;
        auto value=clip_loader(actor.clips.at(index).name);
        Check(value && value->name==actor.clips[index].name && std::isfinite(value->duration) && value->duration>0,"invalid loaded animation");
        for(const auto& track:value->tracks)Check(track.frame<actor.frames.size() && !track.keys.empty(),"invalid loaded animation track");
        loaded_clips[index]=std::move(value);
    }
    const AnimationClip& ClipData(std::size_t index) const {
        if(!clip_loader)return actor.clips.at(index);
        const auto& value=loaded_clips.at(index);Check(bool(value),"animation is not loaded");return *value;
    }
    std::size_t RecordIndex(unsigned record) const {
        Check(record>=records && (record-records)%0x44==0,"invalid animation record token");
        const auto i=(record-records)/0x44;Check(i<actor.clips.size(),"animation record outside table");return i;
    }
    pal4::AnimationEventData& Event(unsigned address) {
        Check(clip>=0 && address>=events && (address-events)%0x14c==0,"invalid animation event token");
        return messages.at(static_cast<std::size_t>(clip)).events.at((address-events)/0x14c);
    }
    void LoadMessages(std::size_t index) {
        auto& record=messages.at(index);if(record.loaded)return;loading=index;
        try {(void)pal4::GameObjectAnimationEventLoader::Load(1,*this);record.loaded=true;}
        catch(...){stream.reset();cursor=0;record=Messages{};throw;}
    }
    std::string_view RecordBasePath(unsigned record) override{Check(record==1,"invalid load record");return base;}
    std::string base;
    std::string_view RecordName(unsigned record) override{Check(record==1,"invalid load record");return actor.clips.at(loading).name;}
    unsigned OpenStream(unsigned mode,unsigned flags,std::string_view path) override {
        Check(mode==2 && flags==1,"invalid AMF stream mode");stream=read(path);cursor=0;
        auto& record=messages.at(loading);record.member=path;record.source_bytes=stream?stream->size():0;std::vector<std::uint8_t>().swap(record.source);if(retain_source && stream && stream->size()<=4096)record.source=*stream;return stream?1u:0u;
    }
    void ReadStream(unsigned handle,std::span<std::byte> output) override {
        Check(handle==1 && stream && cursor<=stream->size() && output.size()<=stream->size()-cursor,"truncated AMF stream");
        std::memcpy(output.data(),stream->data()+cursor,output.size());cursor+=output.size();
    }
    void CloseStream(unsigned handle,unsigned flags) override{Check(handle==1 && !flags && stream.has_value(),"invalid AMF close");stream.reset();cursor=0;}
    void ClearEvents(unsigned record) override{Check(record==1,"invalid AMF event owner");messages.at(loading).events.clear();}
    void InsertEvent(unsigned record,const pal4::AnimationEventData& event) override {
        Check(record==1 && messages.at(loading).events.size()<65536,"AMF event budget exceeded");
        Check(std::isfinite(std::bit_cast<float>(Word(event,192))),"non-finite AMF trigger");messages.at(loading).events.push_back(event);
    }
    bool LoadSidecar(unsigned record,std::string_view path) override {
        Check(record==1,"invalid AMD owner");const auto data=read(path);
        if(!data)return false;
        Check(pal4::AnimationStringTableCollection::LoadFromBytes(messages.at(loading).tables,*data),"invalid AMD tables");return true;
    }
    void InitializeAnimationData(pal4::GameObjectEmbeddedAnimationState&) override{Missing();}
    void CleanupAnimationData(pal4::GameObjectEmbeddedAnimationState&) override{Missing();}
    void CleanupAnimationDataRange(pal4::GameObjectEmbeddedAnimationState&) override{Missing();}
    unsigned AnimationArrayCount(unsigned) override{Missing();}
    void DestructAnimationArray(unsigned,unsigned,unsigned) override{Missing();}
    unsigned FreeMemory(unsigned) override{Missing();}
    void FreeWrappedMemory(unsigned) override{Missing();}
    unsigned FindHierarchy(unsigned clump) override{Check(clump==1,"invalid playback clump");return 2;}
    void ProcessAnimationData(pal4::GameObjectEmbeddedAnimationState&,unsigned,int) override{Missing();}
    unsigned HierarchyInterpolator(unsigned hierarchy) override{Check(hierarchy==2,"invalid playback hierarchy");return 3;}
    unsigned CurrentAnimation(unsigned interpolator) override{Check(interpolator==3,"invalid playback interpolator");return clip<0?0:static_cast<unsigned>(clip)+1;}
    float AnimationDuration(unsigned animation) override{Check(animation>0 && animation<=actor.clips.size(),"invalid playback animation");return ClipData(animation-1).duration;}
    unsigned RecordAnimationPointer(unsigned record) override{return RecordAnimation(record);}
    float InterpolatorTime(unsigned interpolator) override{Check(interpolator==3,"invalid playback time");return time;}
    void SetInterpolatorTime(unsigned interpolator,float value) override{SetCurrentTime(interpolator,value);}
    void AddInterpolatorTime(unsigned interpolator,float delta) override{SetCurrentTime(interpolator,time+delta);}
    void UpdateAnimationTime(pal4::GameObjectEmbeddedAnimationState& current,float value) override {
        Check(&current==&state,"invalid event playback state");(void)pal4::GameObjectAnimationEventDispatcher::UpdateTime(state,value,*this);
    }
    void CleanupCurrentRecord(unsigned record) override {
        if(!record)return;
        for(auto& event:messages.at(RecordIndex(record)).events)(void)pal4::GameObjectAnimationEventData::NormalizeTime(event);
    }
    unsigned NextActionName(unsigned) override{Missing();}
    unsigned EmptyActionName() override{Missing();}
    void DispatchNextAction(pal4::GameObjectEmbeddedAnimationState&,unsigned,float) override{Missing();}
    void BlendAnimations(unsigned target,unsigned from,unsigned to,float weight) override {
        Check(target==3 && from==4 && to==5 && blend_clips[0]>=0 && blend_clips[1]>=0,"invalid blend interpolators");
        blend_weight=weight;blend_pose=true;pose_dirty=true;++blend_frames;
    }
    unsigned AnimationFromInterpolator(unsigned source) override{Check(source==5 && blend_clips[1]>=0,"invalid blend source");return static_cast<unsigned>(blend_clips[1])+1;}
    void SetInterpolatorAnimation(unsigned target,unsigned animation) override{SetCurrentAnimation(target,animation);}
    void UpdateHierarchyMatrices(unsigned hierarchy) override{Check(hierarchy==2,"invalid matrix hierarchy");pose_dirty=true;}
    void SetHierarchyFlags(unsigned,unsigned) override{Missing();}
    void AttachHierarchy(unsigned) override{Missing();}
    void AttachSkinAtomics(unsigned,unsigned) override{Missing();}
    int AnimationCount(pal4::GameObjectEmbeddedAnimationState& current) override{Check(&current==&state,"invalid clip table");return static_cast<int>(actor.clips.size());}
    std::string_view ActionName(pal4::GameObjectEmbeddedAnimationState&,int index) override{return actor.clips.at(static_cast<std::size_t>(index)).name;}
    int CompareActionNames(std::string_view a,std::string_view b) override{return Same(a,b)?0:1;}
    unsigned RecordAnimation(unsigned record) override{return static_cast<unsigned>(RecordIndex(record))+1;}
    void SetCurrentAnimation(unsigned interpolator,unsigned animation) override {
        Check(interpolator>=3 && interpolator<=5 && animation>0 && animation<=actor.clips.size(),"invalid animation selection");
        if(interpolator==3){clip=static_cast<int>(animation)-1;blend_pose=false;pose_dirty=true;}
        else blend_clips[interpolator-4]=static_cast<int>(animation)-1;
    }
    void SetCurrentTime(unsigned interpolator,float value) override {
        Check(interpolator==3 && clip>=0 && std::isfinite(value) && value>=0 && value<=ClipData(clip).duration+.001f,"invalid playback time");time=value;pose_dirty=true;
    }
    void UpdateMatrices(unsigned hierarchy) override{UpdateHierarchyMatrices(hierarchy);}
    unsigned EventBegin(unsigned record) override{Check(record==state.words[3],"invalid active event record");return events;}
    unsigned EventEnd(unsigned record) override{Check(record==state.words[3] && clip>=0,"invalid event end");return events+static_cast<unsigned>(messages.at(clip).events.size())*0x14c;}
    float TriggerTime(unsigned address) override{return std::bit_cast<float>(Word(Event(address),192));}
    unsigned FiredFlag(unsigned address) override{return Word(Event(address),196);}
    void SetFiredFlag(unsigned address,unsigned value) override{auto& event=Event(address);std::memcpy(event.data()+196,&value,4);}
    unsigned ListenerAt(unsigned address) override{Check(address==slot,"invalid HOLD listener slot");return hold;}
    bool MatchesMessage(unsigned listener,unsigned message) override{Check(listener==hold && message>=0x80,"invalid HOLD match");return Same(Text(Event(message-0x80),128),"hold");}
    void Dispatch(unsigned listener,pal4::GameObjectEmbeddedAnimationState& current,unsigned address) override {
        Check(&current==&state,"invalid animation dispatch state");
        if(listener==fallback && message_callback){++fallback_calls;message_callback(Event(address));return;}
        if(listener!=hold)throw std::logic_error("unbound animation message "+std::string(Text(Event(address),128)));
        (void)pal4::GameObjectEmbeddedAnimation::SetPauseFlag(state,1);++holds;
    }
};
ActorPlayback::ActorPlayback(const Actor& actor,std::string model,ActorMemberReader read,ActorMessageCallback fallback,bool retain_source,ActorClipLoader loader):impl_(std::make_unique<Impl>(actor,std::move(model),std::move(read),std::move(fallback),retain_source,loader?std::move(loader):actor.clip_loader)){impl_->base=impl_->model+"\\";}
ActorPlayback::~ActorPlayback()=default;
std::uint64_t ActorPlayback::BindAnimationImage(std::span<std::uint32_t,31> image,std::uint32_t address,std::uint32_t clump){
    auto& s=*impl_;Check(clump && !s.operations && s.bound_image.empty() && address && address%4==0 && address<=std::numeric_limits<std::uint32_t>::max()-sizeof(s.state),"invalid or duplicate animation image binding");
    Check(!image[21] && !image[22] && !image[23],"animation script record collection is not bound");
    Check(s.next_binding<std::numeric_limits<std::uint64_t>::max(),"animation image binding exhausted");
    std::copy(image.begin(),image.end(),s.previous_image.begin());
    s.bound_image=image;s.bound_clump=clump;s.binding=s.next_binding++;s.PublishImage();return s.binding;
}
bool ActorPlayback::UnbindAnimationImage(std::uint64_t binding){
    auto& s=*impl_;Check(!s.operations && binding && s.binding==binding && !s.bound_image.empty(),"invalid animation image binding release");
    const bool intact=s.ImageIntact();if(intact)std::copy(s.previous_image.begin(),s.previous_image.end(),s.bound_image.begin());
    s.bound_image={};s.binding=0;s.bound_clump=0;return intact;
}
bool ActorPlayback::OwnsAnimationImage(std::uint64_t binding) const{return binding && impl_->binding==binding && !impl_->bound_image.empty();}
bool ActorPlayback::Select(std::string_view action) {
    return impl_->Run([&]{for(std::size_t i=0;i<impl_->actor.clips.size();++i)if(Same(impl_->actor.clips[i].name,action)){impl_->LoadClip(i);impl_->LoadMessages(i);return pal4::GameObjectAnimationSwitch::SetCurrentAnimation(impl_->state,impl_->actor.clips[i].name,*impl_);}return false;});
}
bool ActorPlayback::SelectBlended(std::string_view action,float seconds) {
    return impl_->Run([&]{
        Check(std::isfinite(seconds) && seconds>0,"invalid animation blend duration");
        Check(impl_->clip>=0,"blend requires a current animation");
        for(std::size_t i=0;i<impl_->actor.clips.size();++i)if(Same(impl_->actor.clips[i].name,action)){impl_->LoadClip(i);impl_->LoadMessages(i);return pal4::GameObjectAnimationSwitch::SetCurrentAnimationBlended(impl_->state,impl_->actor.clips[i].name,seconds,*impl_);}
        return false;
    });
}
void ActorPlayback::Advance(float elapsed) {
    impl_->Run([&]{Check(std::isfinite(elapsed) && elapsed>=0,"invalid animation elapsed");if(impl_->clip>=0)Check(pal4::GameObjectEmbeddedAnimation::UpdateInterpolator(impl_->state,elapsed,*impl_),"animation interpolator cannot advance");++impl_->frames;});
}
bool ActorPlayback::ProcessRenderAnimation(std::uint32_t camera) {
    return impl_->Run([&]{return pal4::GameObjectEmbeddedAnimation::ProcessAnimation(impl_->state,camera,*impl_);});
}
void ActorPlayback::SetRestart(bool enabled){impl_->Run([&]{(void)pal4::GameObjectEmbeddedAnimation::SetRestartFlag(impl_->state,enabled);});}
void ActorPlayback::SetPause(bool enabled){impl_->Run([&]{(void)pal4::GameObjectEmbeddedAnimation::SetPauseFlag(impl_->state,enabled);});}
void ActorPlayback::Seek(float seconds) {
    impl_->Run([&]{Check(std::isfinite(seconds) && !impl_->state.words[15],"invalid seek during animation blend");Check(pal4::GameObjectEmbeddedAnimation::SetTime(impl_->state,seconds,*impl_),"invalid animation seek");});
}
float ActorPlayback::Duration() const{return impl_->clip<0?0:impl_->ClipData(impl_->clip).duration;}
bool ActorPlayback::Completed() const{return pal4::GameObjectEmbeddedAnimation::CompletedFlag(impl_->state)!=0;}
void ActorPlayback::AttachHoldListener(){impl_->Run([&]{impl_->state.words[7]=Impl::slot;impl_->state.words[8]=Impl::slot+4;});}
void ActorPlayback::RemoveHoldListener(){impl_->Run([&]{impl_->state.words[7]=impl_->state.words[8]=0;});}
std::string_view ActorPlayback::MessageName() const{return impl_->state.words[10]==Impl::hold?"hold":std::string_view{};}
std::string_view ActorPlayback::Action() const{return impl_->clip<0?std::string_view{}:impl_->actor.clips[impl_->clip].name;}
int ActorPlayback::Clip() const{return impl_->clip;}
float ActorPlayback::Time() const{return impl_->time;}
void ActorPlayback::Sample(std::vector<Vertex>& output,std::vector<Matrix>* frames) {
    if(impl_->pose_dirty) {
        if(impl_->blend_pose)SampleBlendedActorClipVertices(impl_->actor,impl_->ClipData(impl_->blend_clips[0]),impl_->ClipData(impl_->blend_clips[1]),impl_->blend_weight,impl_->pose,&impl_->pose_frames);
        else SampleActorClipVertices(impl_->actor,impl_->clip<0?nullptr:&impl_->ClipData(impl_->clip),impl_->time,impl_->pose,AnimationTimeMode::clamp,&impl_->pose_frames);
        impl_->pose_dirty=false;
    }
    output=impl_->pose;
    if(frames)*frames=impl_->pose_frames;
}
void ActorPlayback::WriteReport(std::ostream& output,bool include_source) const {
    const auto& s=*impl_;output<<std::setprecision(9)<<"{\"action\":"<<std::quoted(std::string(Action()))<<",\"time\":"<<s.time
        <<",\"blending\":"<<(s.state.words[15]?"true":"false")<<",\"blend_frames\":"<<s.blend_frames<<",\"blend_weight\":"<<s.blend_weight<<",\"frames\":"<<s.frames<<",\"hold_events\":"<<s.holds<<",\"default_handler\":"<<(s.message_callback?"true":"false")<<",\"fallback_calls\":"<<s.fallback_calls<<",\"paused\":"<<(s.state.words[19]?"true":"false")
        <<",\"message\":"<<std::quoted(std::string(MessageName()))<<",\"duration\":"<<(s.clip<0?0:s.ClipData(s.clip).duration)<<",\"amf\":";
    if(s.clip<0){output<<"null}";return;}
    const auto& record=s.messages.at(s.clip);output<<"{\"member\":"<<std::quoted(record.member)<<",\"bytes\":"<<record.source_bytes<<",\"events\":[";
    bool first=true;for(const auto& event:record.events){if(!first)output<<',';first=false;output<<"{\"model\":"<<std::quoted(std::string(Text(event,0)))<<",\"action\":"<<std::quoted(std::string(Text(event,64)))<<",\"message\":"<<std::quoted(std::string(Text(event,128)))<<",\"trigger\":"<<std::bit_cast<float>(Word(event,192))<<",\"fired\":"<<Word(event,196)<<'}';}
    output<<"],\"source_hex\":\"";if(include_source && record.source.size()<=4096)for(const auto byte:record.source){constexpr char hex[]="0123456789abcdef";output<<hex[byte>>4]<<hex[byte&15];}output<<"\"}}";
}
}
