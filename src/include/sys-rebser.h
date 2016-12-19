//
//  File: %sys-rebser.h
//  Summary: {Structure Definition for Series (REBSER)}
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
// This contains the struct definition for the "REBSER" struct Reb_Series.
// It is a small-ish descriptor for a series (though if the amount of data
// in the series is small enough, it is embedded into the structure itself.)
//
// Every string, block, path, etc. in Rebol has a REBSER.  The implementation
// of them is reused in many places where Rebol needs a general-purpose
// dynamically growing structure.  It is also used for fixed size structures
// which would like to participate in garbage collection.
//
// The REBSER is fixed-size, and is allocated as a "node" from a memory pool.
// That pool quickly grants and releases memory ranges that are sizeof(REBSER)
// without needing to use malloc() and free() for each individual allocation.
// These nodes can also be enumerated in the pool without needing the series
// to be tracked via a linked list or other structure.  The garbage collector
// is one example of code that performs such an enumeration.
//
// A REBSER node pointer will remain valid as long as outstanding references
// to the series exist in values visible to the GC.  On the other hand, the
// series's data pointer may be freed and reallocated to respond to the needs
// of resizing.  (In the future, it may be reallocated just as an idle task
// by the GC to reclaim or optimize space.)  Hence pointers into data in a
// managed series *must not be held onto across evaluations*, without
// special protection or accomodation.
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * For the forward declarations of series subclasses, see %reb-defs.h
//
// * Because a series contains a union member that embeds a REBVAL directly,
//   `struct Reb_Value` must be fully defined before this file can compile.
//   Hence %sys-rebval.h must already be included.
//
// * For the API of operations available on REBSER types, see %sys-series.h
//
// * REBARR is a series that contains Rebol values (REBVALs).  It has many
//   concerns specific to special treatment and handling, in interaction with
//   the garbage collector as well as handling "relative vs specific" values.
//
// * Several related types (REBFUN for function, REBCTX for context) are
//   actually stylized arrays.  They are laid out with special values in their
//   content (e.g. at the [0] index), or by links to other series in their
//   `->misc` field of the REBSER node.  Hence series are the basic building
//   blocks of nearly all variable-size structures in the system.
//


//=////////////////////////////////////////////////////////////////////////=//
//
// SERIES <<HEADER>> FLAGS
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Series have two places to store bits...in the "header" and in the "info".
// The following are the SERIES_FLAG_XXX that are used in the header, while
// the SERIES_INFO_XXX flags will be found in the info.
//
// As a general rule for choosing which place to put a bit, if it may be
// interesting to test/set multiple bits at the same time, then they should
// be in the same flag group.
//
// !!! Perhaps things that don't change for the lifetime of the series should
// also prefer the header vs. info?  Such separation might help with caching.
//


//=//// SERIES_FLAG_FIXED_SIZE ////////////////////////////////////////////=//
//
// This means a series cannot be expanded or contracted.  Values within the
// series are still writable (assuming SERIES_INFO_LOCKED isn't set).
//
// !!! Is there checking in all paths?  Do series contractions check this?
//
// One important reason for ensuring a series is fixed size is to avoid
// the possibility of the data pointer being reallocated.  This allows
// code to ignore the usual rule that it is unsafe to hold a pointer to
// a value inside the series data.
//
// !!! Strictly speaking, SERIES_FLAG_NO_RELOCATE could be different
// from fixed size... if there would be a reason to reallocate besides
// changing size (such as memory compaction).
//
#define SERIES_FLAG_FIXED_SIZE \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 0)


//=//// SERIES_FLAG_UTF8_STRING ///////////////////////////////////////////=//
//
// Indicates the series holds a UTF-8 encoded string.
//
// !!! Currently this is only used to store ANY-WORD! symbols, which are
// read-only and cannot be indexed into, e.g. with `next 'foo`.  This is
// because UTF-8 characters are encoded at variable sizes, and the series
// indexing does not support that at this time.  However, it would be nice
// if a way could be figured out to unify ANY-STRING! with ANY-WORD! somehow
// in order to implement the "UTF-8 Everywhere" manifesto:
//
// http://utf8everywhere.org/
//
#define SERIES_FLAG_UTF8_STRING \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 1)


