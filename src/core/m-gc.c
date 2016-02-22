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
**  Module:  m-gc.c
**  Summary: main memory garbage collection
**  Section: memory
**  Author:  Carl Sassenrath, Ladislav Mecir, HostileFork
**  Notes:
**
**      The garbage collector is based on a conventional "mark and sweep":
**
**          https://en.wikipedia.org/wiki/Tracing_garbage_collection
**
**      From an optimization perspective, there is an attempt to not incur
**      function call overhead just to check if a GC-aware item has its
**      SER_MARK flag set.  So the flag is checked by a macro before making
**      any calls to process the references inside of an item.
**
**      "Shallow" marking only requires setting the flag, and is suitable for
**      series like strings (which are not containers for other REBVALs).  In
**      debug builds shallow marking is done with a function anyway, to give
**      a place to put assertion code or set breakpoints to catch when a
**      shallow mark is set (when that is needed).
**
**      "Deep" marking was originally done with recursion, and the recursion
**      would stop whenever a mark was hit.  But this meant deeply nested
**      structures could quickly wind up overflowing the C stack.  Consider:
**
**          a: copy []
**          loop 200'000 [a: append/only copy [] a]
**          recycle
**
**      The simple solution is that when an unmarked item is hit that it is
**      marked and put into a queue for processing (instead of recursed on the
**      spot.  This queue is then handled as soon as the marking stack is
**      exited, and the process repeated until no more items are queued.
**
**    Regarding the two stages:
**
**      MARK -  Mark all series and gobs ("collectible values")
**              that can be found in:
**
**              Root Block: special structures and buffers
**              Task Block: special structures and buffers per task
**              Data Stack: current state of evaluation
**              Safe Series: saves the last N allocations
**
**      SWEEP - Free all collectible values that were not marked.
**
**    GC protection methods:
**
**      KEEP flag - protects an individual series from GC, but
**          does not protect its contents (if it holds values).
**          Reserved for non-block system series.
**
**      Root_Vars - protects all series listed. This list is
**          used by Sweep as the root of the in-use memory tree.
**          Reserved for important system series only.
**
**      Task_Vars - protects all series listed. This list is
**          the same as Root, but per the current task context.
**
**      Save_Series - protects temporary series. Used with the
**          SAVE_SERIES and UNSAVE_SERIES macros. Throws and errors
**          must roll back this series to avoid "stuck" memory.
**
**      Safe_Series - protects last MAX_SAFE_SERIES series from GC.
**          Can only be used if no deeply allocating functions are
**          called within the scope of its protection. Not affected
**          by throws and errors.
**
**      Data_Stack - all values in the data stack that are below
**          the TOP (DSP) are automatically protected. This is a
**          common protection method used by native functions.
**
**      DONE flag - do not scan the series; it has no links.
**
***********************************************************************/

#include "sys-core.h"
#include "reb-evtypes.h"

//-- For Serious Debugging:
#ifdef WATCH_GC_VALUE
REBSER *Watcher = 0;
REBVAL *WatchVar = 0;
REBVAL *GC_Break_Point(REBVAL *val) {return val;}
#endif

// This can be put below
#ifdef WATCH_GC_VALUE
            if (Watcher && ser == Watcher)
                GC_Break_Point(val);

        // for (n = 0; n < depth * 2; n++) Prin_Str(" ");
        // Mark_Count++;
        // Print("Mark: %s %x", TYPE_NAME(val), val);
#endif

// was static, but exported for Ren/C
/* static void Queue_Mark_Value_Deep(const REBVAL *val);*/

static void Push_Array_Marked_Deep(REBARR *array);

#ifndef NDEBUG
static void Mark_Series_Only_Debug(REBSER *ser);
#endif

//
//  Push_Array_Marked_Deep: C
// 
// Note: Call MARK_ARRAY_DEEP or QUEUE_MARK_ARRAY_DEEP instead!
// 
// Submits the block into the deferred stack to be processed later
// with Propagate_All_GC_Marks().  We have already set this series
// mark as it's now "spoken for".  (Though we haven't marked its
// dependencies yet, we want to prevent it from being wastefully
// submitted multiple times by another reference that would still
// see it as "unmarked".)
// 
// The data structure used for this processing is a stack and not
// a queue (for performance reasons).  But when you use 'queue'
// as a verb it has more leeway than as the CS noun, and can just
// mean "put into a list for later processing", hence macro names.
//
static void Push_Array_Marked_Deep(REBARR *array)
{
#if !defined(NDEBUG)
    if (!ARRAY_GET_FLAG(array, SER_MANAGED)) {
        Debug_Fmt("Link to non-MANAGED item reached by GC");
        Panic_Array(array);
    }
#endif

    assert(!ARRAY_GET_FLAG(array, SER_EXTERNAL));
    assert(ARRAY_GET_FLAG(array, SER_ARRAY));

    // set by calling macro (helps catch direct calls of this function)
    assert(ARRAY_GET_FLAG(array, SER_MARK));

    // Add series to the end of the mark stack series and update terminator

    if (SERIES_FULL(GC_Mark_Stack)) Extend_Series(GC_Mark_Stack, 8);

    cast(REBARR **, SERIES_DATA(GC_Mark_Stack))[
        SERIES_LEN(GC_Mark_Stack)
    ] = array;

    SET_SERIES_LEN(GC_Mark_Stack, SERIES_LEN(GC_Mark_Stack) + 1);

    cast(REBARR **, SERIES_DATA(GC_Mark_Stack))[
        SERIES_LEN(GC_Mark_Stack)
    ] = NULL;
}


static void Propagate_All_GC_Marks(void);

#ifndef NDEBUG
    static REBOOL in_mark = FALSE;
#endif

// NOTE: The following macros uses S parameter multiple times, hence if S has
// side effects this will run that side-effect multiply.

// Deferred form for marking series that prevents potentially overflowing the
// C execution stack.

#define QUEUE_MARK_ARRAY_DEEP(a) \
    do { \
        if (!ARRAY_GET_FLAG((a), SER_MARK)) { \
            ARRAY_SET_FLAG((a), SER_MARK); \
            Push_Array_Marked_Deep(a); \
        } \
    } while (0)


#define QUEUE_MARK_CONTEXT_DEEP(c) \
    do { \
        assert(ARRAY_GET_FLAG(CONTEXT_VARLIST(c), SER_CONTEXT)); \
        QUEUE_MARK_ARRAY_DEEP(CONTEXT_KEYLIST(c)); \
        QUEUE_MARK_ARRAY_DEEP(CONTEXT_VARLIST(c)); \
    } while (0)


// Non-Queued form for marking blocks.  Used for marking a *root set item*,
// don't recurse from within Mark_Value/Mark_Gob/Mark_Array_Deep/etc.

#define MARK_ARRAY_DEEP(a) \
    do { \
        assert(!in_mark); \
        QUEUE_MARK_ARRAY_DEEP(a); \
        Propagate_All_GC_Marks(); \
    } while (0)

#define MARK_CONTEXT_DEEP(c) \
    do { \
        assert(!in_mark); \
        QUEUE_MARK_CONTEXT_DEEP(c); \
        Propagate_All_GC_Marks(); \
    } while (0)


// Non-Deep form of mark, to be used on non-BLOCK! series or a block series
// for which deep marking is not necessary (such as an 'typed' words block)

#ifdef NDEBUG
    #define MARK_SERIES_ONLY(s) SERIES_SET_FLAG((s), SER_MARK)
#else
    #define MARK_SERIES_ONLY(s) Mark_Series_Only_Debug(s)
#endif


// Assertion for making sure that all the deferred marks have been propagated

#define ASSERT_NO_GC_MARKS_PENDING() \
    assert(SERIES_LEN(GC_Mark_Stack) == 0)


#if !defined(NDEBUG)
//
//  Mark_Series_Only_Debug: C
// 
// Hook point for marking and tracing a single series mark.
//
static void Mark_Series_Only_Debug(REBSER *series)
{
    if (!SERIES_GET_FLAG(series, SER_MANAGED)) {
        Debug_Fmt("Link to non-MANAGED item reached by GC");
        Panic_Series(series);
    }

    SERIES_SET_FLAG(series, SER_MARK);
}
#endif


//
//  Queue_Mark_Gob_Deep: C
// 
// 'Queue' refers to the fact that after calling this routine,
// one will have to call Propagate_All_GC_Marks() to have the
// deep transitive closure be guaranteed fully marked.
// 
// Note: only referenced blocks are queued, the GOB structure
// itself is processed via recursion.  Deeply nested GOBs could
// in theory overflow the C stack.
//
static void Queue_Mark_Gob_Deep(REBGOB *gob)
{
    REBGOB **pane;
    REBCNT i;

    if (IS_GOB_MARK(gob)) return;

    MARK_GOB(gob);

    if (GOB_PANE(gob)) {
        SERIES_SET_FLAG(GOB_PANE(gob), SER_MARK);
        pane = GOB_HEAD(gob);
        for (i = 0; i < GOB_LEN(gob); i++, pane++)
            Queue_Mark_Gob_Deep(*pane);
    }

    if (GOB_PARENT(gob)) Queue_Mark_Gob_Deep(GOB_PARENT(gob));

    if (GOB_CONTENT(gob)) {
        if (GOB_TYPE(gob) >= GOBT_IMAGE && GOB_TYPE(gob) <= GOBT_STRING)
            SERIES_SET_FLAG(GOB_CONTENT(gob), SER_MARK);
        else if (GOB_TYPE(gob) >= GOBT_DRAW && GOB_TYPE(gob) <= GOBT_EFFECT)
            QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(GOB_CONTENT(gob)));
    }

    if (GOB_DATA(gob)) {
        switch (GOB_DTYPE(gob)) {
        case GOBD_INTEGER:
        case GOBD_NONE:
        default:
            break;
        case GOBD_OBJECT:
            QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(GOB_DATA(gob)));
            break;
        case GOBD_STRING:
        case GOBD_BINARY:
            MARK_SERIES_ONLY(GOB_DATA(gob));
            break;
        case GOBD_BLOCK:
            QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(GOB_DATA(gob)));
        }
    }
}


