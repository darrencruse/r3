//
// Rebol 3 Language Interpreter and Run-time Environment
// "Ren-C" branch @ https://github.com/metaeducation/ren-c
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
//  Summary: REDUCE and COMPOSE natives and associated service routines
//  File: %n-reduce.h
//
//=////////////////////////////////////////////////////////////////////////=//
//
// !!! The R3-Alpha REDUCE routine contained several auxiliariy refinements
// used by fringe dialects.  These need review for whether they are still in
// working order--or if they need to just be replaced or removed.
//

#include "sys-core.h"

//
//  Reduce_Array_Throws: C
//
// Reduce array from the index position specified in the value.
// Collect all values from stack and make them into a BLOCK! REBVAL.
//
// !!! Review generalization of this to produce an array and not a REBVAL
// of a particular kind.
//
REBOOL Reduce_Array_Throws(
    REBVAL *out,
    REBARR *array,
    REBCNT index,
    REBOOL into
) {
    REBDSP dsp_orig = DSP;
    REBIXO indexor = index;

    // Through the DO_NEXT_MAY_THROW interface, we can't tell the difference
    // between DOing an array that literally contains an UNSET! and an empty
    // array, because both give back an unset value and an end position.
    // We'd like REDUCE to treat `reduce []` and `reduce [#[unset!]]` in
    // a different way, so must do a special check to handle the former.
    //
    if (IS_END(ARR_AT(array, index))) {
        if (into)
            return FALSE;

        Val_Init_Block(out, Make_Array(0));
        return FALSE;
    }

    while (indexor != END_FLAG) {
        REBVAL reduced;
        VAL_INIT_WRITABLE_DEBUG(&reduced);

        DO_NEXT_MAY_THROW(indexor, &reduced, array, indexor);

        if (indexor == THROWN_FLAG) {
            *out = reduced;
            DS_DROP_TO(dsp_orig);
            return TRUE;
        }

        DS_PUSH(&reduced);
    }

    Pop_Stack_Values(out, dsp_orig, into ? REB_MAX : REB_BLOCK);
    return FALSE;
}


//
//  Reduce_Only: C
//
// Reduce only words and paths not found in word list.
//
void Reduce_Only(
    REBVAL *out,
    REBARR *block,
    REBCNT index,
    REBVAL *words,
    REBOOL into
) {
    REBDSP dsp_orig = DSP;
    REBVAL *val;
    const REBVAL *v;
    REBARR *arr = 0;
    REBCNT idx = 0;

    if (IS_BLOCK(words)) {
        arr = VAL_ARRAY(words);
        idx = VAL_INDEX(words);
    }

    for (val = ARR_AT(block, index); NOT_END(val); val++) {
        if (IS_WORD(val)) {
            // Check for keyword:
            if (
                arr &&
                NOT_FOUND != Find_Word_In_Array(arr, idx, VAL_WORD_CANON(val))
            ) {
                DS_PUSH(val);
                continue;
            }
            v = GET_OPT_VAR_MAY_FAIL(val);
            DS_PUSH(v);
        }
        else if (IS_PATH(val)) {
            if (arr) {
                // Check for keyword/path:
                v = VAL_ARRAY_AT(val);
                if (IS_WORD(v)) {
                    if (
                        NOT_FOUND
                        != Find_Word_In_Array(arr, idx, VAL_WORD_CANON(v))
                    ) {
                        DS_PUSH(val);
                        continue;
                    }
                }
            }

            // pushes val on stack
            DS_PUSH_TRASH_SAFE;
            if (Do_Path_Throws(DS_TOP, NULL, val, NULL))
                fail (Error_No_Catch_For_Throw(DS_TOP));
        }
        else DS_PUSH(val);
        // No need to check for unwinds (THROWN) here, because unwinds should
        // never be accessible via words or paths.
    }

    Pop_Stack_Values(out, dsp_orig, into ? REB_MAX : REB_BLOCK);

    assert(DSP == dsp_orig);
}


