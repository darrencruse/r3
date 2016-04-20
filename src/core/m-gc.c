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
**      SERIES_FLAG_MARK flag set.  So the flag is checked by a macro before making
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

#include "stdio.h" // for FILE operations
#define REN_C_STDIO_OK

#include "sys-core.h"

#include "mem-pools.h" // low-level memory pool access
#include "mem-series.h" // low-level series memory access

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

enum mem_dump_kind {
    REB_KIND_SERIES = REB_MAX + 4,
    REB_KIND_ARRAY,
    REB_KIND_CONTEXT,
    REB_KIND_KEYLIST,
    REB_KIND_VARLIST,
    REB_KIND_FIELD,
    REB_KIND_STU,
    REB_KIND_HASH,
    REB_KIND_CHUNK,
    REB_KIND_CALL,
    REB_KIND_ROUTINE_INFO,
    REB_KIND_MAX
};

struct Reb_Mem_Dump {
    void *parent;
    FILE *out;
};

struct mark_stack_elem {
    REBARR *array;
    const REBARR *key_list;
    REBMDP *dump;
#ifndef NDEBUG
    int *guard;
#endif
};

struct mem_dump_entry {
    const void *addr;
    const char *name;
    const void *parent;
    const char *edge; /* name of the edge from parent to this ndoe */
    int kind;
    REBCNT size;
};

static void Dump_Mem_Entry(REBMDP *dump,
    const struct mem_dump_entry *entry)
{
    char n[8];
    if (!dump || !dump->out) return;
    if (entry->addr == entry->parent) return;
    if (entry->parent == NULL) {
        // Windows prints 00000 for NULL
        fprintf(dump->out, "%p,(nil),%d,%d,%s,%s\n",
            entry->addr,
            entry->kind,
            entry->size,
            entry->edge == NULL ? "(null)" : entry->edge,
            entry->name == NULL ? "(null)" : entry->name);
    }
    else {
        fprintf(dump->out, "%p,%p,%d,%d,%s,%s\n",
            entry->addr,
            entry->parent,
            entry->kind,
            entry->size,
            entry->edge == NULL? "(null)" : entry->edge,
            entry->name == NULL? "(null)" : entry->name);
    }
}

static void Dump_Mem_Comment(REBMDP *dump, const char *s)
{
    if (!dump) return;
    fprintf(dump->out, "#%s\n", s);
}

// was static, but exported for Ren/C
/* static void Queue_Mark_Value_Deep(const REBVAL *val, const void *parent, REBMDP *dump);*/

static void Push_Array_Marked_Deep(REBARR *array, const REBARR *key_listr, REBMDP *dump);

#ifndef NDEBUG
static void Mark_Series_Only_Debug_Core(REBSER *ser);
#endif

#ifndef NDEBUG
static void Panic_Mark_Stack(struct mark_stack_elem *elem)
{
    /* reference the freed guard to cause a crash and a backtrace */
    int i = *elem->guard;
}
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
static void Push_Array_Marked_Deep(REBARR *array, const REBARR *key_list, REBMDP *dump)
{
    struct mark_stack_elem *elem;

#if !defined(NDEBUG)
    if (!GET_ARR_FLAG(array, SERIES_FLAG_MANAGED)) {
        Debug_Fmt("Link to non-MANAGED item reached by GC");
        Panic_Array(array);
    }
#endif

    assert(GET_ARR_FLAG(array, SERIES_FLAG_ARRAY));

    if (GET_ARR_FLAG(array, CONTEXT_FLAG_STACK)) {
        //
        // If the array's storage was on the stack and that stack level has
        // been popped, its data has been nulled out, and the series only
        // exists to keep words or objects holding it from crashing.
        //
        if (!GET_ARR_FLAG(array, SERIES_FLAG_ACCESSIBLE))
            return;
    }

    // !!! Are there actually any "external" series that are value-bearing?
    // e.g. a REBSER node which has a ->data pointer to REBVAL[...] and
    // expects this to be managed with GC, even though if the REBSER is
    // GC'd it shouldn't free that data?
    //
    assert(!GET_ARR_FLAG(array, SERIES_FLAG_EXTERNAL));

    // set by calling macro (helps catch direct calls of this function)
    assert(GET_ARR_FLAG(array, SERIES_FLAG_MARK));

    // Add series to the end of the mark stack series and update terminator

    if (SER_FULL(GC_Mark_Stack)) Extend_Series(GC_Mark_Stack, 8);
    if (key_list != NULL) {
        assert(ARR_LEN(array) <= ARR_LEN(key_list));
    }
    elem = SER_AT(struct mark_stack_elem, GC_Mark_Stack, SER_LEN(GC_Mark_Stack));
    elem->array = array;
    elem->key_list = key_list;
    elem->dump = dump;
#ifndef NDEBUG
    elem->guard = malloc(sizeof(int));
    free(elem->guard);
#endif

    SET_SERIES_LEN(GC_Mark_Stack, SER_LEN(GC_Mark_Stack) + 1);

    elem = SER_AT(struct mark_stack_elem, GC_Mark_Stack, SER_LEN(GC_Mark_Stack));
    elem->array = NULL;
    elem->key_list = NULL;
#ifndef NDEBUG
    elem->guard = NULL;
#endif
}


static void Propagate_All_GC_Marks(REBMDP *dump);

#ifndef NDEBUG
    static REBOOL in_mark = FALSE;
#endif

// NOTE: The following macros uses S parameter multiple times, hence if S has
// side effects this will run that side-effect multiply.

// Deferred form for marking series that prevents potentially overflowing the
// C execution stack.

#define QUEUE_MARK_ARRAY_DEEP(a, name, parent, edge, kind, keylist, dump) \
    do { \
        if (kind != REB_KIND_KEYLIST) { \
            struct mem_dump_entry tmp_entry = { \
                (a), (name), (parent), (edge), (kind), sizeof(REBARR) /* size is counted in the contained REBVALs */ \
            };\
            Dump_Mem_Entry(dump, &tmp_entry); \
        } else {\
            struct mem_dump_entry tmp_entry = { \
                (a), (name), (parent), (edge), (kind), sizeof(REBARR) + ARR_LEN(a) * sizeof(REBVAL) \
            }; \
            Dump_Mem_Entry(dump, &tmp_entry); \
        } \
        if (!GET_ARR_FLAG((a), SERIES_FLAG_MARK)) { \
            SET_ARR_FLAG((a), SERIES_FLAG_MARK); \
            Push_Array_Marked_Deep(a, keylist, (kind == REB_KIND_KEYLIST) ? NULL : dump); \
        } \
    } while (0)