//=//// STRING_FLAG_CANON /////////////////////////////////////////////////=//
//
// This is used to indicate when a SERIES_FLAG_UTF8_STRING series represents
// the canon form of a word.  This doesn't mean anything special about the
// case of its letters--just that it was loaded first.  Canon forms can be
// GC'd and then delegate the job of being canon to another spelling.
//
// A canon string is unique because it does not need to store a pointer to
// its canon form.  So it can use the REBSER.misc field for the purpose of
// holding an index during binding.
//
#define STRING_FLAG_CANON \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 2)


//=//// SERIES_FLAG_ARRAY /////////////////////////////////////////////////=//
//
// Indicates that this is a series of REBVAL value cells, and suitable for
// using as the payload of an ANY-ARRAY! value.  When a series carries this
// bit, then if it is also NODE_FLAG_MANAGED the garbage ollector will process
// its transitive closure to make sure all the values it contains (and the
// values its references contain) do not have series GC'd out from under them.
//
// Note: R3-Alpha used `SER_WIDE(s) == sizeof(REBVAL)` as the test for if
// something was an array.  But this allows creation of series that have
// items which are incidentally the size of a REBVAL, but not actually arrays.
//
#define SERIES_FLAG_ARRAY \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 3)


//=//// ARRAY_FLAG_VOIDS_LEGAL ////////////////////////////////////////////=//
//
// Identifies arrays in which it is legal to have void elements.  This is true
// for instance on reified C va_list()s which were being used for unevaluated
// applies (like R3-Alpha's APPLY/ONLY).  When those va_lists need to be put
// into arrays for the purposes of GC protection, they may contain voids which
// they need to track.
//
// Note: ARRAY_FLAG_VARLIST also implies legality of voids, which
// are used to represent unset variables.
//
#define ARRAY_FLAG_VOIDS_LEGAL \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 4)


//=//// ARRAY_FLAG_PARAMLIST //////////////////////////////////////////////=//
//
// ARRAY_FLAG_PARAMLIST indicates the array is the parameter list of a
// FUNCTION! (the first element will be a canon value of the function)
//
#define ARRAY_FLAG_PARAMLIST \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 5)


//=//// ARRAY_FLAG_VARLIST ////////////////////////////////////////////////=//
//
// This indicates this series represents the "varlist" of a context (which is
// interchangeable with the identity of the varlist itself).  A second series
// can be reached from it via the `->misc` field in the series node, which is
// a second array known as a "keylist".
//
// See notes on REBCTX for further details about what a context is.
//
#define ARRAY_FLAG_VARLIST \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 6)


//=//// CONTEXT_FLAG_STACK ////////////////////////////////////////////////=//
//
// This indicates that a context's varlist data lives on the stack.  That
// means that when the function terminates, the data will no longer be
// accessible (so SERIES_INFO_INACCESSIBLE will be true).
//
// !!! Ultimately this flag may be unnecessary because stack-based and
// dynamic series will "hybridize" so that they may have some stack
// fields and some fields in dynamic memory.  For now it's a good sanity
// check that things which should only happen to stack contexts (like becoming
// inaccessible) are checked against this flag.
//
#define CONTEXT_FLAG_STACK \
    FLAGIT_LEFT(GENERAL_SERIES_BIT + 7)


#if !defined(NDEBUG)
    //=//// SERIES_FLAG_LEGACY ////////////////////////////////////////////=//
    //
    // This is a flag which is marked at the root set of the body of legacy
    // functions.  It can be used in a dynamic examination of a call to see if
    // it "originates from legacy code".  This is a vague concept given the
    // ability to create blocks and run them--so functions like COPY would
    // have to propagate the flag to make it "more accurate".  But it's good
    // enough for casual compatibility in many cases.
    //
    #define SERIES_FLAG_LEGACY \
        FLAGIT_LEFT(GENERAL_SERIES_BIT + 8)
