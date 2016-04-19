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
**  Module:  m-stack.c
**  Summary: data and function call stack implementation
**  Section: memory
**  Notes:
**
***********************************************************************/

#include "sys-core.h"


#define CHUNKER_FROM_CHUNK(c) \
    cast(struct Reb_Chunker*, \
        cast(REBYTE*, (c)) - (c)->offset \
        - offsetof(struct Reb_Chunker, payload))

//
//  Init_Stacks: C
//
void Init_Stacks(REBCNT size)
{
    // We always keep one chunker around for the first chunk push, and prep
    // one chunk so that the push and drop routines never worry about testing
    // for the empty case.

    TG_Root_Chunker = cast(struct Reb_Chunker*, Alloc_Mem(BASE_CHUNKER_SIZE + CS_CHUNKER_PAYLOAD));
#if !defined(NDEBUG)
    memset(TG_Root_Chunker, 0xBD, sizeof(struct Reb_Chunker));
#endif
    TG_Root_Chunker->next = NULL;
    TG_Root_Chunker->size = CS_CHUNKER_PAYLOAD;
    TG_Top_Chunk = cast(struct Reb_Chunk*, &TG_Root_Chunker->payload);
    TG_Top_Chunk->prev = NULL;
    TG_Top_Chunk->size.bits = BASE_CHUNK_SIZE; // zero values for initial chunk
    TG_Top_Chunk->offset = 0;

    // Implicit termination trick--see VALUE_FLAG_NOT_END and related notes
    cast(
        struct Reb_Chunk*, cast(REBYTE*, TG_Top_Chunk) + BASE_CHUNK_SIZE
    )->size.bits = 0;
    assert(IS_END(&TG_Top_Chunk->values[0]));

    // Start the data stack out with just one element in it, and make it an
    // unwritable trash for the debug build.  This helps avoid both accidental
    // reads and writes of an empty stack, as well as meaning that indices
    // into the data stack can be unsigned (no need for -1 to mean empty,
    // because 0 can)
    {
        DS_Array = Make_Array(1);
        DS_Movable_Base = ARR_HEAD(DS_Array);

        SET_TRASH_SAFE(ARR_HEAD(DS_Array));

    #if !defined(NDEBUG)
        MARK_VAL_UNWRITABLE_DEBUG(ARR_HEAD(DS_Array));
    #endif

        // The END marker will signal DS_PUSH that it has run out of space,
        // and it will perform the allocation at that time.
        //
        SET_ARRAY_LEN(DS_Array, 1);
        SET_END(ARR_TAIL(DS_Array));
        ASSERT_ARRAY(DS_Array);

        // Reuse the expansion logic that happens on a DS_PUSH to get the
        // initial stack size.  It requires you to be on an END to run.  Then
        // drop the hypothetical thing pushed.
        //
        DS_Index = 1;
        Expand_Data_Stack_May_Fail(size);
        DS_DROP;
    }

    // !!! Historically the data stack used a "special GC" because it was
    // not always terminated with an END marker.  It also had some fixed
    // sized assumptions about how much it would grow during a function
    // call which let it not check to see if it needed to expand on every
    // push.  Ren-C turned it into an ordinary series and sought to pin
    // other things down first, but there may be some optimizations that
    // get added back in--hopefully that will benefit all series.
    //
    Set_Root_Series(TASK_STACK, ARR_SERIES(DS_Array));

    // Call stack (includes pending functions, parens...anything that sets
    // up a `struct Reb_Frame` and calls Do_Core())  Singly linked.
    //
    TG_Frame_Stack = NULL;
}


//
//  Shutdown_Stacks: C
//
void Shutdown_Stacks(void)
{
    assert(FS_TOP == NULL);
    assert(DSP == 0); // !!! Why not free data stack here?

    assert(TG_Top_Chunk == cast(struct Reb_Chunk*, &TG_Root_Chunker->payload));

    // Because we always keep one chunker of headroom allocated, and the
    // push/drop is not designed to manage the last chunk, we *might* have
    // that next chunk of headroom still allocated.
    //
    if (TG_Root_Chunker->next)
        Free_Mem(TG_Root_Chunker->next, TG_Root_Chunker->next->size + BASE_CHUNKER_SIZE);

    // OTOH we always have to free the root chunker.
    //
    Free_Mem(TG_Root_Chunker, TG_Root_Chunker->size + BASE_CHUNKER_SIZE);
}