#define QUEUE_MARK_CONTEXT_DEEP(c, name, parent, edge, dump) \
    do { \
        assert(GET_ARR_FLAG(CTX_VARLIST(c), ARRAY_FLAG_CONTEXT_VARLIST)); \
        QUEUE_MARK_ARRAY_DEEP(CTX_KEYLIST(c), NULL, CTX_VARLIST(c), "<keylist>", REB_KIND_KEYLIST, CTX_KEYLIST(c), dump); \
        QUEUE_MARK_ARRAY_DEEP(CTX_VARLIST(c), (name), (parent), (edge), REB_KIND_ARRAY, CTX_KEYLIST(c), dump); \
    } while (0)

// Non-Queued form for marking blocks.  Used for marking a *root set item*,
// don't recurse from within Mark_Value/Mark_Gob/Mark_Array_Deep/etc.

#define MARK_ARRAY_DEEP(a, name, parent, edge, kind, keylist, dump) \
    do { \
        assert(!in_mark); \
        QUEUE_MARK_ARRAY_DEEP(a, name, parent, edge, kind, keylist, dump); \
        Propagate_All_GC_Marks(dump); \
    } while (0)

#define MARK_CONTEXT_DEEP(c, name, parent, edge, dump) \
    do { \
        assert(!in_mark); \
        QUEUE_MARK_CONTEXT_DEEP(c, name, parent, edge, dump); \
        Propagate_All_GC_Marks(dump); \
    } while (0)


// Non-Deep form of mark, to be used on non-BLOCK! series or a block series
// for which deep marking is not necessary (such as an 'typed' words block)

#ifdef NDEBUG
    #define MARK_SERIES_ONLY_CORE(s) SET_SER_FLAG((s), SERIES_FLAG_MARK)
#else
    #define MARK_SERIES_ONLY_CORE(s) Mark_Series_Only_Debug_Core(s)
#endif

#define MARK_SERIES_ONLY(s, name, parent, edge, kind, dump) do {\
    struct mem_dump_entry tmp_entry = { \
        (s), (name), (parent), (edge), (kind), SER_TOTAL(s) + sizeof(REBSER)\
    }; \
    Dump_Mem_Entry(dump, &tmp_entry); \
    if (!GET_SER_FLAG((s), SERIES_FLAG_MARK)) { \
        MARK_SERIES_ONLY_CORE(s); \
    } \
} while (0)


// Assertion for making sure that all the deferred marks have been propagated

#define ASSERT_NO_GC_MARKS_PENDING() \
    assert(SER_LEN(GC_Mark_Stack) == 0)