#endif

// ^-- STOP AT FLAGIT_LEFT(15) --^
//
// The rightmost 16 bits of the series flags are used to store an arbitrary
// per-series-type 16 bit number.  Right now, that's used by the string series
// to save their REBSYM id integer(if they have one).  Note that the flags
// are flattened in kind of a wasteful way...some are mutually exclusive and
// could use the same bit, if needed.
//
#if defined(__cplusplus) && (__cplusplus >= 201103L)
    static_assert(GENERAL_SERIES_BIT + 8 < 16, "SERIES_FLAG_XXX too high");
#endif


//=////////////////////////////////////////////////////////////////////////=//
//
// SERIES <<INFO>> BITS
//
//=////////////////////////////////////////////////////////////////////////=//
//
// See remarks above about the two places where series store bits.  These
// are the info bits, which are more likely to be changed over the lifetime
// of the series--defaulting to FALSE.
//
// See Init_Endlike_Header() for why the leading bits are chosen the way they
// are (and why SERIES_INFO_8_IS_FALSE is unused also).  This means that the
// Reb_Series->info field can function as an implicit END for
// Reb_Series->content, as well as be distinguished from a REBVAL*, a REBSER*,
// or a UTF8 string.
//

#define SERIES_INFO_0_IS_TRUE FLAGIT_LEFT(0) // NODE_FLAG_VALID
#define SERIES_INFO_1_IS_TRUE FLAGIT_LEFT(1) // NODE_FLAG_END
#define SERIES_INFO_2_IS_FALSE FLAGIT_LEFT(2) // NOT(NODE_FLAG_CELL)


//=//// SERIES_INFO_HAS_DYNAMIC ///////////////////////////////////////////=//
//
// Indicates that this series has a dynamically allocated portion.  If it does
// not, then its data pointer is the address of the embedded value inside of
// it, and that the length is stored in the rightmost byte of the header
// bits (of which this is one bit).
//
// This bit will be flipped if a series grows.  (In the future it should also
// be flipped when the series shrinks, but no shrinking in the GC yet.)
//
#define SERIES_INFO_HAS_DYNAMIC \
    FLAGIT_LEFT(3)


//=//// SERIES_INFO_BLACK /////////////////////////////////////////////////=//
//
// This is a generic bit for the "coloring API", e.g. Is_Series_Black(),
// Flip_Series_White(), etc.  These let native routines engage in marking
// and unmarking nodes without potentially wrecking the garbage collector by
// reusing NODE_FLAG_MARKED.  Purposes could be for recursion protection or
// other features, to avoid having to make a map from REBSER to REBOOL.
//
#define SERIES_INFO_BLACK \
    FLAGIT_LEFT(4)


//=//// SERIES_INFO_LOCKED ////////////////////////////////////////////////=//
//
// This indicates that the series size or values cannot be modified.  The
// check is honored by some layers of abstraction, but if one manages to get
// a raw non-const pointer into a value in the series data...then by that
// point it cannot be enforced.
//
// Note: There is a feature in PROTECT (TYPESET_FLAG_LOCKED) which protects a
// certain variable in a context from being changed.  It is similar, but
// distinct.  SERIES_INFO_LOCKED is a protection on a series itself--which
// ends up affecting all values with that series in the payload.
//
#define SERIES_INFO_LOCKED \
    FLAGIT_LEFT(5)


