//
//  File: %sys-rebnod.h
//  Summary: {Definitions for the Rebol_Header-having "superclass" structure}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2016 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
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
// In order to implement several "tricks", the first pointer-size slots of
// many datatypes is a `Reb_Header` structure.  The bit layout of this header
// is chosen in such a way that not only can Rebol value pointers (REBVAL*)
// be distinguished from Rebol series pointers (REBSER*), but these can be
// discerned from a valid UTF-8 string just by looking at the first byte.
//
// On a semi-superficial level, this permits a kind of dynamic polymorphism,
// such as that used by panic():
//
//     REBVAL *value = ...;
//     panic (value); // can tell this is a value
//
//     REBSER *series = ...;
//     panic (series) // can tell this is a series
//
//     const char *utf8 = ...;
//     panic (utf8); // can tell this is UTF-8 data (not a series or value)
//
// But a more compelling case is the planned usage through the API, so that
// variadic combinations of strings and values can be intermixed, as in:
//
//     rebDo("[", "poke", series, "1", value, "]") 
//
// Internally, the ability to discern these types helps certain structures or
// arrangements from having to find a place to store a kind of "flavor" bit
// for a stored pointer's type.  They can just check the first byte instead.
//
// For lack of a better name, the generic type covering the superclass is
// called a "Rebol Node".
//


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE HEADER a.k.a `struct Reb_Header` (for REBVAL and REBSER uses)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Assignments to bits and fields in the header are done through a native
// platform-sized integer...while still being able to control the underlying
// ordering of those bits in memory.  See FLAGIT_LEFT() in %reb-c.h for how
// this is achieved.
//
// This control allows the leftmost byte of a Rebol header (the one you'd
// get by casting REBVAL* to an unsigned char*) to always start with the bit
// pattern `10`.  This pattern corresponds to what UTF-8 calls "continuation
// bytes", which may never legally start a UTF-8 string:
//
// https://en.wikipedia.org/wiki/UTF-8#Codepage_layout
//
// There are also applications of Reb_Header as an "implicit terminator".
// Such header patterns don't actually start valid REBNODs, but have a bit
// pattern able to signal the IS_END() test for REBVAL.  See notes on
// NODE_FLAG_END and NODE_FLAG_CELL.
//

