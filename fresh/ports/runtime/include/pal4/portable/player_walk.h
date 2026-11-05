#pragma once
#include "pal4/portable/player_action.h"
#include "pal4/portable/surface_motion.h"
namespace pal4::portable {
struct PlayerWalkCompletion {ScriptToken token;std::string error;};
using PlayerWalkMoved=std::function<void(std::uint32_t,Point3,Point3)>;
class PlayerWalks {
public:
    PlayerWalks(PlayerRoster& players,PlayerActions& actions,const CollisionMesh& collision,const Scene& world,PlayerWalkMoved moved);
    ~PlayerWalks();
    PlayerWalks(const PlayerWalks&)=delete;
    PlayerWalks& operator=(const PlayerWalks&)=delete;
    ScriptResponse Start(ScriptToken token,std::int32_t player,Point3 target,bool wait,bool run=false);
    ScriptResponse End(ScriptToken token,std::int32_t player);
    std::vector<PlayerWalkCompletion> Advance(float elapsed);
    bool Requested() const;
    bool Active() const;
    void WriteReport(std::ostream& output) const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
