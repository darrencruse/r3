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
**  Module:  c-function.c
**  Summary: support for functions, actions, closures and routines
**  Section: core
**  Author:  Carl Sassenrath, Shixin Zeng
**  Notes:
**
***********************************************************************/
/*
    Structure of functions:

        spec - interface spec block
        body - body code
        args - args list (see below)

    Args list is a block of word+values:

        word - word, 'word, :word, /word
        value - typeset! or none (valid datatypes)

    Args list provides:

        1. specifies arg order, arg kind (e.g. 'word)
        2. specifies valid datatypes (typesets)
        3. used for word and type in error output
        4. used for debugging tools (stack dumps)
        5. not used for MOLD (spec is used)
        6. used as a (pseudo) frame of function variables

*/

#include "sys-core.h"

//
//  List_Func_Words: C
// 
// Return a block of function words, unbound.
// Note: skips 0th entry.
//
REBARR *List_Func_Words(const REBVAL *func)
{
    REBARR *array = Make_Array(VAL_FUNC_NUM_PARAMS(func));
    REBVAL *typeset = VAL_FUNC_PARAMS_HEAD(func);

    for (; !IS_END(typeset); typeset++) {
        enum Reb_Kind kind;

        if (VAL_GET_EXT(typeset, EXT_WORD_HIDE)) {
            // "true local" (e.g. it was a SET-WORD! in the spec)
            // treat as invisible and do not expose via WORDS-OF
            continue;
        }

        if (VAL_GET_EXT(typeset, EXT_TYPESET_REFINEMENT))
            kind = REB_REFINEMENT;
        else if (VAL_GET_EXT(typeset, EXT_TYPESET_QUOTE)) {
            if (VAL_GET_EXT(typeset, EXT_TYPESET_EVALUATE))
                kind = REB_LIT_WORD;
            else
                kind = REB_GET_WORD;
        }
        else {
            // Currently there's no meaning for non-quoted non-evaluating
            // things (only 3 param types for foo:, 'foo, :foo)
            assert(VAL_GET_EXT(typeset, EXT_TYPESET_EVALUATE));
            kind = REB_WORD;
        }

        Val_Init_Word_Unbound(
            Alloc_Tail_Array(array), kind, VAL_TYPESET_SYM(typeset)
        );
    }

    return array;
}


//
//  List_Func_Typesets: C
// 
// Return a block of function arg typesets.
// Note: skips 0th entry.
//
REBARR *List_Func_Typesets(REBVAL *func)
{
    REBARR *array = Make_Array(VAL_FUNC_NUM_PARAMS(func));
    REBVAL *typeset = VAL_FUNC_PARAMS_HEAD(func);

    for (; !IS_END(typeset); typeset++) {
        REBVAL *value = Alloc_Tail_Array(array);
        *value = *typeset;

        // !!! It's already a typeset, but this will clear out the header
        // bits.  This may not be desirable over the long run (what if
        // a typeset wishes to encode hiddenness, protectedness, etc?)

        VAL_RESET_HEADER(value, REB_TYPESET);
    }

    return array;
}