//
//  Queue_Mark_Field_Deep: C
// 
// 'Queue' refers to the fact that after calling this routine,
// one will have to call Propagate_All_GC_Marks() to have the
// deep transitive closure be guaranteed fully marked.
// 
// Note: only referenced blocks are queued, fields that are structs
// will be processed via recursion.  Deeply nested structs could
// in theory overflow the C stack.
//
static void Queue_Mark_Field_Deep(const REBSTU *stu, struct Struct_Field *field)
{
    if (field->type == STRUCT_TYPE_STRUCT) {
        unsigned int len = 0;
        REBSER *field_fields = field->fields;

        MARK_SERIES_ONLY(field_fields);
        QUEUE_MARK_ARRAY_DEEP(field->spec);

        for (len = 0; len < SERIES_LEN(field_fields); len++) {
            Queue_Mark_Field_Deep(
                stu, cast(struct Struct_Field*, SERIES_AT(field_fields, len))
            );
        }
    }
    else if (field->type == STRUCT_TYPE_REBVAL) {
        REBCNT i;

        assert(field->size == sizeof(REBVAL));
        for (i = 0; i < field->dimension; i ++) {
            REBVAL *data = cast(REBVAL*,
                SERIES_AT(
                    STRUCT_DATA_BIN(stu),
                    STRUCT_OFFSET(stu) + field->offset + i * field->size
                )
            );

            if (field->done)
                Queue_Mark_Value_Deep(data);
        }
    }
    else {
        // ignore primitive datatypes
    }
}


