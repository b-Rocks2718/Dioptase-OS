#ifndef ASSETS_H
#define ASSETS_H

#include "realm.h"

/*
 * Compact 5x7 ASCII bitmap font spanning characters 32 (' ') through 126 ('~').
 * Each column is 1 byte, representing 7 vertical pixels from bit 0 (top)
 * to bit 6 (bottom).
 */
extern const unsigned char font5x7[95][5];

/* Palette entries in 12-bit 0x0BGR color format. */
#define PAL_BLACK       COLOR_RGB(0, 0, 0)
#define PAL_DARK_GRAY   COLOR_RGB(4, 4, 4)
#define PAL_GRAY        COLOR_RGB(8, 8, 8)
#define PAL_LIGHT_GRAY  COLOR_RGB(12, 12, 12)
#define PAL_WHITE       COLOR_RGB(15, 15, 15)

#define PAL_GRASS_BASE  COLOR_RGB(2, 9, 2)
#define PAL_GRASS_LIGHT COLOR_RGB(3, 11, 3)
#define PAL_DIRT        COLOR_RGB(9, 6, 2)
#define PAL_DIRT_LIGHT  COLOR_RGB(11, 8, 4)
#define PAL_WATER_DEEP  COLOR_RGB(1, 4, 11)
#define PAL_WATER_SHALLOW COLOR_RGB(3, 7, 14)
#define PAL_WATER_FOAM  COLOR_RGB(6, 11, 15)
#define PAL_SAND        COLOR_RGB(13, 12, 7)

#define PAL_WOOD_BARK   COLOR_RGB(6, 4, 1)
#define PAL_LEAVES_DEEP COLOR_RGB(1, 7, 2)
#define PAL_LEAVES_LGT  COLOR_RGB(3, 11, 3)
#define PAL_STONE_DARK  COLOR_RGB(6, 6, 6)
#define PAL_STONE_LGT   COLOR_RGB(10, 10, 10)
#define PAL_GOLD_DARK   COLOR_RGB(12, 9, 0)
#define PAL_GOLD_BRIGHT COLOR_RGB(15, 14, 2)
#define PAL_WHEAT       COLOR_RGB(14, 12, 3)

#define PAL_BANNER_BLUE COLOR_RGB(2, 4, 14)
#define PAL_ROOF_RED    COLOR_RGB(12, 3, 2)
#define PAL_SKIN_TONE   COLOR_RGB(14, 11, 8)
#define PAL_LEATHER     COLOR_RGB(9, 6, 3)
#define PAL_STEEL       COLOR_RGB(11, 12, 13)
#define PAL_STEEL_BRIGHT COLOR_RGB(15, 15, 15)

#define PAL_GOBLIN_SKIN COLOR_RGB(4, 12, 2)
#define PAL_ORC_SKIN    COLOR_RGB(5, 8, 3)
#define PAL_BONE        COLOR_RGB(13, 13, 12)
#define PAL_OGRE_SKIN   COLOR_RGB(9, 6, 3)
#define PAL_BOSS_PURPLE COLOR_RGB(8, 1, 10)
#define PAL_CRIMSON     COLOR_RGB(15, 1, 2)

#define PAL_HUD_BG      COLOR_RGB(1, 1, 2)
#define PAL_HUD_BORDER  COLOR_RGB(4, 5, 7)
#define PAL_HUD_PANEL   COLOR_RGB(2, 2, 4)

#endif /* ASSETS_H */