//=//// SERIES_INFO_INACCESSIBLE //////////////////////////////////////////=//
//
// This indicates that the memory pointed at by `->data` has "gone bad".
//
// Currently this used to note when a CONTEXT_FLAG_STACK series has had its
// stack level popped (there's no data to lookup for words bound to it).
//
// !!! The FFI also uses this for STRUCT! when an interface to a C structure
// is using external memory instead of a Rebol series, and that external
// memory goes away.  Since FFI is shifting to becoming a user extension, it
// might approach this problem in a different way in the future.
//
#define SERIES_INFO_INACCESSIBLE \
    FLAGIT_LEFT(6)


//=//// SERIES_INFO_POWER_OF_2 ////////////////////////////////////////////=//
//
// This is set when an allocation size was rounded to a power of 2.  The bit
// was introduced in Ren-C when accounting was added to make sure the system's
// notion of how much memory allocation was outstanding would balance out to
// zero by the time of exiting the interpreter.
//
// The problem was that the allocation size was measured in terms of the
// number of elements in the series.  If the elements themselves were not the
// size of a power of 2, then to get an even power-of-2 size of memory
// allocated, the memory block would not be an even multiple of the element
// size.  So rather than track the "actual" memory allocation size as a 32-bit
// number, a single bit flag remembering that the allocation was a power of 2
// was enough to recreate the number to balance accounting at free time.
//
// !!! The original code which created series with items which were not a
// width of a power of 2 was in the FFI.  It has been rewritten to not use
// such custom structures, but the support for this remains in case there
// was a good reason to have a non-power-of-2 size in the future.
//
// !!! ...but rationale for why series were ever allocated to a power of 2
// should be revisited.  Current conventional wisdom suggests that asking
// for the amount of memory you need and not using powers of 2 is
// generally a better idea:
//
// http://stackoverflow.com/questions/3190146/
//
#define SERIES_INFO_POWER_OF_2 \
    FLAGIT_LEFT(7)


#define SERIES_INFO_8_IS_FALSE FLAGIT_LEFT(8) // see Init_Endlike_Header()


//=//// SERIES_INFO_SHARED_KEYLIST ////////////////////////////////////////=//
//
// This is indicated on the keylist array of a context when that same array
// is the keylist for another object.  If this flag is set, then modifying an
// object using that keylist (such as by adding a key/value pair) will require
// that object to make its own copy.
//
// Note: This flag did not exist in R3-Alpha, so all expansions would copy--
// even if expanding the same object by 1 item 100 times with no sharing of
// the keylist.  That would make 100 copies of an arbitrary long keylist that
// the GC would have to clean up.
//
#define SERIES_INFO_SHARED_KEYLIST \
    FLAGIT_LEFT(9)


//=//// SERIES_INFO_EXTERNAL //////////////////////////////////////////////=//
//
// This indicates that when the series was created, the `->data` pointer was
// poked in by the creator.  It takes responsibility for freeing it, so don't
// free() on GC.
//
// !!! This is a somewhat questionable feature, only used by the FFI.  It's
// not clear that the right place to hook in the behavior is to have a
// series physically allow external `->data` pointers vs. at a higher level
// test some condition, using the series data or handle based on that.
//
#define SERIES_INFO_EXTERNAL \
    FLAGIT_LEFT(10)


// ^-- STOP AT FLAGIT_LEFT(15) --^
//
// The rightmost 16 bits of the series info is used to store an 8 bit length
// for non-dynamic series and an 8 bit width of the series.  So the info
// flags need to stop at FLAGIT_LEFT(15).
//
#if defined(__cplusplus) && (__cplusplus >= 201103L)
    static_assert(10 < 16, "SERIES_INFO_XXX too high");
#endif