//
//  Reduce_Array_No_Set_Throws: C
//
REBOOL Reduce_Array_No_Set_Throws(
    REBVAL *out,
    REBARR *block,
    REBCNT index,
    REBOOL into
) {
    REBDSP dsp_orig = DSP;
    REBIXO indexor = index;

    while (index < ARR_LEN(block)) {
        REBVAL *value = ARR_AT(block, index);
        if (IS_SET_WORD(value)) {
            DS_PUSH(value);
            index++;
        }
        else {
            REBVAL reduced;
            VAL_INIT_WRITABLE_DEBUG(&reduced);

            DO_NEXT_MAY_THROW(indexor, &reduced, block, indexor);
            if (indexor == THROWN_FLAG) {
                *out = reduced;
                DS_DROP_TO(dsp_orig);
                return TRUE;
            }
            DS_PUSH(&reduced);
        }
    }

    Pop_Stack_Values(out, dsp_orig, into ? REB_MAX : REB_BLOCK);

    return FALSE;
}


//
//  reduce: native [
//
//  {Evaluates expressions and returns multiple results.}
//
//      value
//      /no-set
//          "Keep set-words as-is. Do not set them."
//      /only
//          "Only evaluate words and paths, not functions"
//      words [block! none!]
//          "Optional words that are not evaluated (keywords)"
//      /into
//          {Output results into a series with no intermediate storage}
//      target [any-array!]
//  ]
//
REBNATIVE(reduce)
{
    PARAM(1, value);
    REFINE(2, no_set);
    REFINE(3, only);
    PARAM(4, words);
    REFINE(5, into);
    PARAM(6, target);

    REBVAL *value = ARG(value);

    if (IS_BLOCK(value)) {
        if (REF(into))
            *D_OUT = *ARG(target);

        if (REF(no_set)) {
            if (Reduce_Array_No_Set_Throws(
                D_OUT, VAL_ARRAY(value), VAL_INDEX(value), REF(into)
            )) {
                return R_OUT_IS_THROWN;
            }
        }
        else if (REF(only)) {
            Reduce_Only(
                D_OUT,
                VAL_ARRAY(value),
                VAL_INDEX(value),
                ARG(words),
                REF(into)
            );
        }
        else {
            if (Reduce_Array_Throws(
                D_OUT, VAL_ARRAY(value), VAL_INDEX(value), REF(into)
            )) {
                return R_OUT_IS_THROWN;
            }
        }

        return R_OUT;
    }

    if (REF(only) || REF(no_set) || REF(into)) {
        //
        // !!! These features on single elements have not been defined or
        // implemented, and should be reviewed.
        //
        fail (Error(RE_MISC));
    }

    // A single element should do what is effectively an evaluation but with
    // no arguments.  This is a change in behavior from R3-Alpha, which would
    // just return the input as is, e.g. `reduce quote (1 + 2)` => (1 + 2).
    //
    // !!! Should the error be more "reduce-specific" if args were required?
    //
    if (DO_VALUE_THROWS(D_OUT, value))
        return R_OUT_IS_THROWN;

    return R_OUT;
}


//
//  Compose_Values_Throws: C
//
// Compose a block from a block of un-evaluated values and GROUP! arrays that
// are evaluated.  This calls into Do_Core, so if 'into' is provided, then its
// series must be protected from garbage collection.
//
//     deep - recurse into sub-blocks
//     only - parens that return blocks are kept as blocks
//
// Writes result value at address pointed to by out.
//
REBOOL Compose_Values_Throws(
    REBVAL *out,
    REBVAL value[],
    REBOOL deep,
    REBOOL only,
    REBOOL into
) {
    REBDSP dsp_orig = DSP;

    for (; NOT_END(value); value++) {
        if (IS_GROUP(value)) {
            REBVAL evaluated;
            VAL_INIT_WRITABLE_DEBUG(&evaluated);

            if (DO_ARRAY_THROWS(&evaluated, value)) {
                *out = evaluated;
                DS_DROP_TO(dsp_orig);
                return TRUE;
            }

            if (IS_BLOCK(&evaluated) && !only) {
                //
                // compose [blocks ([a b c]) merge] => [blocks a b c merge]
                //
                REBVAL *push = VAL_ARRAY_AT(&evaluated);
                while (!IS_END(push)) {
                    DS_PUSH(push);
                    push++;
                }
            }
            else if (!IS_UNSET(&evaluated)) {
                //
                // compose [(1 + 2) inserts as-is] => [3 inserts as-is]
                // compose/only [([a b c]) unmerged] => [[a b c] unmerged]
                //
                DS_PUSH(&evaluated);
            }
            else {
                //
                // compose [(print "Unsets *vanish*!")] => []
                //
            }
        }
        else if (deep) {
            if (IS_BLOCK(value)) {
                //
                // compose/deep [does [(1 + 2)] nested] => [does [3] nested]

                REBVAL composed;
                VAL_INIT_WRITABLE_DEBUG(&composed);

                if (Compose_Values_Throws(
                    &composed, VAL_ARRAY_HEAD(value), TRUE, only, into
                )) {
                    *out = composed;
                    DS_DROP_TO(dsp_orig);
                    return TRUE;
                }

                DS_PUSH(&composed);
            }
            else {
                DS_PUSH(value);
                if (ANY_ARRAY(value)) {
                    //
                    // compose [copy/(orig) (copy)] => [copy/(orig) (copy)]
                    // !!! path and second group are copies, first group isn't
                    //
                    VAL_ARRAY(DS_TOP) = Copy_Array_Shallow(VAL_ARRAY(value));
                    MANAGE_ARRAY(VAL_ARRAY(DS_TOP));
                }
            }
        }
        else {
            //
            // compose [[(1 + 2)] (reverse "wollahs")] => [[(1 + 2)] "shallow"]
            //
            DS_PUSH(value);
        }
    }

    Pop_Stack_Values(out, dsp_orig, into ? REB_MAX : REB_BLOCK);

    return FALSE;
}