struct Reb_Header {
    //
    // Uses REBUPT which is 32-bits on 32 bit platforms and 64-bits on 64 bit
    // machines.  Note the numbers and layout in the headers will not be
    // directly comparable across architectures.
    //
    // !!! A clever future application of the 32 unused header bits on 64-bit
    // architectures might be able to add optimization or instrumentation
    // abilities as a bonus.
    //
    REBUPT bits;
};


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE_FLAG_VALID (leftmost bit)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// The first bit will be 1 for all Reb_Header in the system that are not free.
// Freed nodes actually have *all* 0 bits in the header.
//
// The C++ debug build is actually able to enforce that a 0 in this position
// makes a cell unwritable by routines like VAL_RESET_HEADER().  It can do
// this because constructors provide a hook point to ensure valid REBVAL
// cells on the stack have the bit pre-initialized to 1.
//
// !!! UTF-8 empty strings (just a 0 terminator byte) are indistingushable,
// since only one byte may be valid to examine without crashing.  But in a
// working state, the system should never be in a position of needing to
// distinguish a freed node from an empty string.  Debug builds can use
// heuristics to guess which it is when providing diagnostics.
//
#define NODE_FLAG_VALID \
    FLAGIT_LEFT(0)


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE_FLAG_END (second-leftmost bit)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// If set, it means this header should signal the termination of an array
// of REBVAL, as in `for (; NOT_END(value); ++value) {}` loops.  In this
// sense it means the header is functioning much like a null-terminator for
// C strings.
//
// *** This bit being set does not necessarily mean the header is sitting at
// the head of a full REBVAL-sized slot! ***
//
// Some data structures punctuate arrays of REBVALs with a Reb_Header that
// has the NODE_FLAG_END bit set, and the NODE_FLAG_CELL bit clear.  This
// functions fine as the terminator for a finite number of REBVAL cells, but
// can only be read with IS_END() with no other operations legal.
//
// It's only valid to overwrite end markers when NODE_FLAG_CELL is set.
//
#define NODE_FLAG_END \
    FLAGIT_LEFT(1)


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE_FLAG_CELL (third-leftmost bit)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// If this bit is set in the header, it indicates the slot the header is for
// is `sizeof(REBVAL)`.
//
// Originally it was just for the debug build, to make it safer to use the
// implementation trick of "implicit END markers".  Checking NODE_FLAG_CELL
// before allowing an operation like Val_Init_Word() to write a location
// avoided clobbering NODE_FLAG_END signals that were backed by only
// `sizeof(struct Reb_Header)`.
//
// However, in the release build it became used to distinguish "pairing"
// nodes (holders for two REBVALs in the same pool as ordinary REBSERs)
// from an ordinary REBSER node.  Plain REBSERs have the cell mask clear,
// while paring values have it set.
//
#define NODE_FLAG_CELL \
    FLAGIT_LEFT(2)


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE_FLAG_MANAGED (fourth-leftmost bit)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// The GC-managed bit is used on series to indicate that its lifetime is
// controlled by the garbage collector.  If this bit is not set, then it is
// still manually managed...and during the GC's sweeping phase the simple fact
// that it isn't NODE_FLAG_MARKED won't be enough to consider it for freeing.
//
// See MANAGE_SERIES for details on the lifecycle of a series (how it starts
// out manually managed, and then must either become managed or be freed
// before the evaluation that created it ends).
//
#define NODE_FLAG_MANAGED \
    FLAGIT_LEFT(3)


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE_FLAG_MARKED (fifth-leftmost bit)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// This flag is used by the mark-and-sweep of the garbage collector, and
// should not be referenced outside of %m-gc.c.
//
// See `SERIES_INFO_BLACK` for a generic bit available to other routines
// that wish to have an arbitrary marker on series (for things like
// recursion avoidance in algorithms).
//
#define NODE_FLAG_MARKED \
    FLAGIT_LEFT(4)


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE_FLAG_ROOT (fifth-leftmost bit)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// This indicates the node should be treated as a root for GC purposes.  It
// only means anything on a REBVAL if that REBVAL happens to live in the key
// slot of a paired REBSER--it should not generally be set otherwise.
//
// !!! Review the implications of this flag "leaking" if a key is ever bit
// copied out of a pairing that uses it.  It might not be a problem so long
// as the key is ensured read-only, so that the bit is just noise on any
// non-key that has it...but the consequences may be more sinister.
//
#define NODE_FLAG_ROOT \
    FLAGIT_LEFT(5)


// v-- BEGIN GENERAL VALUE AND SERIES BITS WITH THIS INDEX

#define GENERAL_VALUE_BIT 6
#define GENERAL_SERIES_BIT 6


//=////////////////////////////////////////////////////////////////////////=//
//
//  NODE STRUCTURE
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Though the name Node is used for a superclass that can be "in use" or
// "free", this is the definition of the structure for its layout when it
// does *not* have NODE_FLAG_VALID set.  In that case, the memory manager
// will set the header bits to 0 and use the pointer slot right after the
// header for its linked list of free nodes.
//

typedef struct Reb_Node {
    struct Reb_Header header; // will be header.bits = 0 if node is free

    struct Reb_Node *next_if_free; // if not free, entire node is available

    // Size of a node must be a multiple of 64-bits.  This is because there
    // must be a baseline guarantee for node allocations to be able to know
    // where 64-bit alignment boundaries are.
    //
    /*struct REBI64 payload[N];*/
} REBNOD;

#define IS_FREE_NODE(n) \
    (cast(struct Reb_Node*, (n))->header.bits == 0)


// !!! Definitions for the memory allocator generally don't need to be
// included by all clients, though currently it is necessary to indicate
// whether a "node" is to be allocated from the REBSER pool or the REBGOB
// pool.  Hence, the REBPOL has to be exposed to be included in the
// function prototypes.  Review this necessity when REBGOB is changed.
//
typedef struct rebol_mem_pool REBPOL;
