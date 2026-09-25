#ifndef FILM_CVID_H
#define FILM_CVID_H

#include "film_lib.h"

extern void initClampLUT24(void);

extern void stripdata_new(stripdata_t *data, uint32_t height, uint32_t width);

extern void parseVideo(decode_work_t *work);

#endif // FILM_CVID_H
