#pragma once
/* Shared screen layout for every story page. The race has a 128-column BAT
 * and a fixed 48-row raster offset; other stages use their scrolling BAT. */
#define PCE_DIALOG_TOP_ROW 3
#define PCE_DIALOG_BOTTOM_ROW 18
#define PCE_DIALOG_RACE_ROW (48+PCE_DIALOG_TOP_ROW)
#define PCE_DIALOG_PORTRAIT_X 0
#define PCE_DIALOG_RACE_START_RCR (63+PCE_DIALOG_TOP_ROW*8)
#define PCE_DIALOG_RACE_END_RCR (64+(PCE_DIALOG_TOP_ROW+6)*8)
