/***************************************************************************
    begin                : Sun Mar 08 2009
    copyright            : (C) 1999-2009 by Pete Bernert
    email                : BlackDove@addcom.de

    PCSX rearmed rework (C) notaz, 2012
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version. See also the license.txt file for *
 *   additional informations.                                              *
 *                                                                         *
 ***************************************************************************/
#include "cfg.c"
#include "fps.c"
#include "gte_accuracy.c"
#include "soft.c"
#include "draw.c"
#include "texture.c"
#include "soft.h"
#include "prim.c"
//#include "hud.c" 
#include "menu.c"
#include "key.c"
#include "gpu.c"
#include "externals.h"

short g_m1,g_m2,g_m3;
short DrawSemiTrans;

short          ly0,lx0,ly1,lx1,ly2,lx2,ly3,lx3;        // global psx vertex coords
//long           GlobalTextAddrX,GlobalTextAddrY,GlobalTextTP;
//long           GlobalTextREST,GlobalTextABR,GlobalTextPAGE;

#if 1
unsigned int dwGPUVersion;
int           GlobalTextIL;

unsigned char  *psxVub;
unsigned short *psxVuw;

GLfloat         gl_z=0.0f;
BOOL            bNeedInterlaceUpdate;
BOOL            bNeedRGB24Update;
BOOL            bChangeWinMode;
int            lGPUstatusRet;
unsigned int   ulGPUInfoVals[16];
VRAMLoad_t      VRAMWrite;
VRAMLoad_t      VRAMRead;
int             iDataWriteMode;
int             iDataReadMode;

int            lClearOnSwap;
int            lClearOnSwapColor;
BOOL            bSkipNextFrame;

PSXDisplay_t    PSXDisplay;
PSXDisplay_t    PreviousPSXDisplay;
TWin_t          TWin;
BOOL            bDisplayNotSet;
BOOL            bNeedWriteUpload;
int             iLastRGB24;
#endif

////////////////////////////////////////////////////////////////////////
// global GPU vars
////////////////////////////////////////////////////////////////////////

#if 1
int             lGPUstatusRet;
char            szDispBuf[64];

uint32_t        dwGPUVersion = 0;
int             iGPUHeight = 512;
int             iGPUHeightMask = 511;
int             GlobalTextIL = 0;
int             iTileCheat = 0;

VRAMLoad_t      VRAMWrite;
VRAMLoad_t      VRAMRead;
int             iDataWriteMode;
int             iDataReadMode;

int             lClearOnSwap;
int             lClearOnSwapColor;
BOOL            bSkipNextFrame = FALSE;
int             iColDepth;
BOOL            bChangeRes;
BOOL            bWindowMode;
int             iWinSize;
int    bFullScreen=0;
uint32_t   ulKeybits = 0; //just define this here instead of pulling in all teh keys.c stuff
#endif

#if 1

PSXDisplay_t    PSXDisplay;
PSXDisplay_t    PreviousPSXDisplay;
TWin_t          TWin;
short           imageX0,imageX1;
short           imageY0,imageY1;
BOOL            bDisplayNotSet = TRUE;
GLuint          uiScanLine=0;
int             iUseScanLines=0;
float           iScanlineColor[] = {0,0,0, 0.3f}; // easy on the eyes.
int             lSelectedSlot=0;
int             iBlurBuffer=0;
int             iScanBlend=0;
int             iRenderFVR=0;
int             iNoScreenSaver=0;
uint32_t        ulGPUInfoVals[16];

uint32_t        vBlank=0;
#endif

//from menu.c
unsigned short usCursorActive=0;
GLuint gTexPicName=0;
PSXPoint_t     ptCursorPoint[8];
uint32_t       dwCoreFlags=0;


static void fps_update(void);




