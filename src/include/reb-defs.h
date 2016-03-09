/***********************************************************************
**
**  REBOL [R3] Language Interpreter and Run-time Environment
**
**  Copyright 2012 REBOL Technologies
**  REBOL is a trademark of REBOL Technologies
**
**  Licensed under the Apache License, Version 2.0 (the "License");
**  you may not use this file except in compliance with the License.
**  You may obtain a copy of the License at
**
**  http://www.apache.org/licenses/LICENSE-2.0
**
**  Unless required by applicable law or agreed to in writing, software
**  distributed under the License is distributed on an "AS IS" BASIS,
**  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**  See the License for the specific language governing permissions and
**  limitations under the License.
**
************************************************************************
**
**  Summary: Miscellaneous structures and definitions
**  Module:  reb-defs.h
**  Author:  Carl Sassenrath
**  Notes:
**      This file is used by internal and external C code. It
**      should not depend on many other header files prior to it.
**
***********************************************************************/

#ifndef REB_DEFS_H  // due to sequences within the lib build itself
#define REB_DEFS_H

//
// Forward declarations of the series subclasses defined in %sys-series.h
// Because the Reb_Series structure includes a Reb_Value by value, it
// must be included *after* %sys-value.h
//
#ifdef REB_DEF
    struct Reb_Value; // A Rebol value cell
    #define REBVAL struct Reb_Value

    struct Reb_Series; // Rebol series node
    typedef struct Reb_Series REBSER;

    struct Reb_Array; // REBSER containing REBVALs ("Rebol Array")
    typedef struct Reb_Array REBARR;

    struct Reb_Context; // parallel REBARR key/var arrays + ANY-CONTEXT! value
    typedef struct Reb_Context REBCTX;

    struct Reb_Func;  // function parameters + FUNCTION! value
    typedef struct Reb_Func REBFUN;

    struct Reb_Map; // REBARR listing key/value pairs with hash
    typedef struct Reb_Map REBMAP;

    struct Reb_Frame; // Non-GC'd raw pointer to call frame, see %sys-do.h
    //
    // ^-- Note: This is not a REBCTX or kind of series.  It is a type that is
    // kept *inside* a REBCTX payload, pointing to ephemeral DO state memory
    // on the C stack.  It can be blown away by a longjmp during fail().  It
    // is not aliased as REBFRM to help it stand out as being a non-series
    // and non-value type.
#else
    // The %reb-xxx.h files define structures visible to host code (client)
    // which don't also require pulling in all of the %sys-xxx.h files and
    // dependencies.  Some of these definitions are shared with the core,
    // and mention things like REBSER.  When building as core that's fine,
    // but when building as host this will be undefined unless something
    // is there.  Define as a void so that it can point at it, but not know
    // anything else about it (including size).
    //
    typedef void REBSER;
    typedef void REBARR;
    typedef void REBOBJ;
#endif


#pragma pack(4)

// X/Y coordinate pair as floats:
struct Reb_Pair {
    float x;
    float y;
};

// !!! Use this instead of struct Reb_Pair when all integer pairs are gone?
// (Apparently PAIR went through an int-to-float transition at some point)
/* typedef struct Reb_Pair REBPAR; */

// !!! Temporary name for Reb_Pair "X and Y as floats"
typedef struct Reb_Pair REBXYF;

// X/Y coordinate pair as integers:
typedef struct rebol_xy_int {
    int x;
    int y;
} REBXYI;

// Standard date and time:
typedef struct rebol_dat {
    int year;
    int month;
    int day;
    int time;
    int nano;
    int zone;
} REBOL_DAT;  // not same as REBDAT

#pragma pack()

typedef struct Reb_Mem_Dump REBMDP;

#endif