//
//  Expand_Data_Stack_May_Fail: C
//
// The data stack maintains an invariant that you may never push an END to it.
// So each push looks to see if it's pushing to a cell that contains an END
// and if so requests an expansion.
//
// WARNING: This will invalidate any extant pointers to REBVALs living in
// the stack.  It is for this reason that stack access should be done by
// REBDSP "data stack pointers" and not by REBVAL* across *any* operation
// which could do a push or pop.  (Currently stable w.r.t. pop but there may
// be compaction at some point.)
//
void Expand_Data_Stack_May_Fail(REBCNT amount)
{
    REBCNT len_old = ARR_LEN(DS_Array);
    REBCNT len_new;
    REBCNT n;
    REBVAL *value;

    // The current requests for expansion should only happen when the stack
    // is at its end.  Sanity check that.
    //
    assert(IS_END(DS_TOP));
    assert(DS_TOP == ARR_TAIL(DS_Array));
    assert(DS_TOP - ARR_HEAD(DS_Array) == len_old);

    // If adding in the requested amount would overflow the stack limit, then
    // give a data stack overflow error.
    //
    if (SER_REST(ARR_SERIES(DS_Array)) + amount >= STACK_LIMIT)
        Trap_Stack_Overflow();

    Extend_Series(ARR_SERIES(DS_Array), amount);

    // Update the global pointer representing the base of the stack that
    // likely was moved by the above allocation.  (It's not necessarily a
    // huge win to cache it, but it turns data stack access from a double
    // dereference into a single dereference in the common case, and it was
    // how R3-Alpha did it).
    //
    DS_Movable_Base = ARR_HEAD(DS_Array); // must do before using DS_TOP

    // We fill in the data stack with "GC safe trash" (which is unset it the
    // release build, but will raise an alarm if VAL_TYPE() called on it in
    // the debug build).  In order to serve as a marker for the stack slot
    // being available, it merely must not be IS_END()...
    //
    value = DS_TOP;
    len_new = len_old + amount;
    for (n = len_old; n < len_new; ++n) {
        SET_TRASH_SAFE(value);
        ++value;
    }

    // Update the end marker to serve as the indicator for when the next
    // stack push would need to expand.
    //
    SET_END(value);
    SET_ARRAY_LEN(DS_Array, len_new);
    ASSERT_ARRAY(DS_Array);
}


//
//  Pop_Stack_Values: C
//
// Pops computed values from the stack to make a new ARRAY.
//
REBARR *Pop_Stack_Values(REBDSP dsp_start)
{
    REBCNT len = DSP - dsp_start;
    REBVAL *values = ARR_AT(DS_Array, dsp_start + 1);

    REBARR *array = Copy_Values_Len_Shallow(values, len);

    DS_DROP_TO(dsp_start);
    return array;
}


//
//  Pop_Stack_Values_Into: C
//
// Pops computed values from the stack into an existing ANY-ARRAY.  The
// index of that array will be updated to the insertion tail (/INTO protocol)
//
void Pop_Stack_Values_Into(REBVAL *into, REBDSP dsp_start) {
    REBCNT len = DSP - dsp_start;
    REBVAL *values = ARR_AT(DS_Array, dsp_start + 1);

    assert(ANY_ARRAY(into));
    FAIL_IF_LOCKED_ARRAY(VAL_ARRAY(into));

    VAL_INDEX(into) = Insert_Series(
        ARR_SERIES(VAL_ARRAY(into)),
        VAL_INDEX(into),
        cast(REBYTE*, values),
        len // multiplied by width (sizeof(REBVAL)) in Insert_Series
    );

    DS_DROP_TO(dsp_start);
}