//
//  Queue_Mark_Struct_Deep: C
// 
// 'Queue' refers to the fact that after calling this routine,
// one will have to call Propagate_All_GC_Marks() to have the
// deep transitive closure be guaranteed fully marked.
// 
// Note: only referenced blocks are queued, the actual struct
// itself is processed via recursion.  Deeply nested structs could
// in theory overflow the C stack.
//
static void Queue_Mark_Struct_Deep(const REBSTU *stu)
{
    unsigned int len = 0;
    REBSER *series = NULL;

    // The spec is the only Rebol-value-array in the struct
    QUEUE_MARK_ARRAY_DEEP(stu->spec);

    MARK_SERIES_ONLY(stu->fields);
    MARK_SERIES_ONLY(STRUCT_DATA_BIN(stu));

    assert(!SERIES_GET_FLAG(stu->data, SER_EXTERNAL));
    assert(SERIES_LEN(stu->data) == 1);
    MARK_SERIES_ONLY(stu->data);

    series = stu->fields;
    for (len = 0; len < SERIES_LEN(series); len++) {
        struct Struct_Field *field = cast(struct Struct_Field*,
            SERIES_AT(series, len)
        );

        Queue_Mark_Field_Deep(stu, field);
    }
}


//
//  Queue_Mark_Routine_Deep: C
// 
// 'Queue' refers to the fact that after calling this routine,
// one will have to call Propagate_All_GC_Marks() to have the
// deep transitive closure completely marked.
// 
// Note: only referenced blocks are queued, the routine's RValue
// is processed via recursion.  Deeply nested RValue structs could
// in theory overflow the C stack.
//
static void Queue_Mark_Routine_Deep(const REBROT *rot)
{
    QUEUE_MARK_ARRAY_DEEP(ROUTINE_SPEC(rot));
    ROUTINE_SET_FLAG(ROUTINE_INFO(rot), ROUTINE_MARK);

    MARK_SERIES_ONLY(ROUTINE_FFI_ARG_TYPES(rot));
    QUEUE_MARK_ARRAY_DEEP(ROUTINE_FFI_ARG_STRUCTS(rot));
    MARK_SERIES_ONLY(ROUTINE_EXTRA_MEM(rot));

    if (IS_CALLBACK_ROUTINE(ROUTINE_INFO(rot))) {
        REBFUN *cb_func = CALLBACK_FUNC(rot);
        if (cb_func) {
            // Should take care of spec, body, etc.
            QUEUE_MARK_ARRAY_DEEP(FUNC_PARAMLIST(CALLBACK_FUNC(rot)));
        }
        else {
            // !!! There is a call during MT_Routine that does an evaluation
            // while creating a callback function, before the CALLBACK_FUNC
            // has been set.  If the garbage collector is invoked at that
            // time, this will happen.  This should be reviewed to see if
            // it can be done another way--e.g. by not making the relevant
            // series visible to the garbage collector via MANAGE_SERIES()
            // until fully constructed.
        }
    } else {
        if (ROUTINE_GET_FLAG(ROUTINE_INFO(rot), ROUTINE_VARARGS)) {
            if (ROUTINE_FIXED_ARGS(rot))
                QUEUE_MARK_ARRAY_DEEP(ROUTINE_FIXED_ARGS(rot));

            if (ROUTINE_ALL_ARGS(rot))
                QUEUE_MARK_ARRAY_DEEP(ROUTINE_ALL_ARGS(rot));
        }

        if (ROUTINE_LIB(rot))
            MARK_LIB(ROUTINE_LIB(rot));
        else {
            // may be null if called before the routine! is fully constructed
        }
    }
}


//
//  Queue_Mark_Event_Deep: C
// 
// 'Queue' refers to the fact that after calling this routine,
// one will have to call Propagate_All_GC_Marks() to have the
// deep transitive closure completely marked.
//
static void Queue_Mark_Event_Deep(const REBVAL *value)
{
    REBREQ *req;

    if (
        IS_EVENT_MODEL(value, EVM_PORT)
        || IS_EVENT_MODEL(value, EVM_OBJECT)
        || (
            VAL_EVENT_TYPE(value) == EVT_DROP_FILE
            && GET_FLAG(VAL_EVENT_FLAGS(value), EVF_COPIED)
        )
    ) {
        // !!! Comment says void* ->ser field of the REBEVT is a "port or
        // object" but it also looks to store maps.  (?)
        //
        QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(VAL_EVENT_SER(m_cast(REBVAL*, value))));
    }

    if (IS_EVENT_MODEL(value, EVM_DEVICE)) {
        // In the case of being an EVM_DEVICE event type, the port! will
        // not be in VAL_EVENT_SER of the REBEVT structure.  It is held
        // indirectly by the REBREQ ->req field of the event, which
        // in turn possibly holds a singly linked list of other requests.
        req = VAL_EVENT_REQ(value);

        while (req) {
            // Comment says void* ->port is "link back to REBOL port object"
            if (req->port)
                QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(cast(REBSER*, req->port)));
            req = req->next;
        }
    }
}


//
//  Mark_Devices_Deep: C
// 
// Mark all devices. Search for pending requests.
// 
// This should be called at the top level, and as it is not
// 'Queued' it guarantees that the marks have been propagated.
//
static void Mark_Devices_Deep(void)
{
    REBDEV **devices = Host_Lib->devices;

    int d;
    for (d = 0; d < RDI_MAX; d++) {
        REBREQ *req;
        REBDEV *dev = devices[d];
        if (!dev)
            continue;

        for (req = dev->pending; req; req = req->next)
            if (req->port)
                QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(cast(REBSER*, req->port)));
    }
}


