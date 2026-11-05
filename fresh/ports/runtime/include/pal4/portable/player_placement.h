#pragma once
#include "pal4/portable/player.h"
#include "pal4/portable/scene_movement.h"
#include "pal4/portable/player_notifications.h"
namespace pal4::portable {
class IPlayerPlacementServices {
public:
    virtual ~IPlayerPlacementServices()=default;
    virtual void FollowPlayer(std::uint32_t player)=0;
    virtual void UpdateFloorAttributes(unsigned child,const FloorHit& hit)=0;
    virtual void UpdateGrid(Point3 position,Point3 rotation)=0;
    virtual void SelectAnimation(std::uint32_t player,std::string_view action)=0;
};
class PlayerPlacement {
public:
    PlayerPlacement(PlayerRoster& players,SceneMovement& movement,IPlayerPlacementServices& services,PlayerNotifications& notifications);
    ~PlayerPlacement();
    PlayerPlacement(const PlayerPlacement&)=delete;
    PlayerPlacement& operator=(const PlayerPlacement&)=delete;
    void SetCurrent(Point3 position);
    float CurrentX() const;
    float CurrentY() const;
    float CurrentZ() const;
    void ProcessWaitHandlers();
    void WriteReport(std::ostream& output) const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