//
//  Push_Ended_Trash_Chunk: C
//
// This doesn't necessarily call Alloc_Mem, because chunks are allocated
// sequentially inside of "chunker" blocks, in their ordering on the stack.
// Allocation is only required if we need to step into a new chunk (and even
// then only if we aren't stepping into a chunk that we are reusing from
// a prior expansion).
//
// The "Ended" indicates that there is no need to manually put an end in the
// `num_values` slot.  Chunks are implicitly terminated by their layout,
// because the pointer which indicates the previous chunk on the next chunk
// always has its low bit clear (pointers are not odd on 99% of architectures,
// this is checked by an assertion).
//
REBVAL* Push_Ended_Trash_Chunk(REBCNT num_values, REBARR *opt_holder) {
    const REBCNT size = BASE_CHUNK_SIZE + num_values * sizeof(REBVAL);

    // an extra Reb_Value_Header is placed at the very end of the array to
    // denote a block terminator without a full REBVAL
    const REBCNT size_with_terminator = size + sizeof(struct Reb_Value_Header);

    struct Reb_Chunker *chunker = CHUNKER_FROM_CHUNK(TG_Top_Chunk);

    struct Reb_Chunk *chunk;

    // Establish invariant where 'chunk' points to a location big enough to
    // hold the data (with data's size accounted for in chunk_size).  Note
    // that TG_Top_Chunk is never NULL, due to the initialization leaving
    // one empty chunk at the beginning and manually destroying it on
    // shutdown (this simplifies Push)
    const REBCNT payload_left = chunker->size - TG_Top_Chunk->offset
          - TG_Top_Chunk->size.bits;

    assert(chunker->size >= CS_CHUNKER_PAYLOAD);

    if (payload_left >= size_with_terminator) {
        //
        // Topmost chunker has space for the chunk *and* a pointer with the
        // END marker bit (e.g. last bit 0).  So advance past the topmost
        // chunk (whose size will depend upon num_values)
        //
        chunk = cast(struct Reb_Chunk*,
            cast(REBYTE*, TG_Top_Chunk) + TG_Top_Chunk->size.bits
        );

        // top's offset accounted for previous chunk, account for ours
        //
        chunk->offset = TG_Top_Chunk->offset + TG_Top_Chunk->size.bits;
    }
    else {
        //
        // Topmost chunker has insufficient space
        //

        REBOOL need_alloc = TRUE;
        if (chunker->next) {
            //
            // Previously allocated chunker exists already, check if it is big
            // enough
            //
            assert(!chunker->next->next);
            if (chunker->next->size >= size_with_terminator)
                need_alloc = FALSE;
            else
                Free_Mem(chunker->next, chunker->next->size + BASE_CHUNKER_SIZE);
        }
        if (need_alloc) {
            // No previously allocated chunker...we have to allocate it
            //
            const REBCNT payload_size = BASE_CHUNKER_SIZE
                + (size_with_terminator < CS_CHUNKER_PAYLOAD ?
                    CS_CHUNKER_PAYLOAD : (size_with_terminator << 1));
            chunker->next = cast(struct Reb_Chunker*, Alloc_Mem(payload_size));
            chunker->next->next = NULL;
            chunker->next->size = payload_size - BASE_CHUNKER_SIZE;
        }

        assert(chunker->next->size >= size_with_terminator);

        chunk = cast(struct Reb_Chunk*, &chunker->next->payload);
        chunk->offset = 0;
    }

    // The size does double duty to terminate the previous chunk's REBVALs
    // so that a full-sized REBVAL that is largely empty isn't needed to
    // convey IS_END().  It must yield its lowest two bits as zero to serve
    // this purpose, so WRITABLE_MASK_DEBUG and NOT_END_MASK will both
    // be false.  Our chunk should be a multiple of 4 bytes in total size,
    // but check that here with an assert.
    //
    // The memory address for the chunk size matches that of a REBVAL's
    // `header`, but since a `struct Reb_Chunk` is distinct from a REBVAL
    // they won't necessarily have write/read coherence, even though the
    // fields themselves are the same type.  Taking the address of the size
    // creates a pointer, which without a `restrict` keyword is defined as
    // being subject to "aliasing".  Hence a write to the pointer could affect
    // *any* other value of that type.  This is necessary for the trick.
    {
        struct Reb_Value_Header *alias = &chunk->size;
        assert(size % 4 == 0);
        alias->bits = size;
    }

    // Set size also in next element to 0, so it can serve as a terminator
    // for the data range of this until it gets its real size (if ever)
    {
        // See note above RE: aliasing.
        //
        struct Reb_Value_Header *alias = &cast(
            struct Reb_Chunk*,
            cast(REBYTE*, chunk) + size)->size;
        alias->bits = 0;
        assert(IS_END(&chunk->values[num_values]));
    }

    chunk->prev = TG_Top_Chunk;

    chunk->opt_context = NULL;

    TG_Top_Chunk = chunk;

#if !defined(NDEBUG)
    //
    // In debug builds we make sure we put in GC-unsafe trash in the chunk.
    // This helps make sure that the caller fills in the values before a GC
    // ever actually happens.  (We could set it to UNSET! or something
    // GC-safe, but that might wind up being wasted work if unset is not
    // what the caller was wanting...so leave it to them.)
    {
        REBCNT index;
        for (index = 0; index < num_values; index++)
            VAL_INIT_WRITABLE_DEBUG(&chunk->values[index]);
    }
#endif

    assert(CHUNK_FROM_VALUES(&chunk->values[0]) == chunk);
    return &chunk->values[0];
}


