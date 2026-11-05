#include "pal4/portable/dff_collision.h"
#include "rw_binary.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>

namespace pal4::portable {
namespace {
using namespace rw;
struct Frame {double m[3][4]{};std::int32_t parent{};int state{};};
struct Triangle {std::uint16_t a,b,c,material;};
struct Geometry {std::array<double,3> sphere_center{};std::vector<std::array<double,3>> vertices;std::vector<Triangle> triangles;std::vector<std::uint32_t> colors;std::vector<std::vector<std::array<float,2>>> uv;std::vector<Material> material_data;std::uint32_t materials{};};
}
namespace {
std::vector<CollisionTriangle> ParseCollision(std::span<const std::uint8_t> bytes,ObjectCollisionGeometry* objects,bool world_space,bool materialless,Scene* scene,const std::function<Texture(std::string_view)>& texture,bool preserve_atomics=false){
    Input in(bytes);auto root=in.ReadChunk(0,in.Size());Need(root.id==0x10 && root.end==in.Size(),"invalid DFF clump");const auto children=in.Children(root);
    const auto frame_chunks=in.Children(First(children,0x0e));const auto fs=First(frame_chunks,1);Range(fs.begin,4,fs.end);const auto count=in.U32(fs.begin);Need(count<=65536,"too many DFF frames");Range(fs.begin+4,std::size_t(count)*56,fs.end);
    std::vector<Frame> frames(count);
    for(unsigned i=0;i<count;++i){const auto p=fs.begin+4+i*56;auto& f=frames[i];for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)f.m[r][c]=in.Float(p+c*12+r*4);f.parent=std::bit_cast<std::int32_t>(in.U32(p+48));Need(f.parent>=-1 && (f.parent<0 || unsigned(f.parent)<count),"invalid DFF frame parent");}
    std::function<void(unsigned)> resolve=[&](unsigned i){auto& f=frames[i];Need(f.state!=1,"cyclic DFF frames");if(f.state==2)return;f.state=1;if(f.parent>=0){Need(unsigned(f.parent)!=i,"self parent DFF frame");resolve(unsigned(f.parent));const auto& p=frames[f.parent];double result[3][4]{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c){result[r][c]=p.m[r][0]*f.m[0][c]+p.m[r][1]*f.m[1][c]+p.m[r][2]*f.m[2][c]+(c==3?p.m[r][3]:0);}for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)f.m[r][c]=result[r][c];}f.state=2;};
    for(unsigned i=0;i<count;++i)resolve(i);
    const auto gl=in.Children(First(children,0x1a));const auto gs=First(gl,1);Range(gs.begin,4,gs.end);const auto geometry_count=in.U32(gs.begin);Need(geometry_count<=65536,"too many DFF geometries");std::vector<Geometry> geometries;
    for(const auto gc:gl){if(gc.id!=0x0f)continue;const auto parts=in.Children(gc);const auto s=First(parts,1);Range(s.begin,16,s.end);const auto flags=in.U32(s.begin),nt=in.U32(s.begin+4),nv=in.U32(s.begin+8),nm=in.U32(s.begin+12);Need((flags&2) && nm && nm<=1024 && nt<=1000000 && nv<=1000000,"unsupported DFF collision geometry");
        Geometry g;auto p=s.begin+16;if(flags&8){Range(p,std::size_t(nv)*4,s.end);for(unsigned i=0;i<nv;++i)g.colors.push_back(in.U32(p+i*4));p+=std::size_t(nv)*4;}
        auto uv=(flags>>16)&255;if(!uv)uv=(flags&0x80)?2:((flags&4)?1:0);Range(p,std::size_t(nv)*uv*8,s.end);if(scene)for(unsigned layer=0;layer<uv;++layer){std::vector<std::array<float,2>> values;values.reserve(nv);for(unsigned i=0;i<nv;++i)values.push_back({static_cast<float>(in.Float(p+layer*std::size_t(nv)*8+i*8)),static_cast<float>(in.Float(p+layer*std::size_t(nv)*8+i*8+4))});g.uv.push_back(std::move(values));}p+=std::size_t(nv)*uv*8;
        Range(p,std::size_t(nt)*8,s.end);for(unsigned i=0;i<nt;++i){const auto q=p+i*8;g.triangles.push_back({in.U16(q),in.U16(q+2),in.U16(q+6),in.U16(q+4)});}p+=std::size_t(nt)*8;
        for(unsigned m=0;m<nm;++m){Range(p,24,s.end);if(m==0)for(unsigned k=0;k<3;++k)g.sphere_center[k]=in.Float(p+k*4);const auto has_vertices=in.U32(p+16),has_normals=in.U32(p+20);p+=24;if(has_vertices){Range(p,std::size_t(nv)*12,s.end);if(m==0)for(unsigned i=0;i<nv;++i)g.vertices.push_back({in.Float(p+i*12),in.Float(p+i*12+4),in.Float(p+i*12+8)});p+=std::size_t(nv)*12;}if(has_normals){Range(p,std::size_t(nv)*12,s.end);p+=std::size_t(nv)*12;}}
        Need(p==s.end && g.vertices.size()==nv,"invalid DFF morph target");
        for(const auto& part:parts)if(part.id==8){const auto ml=in.Children(part);const auto ms=First(ml,1);Range(ms.begin,4,ms.end);g.materials=in.U32(ms.begin);if(scene)g.material_data=ReadMaterials(in,part);}
        geometries.push_back(std::move(g));}
    Need(geometries.size()==geometry_count,"DFF geometry count mismatch");std::vector<CollisionTriangle> output;unsigned atomic=0;std::size_t total=0;
    struct Group {std::uint32_t atomic;std::array<float,3> center;std::int32_t diffuse,light;std::array<std::uint8_t,4> color;std::array<std::string,8> uv_animations;std::vector<Vertex> vertices;};
    std::vector<Group> groups;std::map<std::string,std::int32_t> textures;std::uint64_t image_bytes=0;
    auto texture_id=[&](const std::string& name){if(name.empty())return std::int32_t(-1);Need(bool(texture) && name.size()<=255 && name.find_first_of("\\/:")==name.npos && name.find("..") == name.npos,"invalid DFF texture name");std::string key=name;for(auto& c:key)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');if(auto it=textures.find(key);it!=textures.end())return it->second;auto image=texture(name);const auto size=std::uint64_t(image.width)*image.height*4;Need(image.width && image.height && image.width<=4096 && image.height<=4096 && image.rgba.size()==size && size<=64ull*1024*1024 && image_bytes+size<=384ull*1024*1024,"invalid DFF texture");image_bytes+=size;Need(scene->textures.size()<4096,"too many DFF textures");const auto id=static_cast<std::int32_t>(scene->textures.size());scene->textures.push_back(std::move(image));textures.emplace(key,id);return id;};
    // DFF triangle stream stores v1,v0,material,v2 (librw geometry.cpp).
    // Render using canonical v0,v1,v2; keep the established collision ordering.
    for(auto child:children){if(child.id!=0x14)continue;const auto a=First(in.Children(child),1);Range(a.begin,16,a.end);const auto frame=in.U32(a.begin),geometry=in.U32(a.begin+4);Need(frame<frames.size() && geometry<geometries.size(),"invalid DFF atomic");const auto& f=frames[frame];const auto& g=geometries[geometry];Need(g.triangles.size()<=1000000-total,"too many DFF collision triangles");total+=g.triangles.size();ObjectCollisionAtomic object_atomic;object_atomic.frame=world_space?0:frame;object_atomic.flags=in.U32(a.begin+8);if(scene && !(object_atomic.flags&4)){++atomic;continue;}
        for(std::size_t i=0;i<g.triangles.size();++i){const auto& t=g.triangles[i];const std::array<unsigned,3> indices=scene?std::array<unsigned,3>{t.b,t.a,t.c}:std::array<unsigned,3>{t.a,t.b,t.c};Need(t.a<g.vertices.size() && t.b<g.vertices.size() && t.c<g.vertices.size() && (t.material<g.materials || (materialless && !g.materials && t.material==65535)),"invalid DFF triangle");CollisionTriangle tri{};tri.atomic=atomic;tri.triangle=static_cast<std::uint32_t>(i);tri.material=t.material;tri.attributes_known=!objects;if(!objects && !g.colors.empty())tri.rgba=std::array<std::uint32_t,3>{g.colors[t.a],g.colors[t.b],g.colors[t.c]};
            for(unsigned j=0;j<3;++j)for(unsigned axis=0;axis<3;++axis){const auto& v=g.vertices[indices[j]];const auto value=world_space?(f.m[axis][0]*v[0]+f.m[axis][1]*v[1]+f.m[axis][2]*v[2]+f.m[axis][3]):v[axis];Need(std::isfinite(value),"non-finite DFF collision point");tri.points[j][axis]=static_cast<float>(value);Need(std::isfinite(tri.points[j][axis]),"DFF collision point overflow");}
            if(scene){const auto& material=g.material_data.at(t.material);const auto diffuse=texture_id(material.texture),light=texture_id(material.lightmap);Need(light<0 || g.uv.size()>=2,"lightmapped DFF geometry missing UV1");auto it=std::find_if(groups.begin(),groups.end(),[&](const auto& group){return (!preserve_atomics||group.atomic==atomic) && group.diffuse==diffuse && group.light==light && group.color==material.color && group.uv_animations==material.uv_animations;});if(it==groups.end()){std::array<float,3> center{};for(unsigned k=0;k<3;++k)center[k]=static_cast<float>(f.m[k][0]*g.sphere_center[0]+f.m[k][1]*g.sphere_center[1]+f.m[k][2]*g.sphere_center[2]+f.m[k][3]);groups.push_back({atomic,center,diffuse,light,material.color,material.uv_animations,{}});it=groups.end()-1;}for(unsigned j=0;j<3;++j){Vertex v{};for(unsigned k=0;k<3;++k)v.position[k]=tri.points[j][k];if(!g.uv.empty())for(unsigned k=0;k<2;++k)v.uv0[k]=g.uv[0][indices[j]][k];if(g.uv.size()>1)for(unsigned k=0;k<2;++k)v.uv1[k]=g.uv[1][indices[j]][k];const auto color=g.colors.empty()?0xffffffffu:g.colors[indices[j]];for(unsigned k=0;k<4;++k)v.color[k]=float((color>>(k*8))&255)/255.f;it->vertices.push_back(v);}}
            else if(objects)object_atomic.triangles.push_back(tri);else output.push_back(tri);}
        if(objects)objects->atomics.push_back(std::move(object_atomic));
        ++atomic;}
    if(objects){Need(!objects->atomics.empty() && objects->atomics.size()<=8192,"invalid DFF object atomics");std::reverse(objects->atomics.begin(),objects->atomics.end());for(std::size_t i=0;i<objects->atomics.size();++i)for(auto& t:objects->atomics[i].triangles)t.atomic=static_cast<std::uint32_t>(i);}
    if(scene){Need(!groups.empty(),"DFF environment has no visible geometry");Need(image_bytes+total*3*sizeof(Vertex)<=384ull*1024*1024,"DFF scene memory budget exceeded");scene->saturation={1,1,1};scene->minimum.fill(std::numeric_limits<float>::infinity());scene->maximum.fill(-std::numeric_limits<float>::infinity());for(auto& group:groups){Need(!group.vertices.empty() && scene->vertices.size()+group.vertices.size()<=3000000,"DFF scene vertex budget exceeded");Draw draw{};draw.first=static_cast<std::uint32_t>(scene->vertices.size());draw.count=static_cast<std::uint32_t>(group.vertices.size());draw.diffuse=group.diffuse;draw.lightmap=group.light;draw.uv_animations=group.uv_animations;for(unsigned i=0;i<4;++i)draw.color[i]=group.color[i]/255.f;for(const auto& v:group.vertices)for(unsigned k=0;k<3;++k){scene->minimum[k]=std::min(scene->minimum[k],v.position[k]);scene->maximum[k]=std::max(scene->maximum[k],v.position[k]);}scene->draws.push_back(draw);if(preserve_atomics)scene->atomic_centers.push_back(group.center);scene->vertices.insert(scene->vertices.end(),group.vertices.begin(),group.vertices.end());}}
    return output;
}
}
std::vector<CollisionTriangle> ReadDffCollision(std::span<const std::uint8_t> bytes){return ParseCollision(bytes,nullptr,true,false,nullptr,{});}
ObjectCollisionGeometry ReadDffEventCollision(std::span<const std::uint8_t> bytes){ObjectCollisionGeometry result;(void)ParseCollision(bytes,&result,true,true,nullptr,{});return result;}
ObjectCollisionGeometry ReadDffObjectCollision(std::span<const std::uint8_t> bytes){ObjectCollisionGeometry result;(void)ParseCollision(bytes,&result,false,false,nullptr,{});return result;}
Scene ReadDffEnvironment(std::span<const std::uint8_t> bytes,const std::function<Texture(std::string_view)>& texture,bool preserve_atomic_draws){Scene result;(void)ParseCollision(bytes,nullptr,true,false,&result,texture,preserve_atomic_draws);return result;}
}