//
//  Mark_Call_Frames_Deep: C
// 
// Mark all function call frames.  In addition to containing the
// arguments that are referred to by pointer during a function
// invocation (acquired via D_ARG(N) calls), it is able to point
// to an arbitrary stable memory location for D_OUT.  This may
// be giving awareness to the GC of a variable on the C stack
// (for example).  This also keeps the function value itself
// live, as well as the "label" word and "where" block value.
// 
// Note that prior to a function invocation, the output value
// slot is written with "safe" TRASH.  This helps the evaluator
// catch cases of when a function dispatch doesn't consciously
// write any value into the output in debug builds.  The GC is
// willing to overlook this safe trash, however, and it will just
// be an UNSET! in the release build.
// 
// This should be called at the top level, and not from inside a
// Propagate_All_GC_Marks().  All marks will be propagated.
//
static void Mark_Call_Frames_Deep(void)
{
    struct Reb_Call *c = *(
        cast(struct Reb_Call**, SERIES_DATA(TG_Do_Stack))
        + SERIES_LEN(TG_Do_Stack)
        - 1
    );

    for (; c != NULL; c = c->prior) {

    #if !defined(NDEBUG)
        const REBYTE *label_str = Get_Sym_Name(c->label_sym);
    #endif

        // The array pointer passed to Do_Core is never NULL.
        //
        // !!! Perhaps it should be NULL if the call originated from C code.
        // At the moment, that code forges a block to show in the backtrace
        // as if it were Rebol, but it might be better to handle it special
        // in the code doing the stack walks.
        //
        QUEUE_MARK_ARRAY_DEEP(c->array);

        if (c->mode == CALL_MODE_0) {
            //
            // The only field we protect if no function is pending or running
            // with this frame is the array itself.  This is important if we
            // do something like `eval copy quote (recycle)`, because while
            // evaluating the paren it has no anchor anywhere in the root set
            // and could be GC'd.  The Reb_Call's array ref is all we have.
            //
            continue;
        }

        if (Is_Value_Managed(&c->cell, FALSE))
            Queue_Mark_Value_Deep(&c->cell);

        QUEUE_MARK_ARRAY_DEEP(FUNC_PARAMLIST(c->func)); // never NULL

        Queue_Mark_Value_Deep(c->out); // never NULL

        if (c->value && Is_Value_Managed(c->value, FALSE))
            Queue_Mark_Value_Deep(c->value);

        // !!! symbols are not currently GC'd, but if they were this would
        // need to keep the label sym alive!
        /* Mark_Symbol_Still_In_Use?(call->label_sym); */

        // !!! This hold on the GC for the closure's arglist while it is on
        // the stack *may* not be strictly necessary.  But at the moment,
        // the series is locked for the duration of its time on the stack,
        // to prevent expansion or contraction...and it thus must stay alive
        // long enough to be unlocked.  It may be the case that it could be
        // unlocked at the beginning of this dispatch and set to NULL, but
        // there may be interesting tricks that can be done by knowing a
        // closure's concrete arg pointers for the duration of its call.
        //
        // For the moment the actual transition from being manually managed
        // to GC-managed happens after all the arguments have been fulfilled.
        // This means it can only be protected if it's actually gotten to
        // the CALL_MODE_FUNCTION phase.  It could just be managed the whole
        // time, but given that CLOSURE! as a type is marked for death and
        // replacement by a finer-grained "object to create for some 'locals'"
        // worrying about the change is not necessary.
        //
        if (IS_CLOSURE(FUNC_VALUE(c->func)) && c->mode == CALL_MODE_FUNCTION)
            QUEUE_MARK_ARRAY_DEEP(c->arglist.array);

        // `param`, and `refine` may both be NULL
        // (`arg` is a cache of the head of the arglist or NULL if frameless)

        if (c->param && Is_Value_Managed(c->param, FALSE))
            Queue_Mark_Value_Deep(c->param);

        if (c->refine && Is_Value_Managed(c->refine, FALSE))
            Queue_Mark_Value_Deep(c->refine);

        Propagate_All_GC_Marks();
    }
}