//
//  compose: native/frameless [
//
//  {Evaluates only the GROUP!s in a block of expressions, returning a block.}
//
//      value
//          "Block to compose (or any other type evaluates to itself)"
//                                          ; ^-- is this sensible?
//      /deep
//          "Compose nested blocks"
//      /only
//          {Insert a block as a single value (not the contents of the block)}
//      /into
//          {Output results into a series with no intermediate storage}
//      out [any-array! any-string! binary!]
//  ]
//
REBNATIVE(compose)
//
// !!! Should 'compose quote (a (1 + 2) b)' give back '(a 3 b)' ?
// !!! What about 'compose quote a/(1 + 2)/b' ?
{
    PARAM(1, value);
    REFINE(2, deep);
    REFINE(3, only);
    REFINE(4, into);
    PARAM(5, out);

    if (D_FRAMELESS) {
        //
        // The ARG(value) is unavailable in a frameless evaluation, so we'll
        // have to evaluate it here.  Note that the usage does not require
        // it to be GC-safe (at time of writing).
        //
        REBVAL value;
        VAL_INIT_WRITABLE_DEBUG(&value);
        SET_TRASH_SAFE(&value); // !!! could c->out offer this before a GC?
        D_PROTECT_X(&value); // must be protected to save its content values[]

        if (D_INDEXOR == END_FLAG)
            fail (Error_No_Arg(D_LABEL_SYM, PAR(value)));

        DO_NEXT_REFETCH_MAY_THROW(&value, D_CALL, DO_FLAG_LOOKAHEAD);

        if (D_INDEXOR == THROWN_FLAG) {
            *D_OUT = value;
            return R_OUT_IS_THROWN;
        }

        if (IS_UNSET(&value))
            fail (Error_Arg_Type(D_LABEL_SYM, PAR(value), Type_Of(&value)));

        if (!IS_BLOCK(&value)) {
            *D_OUT = value;
            return R_OUT;
        }

        if (Compose_Values_Throws(
            D_OUT, VAL_ARRAY_HEAD(&value), FALSE, FALSE, FALSE
        )) {
            // Here we want to be able to recover in situations like:
            //
            //     compose [(exit/from :compose)] print "this should print"
            //
            // So we can't overwrite the index.  Signal check for exit.
            //
            D_MODE = CALL_MODE_THROW_PENDING;
            return R_OUT_IS_THROWN;
        }

        return R_OUT;
    }

    // !!! See above--should all non-BLOCK! be evaluating to themselves?
    //
    if (!IS_BLOCK(ARG(value))) {
        *D_OUT = *ARG(value);
        return R_OUT;
    }

    // Compose_Values_Throws() expects `out` to contain the target if it is
    // passed TRUE as the `into` flag.
    //
    if (REF(into)) *D_OUT = *ARG(out);

    if (Compose_Values_Throws(
        D_OUT, VAL_ARRAY_HEAD(ARG(value)), REF(deep), REF(only), REF(into)
    )) {
        return R_OUT_IS_THROWN;
    }

    return R_OUT;
}
