#pragma once
#include "pce_config.h"
enum { CAM_PLAY, CAM_STORY, CAM_CLEAR, CAM_OVER, CAM_END, CAM_POWER };
/* Observable campaign state lives in console RAM, outside every overlay. */
typedef struct __attribute__((packed)) {
    uint8_t state,lives,powers,result,event,story,page,diagnostic;
    uint8_t boss_kind,boss_round,wave,lap,rank;
    uint16_t score,boss_hp,timer,boost,power_cd;
} PceCampaign;
extern PceCampaign pce_campaign;
/* Player options from the front end. music: 0 off, 1 low, 2 medium, 3 high. */
typedef struct { uint8_t difficulty,lives,music; } PceOptions;
extern PceOptions pce_options;
extern uint8_t pce_death;
uint8_t campaign_hearts(void);
void story_start(void);
void story_step(void);
void mission_start(void);
void mission_frame(void);
void combat_start(void);
void combat_tick(void);
void combat_draw(void);
void campaign_hurt(void);