//
//  Queue_Mark_Value_Deep: C
// 
// This routine is not marked `static` because it is needed by
// Ren/C++ in order to implement its GC_Mark_Hook.
//
void Queue_Mark_Value_Deep(const REBVAL *val)
{
    REBSER *ser = NULL;

    // If this happens, it means somehow Recycle() got called between
    // when an `if (Do_XXX_Throws())` branch was taken and when the throw
    // should have been caught up the stack (before any more calls made).
    //
    assert(!THROWN(val));

#if !defined(NDEBUG)
    if (IS_TRASH_DEBUG(val)) {
        // We allow *safe* trash values to be on the stack at the time
        // of a garbage collection.  These will be UNSET! in the debug
        // builds and they would not interfere with GC (they only exist
        // so that at the end of a process you can confirm that if an
        // UNSET! is in the slot, it was written there purposefully)

        if (VAL_GET_EXT(val, EXT_TRASH_SAFE))
            return;

        // Otherwise would be uninitialized in a release build!
        Debug_Fmt("TRASH! (uninitialized) found by Queue_Mark_Value_Deep");
        assert(FALSE);
    }
#endif

    switch (VAL_TYPE(val)) {
        case REB_UNSET:
            break;

        case REB_TYPESET:
            // As long as typeset is encoded as 64 bits, there's no issue
            // of having to keep alive "user types" or other things...but
            // that might be needed in the future.
            //
            // The symbol stored for typesets in contexts is effectively
            // unbound, and hence has no context to be preserved (until
            // such time as symbols are GC'd and this needs to be noted...)
            //
            break;

        case REB_HANDLE:
            break;

        case REB_DATATYPE:
            // Type spec is allowed to be NULL.  See %typespec.r file
            if (VAL_TYPE_SPEC(val))
                QUEUE_MARK_ARRAY_DEEP(VAL_TYPE_SPEC(val));
            break;

        case REB_TASK: // not yet implemented
            fail (Error(RE_MISC));

        case REB_OBJECT:
        case REB_MODULE:
        case REB_PORT:
        case REB_ERROR: {
            REBCON *context = VAL_CONTEXT(val);

            assert(CONTEXT_TYPE(context) == VAL_TYPE(val));
            assert(VAL_CONTEXT(CONTEXT_VALUE(context)) == context);
            assert(
                VAL_CONTEXT_SPEC(val)
                == VAL_CONTEXT_SPEC(CONTEXT_VALUE(context))
            );
            assert(
                VAL_CONTEXT_BODY(val)
                == VAL_CONTEXT_BODY(CONTEXT_VALUE(context))
            );

            QUEUE_MARK_CONTEXT_DEEP(context);

            if (VAL_CONTEXT_SPEC(val)) {
                //
                // !!! Under the module system, the spec is actually another
                // context of an object constructed with the various pieces
                // of module information.  This idea is being reviewed to
                // see if what is called the "object spec" should be
                // something more like a function spec, with the module
                // information going in something called a "meta"
                //
                QUEUE_MARK_CONTEXT_DEEP(VAL_CONTEXT_SPEC(val));
            }

            if (VAL_CONTEXT_BODY(val)) {
                //
                // !!! The feature of holding onto bodies is currently not
                // implemented.
                //
                QUEUE_MARK_ARRAY_DEEP(VAL_CONTEXT_BODY(val));
            }
            break;
        }

        case REB_FUNCTION:
        case REB_COMMAND:
        case REB_CLOSURE:
            QUEUE_MARK_ARRAY_DEEP(VAL_FUNC_BODY(val));
        case REB_NATIVE:
        case REB_ACTION:
            assert(ARRAY_GET_FLAG(VAL_FUNC_PARAMLIST(val), SER_PARAMLIST));
            assert(VAL_FUNC_SPEC(val) == FUNC_SPEC(VAL_FUNC(val)));
            assert(VAL_FUNC_PARAMLIST(val) == FUNC_PARAMLIST(VAL_FUNC(val)));

            QUEUE_MARK_ARRAY_DEEP(VAL_FUNC_SPEC(val));
            QUEUE_MARK_ARRAY_DEEP(VAL_FUNC_PARAMLIST(val));
            break;

        case REB_WORD:  // (and also used for function STACK backtrace frame)
        case REB_SET_WORD:
        case REB_GET_WORD:
        case REB_LIT_WORD:
        case REB_REFINEMENT:
        case REB_ISSUE:
            if (IS_WORD_BOUND(val)) {
                // Word is bound, so mark its context (which may be a "frame"
                // series or an identifying function word series).  All
                // bound words should keep their contexts from being GC'd...
                // even stack-relative contexts for functions.
                //
                REBCON* context = VAL_WORD_CONTEXT(val);
                if (!IS_FRAME_CONTEXT(context))
                    QUEUE_MARK_CONTEXT_DEEP(context);
                else {
                    // !!! Just the paramlist; make this bleed through the
                    // CONTEXT abstraction less...
                    //
                    QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(context));
                }
            }
            break;

        case REB_NONE:
        case REB_LOGIC:
        case REB_INTEGER:
        case REB_DECIMAL:
        case REB_PERCENT:
        case REB_MONEY:
        case REB_TIME:
        case REB_DATE:
        case REB_CHAR:
        case REB_PAIR:
        case REB_TUPLE:
            break;

        case REB_STRING:
        case REB_BINARY:
        case REB_FILE:
        case REB_EMAIL:
        case REB_URL:
        case REB_TAG:
        case REB_BITSET:
            ser = VAL_SERIES(val);
            assert(SERIES_WIDE(ser) <= sizeof(REBUNI));
            MARK_SERIES_ONLY(ser);
            break;

        case REB_IMAGE:
            //SERIES_SET_FLAG(VAL_SERIES_SIDE(val), SER_MARK); //????
            MARK_SERIES_ONLY(VAL_SERIES(val));
            break;

        case REB_VECTOR:
            MARK_SERIES_ONLY(VAL_SERIES(val));
            break;

        case REB_BLOCK:
        case REB_PAREN:
        case REB_PATH:
        case REB_SET_PATH:
        case REB_GET_PATH:
        case REB_LIT_PATH:
            QUEUE_MARK_ARRAY_DEEP(VAL_ARRAY(val));
            break;

        case REB_MAP: {
            REBMAP* map = VAL_MAP(val);
            QUEUE_MARK_ARRAY_DEEP(MAP_PAIRLIST(map));
            if (MAP_HASHLIST(map))
                MARK_SERIES_ONLY(MAP_HASHLIST(map));
            break;
        }

        case REB_CALLBACK:
        case REB_ROUTINE:
            QUEUE_MARK_ARRAY_DEEP(VAL_ROUTINE_SPEC(val));
            QUEUE_MARK_ARRAY_DEEP(VAL_ROUTINE_PARAMLIST(val));
            Queue_Mark_Routine_Deep(VAL_ROUTINE(val));
            break;

        case REB_LIBRARY:
            MARK_LIB(VAL_LIB_HANDLE(val));
            QUEUE_MARK_ARRAY_DEEP(VAL_LIB_SPEC(val));
            break;

        case REB_STRUCT:
            Queue_Mark_Struct_Deep(&VAL_STRUCT(val));
            break;

        case REB_GOB:
            Queue_Mark_Gob_Deep(VAL_GOB(val));
            break;

        case REB_EVENT:
            Queue_Mark_Event_Deep(val);
            break;

        default: {
            panic (Error_Invalid_Datatype(VAL_TYPE(val)));
        }
    }
}


