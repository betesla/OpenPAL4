#include "pal4/portable/chapter_host.h"
#include "pal4/portable/modern_gamepad.h"
#include <algorithm>
#include <stdexcept>
namespace pal4::portable {
void AdvanceChapterFreeControl(ChapterHost& s,float elapsed) {
    const bool confirm=s.input_confirm || s.confirm_pending;s.confirm_pending=false;
    const bool blocked=!s.game || s.game->State()==ScriptState::failed || !s.error.empty() ||
        !s.free_movement || !s.game_camera || !s.controller.State().enabled ||
        !s.controller.State().actor ||
        (s.dialog && s.dialog->View().visible) || (s.tutorial && s.tutorial->View().visible) ||
        (s.game_media && s.game_media->Waiting()) || (s.game_arena && s.game_arena->Waiting()) ||
        (s.game_effect && s.game_effect->State().active) ||
        (s.camera_tracks && (s.camera_tracks->Playing() || s.camera_tracks->Waiting())) ||
        (s.player_walks && s.player_walks->Active());
    const auto restore_modern=[&]{
#if PAL4_MODERN_GAMEPAD
        if(s.modern_movement_active && s.controller.Initialized() && s.controller.State().actor){
            s.controller.SetMovementMode(1);
            s.controller.SetAnalogMovementResult(false);
            if(s.player_actions)s.player_actions->SetTaskSpeed(*s.controller.State().actor,1);
        }
        s.modern_movement_active=false;
#endif
    };
    if(blocked){restore_modern();s.camera_yaw_input=s.camera_pitch_input=0;if(s.scene_interaction)s.scene_interaction->Reset(confirm);s.free_control_active=false;s.free_idle={};return;}
    const auto index=*s.controller.State().actor;
    if(s.scene_interaction){
        const auto& player=s.players.Slots()[index];const auto sphere=ActorBindSphere(*player.actor);
        if(!sphere)throw std::logic_error("interaction actor bounds are missing");
        if(s.scene_interaction->Advance(confirm,s.confirm_suppressed,player.position,(*sphere)[3],(*sphere)[3],index)){
            restore_modern();s.camera_yaw_input=s.camera_pitch_input=0;s.free_control_active=false;s.free_idle={};return;
        }
    }
#if PAL4_MODERN_GAMEPAD
    if(s.camera_yaw_input || s.camera_pitch_input){
        const auto p=s.game_camera->Pose();auto angles=p.angles;
        angles[0]+=s.camera_yaw_input;angles[1]=std::clamp(angles[1]+s.camera_pitch_input,-89.f,89.f);
        s.game_camera->SetParameters(0,p.target,angles,p.requested_distance);
    }
#endif
    s.camera_yaw_input=s.camera_pitch_input=0;
    const auto pose=s.game_camera->Pose();
#if PAL4_MODERN_GAMEPAD
    const auto plan=PlanModernGamepad(s.input_x,s.input_y,{pose.right,pose.forward},s.players.Slots()[index].rotation[1],elapsed);
    if(plan.active){
        s.controller.SetAnalogMovementTuning(plan.mode,plan.speed_multiplier);
        s.player_actions->SetTaskSpeed(index,plan.animation_multiplier);
        (void)s.free_movement->AdvanceModern(plan.direction,elapsed);
        s.free_control_active=true;s.modern_movement_active=true;
    }else{restore_modern();s.free_control_active=false;}
#else
    (void)restore_modern;
    FreeControlInput input;input.right=s.input_x>.25f;input.left=s.input_x<-.25f;
    input.forward=s.input_y>.25f;input.backward=s.input_y<-.25f;
    if(s.controller.State().input.event_mode){
        const bool moving=input.forward || input.backward || s.controller.State().input.pending[3]!=0;
        s.free_control_active=s.controller.UpdateEventInput(input,elapsed,
            [&](std::uint32_t command,float delta){(void)s.free_movement->AdvanceDiscrete(command,delta,moving);});
    }else s.free_control_active=s.controller.UpdateFreeInput(input,{pose.right,pose.forward},s.players.Slots()[index].position,elapsed,
        [&](Point3 direction,float delta){(void)s.free_movement->Advance(direction,delta);});
#endif
    struct IdleServices {
        ChapterHost& host;std::uint32_t index;
        void ReleaseDialog(){} // 当前无自由交互对话owner；模态对白在上层门控。
        bool Completed(){const auto* playback=host.player_actions->Playback(index);return playback && playback->Completed();}
        std::string_view AnimationName(){return host.players.Slots()[index].animation;}
        std::uint32_t SetAnimation(std::string_view name){
            const bool selected=host.player_actions->SelectTaskAction(index,name);
            if(selected)SelectGameAnimation(host,index,name);
            return selected;
        }
        void SetRestart(){host.player_actions->SetTaskRestart(index,true);}
    } idle{s,index};
    pal4::UpdateActorControlIdle(s.free_idle,elapsed,s.free_control_active,idle);
}
}