//
//  Check_Func_Spec: C
// 
// Check function spec of the form:
// 
// ["description" arg "notes" [type! type2! ...] /ref ...]
// 
// Throw an error for invalid values.
//
REBARR *Check_Func_Spec(REBARR *spec)
{
    REBVAL *item;
    REBARR *keylist;
    REBVAL *typeset;

    keylist = Collect_Frame(
        NULL, ARRAY_HEAD(spec), BIND_ALL | BIND_NO_DUP | BIND_NO_SELF
    );

    // Whatever function is being made, it must fill in the keylist slot 0
    // with an ANY-FUNCTION! value corresponding to the function that it is
    // the keylist of.  Use SET_TRASH so that the debug build will leave
    // an alarm if that value isn't thrown in (the GC would complain...)

    typeset = ARRAY_HEAD(keylist);
    SET_TRASH_IF_DEBUG(typeset);

    // !!! needs more checks
    for (item = ARRAY_HEAD(spec); NOT_END(item); item++) {

        if (ANY_BINSTR(item)) {
            // A goal of the Ren-C design is that core generators like
            // MAKE FUNCTION! an MAKE OBJECT! do not know any keywords or
            // key strings.  As a consequence, the most flexible offering
            // to function generators is to allow them to let as many
            // strings or tags or otherwise be stored in the spec as
            // they might wish to.  It's up to them to take them out.
            //
            // So it's not Check_Func_Spec's job to filter out "bad" string
            // patterns.  Anything is fair game:
            //
            //      [foo [type!] {doc string :-)}]
            //      [foo {doc string :-/} [type!]]
            //      [foo {doc string1 :-/} {doc string2 :-(} [type!]]
            //
            // HELP and other clients of SPEC-OF are left with the burden of
            // sorting out the variants.  The current policy of HELP is only
            // to show strings.
            //
            // !!! Though the system isn't supposed to have a reaction to
            // strings, is there a meaning for BINARY! besides ignoring it?

            continue;
        }

        switch (VAL_TYPE(item)) {
        case REB_BLOCK:
            if (typeset == ARRAY_HEAD(keylist)) {
                // !!! Rebol2 had the ability to put a block in the first
                // slot before any parameters, in which you could put words.
                // This is deprecated in favor of the use of tags.  We permit
                // [catch] and [throw] during Rebol2 => Rebol3 migration.

                REBVAL *attribute = VAL_ARRAY_AT(item);
                for (; NOT_END(attribute); attribute++) {
                    if (IS_WORD(attribute)) {
                        if (VAL_WORD_SYM(attribute) == SYM_CATCH)
                            continue; // ignore it;
                        if (VAL_WORD_SYM(attribute) == SYM_THROW) {
                            // !!! Basically a synonym for <transparent>, but
                            // transparent is now a manipulation done by the
                            // function generators *before* the internal spec
                            // is checked...and the flag is removed.  So
                            // simulating it here is no longer easy...hence
                            // ignore it;
                            continue;
                        }
                        // no other words supported, fall through to error
                    }
                    fail (Error(RE_BAD_FUNC_DEF, item));
                }
                break; // leading block handled if we get here, no more to do
            }

            // Turn block into typeset for parameter at current index
            // Note: Make_Typeset leaves VAL_TYPESET_SYM as-is
            Make_Typeset(VAL_ARRAY_HEAD(item), typeset, 0);
            break;

        case REB_WORD:
            typeset++;
            assert(
                IS_TYPESET(typeset)
                && VAL_TYPESET_SYM(typeset) == VAL_WORD_SYM(item)
            );
            VAL_SET_EXT(typeset, EXT_TYPESET_EVALUATE);
            break;

        case REB_GET_WORD:
            typeset++;
            assert(
                IS_TYPESET(typeset)
                && VAL_TYPESET_SYM(typeset) == VAL_WORD_SYM(item)
            );
            VAL_SET_EXT(typeset, EXT_TYPESET_QUOTE);
            break;

        case REB_LIT_WORD:
            typeset++;
            assert(
                IS_TYPESET(typeset)
                && VAL_TYPESET_SYM(typeset) == VAL_WORD_SYM(item)
            );
            VAL_SET_EXT(typeset, EXT_TYPESET_QUOTE);
            // will actually only evaluate get-word!, get-path!, and paren!
            VAL_SET_EXT(typeset, EXT_TYPESET_EVALUATE);
            break;

        case REB_REFINEMENT:
            typeset++;
            assert(
                IS_TYPESET(typeset)
                && VAL_TYPESET_SYM(typeset) == VAL_WORD_SYM(item)
            );
            VAL_SET_EXT(typeset, EXT_TYPESET_REFINEMENT);

        #if !defined(NDEBUG)
            // Because Mezzanine functions are written to depend on the idea
            // that when they get a refinement it will be a WORD! and not a
            // LOGIC!, we have to capture the desire to get LOGIC! vs WORD!
            // at function creation time...not dispatch time.  We encode the
            // bit in the refinement's typeset that it accepts.
            if (LEGACY(OPTIONS_REFINEMENTS_TRUE)) {
                VAL_TYPESET_BITS(typeset) =
                    (FLAGIT_64(REB_LOGIC) | FLAGIT_64(REB_NONE));
                break;
            }
        #endif
            // Refinements can nominally be only WORD! or NONE!
            VAL_TYPESET_BITS(typeset) =
                (FLAGIT_64(REB_WORD) | FLAGIT_64(REB_NONE));
            break;

        case REB_SET_WORD:
            // "True locals"... these will not be visible via WORDS-OF and
            // will be skipped during argument fulfillment.  We re-use the
            // same option flag that is used to hide words other places.

            typeset++;
            assert(
                IS_TYPESET(typeset)
                && VAL_TYPESET_SYM(typeset) == VAL_WORD_SYM(item)
            );
            VAL_SET_EXT(typeset, EXT_WORD_HIDE);
            break;

        default:
            fail (Error(RE_BAD_FUNC_DEF, item));
        }
    }

    MANAGE_ARRAY(keylist);
    return keylist;
}


//
//  Make_Native: C
//
void Make_Native(REBVAL *out, REBARR *spec, REBFUN func, enum Reb_Kind type)
{
    //Print("Make_Native: %s spec %d", Get_Sym_Name(type+1), SERIES_LEN(spec));
    ENSURE_ARRAY_MANAGED(spec);
    VAL_FUNC_SPEC(out) = spec;
    VAL_FUNC_PARAMLIST(out) = Check_Func_Spec(spec);

    VAL_FUNC_CODE(out) = func;
    VAL_RESET_HEADER(out, type);

    // Save the function value in slot 0 of the paramlist so that having
    // just the paramlist REBSER can get you the full REBVAL of the function
    // that it is the paramlist for.

    *ARRAY_HEAD(VAL_FUNC_PARAMLIST(out)) = *out;

    // These native routines want to be recognized by keylist, not by their
    // VAL_FUNC_CODE pointers.  (RETURN because the code pointer is swapped
    // out for VAL_FUNC_RETURN_TO, and EVAL for 1 test vs. 2 in the eval loop.)
    //
    // PARSE wants to throw its value from nested code to itself, and doesn't
    // want to thread its known D_FUNC value through the call stack.
    //
    if (func == &N_return) {
        *ROOT_RETURN_NATIVE = *out;

        // Curiously, it turns out that extracting the paramlist to a global
        // once and comparing against it is about 30% faster than saving to the
        // root object and extracting VAL_FUNC_PARAMLIST(ROOT_RETURN_NATIVE)
        // each time...
        //
        PG_Return_Paramlist = VAL_FUNC_PARAMLIST(out);
    }
    else if (func == &N_parse)
        *ROOT_PARSE_NATIVE = *out;
    else if (func == &N_eval) {
        //
        // See above note regarding return.  A check for EVAL is done on each
        // function evaluation, so it's worth it to extract.
        //
        PG_Eval_Paramlist = VAL_FUNC_PARAMLIST(out);
    }

}