//
//  Mark_Array_Deep_Core: C
// 
// Mark all series reachable from the array.
//
// !!! At one time there was a notion of a "bare series" which would be marked
// to escape needing to be checked for GC--for instance because it only
// contained symbol words.  However skipping over the values is a limited
// optimization.  (For instance: symbols may become GC'd, and need to see the
// symbol references inside the values...or typesets might be expanded to
// contain dynamically allocated arrays of user types).
//
// !!! A more global optimization would be if there was a flag that was
// maintained about whether there might be any GC'able values in an array.
// It could start out saying there may be...but then if it did a visit and
// didn't see any mark it as not needing GC.  Modifications dirty that bit.
//
static void Mark_Array_Deep_Core(REBARR *array)
{
    REBCNT len;
    REBVAL *value;

#if !defined(NDEBUG)
    //
    // We should have marked this series at queueing time to keep it from
    // being doubly added before the queue had a chance to be processed
    //
    if (!ARRAY_GET_FLAG(array, SER_MARK)) Panic_Array(array);

    // Make sure that a context's varlist wasn't marked without also marking
    // its keylist.  This could happen if QUEUE_MARK_ARRAY is used on a
    // context instead of QUEUE_MARK_CONTEXT.
    //
    if (ARRAY_GET_FLAG(array, SER_CONTEXT))
        assert(ARRAY_GET_FLAG(CONTEXT_KEYLIST(AS_CONTEXT(array)), SER_MARK));
#endif

#ifdef HEAVY_CHECKS
    //
    // The GC is a good general hook point that all series which have been
    // managed will go through, so it's a good time to assert properties
    // about the array.  If
    //
    ASSERT_ARRAY(array);
#else
    //
    // For a lighter check, make sure it's marked as a value-bearing array
    // and that it hasn't been freed.
    //
    assert(ARRAY_GET_FLAG(array, SER_ARRAY));
    assert(!SERIES_FREED(ARRAY_SERIES(array)));
#endif

    value = ARRAY_HEAD(array);
    for (; NOT_END(value); value++) {
    #if !defined(NDEBUG)
        if (IS_TRASH_DEBUG(value))
            Panic_Array(array);
    #endif
        Queue_Mark_Value_Deep(value);
    }
}


//
//  Sweep_Series: C
// 
// Scans all series in all segments that are part of the
// SERIES_POOL.  If a series had its lifetime management
// delegated to the garbage collector with MANAGE_SERIES(),
// then if it didn't get "marked" as live during the marking
// phase then free it.
// 
// The current exception is that any GC-managed series that has
// been marked with the SER_KEEP flag will not be freed--unless
// this sweep call is during shutdown.  During shutdown, those
// kept series will be freed as well.
// 
// !!! Review the idea of SER_KEEP, as it is a lot like
// Guard_Series (which was deleted).  Although SER_KEEP offers a
// less inefficient way to flag a series as protected from the
// garbage collector, it can be put on and left for an arbitrary
// amount of time...making it seem contentious with the idea of
// delegating it to the garbage collector in the first place.
//
ATTRIBUTE_NO_SANITIZE_ADDRESS static REBCNT Sweep_Series(REBOOL shutdown)
{
    REBSEG *seg;
    REBCNT count = 0;

    for (seg = Mem_Pools[SERIES_POOL].segs; seg; seg = seg->next) {
        REBSER *series = cast(REBSER *, seg + 1);
        REBCNT n;
        for (n = Mem_Pools[SERIES_POOL].units; n > 0; n--, series++) {
            // See notes on Make_Node() about how the first allocation of a
            // unit zero-fills *most* of it.  But after that it's up to the
            // caller of Free_Node() to zero out whatever bits it uses to
            // indicate "freeness".  We check the zeroness of the `wide`.
            if (SERIES_FREED(series))
                continue;

            if (SERIES_GET_FLAG(series, SER_MANAGED)) {
                if (shutdown || !SERIES_GET_FLAG(series, SER_MARK)) {
                    GC_Kill_Series(series);
                    count++;
                } else
                    SERIES_CLR_FLAG(series, SER_MARK);
            }
            else
                assert(!SERIES_GET_FLAG(series, SER_MARK));
        }
    }

    return count;
}


//
//  Sweep_Gobs: C
// 
// Free all unmarked gobs.
// 
// Scans all gobs in all segments that are part of the
// GOB_POOL. Free gobs that have not been marked.
//
ATTRIBUTE_NO_SANITIZE_ADDRESS static REBCNT Sweep_Gobs(void)
{
    REBSEG  *seg;
    REBGOB  *gob;
    REBCNT  n;
    REBCNT  count = 0;

    for (seg = Mem_Pools[GOB_POOL].segs; seg; seg = seg->next) {
        gob = (REBGOB *) (seg + 1);
        for (n = Mem_Pools[GOB_POOL].units; n > 0; n--) {
            if (IS_GOB_USED(gob)) {
                if (IS_GOB_MARK(gob))
                    UNMARK_GOB(gob);
                else {
                    Free_Gob(gob);
                    count++;
                }
            }
            gob++;
        }
    }

    return count;
}


//
//  Sweep_Libs: C
// 
// Free all unmarked libs.
// 
// Scans all libs in all segments that are part of the
// LIB_POOL. Free libs that have not been marked.
//
ATTRIBUTE_NO_SANITIZE_ADDRESS static REBCNT Sweep_Libs(void)
{
    REBSEG  *seg;
    REBLHL  *lib;
    REBCNT  n;
    REBCNT  count = 0;

    for (seg = Mem_Pools[LIB_POOL].segs; seg; seg = seg->next) {
        lib = (REBLHL *) (seg + 1);
        for (n = Mem_Pools[LIB_POOL].units; n > 0; n--) {
            if (IS_USED_LIB(lib)) {
                if (IS_MARK_LIB(lib))
                    UNMARK_LIB(lib);
                else {
                    UNUSE_LIB(lib);
                    Free_Node(LIB_POOL, (REBNOD*)lib);
                    count++;
                }
            }
            lib++;
        }
    }

    return count;
}