#define GPUwriteStatus_ext GPUwriteStatus_ext // for gpulib to see this
void GPUwriteStatus_ext(unsigned long gdata)
{
switch((gdata>>24)&0xff)
 {
  case 0x00:
   PSXDisplay.Disabled=1;
   PSXDisplay.DrawOffset.x=PSXDisplay.DrawOffset.y=0;
   drawX=drawY=0;drawW=drawH=0;
   sSetMask=0;lSetMask=0;bCheckMask=FALSE;iSetMask=0;
   usMirror=0;
   GlobalTextAddrX=0;GlobalTextAddrY=0;
   GlobalTextTP=0;GlobalTextABR=0;
   PSXDisplay.RGB24=FALSE;
   PSXDisplay.Interlaced=FALSE;
   bUsingTWin = FALSE;
   return;

  case 0x03:  
   PreviousPSXDisplay.Disabled = PSXDisplay.Disabled;
   PSXDisplay.Disabled = (gdata & 1);

   if (iOffscreenDrawing==4 &&
        PreviousPSXDisplay.Disabled && 
       !(PSXDisplay.Disabled))
    {

     if(!PSXDisplay.RGB24)
      {
       PrepareFullScreenUpload(TRUE);
       UploadScreen(TRUE); 
       updateDisplay();
      }
    }
   return;

  case 0x05: 
   {
    short sx=(short)(gdata & 0x3ff);
    short sy;

    sy = (short)((gdata>>10)&0x3ff);             // really: 0x1ff, but we adjust it later
    if (sy & 0x200) 
     {
      sy|=0xfc00;
      PreviousPSXDisplay.DisplayModeNew.y=sy/PSXDisplay.Double;
      sy=0;
     }
    else PreviousPSXDisplay.DisplayModeNew.y=0;

    if(sx>1000) sx=0;

    if(dwActFixes&8) 
     {
      if((!PSXDisplay.Interlaced) &&
         PreviousPSXDisplay.DisplayPosition.x == sx  &&
         PreviousPSXDisplay.DisplayPosition.y == sy)
       return;

      PSXDisplay.DisplayPosition.x = PreviousPSXDisplay.DisplayPosition.x;
      PSXDisplay.DisplayPosition.y = PreviousPSXDisplay.DisplayPosition.y;
      PreviousPSXDisplay.DisplayPosition.x = sx;
      PreviousPSXDisplay.DisplayPosition.y = sy;
     }
    else
     {
      if((!PSXDisplay.Interlaced) &&
         PSXDisplay.DisplayPosition.x == sx  &&
         PSXDisplay.DisplayPosition.y == sy)
       return;
      PreviousPSXDisplay.DisplayPosition.x = PSXDisplay.DisplayPosition.x;
      PreviousPSXDisplay.DisplayPosition.y = PSXDisplay.DisplayPosition.y;
      PSXDisplay.DisplayPosition.x = sx;
      PSXDisplay.DisplayPosition.y = sy;
     }

    PSXDisplay.DisplayEnd.x=
     PSXDisplay.DisplayPosition.x+ PSXDisplay.DisplayMode.x;
    PSXDisplay.DisplayEnd.y=
     PSXDisplay.DisplayPosition.y+ PSXDisplay.DisplayMode.y+PreviousPSXDisplay.DisplayModeNew.y;

    PreviousPSXDisplay.DisplayEnd.x=
     PreviousPSXDisplay.DisplayPosition.x+ PSXDisplay.DisplayMode.x;
    PreviousPSXDisplay.DisplayEnd.y=
     PreviousPSXDisplay.DisplayPosition.y+ PSXDisplay.DisplayMode.y+PreviousPSXDisplay.DisplayModeNew.y;

    bDisplayNotSet = TRUE;

    if (!(PSXDisplay.Interlaced))
     {
      updateDisplay();
     }
    else
    if(PSXDisplay.InterlacedTest && 
       ((PreviousPSXDisplay.DisplayPosition.x != PSXDisplay.DisplayPosition.x)||
        (PreviousPSXDisplay.DisplayPosition.y != PSXDisplay.DisplayPosition.y)))
     PSXDisplay.InterlacedTest--;
    return;
   }

  case 0x06:
   PSXDisplay.Range.x0=gdata & 0x7ff;      //0x3ff;
   PSXDisplay.Range.x1=(gdata>>12) & 0xfff;//0x7ff;

   PSXDisplay.Range.x1-=PSXDisplay.Range.x0;

   ChangeDispOffsetsX();
   return;

  case 0x07:
   PreviousPSXDisplay.Height = PSXDisplay.Height;

   PSXDisplay.Range.y0=gdata & 0x3ff;
   PSXDisplay.Range.y1=(gdata>>10) & 0x3ff;

   PSXDisplay.Height = PSXDisplay.Range.y1 - 
                       PSXDisplay.Range.y0 +
                       PreviousPSXDisplay.DisplayModeNew.y;

   if (PreviousPSXDisplay.Height != PSXDisplay.Height)
    {
     PSXDisplay.DisplayModeNew.y=PSXDisplay.Height*PSXDisplay.Double;
     ChangeDispOffsetsY();
     updateDisplayIfChanged();
    }
   return;

  case 0x08:
   PSXDisplay.DisplayModeNew.x = dispWidths[(gdata & 0x03) | ((gdata & 0x40) >> 4)];

   if (gdata&0x04) PSXDisplay.Double=2;
   else            PSXDisplay.Double=1;
   PSXDisplay.DisplayModeNew.y = PSXDisplay.Height*PSXDisplay.Double;

   ChangeDispOffsetsY();
 
   PSXDisplay.PAL           = (gdata & 0x08)?TRUE:FALSE; // if 1 - PAL mode, else NTSC
   PSXDisplay.RGB24New      = (gdata & 0x10)?TRUE:FALSE; // if 1 - TrueColor
   PSXDisplay.InterlacedNew = (gdata & 0x20)?TRUE:FALSE; // if 1 - Interlace

   PreviousPSXDisplay.InterlacedNew=FALSE;
   if (PSXDisplay.InterlacedNew)
    {
     if(!PSXDisplay.Interlaced)
      {
       PSXDisplay.InterlacedTest=2;
       PreviousPSXDisplay.DisplayPosition.x = PSXDisplay.DisplayPosition.x;
       PreviousPSXDisplay.DisplayPosition.y = PSXDisplay.DisplayPosition.y;
       PreviousPSXDisplay.InterlacedNew=TRUE;
      }
    }
   else 
    {
     PSXDisplay.InterlacedTest=0;
    }
   updateDisplayIfChanged();
   return;
 }
}

#include <stdint.h>

void CALLBACK GPUrearmedCallbacks(const struct rearmed_cbs *cbs)
{
}

