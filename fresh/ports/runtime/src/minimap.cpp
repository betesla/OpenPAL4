#include "pal4/portable/minimap.h"
#include "pal4/minimap_file_loader.h"
#include "pal4/minimap_grid_state.h"
#include "pal4/minimap_sprite_loader.h"
#include "pal4/minimap_manager.h"
#include "pal4/minimap_texture_state.h"
#include "pal4/minimap_scene_ui_extended_update.h"
#include <algorithm>
#include "pal4/minimap_icon_object.h"
#include <lodepng.h>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
namespace pal4::portable {
static_assert(std::endian::native==std::endian::little);
namespace {
void Require(bool valid,const char* message){if(!valid)throw std::runtime_error(message);}
std::int32_t Integer(double value) {
    if(!std::isfinite(value) || value<std::numeric_limits<std::int32_t>::min() || value>=2147483648.0)return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
bool Name(std::string_view name){return !name.empty() && name.size()<=64 && name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string_view::npos;}
Texture DecodePng(const std::vector<std::uint8_t>& bytes) {
    Require(bytes.size()<=16u*1024*1024,"minimap PNG exceeds budget");
    lodepng::State state;unsigned width{},height{};
    auto error=lodepng_inspect(&width,&height,&state,bytes.data(),bytes.size());
    Require(!error && width && height && width<=2048 && height<=2048 && std::uint64_t(width)*height*4<=16u*1024*1024,"invalid minimap PNG dimensions");
    const auto expected=std::size_t(width)*height*4;Texture image;image.width=width;image.height=height;image.alpha=true;
    state.decoder.zlibsettings.max_output_size=expected*2+height*8+64;
    error=lodepng::decode(image.rgba,width,height,state,bytes);
    if(error)throw std::runtime_error(std::string("minimap PNG decode: ")+lodepng_error_text(error));
    Require(image.rgba.size()==expected,"invalid minimap RGBA length");return image;
}
}
struct SceneMinimap::Impl final:pal4::IMinimapFileRuntime,pal4::IMinimapGridEffectsRuntime,pal4::IMinimapSpriteRuntime,pal4::IMinimapManagerUpdateRuntime,pal4::IMinimapTextureRuntime {
    struct Layer {Texture image;pal4::MinimapGridState grid;};
    MinimapMemberReader read;std::string image_base;std::vector<std::uint8_t> file;std::size_t cursor{};
    std::vector<Layer> layers;std::vector<MinimapIcon> icons;std::span<const MinimapObject> objects;
    std::optional<std::size_t> selected;std::uint32_t refreshes{},reveal_calls{};std::size_t image_bytes{};
    bool missing_image{};std::vector<std::pair<std::string,std::size_t>> sources;
    pal4::MinimapManagerObjectState manager_state;pal4::MinimapIconState player_icon;
    pal4::MinimapTextureState viewport_state;
    Texture viewport;
    std::uint64_t viewport_revision{};
    bool viewport_alpha{true};
    static constexpr std::uint32_t kViewportImage=0x3000,kViewportRaster=0x3001,kViewportTexture=0x3002,kViewportPixels=0x30000000;
    MinimapPlayer player;std::optional<Texture> player_image;std::uint32_t player_calls{},player_updates{};std::size_t player_source_bytes{};
    explicit Impl(MinimapMemberReader r):read(std::move(r)){Require(bool(read),"minimap reader is not bound");}
    std::optional<std::vector<std::uint8_t>> Read(std::string_view member) {
        auto data=read(member);if(data)sources.emplace_back(member,data->size());return data;
    }
    void EnterPackageScope() override{} // Reader owns the fixed original package.
    void LeavePackageScope() override{}
    void InitializeXmlDocument() override{} // Original creates an unused local XML document.
    void DestroyXmlDocument() override{}
    std::string BasePath() override{return {};}
    std::uint32_t OpenRead(const std::string& path) override {
        auto bytes=Read(path);if(!bytes)return 0;
        file=std::move(*bytes);cursor=0;
        Require(file.size()>=4 && file.size()<=4+32*28,"invalid minimap layer file size");
        std::int32_t count{};std::memcpy(&count,file.data(),4);
        Require(count<=32 && (count<=0 || file.size()==4+static_cast<std::size_t>(count)*28),"invalid minimap layer count");return 1;
    }
    void InitializeFileStream(std::uint32_t handle) override{Require(handle==1,"invalid minimap file");}
    void ReadBytes(std::uint32_t handle,void* destination,std::uint32_t count) override {
        Require(handle==1 && cursor<=file.size() && count<=file.size()-cursor,"truncated minimap layer file");
        std::memcpy(destination,file.data()+cursor,count);cursor+=count;
    }
    void SetImageBasePath(const std::string& path) override{image_base=path;}
    std::uint32_t FindImagePath(const std::string& name) override {
        const auto bytes=Read(image_base+name);if(!bytes){missing_image=true;return 0;}
        Layer layer;layer.image=DecodePng(*bytes);Require(image_bytes+layer.image.rgba.size()<=64u*1024*1024,"minimap images exceed budget");
        image_bytes+=layer.image.rgba.size();layers.push_back(std::move(layer));return ImageHandle(layers.size()-1);
    }
    static std::uint32_t ImageHandle(std::size_t i){return 0x1000+static_cast<std::uint32_t>(i)*0x100;}
    static std::uint32_t PixelHandle(std::size_t i){return 0x1000000+static_cast<std::uint32_t>(i)*0x1000000;}
    std::uint32_t AllocateGridObject(std::uint32_t bytes) override{Require(bytes==32 && !layers.empty(),"invalid minimap grid allocation");return static_cast<std::uint32_t>(layers.size());}
    void ConstructGridObject(std::uint32_t object) override{Require(object==layers.size(),"invalid minimap grid constructor");}
    void InitializeGridObject(std::uint32_t object,std::uint32_t image,std::uint32_t scale,const std::array<std::uint8_t,24>& metadata) override {
        Require(object==layers.size() && image==ImageHandle(object-1) && std::bit_cast<std::int32_t>(scale)!=0,"invalid minimap grid initialization");
        for(std::size_t offset=0;offset<metadata.size();offset+=4){float value;std::memcpy(&value,metadata.data()+offset,4);Require(std::isfinite(value),"non-finite minimap metadata");}
        (void)layers.back().grid.Initialize(image,scale,metadata,*this);
    }
    void CloseFile(std::uint32_t handle) override{Require(handle==1,"invalid minimap file close");file.clear();}
    std::uint32_t DestroyPluginObject(std::uint32_t) override{throw std::logic_error("minimap image lifetime belongs to scene");}
    std::uint32_t ImageField(std::uint32_t handle,std::uint32_t offset) override {
        if(handle==kViewportImage){switch(offset){case 4:return viewport.width;case 8:return viewport.height;case 12:return 32;case 16:return viewport.width*4;case 20:return kViewportPixels;default:throw std::logic_error("invalid viewport image field");}}
        return ReadDword(handle+offset);
    }
    std::uint32_t ReadDword(std::uint32_t address) override {
        if(address>=0x100000)return ReadDwordObject(address);
        Require(address>=0x1000,"invalid minimap image address");const auto i=(address-0x1000)/0x100,offset=(address-0x1000)%0x100;
        const auto& image=layers.at(i).image;
        switch(offset){case 4:return image.width;case 8:return image.height;case 12:return 32;case 16:return image.width*4;case 20:return PixelHandle(i);default:throw std::logic_error("unbound minimap image field");}
    }
    std::uint8_t& Pixel(std::uint32_t address) {
        Require(address>=0x1000000,"invalid minimap pixel address");
        return layers.at((address-0x1000000)/0x1000000).image.rgba.at((address-0x1000000)%0x1000000);
    }
    std::uint8_t ReadByte(std::uint32_t address) override{return Pixel(address);}
    void WriteByte(std::uint32_t address,std::uint8_t value) override{Pixel(address)=value;}
    std::int32_t ConvertOpacity(double value) override{return Integer(value);}
    std::array<float,2> Coordinates(const std::array<float,3>& position) const {
        if(!selected.has_value())return {0.f,0.f};std::array<float,2> result{};
        (void)layers.at(*selected).grid.WorldToGridCoordinates(result,position);return result;
    }
    std::uint32_t GetMinimap() override{return 1;}
    void CleanupSection(std::uint32_t map,std::uint32_t section) override{Require(map==1 && section==1,"invalid minimap icon section");icons.clear();}
    std::uint32_t GetObjectManager() override{return 2;}
    std::uint32_t IterateObjects(std::uint32_t manager,const std::function<std::uint32_t(std::uint32_t)>& callback,std::uint32_t context) override {
        Require(manager==2 && context==0 && objects.size()<=65535,"invalid minimap object iteration");
        for(std::size_t i=0;i<objects.size();++i)
            (void)callback(0x100000+static_cast<std::uint32_t>(i)*0x1000);
        return manager;
    }
    std::uint32_t ReadDwordObject(std::uint32_t address) const {
        Require(address>=0x100000,"invalid minimap object address");const auto i=(address-0x100000)/0x1000,offset=(address-0x100000)%0x1000;
        const auto& object=objects[i];switch(offset){case 4:return i+1;case 0x1b0:return object.marker;case 0x184:return object.visible;case 0x198:return object.uses;default:throw std::logic_error("unbound minimap object field");}
    }
    std::array<float,3> ReadPosition(std::uint32_t address) override {
        Require(address>=0x100000 && (address-0x100000)%0x1000==0xac,"invalid minimap object position");
        return objects[(address-0x100000)/0x1000].position;
    }
    std::array<float,2> ConvertGridCoordinates(std::uint32_t map,const std::array<float,3>& position) override{Require(map==1,"invalid minimap coordinates");return Coordinates(position);}
    std::int32_t ConvertFloatToInt(float value) override{return Integer(value);}
    void AddIcon(std::uint32_t map,std::uint32_t label,std::uint32_t category,std::string_view image,std::int32_t x,std::int32_t y) override {
        Require(map==1 && label && label<=objects.size() && category==1,"invalid minimap icon");icons.push_back({objects[label-1].name,std::string(image),x,y,category});
    }
    pal4::MinimapManagerObjectState& Access(std::uint32_t address) override{Require(address==1,"invalid minimap manager");return manager_state;}
    Layer& Grid(std::uint32_t handle){Require(handle && handle<=layers.size(),"invalid minimap grid");return layers[handle-1];}
    std::uint32_t GridPixelBuffer(std::uint32_t handle) override{return Grid(handle).grid.PixelBuffer(*this);}
    std::array<float,2> WorldToGrid(std::uint32_t handle,const std::array<float,3>& world) override {std::array<float,2> result{};(void)Grid(handle).grid.WorldToGridCoordinates(result,world);return result;}
    std::int32_t GridWidth(std::uint32_t handle) override{return static_cast<std::int32_t>(Grid(handle).image.width);}
    std::int32_t GridHeight(std::uint32_t handle) override{return static_cast<std::int32_t>(Grid(handle).image.height);}
    std::int32_t FloatToInt(float value) override{return Integer(value);}
    void SetCoordinates(std::uint32_t object,std::int32_t x,std::int32_t y) override {
        Require(object==2 && player_image.has_value(),"invalid minimap player icon");(void)pal4::MinimapIconObject::SetCenteredCoordinates(player_icon,x,y);
        player.valid=true;player.grid_x=x;player.grid_y=y;player.left=std::bit_cast<std::int32_t>(player_icon.ReadDword(0x24));player.top=std::bit_cast<std::int32_t>(player_icon.ReadDword(0x28));++player_updates;
    }
    void CopyThreeDwords(std::uint32_t object,const std::array<std::uint32_t,3>& data) override{Require(object==2,"invalid minimap player direction");(void)pal4::MinimapIconObject::CopyTriplet(player_icon,data);for(unsigned i=0;i<3;++i)player.direction_bits[i]=player_icon.ReadDword(0x38+i*4);}
    bool ApplyRadius(std::uint32_t handle,std::int32_t x,std::int32_t y) override{++reveal_calls;return Grid(handle).grid.ApplyRadius(x,y,*this)!=0;}
    bool UpdateTexturePosition(std::uint32_t texture,std::int32_t x,std::int32_t y) override{Require(texture==kViewportTexture,"invalid viewport state identity");return viewport_state.UpdatePosition(x,y);}
    void RefreshTexture(std::uint32_t texture) override{Require(texture==kViewportTexture,"invalid viewport refresh identity");(void)viewport_state.RefreshFromGrid(*this);viewport_state.CommitRaster(*this);}
    std::uint32_t GridField12(std::uint32_t grid) override{(void)Grid(grid);return 32;}
    std::uint32_t GridStride(std::uint32_t grid) override{return Grid(grid).image.width*4;}
    void ZeroBytes(std::uint32_t address,std::uint32_t bytes) override{
        Require(address>=kViewportPixels && std::uint64_t(address-kViewportPixels)+bytes<=viewport.rgba.size(),"viewport clear exceeds buffer");
        std::fill_n(viewport.rgba.begin()+(address-kViewportPixels),bytes,std::uint8_t{0});
    }
    void CopyBytes(std::uint32_t destination,std::uint32_t source,std::uint32_t bytes) override{
        Require(destination>=kViewportPixels && std::uint64_t(destination-kViewportPixels)+bytes<=viewport.rgba.size(),"viewport copy exceeds destination");
        Require(source>=0x1000000,"invalid minimap copy source");
        const auto layer=(source-0x1000000)/0x1000000,offset=(source-0x1000000)%0x1000000;
        const auto& input=layers.at(layer).image.rgba;Require(std::uint64_t(offset)+bytes<=input.size(),"viewport copy exceeds source");
        std::copy_n(input.begin()+offset,bytes,viewport.rgba.begin()+(destination-kViewportPixels));
    }
    std::uint32_t CreateRaster(std::uint32_t width,std::uint32_t height,std::uint32_t depth,std::uint32_t mode) override{
        Require(width && height && width<=2048 && height<=2048 && depth==32 && (mode==0x504 || mode==0x604),"invalid viewport raster");viewport_alpha=mode==0x504;return kViewportRaster;
    }
    std::uint32_t CreateImage(std::uint32_t width,std::uint32_t height,std::uint32_t depth) override{
        Require(width && height && width<=2048 && height<=2048 && depth==32,"invalid viewport image");viewport={width,height,{},true};return kViewportImage;
    }
    std::uint32_t AllocateImagePixels(std::uint32_t image) override{Require(image==kViewportImage,"invalid viewport allocation");viewport.rgba.resize(std::size_t(viewport.width)*viewport.height*4);return image;}
    std::uint32_t CreateTexture(std::uint32_t argument) override{Require(!argument,"invalid viewport texture argument");return kViewportTexture;}
    void SetTextureRaster(std::uint32_t texture,std::uint32_t raster) override{Require(texture==kViewportTexture && (!raster || raster==kViewportRaster),"invalid viewport texture/raster");}
    std::uint32_t ReleaseImageData(std::uint32_t image) override{Require(image==kViewportImage,"invalid viewport release");viewport.rgba.clear();return image;}
    void DestroyImage(std::uint32_t image) override{Require(image==kViewportImage,"invalid viewport image destruction");viewport={};}
    void DestroyRaster(std::uint32_t raster) override{Require(raster==kViewportRaster,"invalid viewport raster destruction");}
    void DestroyTexture(std::uint32_t texture) override{Require(texture==kViewportTexture,"invalid viewport texture destruction");}
    void SetRasterFromImage(std::uint32_t raster,std::uint32_t image) override{Require(raster==kViewportRaster && image==kViewportImage,"invalid viewport commit");++viewport_revision;}

};
SceneMinimap::SceneMinimap(std::string world,std::string scene,MinimapMemberReader read,MinimapMemberReader icons):impl_(std::make_unique<Impl>(std::move(read))) {
    Require(Name(world) && Name(scene),"invalid minimap scene identity");pal4::MinimapLayerCollection collection;
    (void)pal4::MinimapFileLoader::Load(collection,world,scene,0,*impl_);
    Require(collection.grid_objects.size()==impl_->layers.size(),"minimap grid/image mismatch");if(!impl_->layers.empty())impl_->selected=0;
    impl_->manager_state.WriteDword(0x50,2); // 原 manager 默认探索模式；ConfigureViewport 绑定内存视口。
    if(impl_->selected && icons){
        const auto bytes=icons("minimapIcon\\actor.png");Require(bytes.has_value(),"original minimap player icon is missing");
        impl_->player_source_bytes=bytes->size();impl_->player_image=DecodePng(*bytes);
        Require(impl_->image_bytes+impl_->player_image->rgba.size()<=64u*1024*1024,"minimap images exceed budget");
        impl_->player_icon.WriteDword(0x2c,impl_->player_image->width);impl_->player_icon.WriteDword(0x30,impl_->player_image->height);
        impl_->manager_state.WriteDword(0x10,2);
    }
}
SceneMinimap::~SceneMinimap()=default;
void SceneMinimap::RefreshObjects(std::span<const MinimapObject> objects) {
    impl_->objects=objects;
    try{(void)pal4::MinimapSpriteLoader::InitializeAndLoad(*impl_);}catch(...){impl_->objects={};throw;}
    impl_->objects={};++impl_->refreshes;
}
bool SceneMinimap::RevealAt(std::array<float,3> position) {
    if(!impl_->selected)return false;
    const auto xy=impl_->Coordinates(position);const bool changed=impl_->layers[*impl_->selected].grid.ApplyRadius(Integer(xy[0]),Integer(xy[1]),*impl_)!=0;
    ++impl_->reveal_calls;return changed;
}
void SceneMinimap::UpdatePlayer(std::array<float,3> position,std::array<float,3> rotation) {
    for(auto vector:{position,rotation})for(float value:vector)Require(std::isfinite(value),"non-finite minimap player update");
    auto& s=*impl_;Require(!s.selected || s.player_image.has_value(),"minimap player icon is not bound");++s.player_calls;
    s.manager_state.WriteDword(0x24,s.selected?static_cast<std::uint32_t>(*s.selected+1):0);
    const std::array<std::uint32_t,3> data{std::bit_cast<std::uint32_t>(rotation[0]),std::bit_cast<std::uint32_t>(rotation[1]),std::bit_cast<std::uint32_t>(rotation[2])};
    pal4::MinimapManagerObject::UpdateGridDisplay(1,position,data,s);
}
void SceneMinimap::ConfigureViewport(unsigned width,unsigned height){
    auto& s=*impl_;if(!s.selected)return;
    const auto& old=s.viewport_state.Record();const auto grid=static_cast<std::uint32_t>(*s.selected+1);
    const bool alpha=DisplayMode()==2;
    if(old.image && old.grid==grid && old.region[2]==width && old.region[3]==height && s.viewport_alpha==alpha)return;
    Require(width && height && width<=2048 && height<=2048,"invalid minimap viewport size");
    Require(s.viewport_state.CreateFromGrid({0,0,width,height},grid,alpha,s),"cannot create minimap viewport");
    s.manager_state.WriteDword(0x28,s.kViewportTexture);s.manager_state.WriteDword(0x34,width);s.manager_state.WriteDword(0x38,height);
    if(s.player.valid)(void)s.viewport_state.UpdatePosition(s.player.grid_x-static_cast<std::int32_t>(width/2),s.player.grid_y-static_cast<std::int32_t>(height/2));
    s.RefreshTexture(s.kViewportTexture);
}
void SceneMinimap::SetSceneType(std::uint32_t type){
    auto& s=*impl_;const auto mode=pal4::MinimapModeForSceneType(type);if(!mode || *mode==DisplayMode())return;
    s.manager_state.WriteDword(0x50,*mode);
    if(s.viewport_state.Record().image){const auto region=s.viewport_state.Record().region;ConfigureViewport(region[2],region[3]);}
}
std::uint32_t SceneMinimap::DisplayMode()const{return impl_->manager_state.ReadDword(0x50);}
bool SceneMinimap::ViewportHasAlpha()const{return impl_->viewport_alpha;}
const Texture* SceneMinimap::ViewportImage()const{return impl_->viewport_state.Record().image?&impl_->viewport:nullptr;}
std::array<std::int32_t,4> SceneMinimap::ViewportRegion()const{
    const auto& region=impl_->viewport_state.Record().region;return {std::bit_cast<std::int32_t>(region[0]),std::bit_cast<std::int32_t>(region[1]),std::bit_cast<std::int32_t>(region[2]),std::bit_cast<std::int32_t>(region[3])};
}
std::uint64_t SceneMinimap::ViewportRevision()const{return impl_->viewport_revision;}
const MinimapPlayer& SceneMinimap::Player() const{return impl_->player;}
const Texture* SceneMinimap::PlayerImage() const{return impl_->player_image?&*impl_->player_image:nullptr;}
void SceneMinimap::SelectLayer(std::size_t index){Require(index<impl_->layers.size(),"invalid selected minimap layer");impl_->selected=index;}
const Texture* SceneMinimap::ActiveImage() const{return impl_->selected?&impl_->layers[*impl_->selected].image:nullptr;}
std::span<const MinimapIcon> SceneMinimap::Icons() const{return impl_->icons;}
void SceneMinimap::WriteReport(std::ostream& out) const {
    const auto& s=*impl_;out<<"{\"layers\":"<<s.layers.size()<<",\"selected\":";if(s.selected)out<<*s.selected;else out<<"null";
    out<<",\"display_mode\":"<<DisplayMode()<<",\"viewport_alpha\":"<<(ViewportHasAlpha()?"true":"false")<<",\"refreshes\":"<<s.refreshes<<",\"reveal_calls\":"<<s.reveal_calls<<",\"missing_image\":"<<(s.missing_image?"true":"false")<<",\"images\":[";
    bool first=true;for(const auto& layer:s.layers){if(!first)out<<',';first=false;out<<"{\"width\":"<<layer.image.width<<",\"height\":"<<layer.image.height<<",\"rgba_bytes\":"<<layer.image.rgba.size()<<",\"rgba_crc32\":"<<lodepng_crc32(layer.image.rgba.data(),layer.image.rgba.size())<<",\"scale\":"<<std::bit_cast<std::int32_t>(layer.grid.Flags())<<'}';}
    out<<"],\"icons\":[";
    first=true;for(const auto& icon:s.icons){if(!first)out<<',';first=false;out<<"{\"name\":"<<std::quoted(icon.name)<<",\"image\":"<<std::quoted(icon.image)<<",\"x\":"<<icon.x<<",\"y\":"<<icon.y<<",\"category\":"<<icon.category<<'}';}
    out<<"],\"viewport\":{\"bound\":"<<(ViewportImage()?"true":"false")<<",\"revision\":"<<s.viewport_revision<<",\"region\":[";
    const auto region=ViewportRegion();for(std::size_t i=0;i<region.size();++i){if(i)out<<',';out<<region[i];}
    out<<"]},\"player\":{\"calls\":"<<s.player_calls<<",\"updates\":"<<s.player_updates<<",\"valid\":"<<(s.player.valid?"true":"false")<<",\"grid\":["<<s.player.grid_x<<','<<s.player.grid_y<<"],\"top_left\":["<<s.player.left<<','<<s.player.top<<"],\"direction_bits\":["<<s.player.direction_bits[0]<<','<<s.player.direction_bits[1]<<','<<s.player.direction_bits[2]<<"],\"viewport_texture_bound\":"<<(ViewportImage()?"true":"false")<<",\"image\":";
    if(s.player_image)out<<"{\"member\":"<<std::quoted("minimapIcon\\actor.png")<<",\"bytes\":"<<s.player_source_bytes<<",\"width\":"<<s.player_image->width<<",\"height\":"<<s.player_image->height<<",\"rgba_crc32\":"<<lodepng_crc32(s.player_image->rgba.data(),s.player_image->rgba.size())<<'}';else out<<"null";
    out<<"},\"sources\":[";first=true;for(const auto& source:s.sources){if(!first)out<<',';first=false;out<<"{\"member\":"<<std::quoted(source.first)<<",\"bytes\":"<<source.second<<'}';}out<<"]}";
}
}