//
//  Drop_Chunk: C
//
// Free an array of previously pushed REBVALs that are protected by GC.  This
// only occasionally requires an actual call to Free_Mem(), due to allocating
// call these arrays sequentially inside of chunks in memory.
//
void Drop_Chunk(REBVAL *opt_head)
{
    struct Reb_Chunk* chunk = TG_Top_Chunk;

    // Passing in `values` is optional, but a good check to make sure you are
    // actually dropping the chunk you think you are.  (On an error condition
    // when dropping chunks to try and restore the top chunk to a previous
    // state, this information isn't available because the call frame data
    // containing the chunk pointer has been longjmp'd past into oblivion.)
    //
    assert(!opt_head || CHUNK_FROM_VALUES(opt_head) == chunk);

    if (chunk->opt_context) {
        REBARR *varlist = CTX_VARLIST(chunk->opt_context);
        assert(
            !GET_ARR_FLAG(varlist, SERIES_FLAG_HAS_DYNAMIC)
            && GET_ARR_FLAG(varlist, CONTEXT_FLAG_STACK)
            && GET_ARR_FLAG(varlist, SERIES_FLAG_ARRAY)
        );
        assert(GET_ARR_FLAG(varlist, SERIES_FLAG_ACCESSIBLE));
        assert(
            CTX_STACKVARS(chunk->opt_context) == &TG_Top_Chunk->values[0]
        );
        CLEAR_ARR_FLAG(varlist, SERIES_FLAG_ACCESSIBLE);

    #if !defined(NDEBUG)
        //
        // The general idea of the "canon" values inside of ANY-CONTEXT!
        // and ANY-FUNCTION! at their slot [0] positions of varlist and
        // paramlist respectively was that all REBVAL instances of that
        // context or object would mirror those bits.  Because we have
        // SERIES_FLAG_ACCESSIBLE then it's possible to keep this invariant
        // and let a stale stackvars pointer be bad inside the context to
        // match any extant REBVALs, but debugging will be more obvious if
        // the bits are deliberately set to bad--even if this is incongruous
        // with those values.  Thus there is no check that these bits line
        // up and we turn the ones in the context itself to garbage here.
        //
        CTX_STACKVARS(chunk->opt_context) = cast(REBVAL*, 0xDECAFBAD);
    #endif
    }

    // Drop to the prior top chunk
    TG_Top_Chunk = chunk->prev;

    if (chunk->offset == 0) {
        // This chunk sits at the head of a chunker.

        struct Reb_Chunker *chunker = CHUNKER_FROM_CHUNK(chunk);

        assert(TG_Top_Chunk);

        // When we've completely emptied a chunker, we check to see if the
        // chunker after it is still live.  If so, we free it.  But we
        // want to keep *this* just-emptied chunker alive for overflows if we
        // rapidly get another push, to avoid Make_Mem()/Free_Mem() costs.

        if (chunker->next) {
            Free_Mem(chunker->next, chunker->next->size + BASE_CHUNKER_SIZE);
            chunker->next = NULL;
        }
    }

    // In debug builds we poison the memory for the chunk... but not the `prev`
    // pointer because we expect that to stick around!
    //
#if !defined(NDEBUG)
    memset(
        cast(REBYTE*, chunk) + sizeof(struct Reb_Chunk*),
        0xBD,
        chunk->size.bits - sizeof(struct Reb_Chunk*)
    );
    assert(IS_END(cast(REBVAL*, chunk)));
#endif
}