//
//  Get_Maybe_Fake_Func_Body: C
// 
// The EXT_FUNC_HAS_RETURN tricks used for definitional scoping acceleration
// make it seem like a generator authored more code in the function's
// body...but the code isn't *actually* there and an optimized internal
// trick is used.
// 
// If the body is fake, it needs to be freed by the caller with
// Free_Series.  This means that the body must currently be shallow
// copied, and the splicing slot must be in the topmost series.
//
REBARR *Get_Maybe_Fake_Func_Body(REBFLG *is_fake, const REBVAL *func)
{
    REBARR *fake_body;

    assert(IS_CLOSURE(func) || IS_FUNCTION(func));

    if (!VAL_GET_EXT(func, EXT_FUNC_HAS_RETURN)) {
        *is_fake = FALSE;
        return VAL_FUNC_BODY(func);
    }

    *is_fake = TRUE;

    // See comments in sysobj.r on standard/func-body.
    fake_body = Copy_Array_Shallow(
        VAL_ARRAY(Get_System(SYS_STANDARD, STD_FUNC_BODY))
    );

    // Index 5 (or 4 in zero-based C) should be #TYPE, a FUNCTION! or CLOSURE!
    // !!! Is the binding important in this fake body??
    assert(IS_ISSUE(ARRAY_AT(fake_body, 4)));
    Val_Init_Word_Unbound(
        ARRAY_AT(fake_body, 4), REB_WORD, SYM_FROM_KIND(VAL_TYPE(func))
    );

    // Index 8 (or 7 in zero-based C) should be #BODY, a "real" body
    assert(IS_ISSUE(ARRAY_AT(fake_body, 7))); // #BODY
    Val_Init_Block(ARRAY_AT(fake_body, 7), VAL_FUNC_BODY(func));

    return fake_body;
}