#if !defined(NDEBUG)
//
//  Mark_Series_Only_Debug: C
// 
// Hook point for marking and tracing a single series mark.
//
static void Mark_Series_Only_Debug_Core(REBSER *series)
{
    if (!GET_SER_FLAG(series, SERIES_FLAG_MANAGED)) {
        Debug_Fmt("Link to non-MANAGED item reached by GC");
        Panic_Series(series);
    }

    SET_SER_FLAG(series, SERIES_FLAG_MARK);
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
static void Queue_Mark_Gob_Deep(REBGOB *gob, const char *name, const void *parent, const char *edge, REBMDP *dump)
{
    REBGOB **pane;
    REBCNT i;

    struct mem_dump_entry entry = {
        .addr = gob,
        .name = name,
        .parent = parent,
        .edge = edge,
        .kind = REB_GOB,
        .size = sizeof(REBGOB)
    };

    Dump_Mem_Entry(dump, &entry);

    if (IS_GOB_MARK(gob)) return;

    MARK_GOB(gob);

    if (GOB_PANE(gob)) {
        MARK_SERIES_ONLY(GOB_PANE(gob), NULL, gob, "<pane>", REB_KIND_SERIES, dump);
        pane = GOB_HEAD(gob);
        for (i = 0; i < GOB_LEN(gob); i++, pane++)
            Queue_Mark_Gob_Deep(*pane, NULL, GOB_PANE(gob), "<has>", dump);
    }

    if (GOB_PARENT(gob)) Queue_Mark_Gob_Deep(GOB_PARENT(gob), NULL, gob, "<parent>", dump);

    if (GOB_CONTENT(gob)) {
        if (GOB_TYPE(gob) >= GOBT_IMAGE && GOB_TYPE(gob) <= GOBT_STRING)
            MARK_SERIES_ONLY(GOB_CONTENT(gob), NULL, gob, "<content>", GOB_TYPE(gob) + REB_KIND_MAX, dump);
        else if (GOB_TYPE(gob) >= GOBT_DRAW && GOB_TYPE(gob) <= GOBT_EFFECT)
            QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(GOB_CONTENT(gob)), NULL, gob, "<content>", GOB_TYPE(gob) + REB_KIND_MAX, NULL, dump);
    }

    if (GOB_DATA(gob)) {
        struct mem_dump_entry entry = {
            .addr = GOB_DATA(gob),
            .name = NULL,
            .parent = gob,
            .edge = "<gob-data>",
            .size = sizeof(REBVAL),
        };
        enum Reb_Kind kind = REB_TRASH;
        switch (GOB_DTYPE(gob)) {
        case GOBD_INTEGER:
            kind = REB_INTEGER;
            // fall through
        case GOBD_NONE:
            kind = REB_NONE;
            // fall through
        default:
            entry.kind = kind;
            Dump_Mem_Entry(dump, &entry);
            break;
        case GOBD_OBJECT:
            QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(GOB_DATA(gob)), NULL, gob, "<gob-data>", dump);
            break;
        case GOBD_STRING:
        case GOBD_BINARY:
            MARK_SERIES_ONLY(GOB_DATA(gob), NULL, gob, "<gob-data>", REB_KIND_SERIES, dump);
            break;
        case GOBD_BLOCK:
            QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(GOB_DATA(gob)), NULL, gob, "<gob-data>", REB_KIND_ARRAY, NULL, dump);
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
static void Queue_Mark_Field_Deep(const REBSTU *stu, struct Struct_Field *field, const void *parent, REBMDP *dump)
{
    struct mem_dump_entry entry = {
        .addr = field,
        .parent = parent,
        .name = Get_Sym_Name(field->sym),
        .edge = "<field>",
        .kind = REB_KIND_FIELD,
        .size = 0 /* counted in fields already */
    };
    Dump_Mem_Entry(dump, &entry);

    if (field->type == STRUCT_TYPE_STRUCT) {
        unsigned int len = 0;
        REBSER *field_fields = field->fields;

        MARK_SERIES_ONLY(field_fields, NULL, field, "<fields>", REB_KIND_FIELD, dump);
        QUEUE_MARK_ARRAY_DEEP(field->spec, NULL, field, "<spec>", REB_KIND_ARRAY, NULL, dump);

        for (len = 0; len < SER_LEN(field_fields); len++) {
            Queue_Mark_Field_Deep(
                stu, SER_AT(struct Struct_Field, field_fields, len), field_fields, dump
            );
        }
    }
    else if (field->type == STRUCT_TYPE_REBVAL) {
        REBCNT i;

        assert(field->size == sizeof(REBVAL));
        for (i = 0; i < field->dimension; i ++) {
            REBVAL *data = cast(REBVAL*, // !!! What? Is this an ARRAY!?
                SER_AT(
                    REBYTE,
                    STRUCT_DATA_BIN(stu),
                    STRUCT_OFFSET(stu) + field->offset + i * field->size
                )
            );

            /* This could lead to an infinite recursive call to Queue_Mark_Field_Deep if this value refers back to this struct */
            if (field->done)
                Queue_Mark_Value_Deep(data, Get_Sym_Name(field->sym), STRUCT_DATA_BIN(stu), "<rebval>", dump);
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
static void Queue_Mark_Struct_Deep(const REBSTU *stu, const char *name, const void *parent, REBMDP *dump)
{
    unsigned int len = 0;
    REBSER *series = NULL;

    struct mem_dump_entry entry = {
        .addr = stu,
        .parent = parent,
        .name = name,
        .edge = "<REBSTU>",
        .kind = REB_KIND_STU,
        .size = sizeof(*stu)
    };

    Dump_Mem_Entry(dump, &entry);

    if (GET_SER_FLAG(STRUCT_DATA_BIN(stu), SERIES_FLAG_MARK)) return; //avoid recursive call

    // The spec is the only Rebol-value-array in the struct
    QUEUE_MARK_ARRAY_DEEP(stu->spec, NULL, stu, "<spec>", REB_KIND_ARRAY, NULL, dump);

    MARK_SERIES_ONLY(stu->fields, NULL, stu, "<fields>", REB_KIND_SERIES, dump);
    MARK_SERIES_ONLY(STRUCT_DATA_BIN(stu), NULL, stu, "<bin>", REB_KIND_SERIES, dump);

    assert(!GET_SER_FLAG(stu->data, SERIES_FLAG_EXTERNAL));
    assert(SER_LEN(stu->data) == 1);
    MARK_SERIES_ONLY(stu->data, NULL, stu, "<data>", REB_KIND_SERIES, dump);

    series = stu->fields;
    MARK_SERIES_ONLY(stu->fields, NULL, stu, "<fields>", REB_KIND_SERIES, dump);
    for (len = 0; len < SER_LEN(series); len++) {
        struct Struct_Field *field
            = SER_AT(struct Struct_Field, series, len);

        Queue_Mark_Field_Deep(stu, field, series, dump);
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
static void Queue_Mark_Routine_Deep(REBROT *rot, const char*name, const void *parent, REBMDP *dump)
{ 
    struct Reb_Routine_Info *rinfo = ROUTINE_INFO(rot);
    struct mem_dump_entry entry = {
        .addr = rinfo, // dump rinfo here, because rot is same as PARAMLIST, which has already been dumped.
        .name = name,
        .parent = parent,
        .edge = "<INFO>",
        .kind = REB_KIND_ROUTINE_INFO,
        .size = sizeof(*rinfo)
    };

    Dump_Mem_Comment(dump, "Dumping Routine/Callback");
    Dump_Mem_Entry(dump, &entry);

    if (ROUTINE_GET_FLAG(ROUTINE_INFO(rot), ROUTINE_MARK)) return;

    ROUTINE_SET_FLAG(ROUTINE_INFO(rot), ROUTINE_MARK);

    QUEUE_MARK_ARRAY_DEEP(ROUTINE_SPEC(rot), NULL, rinfo, "<spec>", REB_KIND_ARRAY, NULL, dump);


    MARK_SERIES_ONLY(ROUTINE_FFI_ARG_TYPES(rot), NULL, rinfo, "<ffi-arg-types>", REB_KIND_SERIES, dump);
    QUEUE_MARK_ARRAY_DEEP(ROUTINE_FFI_ARG_STRUCTS(rot), NULL, rinfo, "<ffi-arg-structs>", REB_KIND_ARRAY, NULL, dump);
    MARK_SERIES_ONLY(ROUTINE_EXTRA_MEM(rot), NULL, rinfo, "<extra-mem>", REB_KIND_SERIES, dump);

    if (IS_CALLBACK_ROUTINE(ROUTINE_INFO(rot))) {
        REBFUN *cb_func = CALLBACK_FUNC(rot);
        if (cb_func) {
            // Should take care of spec, body, etc.
            Dump_Mem_Comment(dump, "Dumping the paramlist of a callback");
            REBARR *paramlist = FUNC_PARAMLIST(CALLBACK_FUNC(rot));
            QUEUE_MARK_ARRAY_DEEP(paramlist, NULL, parent, "<rebfunc>", REB_KIND_ARRAY, paramlist, dump); //paramlist points the same address as rot does
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
        if (ROUTINE_GET_FLAG(ROUTINE_INFO(rot), ROUTINE_VARIADIC)) {
            if (ROUTINE_FIXED_ARGS(rot))
                QUEUE_MARK_ARRAY_DEEP(ROUTINE_FIXED_ARGS(rot), NULL, rinfo, "<fixed-args>", REB_KIND_ARRAY, NULL, dump);

            if (ROUTINE_ALL_ARGS(rot))
                QUEUE_MARK_ARRAY_DEEP(ROUTINE_ALL_ARGS(rot), NULL, rinfo, "<all-args>", REB_KIND_ARRAY, NULL, dump);
        }

        if (ROUTINE_LIB(rot)) {
            if (!IS_MARK_LIB(ROUTINE_LIB(rot))) {
                MARK_LIB(ROUTINE_LIB(rot));
                Dump_Mem_Comment(dump, "Dumping the library referenced by the routine");
                entry.addr = ROUTINE_LIB(rot);
                entry.kind = REB_LIBRARY;
                entry.size = sizeof(*ROUTINE_LIB(rot));
                entry.parent = rinfo;
                entry.edge = "<library>";
                entry.name = NULL;
                Dump_Mem_Entry(dump, &entry);
            }
        }
        else {
            // may be null if called before the routine is fully constructed
        }
    }
    Dump_Mem_Comment(dump, "Done dumping Routine/Callback");
}


//
//  Queue_Mark_Event_Deep: C
// 
// 'Queue' refers to the fact that after calling this routine,
// one will have to call Propagate_All_GC_Marks() to have the
// deep transitive closure completely marked.
//
static void Queue_Mark_Event_Deep(const REBVAL *value, const char *name, const void *parent, REBMDP *dump)
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
        QUEUE_MARK_ARRAY_DEEP(AS_ARRAY(VAL_EVENT_SER(m_cast(REBVAL*, value))), NULL, value, "<port/object/ser>", REB_KIND_SERIES, NULL, dump);
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
                QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(cast(REBSER*, req->port)), NULL, value, "<port>", dump);
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
static void Mark_Devices_Deep(REBMDP *dump)
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
                QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(cast(REBSER*, req->port)), NULL, NULL, "<req-port>", dump);
    }
}


//
//  Mark_Frame_Stack_Deep: C
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
static void Mark_Frame_Stack_Deep(REBMDP *dump)
{
    // The GC must consider all entries, not just those that have been pushed
    // into active evaluation.
    //
    struct Reb_Frame *f = TG_Frame_Stack;
    struct mem_dump_entry entry;
    
    entry.addr = f;
    entry.name = (f == NULL)? NULL : Get_Sym_Name(f->opt_label_sym);
    entry.parent = NULL;
    entry.kind = REB_KIND_CALL;
    entry.edge = "<TG_Frame_Stack>",
    entry.size = 0; // on the stack
    Dump_Mem_Entry(dump, &entry);

    for (; f != NULL; f = f->prior) {
        //
        // Should have taken care of reifying all the VALIST on the stack
        // earlier in the recycle process (don't want to create new arrays
        // once the recycling has started...)
        //
        assert(f->indexor != VALIST_FLAG);

        if (f->indexor == END_FLAG) {
            //
            // This is possible, because the frame could be sitting at the
            // end of a block when a function runs, e.g. `do [zero-arity]`.
            // That frame will stay on the stack while the zero-arity
            // function is running, which could be arbitrarily long...so
            // a GC could happen.
        }
        else {
            assert(f->indexor != THROWN_FLAG);
            QUEUE_MARK_ARRAY_DEEP(f->source.array, NULL, f, "<source>", REB_KIND_ARRAY, NULL, dump);
        }

        if (f->value && Is_Value_Managed(f->value, FALSE))
            Queue_Mark_Value_Deep(f->value, NULL, f, "<value>", dump);

        if (f->mode == CALL_MODE_GUARD_ARRAY_ONLY) {
            //
            // The only fields we protect if no function is pending or running
            // with this frame is the array and the potentially pending value.
            //
            // Consider something like `eval copy quote (recycle)`, because
            // while evaluating the group it has no anchor anywhere in the
            // root set and could be GC'd.  The Reb_Frame's array ref is it.
            //
            goto next;
        }

        // The subfeed may be in use by VARARGS!, and it may be either a
        // context or a single element array.
        //
        if (f->cell.subfeed) {
            if (GET_ARR_FLAG(f->cell.subfeed, ARRAY_FLAG_CONTEXT_VARLIST))
                QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(f->cell.subfeed), NULL, f, "<subfeed>", dump);
            else {
                assert(ARR_LEN(f->cell.subfeed) == 1);
                QUEUE_MARK_ARRAY_DEEP(f->cell.subfeed, NULL, f, "<subfeed>", REB_KIND_ARRAY, NULL, dump);
            }
        }

        QUEUE_MARK_ARRAY_DEEP(FUNC_PARAMLIST(f->func), NULL, f, "<paramlist>", REB_KIND_ARRAY, FUNC_PARAMLIST(f->func), dump); // never NULL

        Queue_Mark_Value_Deep(f->out, NULL, f, "<out>", dump); // never NULL

        // !!! symbols are not currently GC'd, but if they were this would
        // need to keep the label sym alive!
        /* Mark_Symbol_Still_In_Use?(f->label_sym); */

        // In the current implementation (under review) functions use
        // stack-based chunks to gather their arguments, and closures use
        // ordinary arrays.  If the call mode is CALL_MODE_PENDING then
        // the arglist is under construction, but guaranteed to have all
        // cells be safe for garbage collection.
        //
        if (f->flags & DO_FLAG_FRAME_CONTEXT) {
            //
            // Though a Reb_Frame starts off with just a chunk of memory, it
            // may be promoted to a context (backed by a data pointer of
            // that chunk of memory).  This context *may not be managed yet*
            // in the current implementation.
            //
            if (
                GET_ARR_FLAG(
                    CTX_VARLIST(f->data.context),
                    SERIES_FLAG_MANAGED
                )
            ) {
                QUEUE_MARK_CONTEXT_DEEP(f->data.context, NULL, f, "<context>", dump);
            }
            else {
                // Just mark the keylist...
                QUEUE_MARK_ARRAY_DEEP(CTX_KEYLIST(f->data.context), NULL, f, "<keylist>", REB_KIND_ARRAY, CTX_KEYLIST(f->data.context), dump);
            }
        }
        else  {
            // If it's just sequential REBVALs sitting in memory in the chunk
            // stack, then the chunk stack walk already took care of it.
            // (the chunk stack can be used for things other than the call
            // stack, so long as they are stack-like in a call relative way)
        }

        // `param`, and `refine` may both be NULL
        // (`arg` is a cache of the head of the arglist)

        if (f->param && Is_Value_Managed(f->param, FALSE))
            Queue_Mark_Value_Deep(f->param, NULL, f, "<param>", dump);

        if (f->refine && Is_Value_Managed(f->refine, FALSE))
            Queue_Mark_Value_Deep(f->refine, NULL, f, "<param>", dump);

        Propagate_All_GC_Marks(dump);

    next:
        if (f->prior) {
            entry.addr = f->prior;
            entry.name = Get_Sym_Name(f->opt_label_sym);
            entry.parent = f;
            entry.kind = REB_KIND_CALL;
            entry.edge = "<prior>";
            entry.size = 0; // on the stack
            Dump_Mem_Entry(dump, &entry);
        }
    }
}


//
//  Queue_Mark_Value_Deep: C
// 
// This routine is not marked `static` because it is needed by
// Ren/C++ in order to implement its GC_Mark_Hook.
//
void Queue_Mark_Value_Deep(const REBVAL *val, const char *name, const void *parent, const char *edge, REBMDP *dump)
{
    REBSER *ser = NULL;
    struct mem_dump_entry entry;
    enum Reb_Kind kind;

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

        if (GET_VAL_FLAG(val, TRASH_FLAG_SAFE))
            return;

        // Otherwise would be uninitialized in a release build!
        Debug_Fmt("TRASH! (uninitialized) found by Queue_Mark_Value_Deep");
        assert(FALSE);
    }
#endif

    kind = VAL_TYPE(val);

    entry.addr = val;
    entry.name = name;
    entry.parent = parent;
    entry.kind = kind;
    entry.edge = edge;
    entry.size = sizeof(REBVAL);

    if (name == NULL && ANY_WORD(val)) {
        entry.name = VAL_WORD_NAME(val);
    }
    Dump_Mem_Entry(dump, &entry);

    switch (kind) {
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
                QUEUE_MARK_ARRAY_DEEP(VAL_TYPE_SPEC(val), NULL, val, "<spec>", REB_KIND_ARRAY, NULL, dump);
            break;

        case REB_TASK: // not yet implemented
            fail (Error(RE_MISC));

        case REB_OBJECT:
        case REB_MODULE:
        case REB_PORT:
        case REB_FRAME:
        case REB_ERROR: {
            REBCTX *context = VAL_CONTEXT(val);
            assert(CTX_TYPE(context) == VAL_TYPE(val));

        #if !defined(NDEBUG)
            {
                REBVAL *value = CTX_VALUE(context);
                assert(VAL_CONTEXT(value) == context);
                if (IS_FRAME(val))
                    assert(VAL_CONTEXT_FRAME(val) == VAL_CONTEXT_FRAME(value));
                else
                    assert(VAL_CONTEXT_SPEC(val) == VAL_CONTEXT_SPEC(value));

                // Though the general rule is that canon values should match
                // the bits of any instance, an exception is made in the
                // case of the stackvars.  The danger of reusing the memory
                // is high after freeing since the chunk stack pointers
                // remain live, so the canon value has the field trashed
                // in debug builds.
                //
                if (GET_CTX_FLAG(context, CONTEXT_FLAG_STACK)) {
                    assert(
                        VAL_CONTEXT_STACKVARS(val)
                        == VAL_CONTEXT_STACKVARS(value)
                    );
                }
            }
        #endif

            QUEUE_MARK_CONTEXT_DEEP(context, name, val, "<context>", dump);

            if (IS_FRAME(val)) {
                //
                // The FRM_CALL is either on the stack--in which case its
                // already taken care of in terms of marking--or it has gone
                // bad in which case it should be ignored.
                //
                // !!! Should the GC null out bad pointers or just leave them?
            }
            else {
                if (VAL_CONTEXT_SPEC(val)) {
                    //
                    // !!! Under the module system, the spec is another
                    // context of an object constructed with the various pieces
                    // of module information.  This idea is being reviewed to
                    // see if what is called the "object spec" should be
                    // something more like a function spec, with the module
                    // information going in something called a "meta"
                    //
                    QUEUE_MARK_CONTEXT_DEEP(VAL_CONTEXT_SPEC(val), NULL, val, "<context-spec>", dump);
                }
            }

            // If CTX_STACKVARS is not NULL, the marking will be taken
            // care of in the walk of the chunk stack (which may hold data for
            // other stack-like REBVAL arrays that are not in contexts)

            break;
        }

        case REB_FUNCTION: {
            enum Reb_Func_Class fclass = VAL_FUNC_CLASS(val);

            if (fclass == FUNC_CLASS_USER || fclass == FUNC_CLASS_COMMAND)
                QUEUE_MARK_ARRAY_DEEP(VAL_FUNC_BODY(val), NULL, val, "<func-body>", REB_KIND_ARRAY, NULL, dump);

            if (fclass == FUNC_CLASS_ROUTINE || fclass == FUNC_CLASS_CALLBACK)
                Queue_Mark_Routine_Deep(VAL_ROUTINE(val), name, val, dump);

            if (fclass == FUNC_CLASS_SPECIALIZED)
                QUEUE_MARK_CONTEXT_DEEP(val->payload.function.impl.special, NULL, val, "<special>", dump);

            assert(VAL_FUNC_SPEC(val) == FUNC_SPEC(VAL_FUNC(val)));
            assert(VAL_FUNC_PARAMLIST(val) == FUNC_PARAMLIST(VAL_FUNC(val)));

            QUEUE_MARK_ARRAY_DEEP(VAL_FUNC_SPEC(val), NULL, val, "<spec>", REB_KIND_ARRAY, NULL, dump);
            QUEUE_MARK_ARRAY_DEEP(VAL_FUNC_PARAMLIST(val), NULL, val, "<paramlist>", REB_KIND_ARRAY, VAL_FUNC_PARAMLIST(val), dump);
            break;
        }

        case REB_VARARGS: {
            REBARR *subfeed;
            if (GET_VAL_FLAG(val, VARARGS_FLAG_NO_FRAME)) {
                //
                // A single-element shared series node is kept between
                // instances of the same vararg that was created with
                // MAKE ARRAY! - which fits compactly in a REBSER.
                //
                subfeed = *SUBFEED_ADDR_OF_FEED(VAL_VARARGS_ARRAY1(val));
                QUEUE_MARK_ARRAY_DEEP(VAL_VARARGS_ARRAY1(val), NULL, val, "<varargs-array1>", REB_KIND_ARRAY, NULL, dump);
            }
            else {
                subfeed = *SUBFEED_ADDR_OF_FEED(
                    CTX_VARLIST(VAL_VARARGS_FRAME_CTX(val))
                );
                QUEUE_MARK_CONTEXT_DEEP(VAL_VARARGS_FRAME_CTX(val), NULL, val, "<varargs-frame>", dump);
            }

            if (subfeed) {
                if (GET_ARR_FLAG(subfeed, ARRAY_FLAG_CONTEXT_VARLIST))
                    QUEUE_MARK_CONTEXT_DEEP(AS_CONTEXT(subfeed), NULL, val, "<subfeed>", dump);
                else
                    QUEUE_MARK_ARRAY_DEEP(subfeed, NULL, val, "<subfeed>", REB_KIND_ARRAY, NULL, dump);
            }

            break;
        }

        case REB_WORD:  // (and also used for function STACK backtrace frame)
        case REB_SET_WORD:
        case REB_GET_WORD:
        case REB_LIT_WORD:
        case REB_REFINEMENT:
        case REB_ISSUE:
            //
            // All bound words should keep their contexts from being GC'd...
            // even stack-relative contexts for functions.
            //
            if (GET_VAL_FLAG(val, VALUE_FLAG_RELATIVE)) {
                //
                // Marking the function's paramlist should be enough to
                // mark all the function's properties (there is an embedded
                // function value...)
                //
                REBFUN* func = VAL_WORD_FUNC(val);
                assert(GET_VAL_FLAG(val, WORD_FLAG_BOUND)); // should be set
                QUEUE_MARK_ARRAY_DEEP(FUNC_PARAMLIST(func), NULL, val, "<bound-to>", REB_KIND_ARRAY, FUNC_PARAMLIST(func), dump);
            }
            else if (GET_VAL_FLAG(val, WORD_FLAG_BOUND)) {
                REBCTX* context = VAL_WORD_CONTEXT(val);
                QUEUE_MARK_CONTEXT_DEEP(context, NULL, val, "<bound-to>", dump);
            }
            else if (GET_VAL_FLAG(val, WORD_FLAG_PICKUP)) {
                //
                // Special word class that might be seen on the stack during
                // a GC that's used by argument fulfillment when searching
                // for out-of-order refinements.  It holds two REBVAL*s
                // (for the parameter and argument of the refinement) and
                // both should be covered for GC already, because the
                // paramlist and arg variables are "in progress" for a call.
            }
            else {
                // The word is unbound...make sure index is 0 in debug build.
                //
            #if !defined(NDEBUG)
                assert(VAL_WORD_INDEX(val) == 0);
            #endif
            }
            break;

        case REB_NONE:
        case REB_BAR:
        case REB_LIT_BAR:
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
            assert(SER_WIDE(ser) <= sizeof(REBUNI));
            MARK_SERIES_ONLY(ser, NULL, val, "<series>", REB_KIND_SERIES, dump);
            break;

        case REB_IMAGE:
            //SET_SER_FLAG(VAL_SERIES_SIDE(val), SERIES_FLAG_MARK); //????
            MARK_SERIES_ONLY(VAL_SERIES(val), NULL, val, "<series>", REB_KIND_SERIES, dump);
            break;

        case REB_VECTOR:
            MARK_SERIES_ONLY(VAL_SERIES(val), NULL, val, "<series>", REB_KIND_SERIES, dump);
            break;

        case REB_BLOCK:
        case REB_GROUP:
        case REB_PATH:
        case REB_SET_PATH:
        case REB_GET_PATH:
        case REB_LIT_PATH:
            QUEUE_MARK_ARRAY_DEEP(VAL_ARRAY(val), NULL, val, "<series>", REB_KIND_ARRAY, NULL, dump);
            break;

        case REB_MAP: {
            REBMAP* map = VAL_MAP(val);
            QUEUE_MARK_ARRAY_DEEP(MAP_PAIRLIST(map), NULL, val, "<pairlist>", REB_KIND_ARRAY, NULL, dump);
            if (MAP_HASHLIST(map))
                MARK_SERIES_ONLY(MAP_HASHLIST(map), NULL, val, "<hashlist>", REB_KIND_HASH, dump);
            break;
        }

        case REB_LIBRARY:
            if (!IS_MARK_LIB(VAL_LIB_HANDLE(val))) {
                MARK_LIB(VAL_LIB_HANDLE(val));
                QUEUE_MARK_ARRAY_DEEP(VAL_LIB_SPEC(val), NULL, val, "<spec>", REB_KIND_ARRAY, NULL, dump);
                entry.addr = VAL_LIB_HANDLE(val);
                entry.name = NULL;
                entry.edge = "<handle>";
                entry.size = sizeof(*VAL_LIB_HANDLE(val));
                entry.parent = val;
                Dump_Mem_Entry(dump, &entry);
            }
            break;

        case REB_STRUCT:
            Queue_Mark_Struct_Deep(&VAL_STRUCT(val), "<REBSTU>", val, dump);
            break;

        case REB_GOB:
            Queue_Mark_Gob_Deep(VAL_GOB(val), NULL, val, "<REBGOB>", dump);
            break;

        case REB_EVENT:
            Queue_Mark_Event_Deep(val, name, parent, dump);
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
static void Mark_Array_Deep_Core(struct mark_stack_elem *elem, REBMDP *dump)
{
    REBCNT len;
    REBVAL *value, *key = NULL;
    REBARR *array = elem->array;
    const REBARR *keylist = elem->key_list;

    //printf("Marking array at %p\n", array);

#if !defined(NDEBUG)
    //
    // We should have marked this series at queueing time to keep it from
    // being doubly added before the queue had a chance to be processed
    //
    if (!GET_ARR_FLAG(array, SERIES_FLAG_MARK)) Panic_Array(array);

    // Make sure that a context's varlist wasn't marked without also marking
    // its keylist.  This could happen if QUEUE_MARK_ARRAY is used on a
    // context instead of QUEUE_MARK_CONTEXT.
    //
    if (GET_ARR_FLAG(array, ARRAY_FLAG_CONTEXT_VARLIST))
        assert(GET_ARR_FLAG(CTX_KEYLIST(AS_CONTEXT(array)), SERIES_FLAG_MARK));
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
    assert(GET_ARR_FLAG(array, SERIES_FLAG_ARRAY));
    assert(!SER_FREED(ARR_SERIES(array)));
#endif

    value = ARR_HEAD(array);
    if (keylist != NULL) {
        assert(ARR_LEN(array) <= ARR_LEN(keylist));
        key = ARR_HEAD(cast(REBARR*, keylist));
    }

    for (; NOT_END(value); value++) {
        const char *name = NULL;
    #if !defined(NDEBUG)
        if (IS_TRASH_DEBUG(value) && !GET_VAL_FLAG(value, TRASH_FLAG_SAFE))
            Panic_Array(array);
    #endif
        if (dump && key != NULL) {
            switch (VAL_TYPE(key)) {
            case REB_TYPESET:
                name = Get_Sym_Name(VAL_TYPESET_SYM(key));
                break;
            case REB_WORD:
                name = Get_Sym_Name(VAL_WORD_SYM(key));
                break;
            default:
                if (key != ARR_HEAD(cast(REBARR*, keylist))) {// the first element could be function!, native!, etc for FRAMEs
                    printf("unexpected type: %d\n", VAL_TYPE(key));
                    fclose(dump->out);
#ifndef NDEBUG
                    Panic_Mark_Stack(elem);
#endif
                    //Panic_Array(array);
                }
            }
            key++;
        }

        Queue_Mark_Value_Deep(value, name, array, "<has>", dump);
    }
}


//
//  Sweep_Series: C
// 
// Scans all series in all segments that are part of the
// SER_POOL.  If a series had its lifetime management
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

    for (seg = Mem_Pools[SER_POOL].segs; seg; seg = seg->next) {
        REBSER *series = cast(REBSER *, seg + 1);
        REBCNT n;
        for (n = Mem_Pools[SER_POOL].units; n > 0; n--, series++) {
            // See notes on Make_Node() about how the first allocation of a
            // unit zero-fills *most* of it.  But after that it's up to the
            // caller of Free_Node() to zero out whatever bits it uses to
            // indicate "freeness".  We check the zeroness of the `wide`.
            if (SER_FREED(series))
                continue;

            if (GET_SER_FLAG(series, SERIES_FLAG_MANAGED)) {
                if (shutdown || !GET_SER_FLAG(series, SERIES_FLAG_MARK)) {
                    GC_Kill_Series(series);
                    count++;
                } else
                    CLEAR_SER_FLAG(series, SERIES_FLAG_MARK);
            }
            else
                assert(!GET_SER_FLAG(series, SERIES_FLAG_MARK));
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
// have already had their SERIES_FLAG_MARK set to prevent being added
// to the stack multiple times, but the items they can reach
// are not necessarily marked yet.
// 
// Processing continues until all reachable items from the mark
// stack are known to be marked.
//
static void Propagate_All_GC_Marks(REBMDP *dump)
{
    assert(!in_mark);

    Dump_Mem_Comment(dump, "Progagate all GC marks");

    while (SER_LEN(GC_Mark_Stack) != 0) {
        // Data pointer may change in response to an expansion during
        // Mark_Array_Deep_Core(), so must be refreshed on each loop.
        //
        REBARR *array;
        struct mark_stack_elem *elem, *last;

        SET_SERIES_LEN(GC_Mark_Stack, SER_LEN(GC_Mark_Stack) - 1);

        elem = SER_AT(struct mark_stack_elem, GC_Mark_Stack, SER_LEN(GC_Mark_Stack));

        // Drop the series we are processing off the tail, as we could be
        // queuing more of them (hence increasing the tail).
        //

        last = elem + 1;

        last->array = NULL;
        last->key_list = NULL;

        Mark_Array_Deep_Core(elem, elem->dump);
    }
}

//
//  Dump_Memory_Usage: C
//
// Dump detailed memory usage to a file
//
void Dump_Memory_Usage(const REBCHR *path)
{
    REBMDP dump;
#ifdef TO_WINDOWS
    dump.out = _wfopen(cast(const REBUNI*, path), "w");
#else
    dump.out = fopen(cast(const char*, path), "w");
#endif
    if (dump.out == NULL) {
        return;
    }
    dump.parent = NULL;
    Dump_Mem_Comment(&dump, "Addr,parent,type,size,name");

    Recycle_Core(FALSE, &dump);

    fclose(dump.out);
}

//
//  Recycle_Core: C
// 
// Recycle memory no longer needed.
//
REBCNT Recycle_Core(REBOOL shutdown, REBMDP *dump)
{
    REBINT n;
    REBCNT count;
    REBI64 gc_ballast0 = GC_Ballast;

    //Debug_Num("GC", GC_Disabled);

    ASSERT_NO_GC_MARKS_PENDING();

    // If disabled, exit now but set the pending flag.
    if (GC_Disabled || !GC_Active) {
        SET_SIGNAL(SIG_RECYCLE);
        //Print("pending");
        return 0;
    }

    // Some of the call stack frames may have been invoked with a C function
    // call that took a comma-separated list of REBVAL (the way printf works,
    // a variadic va_list).  These call frames have no REBARR series behind
    // them, but still need to be enumerated to protect the values coming up
    // in the later DO/NEXTs.  But enumerating a C va_list can't be undone;
    // the information were be lost if it weren't saved.  We "reify" the
    // va_list into a REBARR before we start the GC (as it makes new series).
    //
    {
        struct Reb_Frame *f = FS_TOP;
        for (; f != NULL; f = f->prior) {
            const REBOOL truncated = TRUE;
            if (f->indexor == VALIST_FLAG)
                Reify_Va_To_Array_In_Frame(f, truncated); // see function
        }
    }

    if (Reb_Opts->watch_recycle) Debug_Str(cs_cast(BOOT_STR(RS_WATCH, 0)));

    GC_Disabled = 1;

#if !defined(NDEBUG)
    PG_Reb_Stats->Recycle_Counter++;
    PG_Reb_Stats->Recycle_Series = Mem_Pools[SER_POOL].free;

    PG_Reb_Stats->Mark_Count = 0;
#endif

    // WARNING: These terminate existing open blocks. This could
    // be a problem if code is building a new value at the tail,
    // but has not yet updated the TAIL marker.
    VAL_TERM_ARRAY(TASK_BUF_EMIT);
    VAL_TERM_ARRAY(TASK_BUF_COLLECT);

    // The data stack logic is that it is contiguous values that has no
    // REB_ENDs in it except at the series end.  Bumping up against that
    // END signal is how the stack knows when it needs to grow.  But every
    // drop of the stack doesn't clean up the value dropped--because the
    // values are not END markers, they are considered fine as far as the
    // stack is concerned to indicate unused capacity.  However, the GC
    // doesn't want to mark these "marker-only" values live.
    //
    // Hence this temporarily puts an END marker at one past the DSP, if it
    // is required to do so.  Then it puts safe trash back--or leaves it as
    // an end if it wasn't disturbed.
    //
    if (IS_END(&DS_Movable_Base[DSP + 1]))
        assert(DSP == ARR_LEN(DS_Array));
    else
        SET_END(&DS_Movable_Base[DSP + 1]);

    // MARKING PHASE: the "root set" from which we determine the liveness
    // (or deadness) of a series.  If we are shutting down, we are freeing
    // *all* of the series that are managed by the garbage collector, so
    // we don't mark anything as live.

    if (!shutdown) {
        REBSER **sp;
        REBVAL **vp;
        REBINT n;

        struct mem_dump_entry entry;


        // Mark series that have been temporarily protected from garbage
        // collection with PUSH_GUARD_SERIES.  We have to check if the
        // series is a context (so the keylist gets marked) or an array (so
        // the values are marked), or if it's just a data series which
        // should just be marked shallow.
        //
        sp = SER_HEAD(REBSER*, GC_Series_Guard);
        entry.name = "GC_Series_Guard";
        entry.edge = NULL;
        entry.addr = GC_Series_Guard;
        entry.parent = NULL;
        entry.kind = REB_KIND_SERIES;
        entry.size = SER_TOTAL(GC_Series_Guard);
        Dump_Mem_Entry(dump, &entry);

        for (n = SER_LEN(GC_Series_Guard); n > 0; n--, sp++) {
            if (GET_SER_FLAG(*sp, ARRAY_FLAG_CONTEXT_VARLIST))
                MARK_CONTEXT_DEEP(AS_CONTEXT(*sp), NULL, GC_Series_Guard, NULL, dump);
            else if (Is_Array_Series(*sp))
                MARK_ARRAY_DEEP(AS_ARRAY(*sp), NULL, GC_Series_Guard, NULL, REB_KIND_SERIES, NULL, dump);
            else
                MARK_SERIES_ONLY(*sp, NULL, GC_Series_Guard, NULL, REB_KIND_SERIES, dump);
        }

        // Mark value stack (temp-saved values):
        vp = SER_HEAD(REBVAL*, GC_Value_Guard);
        entry.name = "GC_Value_Guard";
        entry.addr = GC_Value_Guard;
        entry.parent = NULL;
        entry.edge = NULL;
        entry.kind = REB_KIND_SERIES;
        entry.size = SER_TOTAL(GC_Value_Guard);
        Dump_Mem_Entry(dump, &entry);

        for (n = SER_LEN(GC_Value_Guard); n > 0; n--, vp++) {
            if (NOT_END(*vp))
                Queue_Mark_Value_Deep(*vp, NULL, GC_Value_Guard, "<has>", dump);
            Propagate_All_GC_Marks(dump);
        }

        // Mark chunk stack (non-movable saved arrays of values)
        {
            Dump_Mem_Comment(dump, "Dump chunk stack");

            struct Reb_Chunk *chunk = TG_Top_Chunk;
            entry.name = "TG_Top_Chunk";
            entry.addr = TG_Top_Chunk;
            entry.parent = NULL;
            entry.edge = NULL;
            entry.kind = REB_KIND_CHUNK;
            entry.size = BASE_CHUNK_SIZE;
            Dump_Mem_Entry(dump, &entry);

            while (chunk) {
                REBVAL *chunk_value = &chunk->values[0];
                while (
                    cast(REBYTE*, chunk_value)
                    < cast(REBYTE*, chunk) + chunk->size.bits
                ) {
                    if (NOT_END(chunk_value))
                        Queue_Mark_Value_Deep(chunk_value, NULL, chunk, "<keeps>", dump); //FIXME
                    chunk_value++;
                }
                if (chunk->prev) {
                    entry.name = "Chunk";
                    entry.addr = chunk->prev;
                    entry.parent = chunk;
                    entry.kind = REB_KIND_CHUNK;
                    entry.size = BASE_CHUNK_SIZE;
                    Dump_Mem_Entry(dump, &entry);
                }
                chunk = chunk->prev;
            }
        }

        // Mark all root series:
        //
        Dump_Mem_Comment(dump, "Dumping Root-Context");
        MARK_CONTEXT_DEEP(PG_Root_Context, "Root-Context", NULL, NULL, dump);
        Dump_Mem_Comment(dump, "Dumping Task-Context");
        MARK_CONTEXT_DEEP(TG_Task_Context, "Task-Context", NULL, NULL, dump);

        // Mark potential error object from callback!
        Queue_Mark_Value_Deep(&Callback_Error, "Callback-Error", NULL, NULL, dump);
        Propagate_All_GC_Marks(dump);

        // !!! This hook point is an interim measure for letting a host
        // mark REBVALs that it is holding onto which are not contained in
        // series.  It is motivated by Ren/C++, which wraps REBVALs in
        // `ren::Value` class instances, and is able to enumerate the
        // "live" classes (they "die" when the destructor runs).
        //
        if (GC_Mark_Hook) {
            (*GC_Mark_Hook)();
            Propagate_All_GC_Marks(dump);
        }

        // Mark all devices:
        Dump_Mem_Comment(dump, "Dumping all devices!");
        Mark_Devices_Deep(dump);
        Propagate_All_GC_Marks(dump);

        // Mark function call frames:
        Dump_Mem_Comment(dump, "Dumping function call frames");
        Mark_Frame_Stack_Deep(dump);
        Propagate_All_GC_Marks(dump);
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
    PG_Reb_Stats->Recycle_Series = Mem_Pools[SER_POOL].free - PG_Reb_Stats->Recycle_Series;
    PG_Reb_Stats->Recycle_Series_Total += PG_Reb_Stats->Recycle_Series;
    PG_Reb_Stats->Recycle_Prior_Eval = Eval_Cycles;
#endif

    // Do not adjust task variables or boot strings in shutdown when they
    // are being freed.
    //
    if (!shutdown) {

        // !!! This code was added by Atronix to deal with frequent garbage
        // collection, but the logic is not correct.  The issue has been
        // raised and is commented out pending a correct solution.
        //
        // https://github.com/zsx/r3/issues/32
        //
        REBI64 bytes_freed = GC_Ballast - gc_ballast0;
        REBI64 bytes_used = VAL_INT64(TASK_BALLAST) - GC_Ballast;
	    //printf("Recycled %d (%lld bytes), used: %lld, GC_Ballast: %lld, Task Ballast: %lld\n", count, bytes_freed, bytes_used, GC_Ballast, VAL_INT64(TASK_BALLAST));
        
        /* if the used bytes is beyond the range of (75%, 90%) of Task_Ballast, adjust Task_Ballast to (1.25 * bytes_used)
         * The idea is that before next recycle runs, it can at least allocate (1/10 * Task_Ballast) bytes of memory,
         * and at most (1/4 * Task_Ballast) bytes.
         * 
         * Keep in mind that it needs to alloca Task_Ballast (MEM_BALLAST) bytes of memory before the very first recycle runs.
         */

        if (bytes_used > VAL_INT64(TASK_BALLAST) * 9 / 10) {/* not enough memory was freed */
            //increasing ballast by half
            REBI64 ballast = VAL_INT64(TASK_BALLAST);
            VAL_INT64(TASK_BALLAST) = bytes_used * 5 / 4;
            GC_Ballast += VAL_INT64(TASK_BALLAST) - ballast;
            //printf("Task ballast is increased to %lld\n", VAL_INT64(TASK_BALLAST));
        } else if (bytes_used < VAL_INT64(TASK_BALLAST) * 3 / 4 && VAL_INT64(TASK_BALLAST) > MEM_BALLAST) {
            REBI64 ballast = VAL_INT64(TASK_BALLAST);
            VAL_INT64(TASK_BALLAST) = bytes_used * 5 / 4;
            if (VAL_INT64(TASK_BALLAST) < MEM_BALLAST) {
                VAL_INT64(TASK_BALLAST) = MEM_BALLAST;
            }
            //printf("Task ballast is reduced to %lld\n", VAL_INT64(TASK_BALLAST));
            GC_Ballast -= ballast - VAL_INT64(TASK_BALLAST);
        }

        //printf("After adjustment, GC_Ballast: %lld\n", GC_Ballast);

        GC_Disabled = 0;
        if (Reb_Opts->watch_recycle)
            Debug_Fmt(cs_cast(BOOT_STR(RS_WATCH, 1)), count);

        // Undo the data stack END marking if necessary.
        //
        if (DSP != ARR_LEN(DS_Array))
            SET_TRASH_SAFE(&DS_Movable_Base[DSP + 1]);
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
    return Recycle_Core(FALSE, NULL);
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

    if (SER_FULL(GC_Series_Guard)) Extend_Series(GC_Series_Guard, 8);

    *SER_AT(
        REBSER*,
        GC_Series_Guard,
        SER_LEN(GC_Series_Guard)
    ) = series;

    SET_SERIES_LEN(GC_Series_Guard, SER_LEN(GC_Series_Guard) + 1);
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

    if (SER_FULL(GC_Value_Guard)) Extend_Series(GC_Value_Guard, 8);

    *SER_AT(
        const REBVAL*,
        GC_Value_Guard,
        SER_LEN(GC_Value_Guard)
    ) = value;

    SET_SERIES_LEN(GC_Value_Guard, SER_LEN(GC_Value_Guard) + 1);
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
    // not seen by the GC until such time as it gets the SERIES_FLAG_MANAGED flag
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
    GC_Mark_Stack = Make_Series(100, sizeof(struct mark_stack_elem), MKS_NONE);
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
