#pragma once
#include "pce_config.h"
/* The race's road (road_pce.c, bank $6d): road_start loads the picture and clears the tables (pce_control.ok), road_draw builds
 * the scanline tables of the camera in pce_control (x, y) and race_pce.h's cam_hd, cam_c, cam_s. */
void road_start(void);
void road_draw(void);