//
//  Sweep_Routines: C
// 
// Free all unmarked routines.
// 
// Scans all routines in all segments that are part of the
// RIN_POOL. Free routines that have not been marked.
//
ATTRIBUTE_NO_SANITIZE_ADDRESS static REBCNT Sweep_Routines(void)
{
    REBSEG  *seg;
    REBRIN  *info;
    REBCNT  n;
    REBCNT  count = 0;

    for (seg = Mem_Pools[RIN_POOL].segs; seg; seg = seg->next) {
        info = (REBRIN *) (seg + 1);
        for (n = Mem_Pools[RIN_POOL].units; n > 0; n--) {
            if (ROUTINE_GET_FLAG(info, ROUTINE_USED)) {
                if (ROUTINE_GET_FLAG(info, ROUTINE_MARK))
                    ROUTINE_CLR_FLAG(info, ROUTINE_MARK);
                else {
                    ROUTINE_CLR_FLAG(info, ROUTINE_USED);
                    Free_Routine(info);
                    count ++;
                }
            }
            info ++;
        }
    }

    return count;
}


//
//  Propagate_All_GC_Marks: C
// 
// The Mark Stack is a series containing series pointers.  They
// have already had their SER_MARK set to prevent being added
// to the stack multiple times, but the items they can reach
// are not necessarily marked yet.
// 
// Processing continues until all reachable items from the mark
// stack are known to be marked.
//
static void Propagate_All_GC_Marks(void)
{
    assert(!in_mark);

    while (SERIES_LEN(GC_Mark_Stack) != 0) {
        // Data pointer may change in response to an expansion during
        // Mark_Array_Deep_Core(), so must be refreshed on each loop.
        //
        REBARR *array;

        SET_SERIES_LEN(GC_Mark_Stack, SERIES_LEN(GC_Mark_Stack) - 1);

        array = cast(REBARR **, SERIES_DATA(GC_Mark_Stack))[
            SERIES_LEN(GC_Mark_Stack)
        ];

        // Drop the series we are processing off the tail, as we could be
        // queuing more of them (hence increasing the tail).
        //
        cast(REBARR **, SERIES_DATA(GC_Mark_Stack))[
            SERIES_LEN(GC_Mark_Stack)
        ] = NULL;

        Mark_Array_Deep_Core(array);
    }
}


//
//  Recycle_Core: C
// 
// Recycle memory no longer needed.
//
REBCNT Recycle_Core(REBOOL shutdown)
{
    REBINT n;
    REBCNT count;

    //Debug_Num("GC", GC_Disabled);

    ASSERT_NO_GC_MARKS_PENDING();

    // If disabled, exit now but set the pending flag.
    if (GC_Disabled || !GC_Active) {
        SET_SIGNAL(SIG_RECYCLE);
        //Print("pending");
        return 0;
    }

    if (Reb_Opts->watch_recycle) Debug_Str(cs_cast(BOOT_STR(RS_WATCH, 0)));

    GC_Disabled = 1;

#if !defined(NDEBUG)
    PG_Reb_Stats->Recycle_Counter++;
    PG_Reb_Stats->Recycle_Series = Mem_Pools[SERIES_POOL].free;

    PG_Reb_Stats->Mark_Count = 0;
#endif

    // WARNING: These terminate existing open blocks. This could
    // be a problem if code is building a new value at the tail,
    // but has not yet updated the TAIL marker.
    VAL_TERM_ARRAY(TASK_BUF_EMIT);
    VAL_TERM_ARRAY(TASK_BUF_COLLECT);

    // MARKING PHASE: the "root set" from which we determine the liveness
    // (or deadness) of a series.  If we are shutting down, we are freeing
    // *all* of the series that are managed by the garbage collector, so
    // we don't mark anything as live.

    if (!shutdown) {
        REBSER **sp;
        REBVAL **vp;

        // Mark series that have been temporarily protected from garbage
        // collection with PUSH_GUARD_SERIES.  We have to check if the
        // series is a context (so the keylist gets marked) or an array (so
        // the values are marked), or if it's just a data series which
        // should just be marked shallow.
        //
        sp = cast(REBSER**, SERIES_DATA(GC_Series_Guard));
        for (n = SERIES_LEN(GC_Series_Guard); n > 0; n--, sp++) {
            if (SERIES_GET_FLAG(*sp, SER_CONTEXT))
                MARK_CONTEXT_DEEP(AS_CONTEXT(*sp));
            else if (Is_Array_Series(*sp))
                MARK_ARRAY_DEEP(AS_ARRAY(*sp));
            else
                MARK_SERIES_ONLY(*sp);
        }

        // Mark value stack (temp-saved values):
        vp = cast(REBVAL**, SERIES_DATA(GC_Value_Guard));
        for (n = SERIES_LEN(GC_Value_Guard); n > 0; n--, vp++) {
            if (NOT_END(*vp))
                Queue_Mark_Value_Deep(*vp);
            Propagate_All_GC_Marks();
        }

        // Mark chunk stack (non-movable saved arrays of values)
        {
            struct Reb_Chunk *chunk = TG_Top_Chunk;
            while (chunk) {
                REBVAL *chunk_value = &chunk->values[0];
                while (
                    cast(REBYTE*, chunk_value)
                    < cast(REBYTE*, chunk) + chunk->size
                ) {
                    if (NOT_END(chunk_value))
                        Queue_Mark_Value_Deep(chunk_value);
                    chunk_value++;
                }
                chunk = chunk->prev;
            }
        }

        // Mark all root series:
        //
        MARK_CONTEXT_DEEP(PG_Root_Context);
        MARK_CONTEXT_DEEP(TG_Task_Context);

        // Mark potential error object from callback!
        Queue_Mark_Value_Deep(&Callback_Error);
        Propagate_All_GC_Marks();

        // !!! This hook point is an interim measure for letting a host
        // mark REBVALs that it is holding onto which are not contained in
        // series.  It is motivated by Ren/C++, which wraps REBVALs in
        // `ren::Value` class instances, and is able to enumerate the
        // "live" classes (they "die" when the destructor runs).
        //
        if (GC_Mark_Hook) {
            (*GC_Mark_Hook)();
            Propagate_All_GC_Marks();
        }

        // Mark all devices:
        Mark_Devices_Deep();

        // Mark function call frames:
        Mark_Call_Frames_Deep();
    }

    // SWEEPING PHASE

    // this needs to run before Sweep_Series(), because Routine has series
    // with pointers, which can't be simply discarded by Sweep_Series
    count = Sweep_Routines();

    count += Sweep_Series(shutdown);
    count += Sweep_Gobs();
    count += Sweep_Libs();

    CHECK_MEMORY(4);

#if !defined(NDEBUG)
    // Compute new stats:
    PG_Reb_Stats->Recycle_Series = Mem_Pools[SERIES_POOL].free - PG_Reb_Stats->Recycle_Series;
    PG_Reb_Stats->Recycle_Series_Total += PG_Reb_Stats->Recycle_Series;
    PG_Reb_Stats->Recycle_Prior_Eval = Eval_Cycles;
#endif

    // Do not adjust task variables or boot strings in shutdown when they
    // are being freed.
    //
    if (!shutdown) {
        if (GC_Ballast <= VAL_INT32(TASK_BALLAST) / 2
            && VAL_INT64(TASK_BALLAST) < MAX_I32) {
            //increasing ballast by half
            VAL_INT64(TASK_BALLAST) /= 2;
            VAL_INT64(TASK_BALLAST) *= 3;
        } else if (GC_Ballast >= VAL_INT64(TASK_BALLAST) * 2) {
            //reduce ballast by half
            VAL_INT64(TASK_BALLAST) /= 2;
        }

        // avoid overflow
        if (
            VAL_INT64(TASK_BALLAST) < 0
            || VAL_INT64(TASK_BALLAST) >= MAX_I32
        ) {
            VAL_INT64(TASK_BALLAST) = MAX_I32;
        }

        GC_Ballast = VAL_INT32(TASK_BALLAST);
        GC_Disabled = 0;

        if (Reb_Opts->watch_recycle)
            Debug_Fmt(cs_cast(BOOT_STR(RS_WATCH, 1)), count);
    }

    ASSERT_NO_GC_MARKS_PENDING();

    return count;
}


