//
//  File: %sys-mem.h
//  Summary: "Memory allocation"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2016 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//

#ifdef DBG_CHECK_MEM
#define CHECK_MEMORY(n) if (n > MEM_CARE) Check_Memory()
#else
#define CHECK_MEMORY(n)
#endif

/***********************************************************************
**
*/  typedef struct rebol_mem_segment
/*
**      Linked list of used memory segments.
**
**      Size: 8 bytes
**
***********************************************************************/
{
    struct rebol_mem_segment *next;
    REBUPT size;
} REBSEG;


/***********************************************************************
**
*/  typedef struct rebol_mem_spec
/*
**      Specifies initial pool sizes
**
***********************************************************************/
{
    REBCNT wide;                // size of allocation unit
    REBCNT units;               // units per segment allocation
} REBPOOLSPEC;


/***********************************************************************
**
*/  struct rebol_mem_pool
/*
**      Pools manage fixed sized blocks of memory.
**
***********************************************************************/
{
    REBSEG  *segs;              // first memory segment
    REBNOD  *first;             // first free node in pool
    REBNOD  *last;              // last free node in pool
    REBCNT  wide;               // size of allocation unit
    REBCNT  units;              // units per segment allocation
    REBCNT  free;               // number of units remaining
    REBCNT  has;                // total number of units
//  UL      total;              // total bytes for all segs
//  char    *name;              // identifying string
//  UL      extra;              // reserved
};


/***********************************************************************
**
*/  enum Mem_Pool_Specs
/*
***********************************************************************/
{
    MEM_TINY_POOL = 0,
    MEM_SMALL_POOLS = MEM_TINY_POOL   + 16,
    MEM_MID_POOLS   = MEM_SMALL_POOLS +  4,
    MEM_BIG_POOLS   = MEM_MID_POOLS   +  4, // larger pools
    SER_POOL     = MEM_BIG_POOLS,
    GOB_POOL,
    SYSTEM_POOL,
    MAX_POOLS
};

#define DEF_POOL(size, count) {size, count}
#define MOD_POOL(size, count) {size * MEM_MIN_SIZE, count}

#define MEM_MIN_SIZE sizeof(REBVAL)
#define MEM_BIG_SIZE 1024

#define MEM_BALLAST 3000000