//
//  Make_Function: C
// 
// This is the support routine behind `MAKE FUNCTION!` (or CLOSURE!), the
// basic building block of creating functions in Rebol.
// 
// If `has_return` is passed in as TRUE, then is also the optimized native
// implementation for the function generators FUNC and CLOS.  Ren/C's
// schematic for these generators is *very* different from R3-Alpha, whose
// definition of FUNC was simply:
// 
//     make function! copy/deep reduce [spec body]
// 
// Not only does Ren/C's `make function!` already copy the spec and body,
// but FUNC and CLOS "use the internals to cheat".  They analyze and edit
// the spec, then potentially build an entity whose full "body" acts like:
// 
//     return: make function! [
//         [{Returns a value from a function.} value [any-value!]]
//         [throw/name :value bind-of 'return]
//     ]
//     catch/name (body) bind-of 'return
// 
// This pattern addresses "Definitional Return" in a way that does not
// technically require building RETURN in as a language keyword in any
// specific form.  FUNC and CLOS optimize by not internally building
// or executing the equivalent body, but giving it back from BODY-OF.
// 
// NOTES:
// 
// The spec and body are copied--even for MAKE FUNCTION!--because:
// 
//    (a) It prevents tampering with the spec after it has been analyzed
//        by Check_Func_Spec().  Such changes to the spec will not be
//        reflected in the actual behavior of the function.
// 
//    (b) The BLOCK! values inside the make-spec may actually be imaging
//        series at an index position besides the series head.  However,
//        the REBVAL for a FUNCTION! contains only three REBSER slots--
//        all in use, with no space for offsets.  A copy must be made
//        to truncate to the intended spec and body start (unless one
//        is willing to raise errors on non-head position series :-/)
// 
//    (c) Copying the root of the series into a series the user cannot
//        access makes it possible to "lie" about what the body "above"
//        is.  This gives FUNC and CLOS the edge to pretend to add
//        containing code and simulate its effects, while really only
//        holding onto the body the caller provided.  This trick may
//        prove useful for other optimizing generators.
// 
// While MAKE FUNCTION! has no RETURN, all functions still have EXIT as a
// non-definitional alternative.  Ren/C adds a /WITH refinement so it can
// behave equivalently to old-non-definitonal return.  While not ideal, it
// could help in code which needed to be <transparent>.
// 
// This function will either successfully place a function value into
// `out` or not return...as a failed check on a function spec is
// raised as an error.
//
void Make_Function(REBVAL *out, enum Reb_Kind type, const REBVAL *spec, const REBVAL *body, REBFLG has_return)
{
    REBYTE func_flags = 0; // 8-bits in header, reserved type-specific flags

    if (!IS_BLOCK(spec) || !IS_BLOCK(body))
        fail (Error_Bad_Func_Def(spec, body));

    if (!has_return) {
        // Simpler case: if `make function!` or `make closure!` are used
        // then the function is "effectively <transparent>".   There is no
        // definitional return automatically added.  Non-definitional EXIT
        // and EXIT/WITH will still be available.

        // A small optimization will reuse the global empty array for an
        // empty spec instead of copying (as the spec need not be unique)

        if (VAL_LEN_AT(spec) == 0)
            VAL_FUNC_SPEC(out) = EMPTY_ARRAY;
        else
            VAL_FUNC_SPEC(out) = Copy_Array_At_Deep_Managed(
                VAL_ARRAY(spec), VAL_INDEX(spec)
            );
    }
    else {
        // Trickier case: when the `func` or `clos` natives are used, they
        // must read the given spec the way a user-space generator might.
        // They must decide whether to add a specially handled RETURN
        // local, which will be given a tricky "native" definitional return

        REBVAL *item = VAL_ARRAY_HEAD(spec);
        REBCNT index = 0;
        REBFLG convert_local = FALSE;

        for (; NOT_END(item); index++, item++) {
            if (IS_SET_WORD(item)) {
                // Note a "true local" (indicated by a set-word) is considered
                // to be tacit approval of wanting a definitional return
                // by the generator.  This helps because Red's model
                // for specifying returns uses a SET-WORD!
                //
                //     func [return: [integer!] {returns an integer}]
                //
                // In Ren/C's case it just means you want a local called
                // return, but the generator will be "initializing it
                // with a definitional return" for you.  You don't have
                // to use it if you don't want to...

                // !!! Should FUNC and CLOS be willing to move blocks after
                // a return: to the head to indicate a type check?  It
                // breaks the purity of the model.

                continue;
            }

            if (IS_TAG(item)) {
                if (
                    0 == Compare_String_Vals(item, ROOT_TRANSPARENT_TAG, TRUE)
                ) {
                    // The <transparent> tag is a way to cue FUNC and CLOS that
                    // you do not want a definitional return:
                    //
                    //     foo: func [<transparent> a] [return a]
                    //     foo 10 ;-- ERROR!
                    //
                    // This is redundant with the default for `make function!`.
                    // But having an option to use the familiar arity-2 form
                    // will probably appeal to more users.  Also, having two
                    // independent parameters can save the need for a REDUCE
                    // or COMPOSE that is generally required to composite a
                    // single block parameter that MAKE FUNCTION! requires.

                    VAL_FUNC_SPEC(out) = Copy_Array_At_Deep_Managed(
                        VAL_ARRAY(spec), VAL_INDEX(spec)
                    );
                    has_return = FALSE;

                    // We *could* remove the <transparent> tag, or check to
                    // see if there's more than one, etc.  But Check_Func_Spec
                    // is tolerant of any strings that we leave in the spec.
                    // This tolerance exists because the system is not to have
                    // any features based on recognizing specific keywords,
                    // so there's no need for tags to be "for future expansion"
                    // ... hence the mechanical cost burden of being forced
                    // to copy and remove them is a cost generators may not
                    // want to pay.

                    /*Remove_Series(VAL_FUNC_SPEC(out), index, 1);*/
                }
                else if (
                    0 == Compare_String_Vals(item, ROOT_INFIX_TAG, TRUE)
                ) {
                    // The <infix> option may or may not stick around.  The
                    // main reason not to is that it doesn't make sense for
                    // OP! to be the same interface type as FUNCTION! (or
                    // ANY-FUNCTION!).  An INFIX function generator is thus
                    // kind of tempting that returns an INFIX! (OP!), so
                    // this will remain under consideration.

                    SET_FLAG(func_flags, EXT_FUNC_INFIX);
                }
                else if (
                    0 == Compare_String_Vals(item, ROOT_LOCAL_TAG, TRUE)
                ) {
                    // While using x: and y: for pure locals is one option,
                    // it has two downsides.  One downside is that it makes
                    // the spec look too much "like everything else", so
                    // all the code kind of bleeds together.  Another is that
                    // if you nest one function within another then the outer
                    // function will wind up locals-gathering the locals of
                    // the inner function.  (It will anyway if you put the
                    // whole literal body there, but if you're adding the
                    // locals in a generator to be picked up by code that
                    // rebinds to them then it makes a difference.)
                    //
                    // Having a tag that lets you mark a run of locals is
                    // useful.  It will convert WORD! to SET-WORD! in the
                    // spec, and stop at the next refinement.

                    convert_local = TRUE;

                    // See notes about how we *could* remove ANY-STRING!s like
                    // the <local> tag from the spec, but Check_Func_Spec
                    // doesn't mind...it might be useful for HELP...and it's
                    // cheaper not to.
                }
                else
                    fail (Error(RE_BAD_FUNC_DEF, item));
            }
            else if (ANY_WORD(item)) {
                if (convert_local) {
                    if (IS_WORD(item)) {
                        // We convert words to set-words for pure local status
                        VAL_SET_TYPE(item, REB_SET_WORD);
                    }
                    else if (IS_REFINEMENT(item)) {
                        // A refinement signals us to stop doing the locals
                        // conversion.  Historically, help hides any
                        // refinements that appear behind a /local, so
                        // presumably it would do the same with <local>...
                        // but mechanically there is no way to tell
                        // Check_Func_Spec to hide a refinement.

                        convert_local = FALSE;
                    }
                    else {
                        // We've already ruled out pure locals, so this means
                        // they wrote something like:
                        //
                        //     func [a b <local> 'c #d :e]
                        //
                        // Consider that an error.

                        fail (Error(RE_BAD_FUNC_DEF, item));
                    }
                }

                if (SAME_SYM(VAL_WORD_SYM(item), SYM_RETURN)) {
                    // Although return: is explicitly tolerated,  all these
                    // would cancel a definitional return:
                    //
                    //     func [return [integer!]]
                    //     func [/value return]
                    //     func [/local return]
                    //
                    // The last one because /local is actually "just an ordinary
                    // refinement".  The choice of HELP to omit it could be
                    // a configuration setting.

                    VAL_FUNC_SPEC(out) = Copy_Array_At_Deep_Managed(
                        VAL_ARRAY(spec), VAL_INDEX(spec)
                    );
                    has_return = FALSE;
                }
            }
        }

        if (has_return) {
            // No prior RETURN (or other issue) stopping definitional return!
            // Add the "true local" RETURN: to the spec.

            if (index == 0) {
                // If the incoming spec was [] and we are turning it to
                // [return:], then that's a relatively common pattern
                // (e.g. what DOES would manufacture).  Re-use a global
                // instance of that series as an optimization.

                VAL_FUNC_SPEC(out) = VAL_ARRAY(ROOT_RETURN_BLOCK);
            }
            else {
                VAL_FUNC_SPEC(out) = Copy_Array_At_Extra_Deep_Managed(
                    VAL_ARRAY(spec), VAL_INDEX(spec), 1 // +1 capacity hint
                );
                Append_Value(VAL_FUNC_SPEC(out), ROOT_RETURN_SET_WORD);
            }
        }
    }

    // Spec checking will longjmp out with an error if the spec is bad
    VAL_FUNC_PARAMLIST(out) = Check_Func_Spec(VAL_FUNC_SPEC(out));

    // We copy the body or do the empty body optimization to not copy and
    // use the EMPTY_ARRAY (which probably doesn't happen often...)

    if (VAL_LEN_AT(body) == 0)
        VAL_FUNC_BODY(out) = EMPTY_ARRAY;
    else
        VAL_FUNC_BODY(out) = Copy_Array_At_Deep_Managed(
            VAL_ARRAY(body), VAL_INDEX(body)
        );

    // Even if `has_return` was passed in true, the FUNC or CLOS generator
    // may have seen something to turn it off and turned it false.  But if
    // it's still on, then signal we want the fancy fake return!

    if (has_return) {
        SET_FLAG(func_flags, EXT_FUNC_HAS_RETURN);

        // Boilerplate says:
        //
        //     catch/name [your code here] bind-of 'return
        //
        // Visually for BODY-OF it's better to give user code its own line:
        //
        //     catch/name [
        //         your code here
        //     ] bind-of 'return

        if (ARRAY_LEN(VAL_FUNC_BODY(out)) >= 2)
            VAL_SET_OPT(ARRAY_HEAD(VAL_FUNC_BODY(out)), OPT_VALUE_LINE);
    }

    assert(type == REB_FUNCTION || type == REB_CLOSURE);
    VAL_RESET_HEADER(out, type); // clears value opts and exts in header...
    VAL_EXTS_DATA(out) = func_flags; // ...so we set this after that point

    // Now that we've created the function's fields, we pull a trick.  It
    // would be useful to be able to navigate to a full function value
    // given just its identifying series, but where to put it?  We use
    // slot 0 (a trick learned from FRAME! in R3-Alpha's frame series)

    *ARRAY_HEAD(VAL_FUNC_PARAMLIST(out)) = *out;

    // The argument and local symbols have been arranged in the function's
    // "frame" and are now in index order.  These numbers are put
    // into the binding as *negative* versions of the index, in order
    // to indicate that they are in a function and not an object frame.
    //
    // (This is done for the closure body even though each call is associated
    // with an object frame.  The reason is that this is only the "archetype"
    // body of the closure...it is copied each time and the real numbers
    // filled in.  Having the indexes already done speeds the copying.)

    Bind_Relative(VAL_FUNC_PARAMLIST(out), VAL_FUNC_BODY(out));
}