//=////////////////////////////////////////////////////////////////////////=//
//
// SERIES NODE ("REBSER") STRUCTURE DEFINITION
//
//=////////////////////////////////////////////////////////////////////////=//
//
// A REBSER node is the size of two REBVALs, and there are 3 basic layouts
// which can be overlaid inside the node:
//
//      Dynamic: [header [allocation tracking] info link misc]
//     Singular: [header [REBVAL cell] info link misc]
//      Pairing: [[REBVAL cell] [REBVAL cell]]
//
// `info` is not the start of a "Rebol Node" (REBNODE, e.g. either a REBSER or
// a REBVAL cell).  But in the singular case it is positioned right where
// the next cell after the embedded cell *would* be.  Hence the bit in the
// info corresponding to NODE_FLAG_END is set, making it conform to the
// "terminating array" pattern.  To lower the risk of this implicit terminator
// being accidentally overwritten (which would corrupt link and misc), the
// bit corresponding to NODE_FLAG_CELL is clear.
//
// Singulars have widespread applications in the system, notably the
// efficient implementation of FRAME!.  They also narrow the gap in overhead
// between COMPOSE [A (B) C] vs. REDUCE ['A B 'C] such that the memory cost
// of the array is nearly the same as just having another value in the array.
//
// Pair REBSERs are allocated from the REBSER pool instead of their own to
// help exchange a common "currency" of allocation size more efficiently.
// They are planned for use in the PAIR! and MAP! datatypes, and anticipated
// to play a crucial part in the API--allowing a persistent handle for a
// GC'able REBVAL and associated "meta" value (which can be used for
// reference counting or other tracking.)
//
// Most of the time, code does not need to be concerned about distinguishing
// Pair from the Dynamic and Singular layouts--because it already knows
// which kind it has.  Only the GC needs to be concerned when marking
// and sweeping.
//

struct Reb_Series_Dynamic {
    //
    // `data` is the "head" of the series data.  It may not point directly at
    // the memory location that was returned from the allocator if it has
    // bias included in it.
    //
    REBYTE *data;

    // `len` is one past end of useful data.
    //
    REBCNT len;

    // `rest` is the total number of units from bias to end.  Having a
    // slightly weird name draws attention to the idea that it's not really
    // the "capacity", just the "rest of the capacity after the bias".
    //
    REBCNT rest;

    // This is the 4th pointer on 32-bit platforms which could be used for
    // something when a series is dynamic.  Previously the bias was not
    // a full REBCNT but was limited in range to 16 bits or so.  This means
    // 16 info bits are likely available if needed for dynamic series.
    //
    REBCNT bias;

#if defined(__LP64__) || defined(__LLP64__)
    //
    // The Reb_Series_Dynamic is used in Reb_Series inside of a union with a
    // REBVAL.  On 64-bit machines this will leave one unused 32-bit slot
    // (which will couple with the previous REBCNT) and one naturally aligned
    // 64-bit pointer.  These could be used for some enhancement that would
    // be available per-dynamic-REBSER on 64-bit architectures.
    //
    REBCNT unused_32;
    void *unused_64;
#endif
};


union Reb_Series_Content {
    //
    // If the series does not fit into the REBSER node, then it must be
    // dynamically allocated.  This is the tracking structure for that
    // dynamic data allocation.
    //
    struct Reb_Series_Dynamic dynamic;

    // If not SERIES_INFO_HAS_DYNAMIC, 0 or 1 length arrays can be held in
    // the series node.  This trick is accomplished via "implicit termination"
    // in the ->info bits that come directly after ->content.
    //
    // (See NODE_FLAG_END and NODE_FLAG_CELL for how this is done.)
    //
    RELVAL values[1];
};


struct Reb_Series {

    // The low 2 bits in the header must be 00 if this is an "ordinary" REBSER
    // node.  This allows such nodes to implicitly terminate a "doubular"
    // REBSER node, that is being used as storage for exactly 2 REBVALs.
    // As long as there aren't two of those REBSERs sequentially in the pool,
    // an unused node or a used ordinary one can terminate it.
    //
    // The other bit that is checked in the header is the USED bit, which is
    // bit #9.  This is set on all REBVALs and also in END marking headers,
    // and should be set in used series nodes.
    //
    // The remaining bits are free, and used to hold SYM values for those
    // words that have them.
    //
    struct Reb_Header header;