//
//  Push_Or_Alloc_Vars_For_Call: C
//
// Allocate the series of REBVALs inspected by a function when executed (the
// values behind D_ARG(1), D_REF(2), etc.)  Since the call contains the
// REBFUN pointer, it is known how many parameters are needed.
//
// The call frame will be pushed onto the call stack, and hence its fields
// will be seen by the GC and protected.
// 
// However...we do not set the frame as "Running" at the same time we create
// it.  We need to fulfill its arguments in the caller's frame before we
// actually invoke the function, so it's Dispatch_Call that actually moves
// it to the running status.
//
void Push_Or_Alloc_Vars_For_Call(struct Reb_Frame *f) {
    REBVAL *slot;
    REBCNT num_slots;
    REBARR *varlist;
    REBFUN *actual_func;
    REBVAL *special_arg;

    // Should not already have any vars.  We zero out the union field for
    // the chunk, so that's the one we should check.
    //
#if !defined(NDEBUG)
    assert(!f->data.stackvars);
#endif

    if (FUNC_CLASS(f->func) == FUNC_CLASS_SPECIALIZED) {
        actual_func = CTX_FRAME_FUNC(
            FUNC_VALUE(f->func)->payload.function.impl.special
        );
        special_arg = CTX_VARS_HEAD(
            FUNC_VALUE(f->func)->payload.function.impl.special
        );
    }
    else {
        actual_func = f->func;
        special_arg = NULL;
    }

    // `num_vars` is the total number of elements in the series, including the
    // function's "Self" REBVAL in the 0 slot.
    //
    num_slots = FUNC_NUM_PARAMS(actual_func);

    // For starters clear the context flag; it's just the chunk with no
    // "reification" (Context_For_Frame_May_Reify() might change this)
    //
    f->flags &= ~DO_FLAG_FRAME_CONTEXT;

    // Make REBVALs to hold the arguments.  It will always be at least one
    // slot long, because function frames start with the value of the
    // function in slot 0.
    //
    if (IS_FUNC_DURABLE(FUNC_VALUE(actual_func))) {
        //
        // !!! In the near term, it's hoped that CLOSURE! will go away and
        // that stack frames can be "hybrids" with some pooled allocated
        // vars that survive a call, and some that go away when the stack
        // frame is finished.  The groundwork for this is laid but it's not
        // quite ready--so the classic interpretation is that it's all or
        // nothing... CLOSURE!'s variables args and locals all survive the
        // end of the call, and none of a FUNCTION!'s do.
        //
        varlist = Make_Array(num_slots + 1);
        SET_ARRAY_LEN(varlist, num_slots + 1);
        SET_END(ARR_AT(varlist, num_slots + 1));
        SET_ARR_FLAG(varlist, SERIES_FLAG_FIXED_SIZE);

        // Skip the [0] slot which will be filled with the CTX_VALUE
        //
        slot = ARR_AT(varlist, 1);

        // The NULL stackvars will be picked up by the reification; reuse the
        // work that function does vs. duplicating it here.
        //
        f->data.stackvars = NULL;
    }
    else {
        // We start by allocating the data for the args and locals on the chunk
        // stack.  However, this can be "promoted" into being the data for a
        // frame context if it becomes necessary to refer to the variables
        // via words or an object value.  That object's data will still be this
        // chunk, but the chunk can be freed...so the words can't be looked up.
        //
        // Note that chunks implicitly have an END at the end; no need to
        // put one there.
        //
        f->data.stackvars = Push_Ended_Trash_Chunk(num_slots, NULL);
        assert(CHUNK_LEN_FROM_VALUES(f->data.stackvars) == num_slots);
        slot = &f->data.stackvars[0];

        // For now there's no hybridization; a context with stackvars has
        // no pooled allocation.
        //
        varlist = NULL;
    }

    // Make_Call does not fill the args in the frame--that's up to Do_Core
    // and Apply_Block as they go along.  But the frame has to survive
    // Recycle() during arg fulfillment, slots can't be left uninitialized.
    // It is important to set to UNSET for bookkeeping so that refinement
    // scanning knows when it has filled a refinement slot (and hence its
    // args) or not.
    //
    // !!! Filling with specialized args could be done via a memcpy; doing
    // an unset only writes 1 out of 4 pointer-sized values in release build
    // so maybe faster than a memset (if unsets were the pattern of a uniform
    // byte, currently not true)
    //
    while (num_slots--) {
        //
        // In Rebol2 and R3-Alpha, unused refinement arguments were set to
        // NONE! (and refinements were TRUE as opposed to the WORD! of the
        // refinement itself).  We captured the state of the legacy flag at
        // the time of function creation, so that both kinds of functions
        // can coexist at the same time.
        //
        if (special_arg) {
            *slot = *special_arg;
            ++special_arg;
        }
        else
            SET_BAR(slot);

        slot++;
    }

    if (varlist) {
        //
        // If we had to create a pooled array allocation to store any vars
        // that will outlive the series, there's no way to avoid reifying
        // the context (have to hold onto the allocated varlist pointer
        // somewhere...)
        //
        Context_For_Frame_May_Reify(f, varlist, FALSE);
    }
}