//
//  Clonify_Function: C
// 
// The "Clonify" interface takes in a raw duplicate value that one
// wishes to mutate in-place into a full-fledged copy of the value
// it is a clone of.  This interface can be more efficient than a
// "source in, dest out" copy...and clarifies the dangers when the
// source and destination are the same.
//
void Clonify_Function(REBVAL *value)
{
    REBARR *paramlist_orig;

    // !!! Conceptually the only types it currently makes sense to speak of
    // copying are functions and closures.  Though the concept is a little
    // bit "fuzzy"...the idea is that the series which are reachable from
    // their body series by a deep copy would be their "state".  Hence
    // as a function runs, its "state" can change.  One can thus define
    // a copy as snapshotting that "state".  This has been the classic
    // interpretation that Rebol has taken.

    // !!! However, in R3-Alpha a closure's "archetype" (e.g. the one made
    // by `clos [a] [print a]`) never operates on its body directly... it
    // is copied each time.  And there is no way at present to get a
    // reference to a closure "instance" (an ANY-FUNCTION value with the
    // copied body in it).  Until such time as there's a way

    // !!! This leaves only one function type that is mechanically
    // clonable at all... the FUNCTION!.  While the behavior is questionable,
    // for now we will suspend disbelief and preserve what R3-Alpha did
    // until a clear resolution.

    if (!IS_FUNCTION(value))
        return;

    // No need to modify the spec or header.  But we do need to copy the
    // identifying parameter series, so that the copied function has a
    // unique identity on the stack from the one it is copying.  Otherwise
    // two calls on the stack would be seen as recursions of the same
    // function, sharing each others "stack relative locals".

    paramlist_orig = VAL_FUNC_PARAMLIST(value);

    VAL_FUNC_PARAMLIST(value) = Copy_Array_Shallow(paramlist_orig);
    MANAGE_ARRAY(VAL_FUNC_PARAMLIST(value));

    VAL_FUNC_BODY(value) = Copy_Array_Deep_Managed(VAL_FUNC_BODY(value));

    // Remap references in the body from paramlist_orig to our new copied
    // word list we saved in VAL_FUNC_PARAMLIST(value)

    Rebind_Values_Deep(
        paramlist_orig,
        VAL_FUNC_PARAMLIST(value),
        ARRAY_HEAD(VAL_FUNC_BODY(value)),
        0
    );

    // The above phrasing came from deep cloning code, while the below was
    // in the Copy_Function code.  Evaluate if there is now "dead code"
    // relating to the difference.
/*
    Bind_Relative(
        VAL_FUNC_PARAMLIST(out), VAL_FUNC_PARAMLIST(out), VAL_FUNC_BODY(out)
    );
*/

    // The first element in the paramlist is the identity of the function
    // value itself.  So we must update this value if we make a copy,
    // so the paramlist does not indicate the original.
    *ARRAY_HEAD(VAL_FUNC_PARAMLIST(value)) = *value;
}