    // The `link` field is generally used for pointers to something that
    // when updated, all references to this series would want to be able
    // to see.
    //
    // This field is in the second pointer-sized slot in the REBSER node to
    // push the `content` so it is 64-bit aligned on 32-bit platforms.  This
    // is because a REBVAL may be the actual content, and a REBVAL assumes
    // it is on a 64-bit boundary to start with...in order to position its
    // "payload" which might need to be 64-bit aligned as well.
    //
    union {
        REBSER *hashlist; // MAP datatype uses this
        REBARR *keylist; // used by CONTEXT
        REBARR *schema; // for STRUCT (a REBFLD, parallels object's keylist)
        REBCTX *meta; // paramlists and keylists can store a "meta" object
        REBSTR *synonym; // circularly linked list of othEr-CaSed string forms
    } link;

    union Reb_Series_Content content;

    // `info` is the information about the series which needs to be known
    // even if it is not using a dynamic allocation.
    //
    // It is purposefully positioned in the structure directly after the
    // ->content field, because it has NODE_FLAG_END set to true.  Hence it
    // appears to terminate an array of values if the content is not dynamic.
    // Yet NODE_FLAG_CELL is set to false, so it is not a writable location
    // (an "implicit terminator").
    //
    // !!! Only 32-bits are used on 64-bit platforms.  There could be some
    // interesting added caching feature or otherwise that would use
    // it, while not making any feature specifically require a 64-bit CPU.
    //
    struct Reb_Header info;

    // The `misc` field is an extra pointer-sized piece of data which is
    // resident in the series node, and hence visible to all REBVALs that
    // might be referring to the series.
    //
    union {
        REBNAT dispatcher; // native dispatcher code, see Reb_Function's body
        REBCNT size;    // used for vectors and bitsets
        struct {
            REBCNT wide:16;
            REBCNT high:16;
        } area;
        REBOOL negated; // for bitsets (must be shared, can't be in REBVAL)
        REBFUN *underlying; // specialization -or- final underlying function
        REBFRM *f; // for a FRAME! series, the call frame (or NULL)
        void *fd; // file descriptor for library
        REBSTR *canon; // canon cased form of this symbol (if not canon)
        struct {
            REBINT high:16;
            REBINT low:16;
        } bind_index; // canon words hold index for binding--demo sharing 2
        CLEANUP_FUNC cleaner; // some HANDLE!s use this for GC finalization
    } misc;

#if !defined(NDEBUG)
    int *guard; // intentionally alloc'd and freed for use by Panic_Series
    REBUPT do_count; // also maintains sizeof(REBSER) % sizeof(REBI64) == 0
#endif
};


//=////////////////////////////////////////////////////////////////////////=//
//
// ARR_SERIES() COERCION
//
//=////////////////////////////////////////////////////////////////////////=//
//
// It is desirable to have series subclasses be different types, even though
// there are some common routines for processing them.  e.g. not every
// function that would take a REBSER* would actually be handled in the same
// way for a REBARR*.  Plus, just because a REBCTX* is implemented as a
// REBARR* with a link to another REBARR* doesn't mean most clients should
// be accessing the array--in a C++ build this would mean it would have some
// kind of protected inheritance scheme.
//
// The ARR_SERIES() macro provides a compromise besides a raw cast of a
// pointer to a REBSER*, because in the C++ build it makes sure that the
// incoming pointer type is to a simple series subclass.  (It's just a raw
// cast in the C build.)
//

#if defined(__cplusplus) && __cplusplus >= 201103L
    #include <type_traits>

    template <class T>
    inline REBSER* AS_SERIES(T *p) {
        static_assert(
            std::is_same<T, REBSER>::value
            || std::is_same<T, REBSTR>::value
            || std::is_same<T, REBARR>::value,
            "AS_SERIES works on: REBSER*, REBSTR*, REBARR*"
        );
        return cast(REBSER*, p);
    }
#else
    #define AS_SERIES(p) cast(REBSER*, (p))
#endif