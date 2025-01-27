// Define the BlockData class

// I borrowed a lot of code from XviD's sources here, so I thank all the developpers
// of this wonderful codec

// See legal notice in Copying.txt for more information

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .

#ifndef __MV_INTERFACES_H__
#define __MV_INTERFACES_H__

//#define _STLP_USE_STATIC_LIB

#if 0
#pragma pack(16)
// in vector.h it is defined properly
#pragma warning(disable:4103) // disable pack to change alignment warning ( stlport related )
#pragma warning(disable:4800) // disable warning about bool to int unefficient conversion
#pragma warning(disable:4996) // disable warning about insecure deprecated string functions
#endif
// all luma/chroma planes of all frames will have the effective frame area */
// aligned to this (source plane can be accessed with aligned loads, 64 required for effective use of x264 sad on Core2) 1.9.5
#define ALIGN_PLANES (64)

// ALIGN_PLANES aligns the sourceblock UNLESS overlap != 0 OR special case: MMX function AND Block=16, Overlap = 8
// ALIGN_SOURCEBLOCK creates aligned copy of Source block. Set it to 1 if you don't want alignment.
#define ALIGN_SOURCEBLOCK (16) // REQUIRED for 16x16 x264 sam sad functions (MAnalyse and other filters ?)

// special feature, disable in the standard release !!!
//#define RETURN_PREV_LEVEL_SAD_AT_LEVEL_0

//this options make things usually slower
// complex check in lumaSAD & DCT code in SearchMV / PseudoEPZ
#define ALLOW_DCT

// make the check if it is no default reference (zero, global,...)
//#define	ONLY_CHECK_NONDEFAULT_MV

// build hardware-accelerated motion search based on DX12 Motion Estimation API (Win10 SDK 10.0.20348.0 minimum required)
// add to project additional includes ?, require libs d3d12.lib;dxgi.lib; to linker input
//#define	DX12_ME 


#define DEBUG_CLIENTBLOCK

#include	"VECTOR.h"


//#define MOTION_DEBUG          // allows to output debug information to the debug output
//#define MOTION_PROFILE        // allows to make a profiling of the motion estimation

#define N_PER_BLOCK 3

// increased in v1.4.1
#define MV_DEFAULT_SCD1             400
#define MV_DEFAULT_SCD2             130

//#define MV_BUFFER_FRAMES 10

constexpr VECTOR zeroMV = { {{ 0, 0} }, -1  };

#define RAND_OFFSET_MAX 256 
/* overhead about 1.6% max over 2 MB largepage, support about 256 different allocations without addresses of same offset inside frame hit same cache set. */
#define L2L3_CACHE_LINE_SIZE 128

#endif