//
//  Do_Native_Throws: C
//
REBFLG Do_Native_Throws(struct Reb_Call *call_)
{
    REB_R ret;

    Eval_Natives++;

    if (VAL_FUNC_PARAMLIST(D_FUNC) == PG_Return_Paramlist) {
        REBVAL name;

        // The EXT_FUNC_HAS_RETURN uses the RETURN native and its spec, and
        // the call validation should have ensured we got exactly one
        // parameter--which can be any type.

        assert(D_ARGC == 1);

        // The originating `Push_New_Arglist_For_Call()` that produced this
        // return native should have overwritten its code pointer with the
        // identifying series of the function--or closure frame--it wants
        // to jump to.

        assert(VAL_FUNC_CODE(D_FUNC) != VAL_FUNC_CODE(ROOT_RETURN_NATIVE));
        ASSERT_ARRAY(VAL_FUNC_RETURN_TO(D_FUNC));

        // We only have a REBSER*, but the goal is to actually THROW a full
        // REBVAL (FUNCTION! or OBJECT! if it's a closure) which matches
        // the paramlist.  For the moment, how to get that value depends...

        if (ARRAY_GET_FLAG(VAL_FUNC_RETURN_TO(D_FUNC), SER_FRAME)) {
            // The function was actually a CLOSURE!, so "when it took BIND-OF
            // on 'RETURN" it "would have gotten back an OBJECT!".  We can
            // get that object to use as the throw name just by putting the
            // frame with a REB_OBJECT.

            Val_Init_Object(D_OUT, AS_FRAME(VAL_FUNC_RETURN_TO(D_FUNC)));
        }
        else {
            // It was a stack-relative FUNCTION!, and what we have is more
            // akin to an object's keylist than it is to the varlist.
            // Since there was no good WORD! ("unword" in those days) to
            // put in the 0 slot, it was left empty.  Ren/C uses this value
            // sized slot to hold the full function value just for cases
            // like this...

            *D_OUT = *ARRAY_HEAD(VAL_FUNC_RETURN_TO(D_FUNC));
            assert(IS_FUNCTION(D_OUT));
            assert(VAL_FUNC_PARAMLIST(D_OUT) == VAL_FUNC_RETURN_TO(D_FUNC));
        }

        CONVERT_NAME_TO_THROWN(D_OUT, D_ARG(1));

        // Now it's ready to throw!
        return TRUE;
    }

    // For all other native function pointers (for now)...ordinary dispatch.

    ret = VAL_FUNC_CODE(D_FUNC)(call_);

    switch (ret) {
    case R_OUT: // for compiler opt
    case R_OUT_IS_THROWN:
        break;
    case R_NONE:
        SET_NONE(D_OUT);
        break;
    case R_UNSET:
        SET_UNSET(D_OUT);
        break;
    case R_TRUE:
        SET_TRUE(D_OUT);
        break;
    case R_FALSE:
        SET_FALSE(D_OUT);
        break;
    case R_ARG1:
        *D_OUT = *D_ARG(1);
        break;
    case R_ARG2:
        *D_OUT = *D_ARG(2);
        break;
    case R_ARG3:
        *D_OUT = *D_ARG(3);
        break;
    default:
        assert(FALSE);
    }

    // The VAL_OPT_THROWN bit is being eliminated, but used temporarily to
    // check the actions and natives are returning the correct thing.
    assert(THROWN(D_OUT) == (ret == R_OUT_IS_THROWN));
    return ret == R_OUT_IS_THROWN;
}


