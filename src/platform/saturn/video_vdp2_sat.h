#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../render.h"
bool movie_vdp2_open(int w,int h);
void movie_vdp2_close(void);
void movie_vdp2_geometry(int x,int y,int w,int h,const RRect *clip);
void movie_vdp2_geometry_tick(void);
int movie_vdp2_present(const uint32_t *cells,uint8_t *dirty,bool first);