//
//  Recycle: C
// 
// Recycle memory no longer needed.
//
REBCNT Recycle(void)
{
    // Default to not passing the `shutdown` flag.
    return Recycle_Core(FALSE);
}


//
//  Guard_Series_Core: C
//
void Guard_Series_Core(REBSER *series)
{
    // It would seem there isn't any reason to save a series from being
    // garbage collected if it is already invisible to the garbage
    // collector.  But some kind of "saving" feature which added a
    // non-managed series in as if it were part of the root set would
    // be useful.  That would be for cases where you are building a
    // series up from constituent values but might want to abort and
    // manually free it.  For the moment, we don't have that feature.
    ASSERT_SERIES_MANAGED(series);

    if (SERIES_FULL(GC_Series_Guard)) Extend_Series(GC_Series_Guard, 8);

    cast(REBSER **, SERIES_DATA(GC_Series_Guard))[
        SERIES_LEN(GC_Series_Guard)
    ] = series;

    SET_SERIES_LEN(GC_Series_Guard, SERIES_LEN(GC_Series_Guard) + 1);
}


//
//  Guard_Value_Core: C
//
void Guard_Value_Core(const REBVAL *value)
{
    // Cheap check; require that the value already contain valid data when
    // the guard call is made (even if GC isn't necessarily going to happen
    // immediately, and value could theoretically become valid before then.)
    //
    assert(IS_END(value) || VAL_TYPE(value) < REB_MAX);

#ifdef STRESS_CHECK_GUARD_VALUE_POINTER
    //
    // Technically we should never call this routine to guard a value that
    // lives inside of a series.  Not only would we have to guard the
    // containing series, we would also have to lock the series from
    // being able to resize and reallocate the data pointer.  But this is
    // a somewhat expensive check, so it's only feasible to run occasionally.
    //
    ASSERT_NOT_IN_SERIES_DATA(value);
#endif

    if (SERIES_FULL(GC_Value_Guard)) Extend_Series(GC_Value_Guard, 8);

    cast(const REBVAL **, SERIES_DATA(GC_Value_Guard))[
        SERIES_LEN(GC_Value_Guard)
    ] = value;

    SET_SERIES_LEN(GC_Value_Guard, SERIES_LEN(GC_Value_Guard) + 1);
}


//
//  Init_GC: C
// 
// Initialize garbage collector.
//
void Init_GC(void)
{
    // TRUE when recycle is enabled (set by RECYCLE func)
    //
    GC_Active = FALSE;

    // GC disabled counter for critical sections.  Used liberally in R3-Alpha.
    // But with Ren-C's introduction of the idea that an allocated series is
    // not seen by the GC until such time as it gets the SER_MANAGED flag
    // set, there are fewer legitimate justifications to disabling the GC.
    //
    GC_Disabled = 0;

    GC_Ballast = MEM_BALLAST;

    // Temporary series protected from GC. Holds series pointers.
    GC_Series_Guard = Make_Series(15, sizeof(REBSER *), MKS_NONE);

    // Temporary values protected from GC. Holds value pointers.
    GC_Value_Guard = Make_Series(15, sizeof(REBVAL *), MKS_NONE);

    // The marking queue used in lieu of recursion to ensure that deeply
    // nested structures don't cause the C stack to overflow.
    GC_Mark_Stack = Make_Series(100, sizeof(REBARR *), MKS_NONE);
    TERM_SEQUENCE(GC_Mark_Stack);
}


//
//  Shutdown_GC: C
//
void Shutdown_GC(void)
{
    Free_Series(GC_Series_Guard);
    Free_Series(GC_Value_Guard);
    Free_Series(GC_Mark_Stack);
}
