//
//  File: %reb-defs.h
//  Summary: "Miscellaneous structures and definitions"
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
// This file is used by internal and external C code. It
// should not depend on many other header files prior to it.
//

#ifndef REB_DEFS_H  // due to sequences within the lib build itself
#define REB_DEFS_H

//
// Forward declarations of the series subclasses defined in %sys-series.h
// Because the Reb_Series structure includes a Reb_Value by value, it
// must be included *after* %sys-value.h
//
#ifdef REB_DEF
    struct Reb_Value;
    #define RELVAL struct Reb_Value // maybe IS_RELATIVE()

    #ifdef __cplusplus
        #define REBVAL struct Reb_Specific_Value // guaranteed IS_SPECIFIC()
    #else
        #define REBVAL struct Reb_Value // IS_SPECIFIC(), unchecked
    #endif

    struct Reb_Series; // Rebol series node
    typedef struct Reb_Series REBSER;

    // UTF-8 Everywhere series (used for WORD!s only ATM)
    typedef REBSER REBSTR;

    struct Reb_Array; // REBSER containing REBVALs ("Rebol Array")
    typedef struct Reb_Array REBARR;

    struct Reb_Context; // parallel REBARR key/var arrays + ANY-CONTEXT! value
    typedef struct Reb_Context REBCTX;

    struct Reb_Func;  // function parameters + FUNCTION! value
    typedef struct Reb_Func REBFUN;

    struct Reb_Map; // REBARR listing key/value pairs with hash
    typedef struct Reb_Map REBMAP;

    struct Reb_Frame; // Non-GC'd raw call frame, see %sys-frame.h
    typedef struct Reb_Frame REBFRM;

    // The C build simply defines a REBIXO as a synonym for a pointer-sized int.
    // In the C++ build, the indexor is a more restrictive class...which redefines
    // a subset of operations for integers but does *not* implicitly cast to one
    // Hence if a THROWN_FLAG, END_FLAG, VA_LIST_FLAG etc. is used with integer
    // math or put into an `int` variable accidentally, this will be caught.
    //
    // Because indexors are not stored in REBVALs or places where memory usage
    // outweighs the concern of the native performance, they use `REBUPT`
    // instead of REBCNT.  The C++ build maintains that size for its class too.
    //
    // !!! The feature is now selectively enabled, temporarily in order to make
    // the binding in Ren-Cpp binary compatible regardless of whether the build
    // was done with C or C++
    //

    struct Reb_Binder; // used as argument in %tmp-funcs.h, needs forward decl

    #define END_FLAG 0x80000000  // end of block as index
    #define THROWN_FLAG (END_FLAG - 0x75) // throw as an index

    // The VA_LIST_FLAG is the index used when a C va_list pointer is the input.
    // Because access to a `va_list` is strictly increasing through va_arg(),
    // there is no way to track an index; fetches are indexed automatically
    // and sequentially without possibility for mutation of the list.  Should
    // this index be used it will always be the index of a DO_NEXT until either
    // an END_FLAG or a THROWN_FLAG is reached.
    //
    #define VA_LIST_FLAG (END_FLAG - 0xBD)

    // This is used internally in frames in the debug build when the index
    // does not apply (e.g. END, THROWN, VA_LIST)
    //
    #if !defined(NDEBUG)
        #define TRASHED_INDEX (END_FLAG - 0xAE)
    #endif

    #if defined(NDEBUG) || !defined(__cplusplus) || (__cplusplus < 201103L)
        typedef REBUPT REBIXO;
    #else
        #include "sys-do-cpp.h"

        #if 0
            typedef Reb_Indexor REBIXO;
        #else
            typedef REBUPT REBIXO;
        #endif
    #endif

    struct Reb_Path_Value_State;
    typedef struct Reb_Path_Value_State REBPVS;

    // A standard integer is currently used to represent the data stack
    // pointer.  `unsigned int` instead of a `REBCNT` in order to leverage the
    // native performance of the integer type unconstrained by bit size, as
    // data stack pointers are not stored in REBVALs or similar, and
    // performance in comparing and manipulation is more important than size.
    //
    // Note that a value of 0 indicates an empty stack; the [0] entry is made
    // to be alerting trash to trap invalid reads or writes of empty stacks.
    //
    typedef unsigned int REBDSP;
    struct Reb_Chunk;
    struct Reb_Chunker;
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
    typedef void REBSTR;
#endif


#pragma pack(4)

struct Reb_Header {
    REBUPT bits;
};


//
// With these definitions:
//
//     struct Foo_Type { struct Reb_Header header; int x; }
//     struct Foo_Type *foo = ...;
//
//     struct Bar_Type { struct Reb_Header header; float x; }
//     struct Bar_Type *bar = ...;
//
// This C code:
//
//     foo->header.bits = 1020;
//
// ...is actually different *semantically* from this code:
//
//     struct Reb_Header *alias = &foo->header;
//     alias->bits = 1020;
//
// The first is considered as not possibly able to affect the header in a
// Bar_Type.  It only is seen as being able to influence the header in other
// Foo_Type instances.
//
// The second case, by forcing access through a generic aliasing pointer,
// will cause the optimizer to realize all bets are off for any type which
// might contain a `struct Reb_Header`.
//
// This is an important point to know, with certain optimizations of writing
// headers through one type and then reading them through another.  That
// trick is used for "implicit termination", see documentation of IS_END().
//
// (Note that this "feature" of writing through pointers actually slows
// things down.  Desire to control this behavior is why the `restrict`
// keyword exists in C99: https://en.wikipedia.org/wiki/Restrict )
//
inline static void Init_Header_Aliased(struct Reb_Header *alias, REBUPT bits)
{
    alias->bits = bits; // write from generic pointer to `struct Reb_Header`
}


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