//
//  Do_Action_Throws: C
//
REBFLG Do_Action_Throws(struct Reb_Call *call_)
{
    REBCNT type = VAL_TYPE(D_ARG(1));
    REBACT action;
    REB_R ret;

    Eval_Natives++;

    assert(type < REB_MAX);

    // Handle special datatype test cases (eg. integer?).  Note that this
    // has a frameless implementation which is the one that typically runs
    // when a frame is not required (such as when running under trace, where
    // the values need to be inspectable)
    //
    if (VAL_FUNC_ACT(D_FUNC) < REB_MAX) {
        if (type == VAL_FUNC_ACT(D_FUNC))
            SET_TRUE(D_OUT);
        else
            SET_FALSE(D_OUT);

        return FALSE;
    }

    action = Value_Dispatch[type];
    if (!action) fail (Error_Illegal_Action(type, VAL_FUNC_ACT(D_FUNC)));
    ret = action(call_, VAL_FUNC_ACT(D_FUNC));

    switch (ret) {
    case R_OUT: // for compiler opt
    case R_OUT_IS_THROWN:
        break;
    case R_NONE:
        SET_NONE(D_OUT);
        break;
    case R_UNSET:
        SET_UNSET(D_OUT);
        break;
    case R_TRUE:
        SET_TRUE(D_OUT);
        break;
    case R_FALSE:
        SET_FALSE(D_OUT);
        break;
    case R_ARG1:
        *D_OUT = *D_ARG(1);
        break;
    case R_ARG2:
        *D_OUT = *D_ARG(2);
        break;
    case R_ARG3:
        *D_OUT = *D_ARG(3);
        break;
    default:
        assert(FALSE);
    }

    // The VAL_OPT_THROWN bit is being eliminated, but used temporarily to
    // check the actions and natives are returning the correct thing.
    assert(THROWN(D_OUT) == (ret == R_OUT_IS_THROWN));
    return ret == R_OUT_IS_THROWN;
}


//
//  Do_Function_Throws: C
//
REBFLG Do_Function_Throws(struct Reb_Call *call_)
{
    Eval_Functions++;

    // Functions have a body series pointer, but no VAL_INDEX, so use 0
    //
    if (Do_At_Throws(D_OUT, VAL_FUNC_BODY(D_FUNC), 0)) {
        //
        // Every function responds to non-definitional EXIT
        //
        if (IS_NATIVE(D_OUT) && VAL_FUNC_CODE(D_OUT) == &N_exit) {
            CATCH_THROWN(D_OUT, D_OUT);
            return FALSE;
        }

        if (
            IS_FUNCTION(D_OUT)
            && VAL_GET_EXT(D_FUNC, EXT_FUNC_HAS_RETURN)
            && VAL_FUNC_PARAMLIST(D_OUT) == VAL_FUNC_PARAMLIST(D_FUNC)
        ) {
            // Optimized definitional return!!  Courtesy of REBNATIVE(func),
            // a "hacked" REBNATIVE(return) that knew our paramlist, and
            // the gracious cooperation of a throw by Do_Native_Throws()...
            //
            CATCH_THROWN(D_OUT, D_OUT);
            return FALSE;
        }

        return TRUE; // throw wasn't for us...
    }

    return FALSE;
}