//
//  Context_For_Frame_May_Reify: C
//
// A Reb_Frame does not allocate a REBSER for its frame to be used in the
// context by default.  But one can be allocated on demand, even for a NATIVE!
// in order to have a binding location for the debugger (for instance).
// If it becomes necessary to create words bound into the frame that is
// another case where the frame needs to be brought into existence.
//
// If there's already a frame this will return it, otherwise create it.
//
REBCTX *Context_For_Frame_May_Reify(
    struct Reb_Frame *f,
    REBARR *opt_varlist, // if a CLOSURE! and varlist is preallocated
    REBOOL ensure_managed
) {
    REBCTX *context;
    struct Reb_Chunk *chunk;

    if (f->flags & DO_FLAG_FRAME_CONTEXT)
        return f->data.context;

    if (opt_varlist) {
        //
        // This is an a-priori creation of pooled data... arg isn't ready to
        // check yet.
        //
    #if !defined(NDEBUG)
        assert(f->mode == CALL_MODE_GUARD_ARRAY_ONLY); // APPLY doesn't init
    #endif

        context = AS_CONTEXT(opt_varlist);
        assert(GET_ARR_FLAG(AS_ARRAY(context), SERIES_FLAG_HAS_DYNAMIC));
    }
    else {
        assert(f->mode != CALL_MODE_GUARD_ARRAY_ONLY);
        context = AS_CONTEXT(Make_Series(
            1, // length report will not come from this, but from end marker
            sizeof(REBVAL),
            MKS_NO_DYNAMIC // use the REBVAL in the REBSER--no allocation
        ));

        assert(!GET_ARR_FLAG(AS_ARRAY(context), SERIES_FLAG_HAS_DYNAMIC));
    }

    SET_ARR_FLAG(AS_ARRAY(context), SERIES_FLAG_ARRAY);
    SET_ARR_FLAG(CTX_VARLIST(context), ARRAY_FLAG_CONTEXT_VARLIST);

    // We have to set the lock flag on the series as long as it is on
    // the stack.  This means that no matter what cleverness the GC
    // might think it can do shuffling data around, the closure frame
    // is not a candidate for this cleverness.
    //
    // !!! Review the overall philosophy of not allowing the frame of
    // functions/closures to grow.  It is very likely a good idea, but
    // there may be reasons to introduce some kind of flexibility.
    //
    SET_ARR_FLAG(CTX_VARLIST(context), SERIES_FLAG_FIXED_SIZE);

    // We do not Manage_Context, because we are reusing a word series here
    // that has already been managed.  The arglist array was managed when
    // created and kept alive by Mark_Call_Frames
    //
    INIT_CTX_KEYLIST_SHARED(context, FUNC_PARAMLIST(f->func));
    ASSERT_ARRAY_MANAGED(CTX_KEYLIST(context));

    // We do not manage the varlist, because we'd like to be able to free
    // it *if* nothing happens that causes it to be managed.  Note that
    // initializing word REBVALs that are bound into it will ensure
    // managedness, as will creating a REBVAL for it.
    //
    if (ensure_managed)
        ENSURE_ARRAY_MANAGED(CTX_VARLIST(context));
    else {
        // Might there be a version that doesn't ensure but also accepts if
        // it happens to be managed?  (Current non-ensuring client assumes
        // it's not managed...
        //
        assert(!GET_ARR_FLAG(CTX_VARLIST(context), SERIES_FLAG_MANAGED));
    }

    // When in CALL_MODE_PENDING or CALL_MODE_FUNCTION, the arglist will
    // be marked safe from GC.  It is managed because the pointer makes
    // its way into bindings that ANY-WORD! values may have, and they
    // need to not crash.
    //
    // !!! Note that theoretically pending mode arrays do not need GC
    // access as no running could could get them, but the debugger is
    // able to access this information.  GC protection for pending
    // frames could be issued on demand by the debugger, however.
    //
    VAL_RESET_HEADER(CTX_VALUE(context), REB_FRAME);
    INIT_VAL_CONTEXT(CTX_VALUE(context), context);
    INIT_CONTEXT_FRAME(context, f);

    // Give this series the data from what was in the chunk, and make note
    // of the series in the chunk so that it can be marked as "gone bad"
    // when that chunk gets freed (could happen during a fail() or when
    // the stack frame finishes normally)
    //
    CTX_STACKVARS(context) = f->data.stackvars;
    if (f->data.stackvars) {
        assert(!opt_varlist);

        chunk = CHUNK_FROM_VALUES(f->data.stackvars);
        assert(!chunk->opt_context);
        chunk->opt_context = context;

        SET_CTX_FLAG(context, CONTEXT_FLAG_STACK);
        SET_CTX_FLAG(context, SERIES_FLAG_ACCESSIBLE);
    }
    else
        assert(opt_varlist);

    // A reification of a frame for native code should not allow changing
    // the values out from under it, because that could cause it to crash
    // the interpreter.  (Generally speaking, modification should only be
    // possible in the debugger anyway.)  For now, protect unless it's a
    // user function.
    //
    if (VAL_FUNC_CLASS(FUNC_VALUE(f->func)) != FUNC_CLASS_USER)
        SET_ARR_FLAG(AS_ARRAY(context), SERIES_FLAG_LOCKED);

    // Finally we mark the flags to say this contains a valid frame, so that
    // future calls to this routine will return it instead of making another.
    // This flag must be cleared when the call is finished (as the Reb_Frame
    // will be blown away if there's an error, no concerns about that).
    //
    ASSERT_CONTEXT(context);
    f->data.context = context;
    f->flags |= DO_FLAG_FRAME_CONTEXT;

    return context;
}


#if !defined(NDEBUG)

//
//  FRM_ARG_Debug: C
// 
// Debug-only version of getting a variable out of a call
// frame, which asserts if you use an index that is higher
// than the number of arguments in the frame.
//
REBVAL *FRM_ARG_Debug(struct Reb_Frame *frame, REBCNT n)
{
    assert(n != 0 && n <= FRM_NUM_ARGS(frame));
    return &frame->arg[n - 1];
}

#endif
