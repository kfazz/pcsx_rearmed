/***************************************************************************
                          stdafx.h  -  description
                             -------------------
    begin                : Sun Mar 08 2009
    copyright            : (C) 1999-2009 by Pete Bernert
    web                  : www.pbernert.com   
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

//*************************************************************************// 
// History of changes:
//
// 2009/03/08 - Pete  
// - generic cleanup for the Peops release
//
//*************************************************************************// 

#ifndef __GPU_STDAFX__
#define __GPU_STDAFX__

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _GPU_API_
#define _GPU_API_ 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

//#include <GL/gl.h>

#include "../../glsym/glsym.h"

#include <math.h> 

#define __inline inline


#define SHADETEXBIT(x) ((x>>24) & 0x1)
#define SEMITRANSBIT(x) ((x>>25) & 0x1)

#if 0
#define glError() { \
       GLenum err = glGetError(); \
       while (err != GL_NO_ERROR) { \
               printf("glError: %d caught at %s:%u\n", err, __FILE__, __LINE__); \
               err = glGetError(); \
       } \
}
#else
#define glError() 
#endif

#ifdef __cplusplus
}
#endif

#endif