//
//  Do_Closure_Throws: C
// 
// Do a closure by cloning its body and rebinding it to
// a new frame of words/values.
//
REBFLG Do_Closure_Throws(struct Reb_Call *call_)
{
    REBARR *body;
    REBFRM *frame;

    Eval_Functions++;

    // The head value of a function/closure paramlist should be the value
    // of the function/closure itself that has that paramlist.
    //
    assert(IS_CLOSURE(ARRAY_HEAD(VAL_FUNC_PARAMLIST(D_FUNC))));
#if !defined(NDEBUG)
    if (
        VAL_FUNC_PARAMLIST(ARRAY_HEAD(VAL_FUNC_PARAMLIST(D_FUNC)))
        != VAL_FUNC_PARAMLIST(D_FUNC)
    ) {
        Panic_Array(
            VAL_FUNC_PARAMLIST(ARRAY_HEAD(VAL_FUNC_PARAMLIST(D_FUNC)))
        );
    }
#endif

    // We will extract the arglist from ownership and manual memory management
    // by the call, to be used in a GC-managed object frame by the closure.
    // Since it's not GC protected by the call, it should not be inspected
    // as it could go bad at any point...set call field to junk in debug.
    //
    frame = AS_FRAME(call_->arglist.array);
#if !defined(NDEBUG)
    call_->arglist.array = cast(REBARR*, 0xDECAFBAD);
#endif

    // Formerly the arglist's 0 slot had a CLOSURE! value in it, but we now
    // are going to be switching it to an OBJECT!.

    ARRAY_SET_FLAG(FRAME_VARLIST(frame), SER_FRAME);
    VAL_RESET_HEADER(FRAME_CONTEXT(frame), REB_OBJECT);
    VAL_FRAME(FRAME_CONTEXT(frame)) = frame;
    FRAME_KEYLIST(frame) = VAL_FUNC_PARAMLIST(D_FUNC);
    FRAME_SPEC(frame) = NULL;
    FRAME_BODY(frame) = NULL;
    ASSERT_FRAME(frame);

#if !defined(NDEBUG)
    // !!! A second sweep for the definitional return used to be necessary in
    // the dispatch of closures since the frame hadn't been created yet to
    // put in the RETURN_TO slot.  Now that the call's `arglist` is known
    // to be the pre-created array we'll mutate into a frame, the Do_Core
    // sweep went ahead and put it in for us.  Temporarily leave in the sweep
    // with aparanoid check to make sure, but delete this eventually.

    if (VAL_GET_EXT(D_FUNC, EXT_FUNC_HAS_RETURN)) {
        REBVAL *key = VAL_FUNC_PARAM(D_FUNC, 1);
        REBVAL *value = FRAME_VAR(frame, 1);

        for (; NOT_END(key); key++, value++) {
            if (SAME_SYM(VAL_TYPESET_SYM(key), SYM_RETURN)) {
                assert(IS_NATIVE(value));
                assert(PG_Return_Paramlist == VAL_FUNC_PARAMLIST(value));
                assert(VAL_FUNC_RETURN_TO(value) == FRAME_VARLIST(frame));
            }
        }
    }
#endif

    // We do not Manage_Frame, because we are reusing a word series here
    // that has already been managed...only extract and manage the arglist
    //
    ASSERT_ARRAY_MANAGED(FRAME_KEYLIST(frame));
    MANAGE_ARRAY(FRAME_VARLIST(frame));

    // Clone the body of the closure to allow us to rebind words inside
    // of it so that they point specifically to the instances for this
    // invocation.  (Costly, but that is the mechanics of words.)
    //
    body = Copy_Array_Deep_Managed(VAL_FUNC_BODY(D_FUNC));
    Rebind_Values_Deep(
        VAL_FUNC_PARAMLIST(D_FUNC),
        FRAME_VARLIST(frame),
        ARRAY_HEAD(body),
        REBIND_TYPE
    );

    // Protect the body from garbage collection during the course of the
    // execution.  (We could also protect it by stowing it in the call
    // frame's copy of the closure value, which we might think of as its
    // "archetype", but it may be valuable to keep that as-is.)
    //
    PUSH_GUARD_ARRAY(body);

    if (Do_At_Throws(D_OUT, body, 0)) {
        DROP_GUARD_ARRAY(body);
        if (IS_NATIVE(D_OUT) && VAL_FUNC_CODE(D_OUT) == &N_exit) {
            // Every function responds to non-definitional EXIT
            CATCH_THROWN(D_OUT, D_OUT);
            return FALSE;
        }

        if (
            IS_OBJECT(D_OUT)
            && VAL_GET_EXT(D_FUNC, EXT_FUNC_HAS_RETURN)
            && VAL_FRAME(D_OUT) == frame
        ) {
            // Optimized definitional return!!  Courtesy of REBNATIVE(clos),
            // a "hacked" REBNATIVE(return) that knew our frame, and
            // the gracious cooperation of a throw by Do_Native_Throws()...

            CATCH_THROWN(D_OUT, D_OUT);
            return FALSE;
        }

        return TRUE; // throw wasn't for us
    }

    // References to parts of the closure's copied body may still be
    // extant, but we no longer need to hold this reference on it
    //
    DROP_GUARD_ARRAY(body);

    return FALSE;
}


//
//  Do_Routine_Throws: C
//
REBFLG Do_Routine_Throws(struct Reb_Call *call_)
{
    REBARR *args = Copy_Values_Len_Shallow(
        D_ARGC > 0 ? D_ARG(1) : NULL,
        D_ARGC
    );

    Call_Routine(D_FUNC, args, D_OUT);

    Free_Array(args);

    return FALSE; // You cannot "throw" a Rebol value across an FFI boundary
}


//
//  func: native [
//  
//  "Defines a user function with given spec and body."
//  
//      spec [block!] 
//      {Help string (opt) followed by arg words (and opt type and string)}
//      body [block!] "The body block of the function"
//  ]
//
REBNATIVE(func)
//
// Native optimized implementation of a "definitional return" function
// generator.  FUNC uses "stack-relative binding" for optimization,
// which leads to less desirable behaviors than CLOS...while more
// performant.
// 
// See comments on Make_Function for full notes.
{
    PARAM(1, spec);
    PARAM(2, body);

    const REBFLG has_return = TRUE;

    Make_Function(D_OUT, REB_FUNCTION, ARG(spec), ARG(body), has_return);

    return R_OUT;
}


//
//  clos: native [
//  
//  "Defines a closure function."
//  
//      spec [block!] 
//      {Help string (opt) followed by arg words (and opt type and string)}
//      body [block!] "The body block of the function"
//  ]
//
REBNATIVE(clos)
//
// Native optimized implementation of a "definitional return" "closure"
// generator.  Each time a CLOS-created function is called, it makes
// a copy of its body and binds all the local words in that copied
// body into a uniquely persistable object.  This provides desirable
// behaviors of "leaked" bound variables surviving the end of the
// closure's call on the stack... as well as recursive instances
// being able to uniquely identify their bound variables from each
// other.  Yet this uses more memory and puts more strain on the
// garbage collector than FUNC.
// 
// A solution that can accomplish closure's user-facing effects with
// enough efficiency to justify replacing FUNC's implementation
// with it is sought, but no adequate tradeoff has been found.
// 
// See comments on Make_Function for full notes.
{
    PARAM(1, spec);
    PARAM(2, body);

    const REBFLG has_return = TRUE;

    Make_Function(D_OUT, REB_CLOSURE, ARG(spec), ARG(body), has_return);

    return R_OUT;
}
