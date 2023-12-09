#pragma once

#include "script/include.h"

#define CFG_LENGTH 302

// 設定ファイルフォーマットのバージョン番号です。
// フォーマットの変更により互換性が失われた場合、1つ増加します。
// ||
// Version number of the configuration file format.
// Increase by 1 if a format change breaks compatibility.
#define CFG_VERSION 3u

void ConfigMenu();
int32_t LoadConfig(void);
int32_t SaveConfig(void);
uint32_t ConfigChecksum(int32_t *cfgbuf);
extern int32_t restart;
extern int32_t loadres;
#ifdef ENABLE_JOYSTICK
extern YGS2kSJoyKey joyKeyAssign[10 *2];
#endif
#ifdef ENABLE_GAME_CONTROLLER
extern int32_t playerCons[2];
extern YGS2kSConKey conKeyAssign[8 * 2];
#endif
#ifdef ENABLE_KEYBOARD
extern SDL_Scancode keyAssign[10 * 2];
#endif
extern int32_t lvupbonus;
extern int32_t downtype;
extern int32_t w_reverse;
extern int32_t showcombo;
extern int32_t breakeffect;
extern int32_t lasttopframe;
extern int32_t lastmaxPlay;
extern int32_t maxPlay;
extern int32_t fontsize;
extern int32_t movesound;
extern int32_t dispnext;

extern YGS2kEWaveFormat lastWavebgm;
extern YGS2kEWaveFormat wavebgm;
extern int32_t fldtr;
extern int32_t dtc;
#ifdef ENABLE_KEYBOARD
extern int32_t dispnextkey[2];
#endif
extern int32_t digitc[12];
extern int32_t fontc[12];
extern int32_t rots[2];
extern int32_t background;
extern int32_t blockflash;
extern int32_t fastlrmove;
extern int32_t sonicdrop;
extern int32_t nanameallow;
extern int32_t fourwayfilter;
extern int32_t fourwaypriorityup;
extern int32_t smooth;
extern int32_t nextblock;
extern int32_t displayIndex;
extern YGS2kEScreenModeFlag screenMode;
extern int32_t screenIndex;
extern int32_t segacheat;
