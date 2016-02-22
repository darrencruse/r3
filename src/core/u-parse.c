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
**  Module:  u-parse.c
**  Summary: parse dialect interpreter
**  Section: utility
**  Author:  Carl Sassenrath
**  Notes:
**
***********************************************************************/

#include "sys-core.h"

typedef struct reb_parse {
    REBSER *series;
    enum Reb_Kind type;
    REBCNT find_flags;
    REBINT result;
    REBVAL *out;
} REBPARSE;

enum parse_flags {
    PF_SET_OR_COPY, // test PF_COPY first; if false, this means PF_SET
    PF_COPY,
    PF_NOT,
    PF_NOT2,
    PF_THEN,
    PF_AND,
    PF_REMOVE,
    PF_INSERT,
    PF_CHANGE,
    PF_RETURN,
    PF_WHILE,
    PF_MAX
};

#define MAX_PARSE_DEPTH 512

// Returns SYMBOL or 0 if not a command:
#define GET_CMD(n) (((n) >= SYM_BAR && (n) <= SYM_END) ? (n) : 0)
#define VAL_CMD(v) GET_CMD(VAL_WORD_CANON(v))
#define HAS_CASE(p) LOGICAL(p->find_flags & AM_FIND_CASE)
#define IS_OR_BAR(v) (IS_WORD(v) && VAL_WORD_CANON(v) == SYM_BAR)
#define SKIP_TO_BAR(r) while (NOT_END(r) && !IS_SAME_WORD(r, SYM_BAR)) r++;
#define IS_ARRAY_INPUT(p) (p->type >= REB_BLOCK)

static REBCNT Parse_Rules_Loop(REBPARSE *parse, REBCNT index, const REBVAL *rules, REBCNT depth);

void Print_Parse_Index(
    enum Reb_Kind type,
    const REBVAL rule[], // positioned at the current rule
    REBSER *series,
    REBCNT index
) {
    REBVAL item;
    VAL_INIT_WRITABLE_DEBUG(&item);
    Val_Init_Series(&item, type, series);
    VAL_INDEX(&item) = index;

    // Either the rules or the data could be positioned at the end.  The
    // data might even be past the end.
    //
    // !!! Or does PARSE adjust to ensure it never is past the end, e.g.
    // when seeking a position given in a variable or modifying?
    //
    if (IS_END(rule)) {
        if (index >= SERIES_LEN(series))
            Debug_Fmt("[]: ** END **");
        else
            Debug_Fmt("[]: %r", &item);
    }
    else {
        if (index >= SERIES_LEN(series))
            Debug_Fmt("%r: ** END **", rule);
        else
            Debug_Fmt("%r: %r", rule, &item);
    }
}


//
//  Set_Parse_Series: C
// 
// Change the series and return the new index.
//
static REBCNT Set_Parse_Series(REBPARSE *parse, const REBVAL *item)
{
    parse->series = VAL_SERIES(item);
    parse->type = VAL_TYPE(item);
    if (IS_BINARY(item) || (parse->find_flags & AM_FIND_CASE))
        parse->find_flags |= AM_FIND_CASE;
    else
        parse->find_flags &= ~AM_FIND_CASE;
    return (VAL_INDEX(item) > VAL_LEN_HEAD(item)) ? VAL_LEN_HEAD(item) : VAL_INDEX(item);
}


//
//  Get_Parse_Value: C
// 
// Get the value of a word (when not a command) or path.
// Returns all other values as-is.
// 
// !!! Because path evaluation does not necessarily wind up
// pointing to a variable that exists in memory, a derived
// value may be created during that process.  Previously
// this derived value was kept on the stack, but that
// meant every path evaluation PUSH'd without a known time
// at which a corresponding DROP would be performed.  To
// avoid the stack overflow, this requires you to pass in
// a "safe" storage value location that will be good for
// as long as the returned pointer is needed.  It *may*
// not be used in the case of a word fetch, so pay attention
// to the return value and not the contents of that variable.
// 
// !!! (Review if this can be done a better way.)
//
static const REBVAL *Get_Parse_Value(REBVAL *safe, const REBVAL *item)
{
    if (IS_WORD(item)) {
        const REBVAL *var;

        if (VAL_CMD(item)) return item;

        // If `item` is not bound, there will be a fail() during GET_VAR
        //
        var = GET_VAR(item);

        // While NONE! is legal and represents a no-op in parse, if a
        // you write `parse "" [to undefined-value]`...and undefined-value
        // is bound...you may get an UNSET! back.  This should be an
        // error, as it is in the evaluator.  (See how this is handled
        // by REB_WORD in %c-do.c)
        //
        if (IS_UNSET(var))
            fail (Error(RE_NO_VALUE, item));

        return var;
    }

    if (IS_PATH(item)) {
        //
        // !!! REVIEW: how should GET-PATH! be handled?

        if (Do_Path_Throws(safe, NULL, item, NULL))
            fail (Error_No_Catch_For_Throw(safe));

        // See notes above about UNSET!
        //
        if (IS_UNSET(safe))
            fail (Error(RE_NO_VALUE, item));

        return safe;
    }

    return item;
}


//
//  Parse_Next_String: C
// 
// Match the next item in the string ruleset.
// 
// If it matches, return the index just past it.
// Otherwise return NOT_FOUND.
//
static REBCNT Parse_Next_String(REBPARSE *parse, REBCNT index, const REBVAL *item, REBCNT depth)
{
    // !!! THIS CODE NEEDS CLEANUP AND REWRITE BASED ON OTHER CHANGES
    REBSER *series = parse->series;
    REBSER *ser;
    REBCNT flags = parse->find_flags | AM_FIND_MATCH | AM_FIND_TAIL;
    int rewrite_needed;

    REBVAL save;
    VAL_INIT_WRITABLE_DEBUG(&save);

    if (Trace_Level) {
        Trace_Value(7, item);

        // This used STR_AT (obsolete) but it's not clear that this is
        // necessarily a byte sized series.  Switched to BIN_AT and added
        // an assert.
        //
        assert(BYTE_SIZE(series));

        Trace_String(8, BIN_AT(series, index), SERIES_LEN(series) - index);
    }

    if (IS_NONE(item)) return index;

    if (index >= SERIES_LEN(series)) return NOT_FOUND;

    switch (VAL_TYPE(item)) {

    // Do we match a single character?
    case REB_CHAR:
        if (HAS_CASE(parse))
            index = (VAL_CHAR(item) == GET_ANY_CHAR(series, index)) ? index+1 : NOT_FOUND;
        else
            index = (UP_CASE(VAL_CHAR(item)) == UP_CASE(GET_ANY_CHAR(series, index))) ? index+1 : NOT_FOUND;
        break;

    case REB_EMAIL:
    case REB_STRING:
    case REB_BINARY:
        index = Find_Str_Str(series, 0, index, SERIES_LEN(series), 1, VAL_SERIES(item), VAL_INDEX(item), VAL_LEN_AT(item), flags);
        break;

    case REB_BITSET:
        if (Check_Bit(
            VAL_SERIES(item), GET_ANY_CHAR(series, index), NOT(HAS_CASE(parse))
        )) {
            // We matched to a char set, advance.
            //
            index++;
        }
        else
            index = NOT_FOUND;
        break;
/*
    case REB_DATATYPE:  // Currently: integer!
        if (VAL_TYPE_KIND(item) == REB_INTEGER) {
            REBCNT begin = index;
            while (IS_LEX_NUMBER(*str)) str++, index++;
            if (begin == index) index = NOT_FOUND;
        }
        break;
*/
    case REB_TAG:
    case REB_FILE:
//  case REB_ISSUE:
        // !! Can be optimized (w/o COPY)
        ser = Copy_Form_Value(item, 0);
        index = Find_Str_Str(
            series,
            0,
            index,
            SERIES_LEN(series),
            1,
            ser,
            0,
            SERIES_LEN(ser),
            flags
        );
        Free_Series(ser);
        break;

    case REB_NONE:
        break;

    // Parse a sub-rule block:
    case REB_BLOCK:
        index = Parse_Rules_Loop(parse, index, VAL_ARRAY_AT(item), depth);
        // index may be THROWN_FLAG
        break;

    // Do an expression:
    case REB_GROUP:
        // might GC
        if (DO_ARRAY_THROWS(&save, item)) {
            *parse->out = save;
            return THROWN_FLAG;
        }

        index = MIN(index, SERIES_LEN(series)); // may affect tail
        break;

    default:
        fail (Error(RE_PARSE_RULE, item));
    }

    return index;
}


//
//  Parse_Next_Array: C
// 
// Used for parsing ANY-ARRAY! to match the next item in the ruleset.
// If it matches, return the index just past it. Otherwise, return zero.
//
static REBCNT Parse_Next_Array(
    REBPARSE *parse,
    REBCNT index,
    const REBVAL *item,
    REBCNT depth
) {
    // !!! THIS CODE NEEDS CLEANUP AND REWRITE BASED ON OTHER CHANGES
    REBARR *array = AS_ARRAY(parse->series);
    REBVAL *blk = ARRAY_AT(array, index);

    REBVAL save;
    VAL_INIT_WRITABLE_DEBUG(&save);

    if (Trace_Level) {
        Trace_Value(7, item);
        Trace_Value(8, blk);
    }

    // !!! The previous code did not have a handling for this, but it fell
    // through to `no_result`.  Is that correct?
    //
    if (IS_END(blk)) goto no_result;

    switch (VAL_TYPE(item)) {

    // Look for specific datattype:
    case REB_DATATYPE:
        index++;
        if (VAL_TYPE(blk) == VAL_TYPE_KIND(item)) break;
        goto no_result;

    // Look for a set of datatypes:
    case REB_TYPESET:
        index++;
        if (TYPE_CHECK(item, VAL_TYPE(blk))) break;
        goto no_result;

    // 'word
    case REB_LIT_WORD:
        index++;
        if (IS_WORD(blk) && (VAL_WORD_CANON(blk) == VAL_WORD_CANON(item))) break;
        goto no_result;

    case REB_LIT_PATH:
        index++;
        if (IS_PATH(blk) && !Cmp_Block(blk, item, FALSE)) break;
        goto no_result;

    case REB_NONE:
        break;

    // Parse a sub-rule block:
    case REB_BLOCK:
        index = Parse_Rules_Loop(parse, index, VAL_ARRAY_AT(item), depth);
        // index may be THROWN_FLAG
        break;

    // Do an expression:
    case REB_GROUP:
        // might GC
        if (DO_ARRAY_THROWS(&save, item)) {
            *parse->out = save;
            return THROWN_FLAG;
        }
        // old: if (IS_ERROR(item)) Throw_Error(VAL_CONTEXT(item));
        index = MIN(index, ARRAY_LEN(array)); // may affect tail
        break;

    // Match with some other value:
    default:
        index++;
        if (Cmp_Value(blk, item, HAS_CASE(parse))) goto no_result;
    }

    return index;

no_result:
    return NOT_FOUND;
}


//
//  To_Thru: C
//
static REBCNT To_Thru(REBPARSE *parse, REBCNT index, const REBVAL *block, REBOOL is_thru)
{
    REBSER *series = parse->series;
    REBCNT type = parse->type;
    REBVAL *blk;
    const REBVAL *item;
    REBCNT cmd;
    REBCNT i;
    REBCNT len;

    REBVAL save;
    VAL_INIT_WRITABLE_DEBUG(&save);

    for (; index <= SERIES_LEN(series); index++) {

        for (blk = VAL_ARRAY_HEAD(block); NOT_END(blk); blk++) {

            item = blk;

            // Deal with words and commands
            if (IS_WORD(item)) {
                if ((cmd = VAL_CMD(item))) {
                    if (cmd == SYM_END) {
                        if (index >= SERIES_LEN(series)) {
                            index = SERIES_LEN(series);
                            goto found;
                        }
                        goto next;
                    }
                    else if (cmd == SYM_QUOTE) {
                        item = ++blk; // next item is the quoted value
                        if (IS_END(item)) goto bad_target;
                        if (IS_GROUP(item)) {
                            // might GC
                            if (DO_ARRAY_THROWS(&save, item)) {
                                *parse->out = save;
                                return THROWN_FLAG;
                            }
                            item = &save;
                        }

                    }
                    else goto bad_target;
                }
                else {
                    // !!! Should mutability be enforced?  It might have to
                    // be if set/copy are used...
                    item = GET_MUTABLE_VAR(item);
                }
            }
            else if (IS_PATH(item)) {
                item = Get_Parse_Value(&save, item);
            }

            // Try to match it:
            if (type >= REB_BLOCK) {
                if (ANY_ARRAY(item)) goto bad_target;
                i = Parse_Next_Array(parse, index, item, 0);
                if (i == THROWN_FLAG)
                    return THROWN_FLAG;

                if (i != NOT_FOUND) {
                    if (!is_thru) i--;
                    index = i;
                    goto found;
                }
            }
            else if (type == REB_BINARY) {
                REBYTE ch1 = *BIN_AT(series, index);

                // Handle special string types:
                if (IS_CHAR(item)) {
                    if (VAL_CHAR(item) > 0xff) goto bad_target;
                    if (ch1 == VAL_CHAR(item)) goto found1;
                }
                else if (IS_BINARY(item)) {
                    if (ch1 == *VAL_BIN_AT(item)) {
                        len = VAL_LEN_AT(item);
                        if (len == 1) goto found1;
                        if (0 == Compare_Bytes(
                            BIN_AT(series, index),
                            VAL_BIN_AT(item),
                            len,
                            FALSE
                        )) {
                            if (is_thru) index += len;
                            goto found;
                        }
                    }
                }
                else if (IS_INTEGER(item)) {
                    if (VAL_INT64(item) > 0xff) goto bad_target;
                    if (ch1 == VAL_INT32(item)) goto found1;
                }
                else goto bad_target;
            }
            else { // String
                REBCNT ch1 = GET_ANY_CHAR(series, index);
                REBCNT ch2;

                if (!HAS_CASE(parse)) ch1 = UP_CASE(ch1);

                // Handle special string types:
                if (IS_CHAR(item)) {
                    ch2 = VAL_CHAR(item);
                    if (!HAS_CASE(parse)) ch2 = UP_CASE(ch2);
                    if (ch1 == ch2) goto found1;
                }
                // bitset
                else if (IS_BITSET(item)) {
                    if (Check_Bit(VAL_SERIES(item), ch1, NOT(HAS_CASE(parse))))
                        goto found1;
                }
                else if (ANY_STR(item)) {
                    ch2 = VAL_ANY_CHAR(item);
                    if (!HAS_CASE(parse)) ch2 = UP_CASE(ch2);
                    if (ch1 == ch2) {
                        len = VAL_LEN_AT(item);
                        if (len == 1) goto found1;
                        i = Find_Str_Str(series, 0, index, SERIES_LEN(series), 1, VAL_SERIES(item), VAL_INDEX(item), len, AM_FIND_MATCH | parse->find_flags);
                        if (i != NOT_FOUND) {
                            if (is_thru) i += len;
                            index = i;
                            goto found;
                        }
                    }
                }
                else if (IS_INTEGER(item)) {
                    ch1 = GET_ANY_CHAR(series, index);  // No casing!
                    if (ch1 == (REBCNT)VAL_INT32(item)) goto found1;
                }
                else goto bad_target;
            }

next:       // Check for | (required if not end)
            blk++;
            if (IS_END(blk)) break;
            if (IS_GROUP(blk)) blk++;
            if (IS_END(blk)) break;
            if (!IS_OR_BAR(blk)) {
                item = blk;
                goto bad_target;
            }
        }
    }
    return NOT_FOUND;

found:
    if (NOT_END(blk + 1) && IS_GROUP(blk + 1)) {
        REBVAL evaluated;
        VAL_INIT_WRITABLE_DEBUG(&evaluated);

        if (DO_ARRAY_THROWS(&evaluated, blk + 1)) {
            *parse->out = evaluated;
            return THROWN_FLAG;
        }
        // !!! ignore evaluated if it's not THROWN?
    }
    return index;

found1:
    if (NOT_END(blk + 1) && IS_GROUP(blk + 1)) {
        REBVAL evaluated;
        VAL_INIT_WRITABLE_DEBUG(&evaluated);

        if (DO_ARRAY_THROWS(&evaluated, blk + 1)) {
            *parse->out = save;
            return THROWN_FLAG;
        }
        // !!! ignore evaluated if it's not THROWN?
    }
    return index + (is_thru ? 1 : 0);

bad_target:
    fail (Error(RE_PARSE_RULE, item));
}


//
//  Parse_To: C
// 
// Parse TO a specific:
//     1. integer - index position
//     2. END - end of input
//     3. value - according to datatype
//     4. block of values - the first one we hit
//
static REBCNT Parse_To(REBPARSE *parse, REBCNT index, const REBVAL *item, REBOOL is_thru)
{
    REBSER *series = parse->series;
    REBCNT i;
    REBSER *ser;

    // TO a specific index position.
    if (IS_INTEGER(item)) {
        i = (REBCNT)Int32(item) - (is_thru ? 0 : 1);
        if (i > SERIES_LEN(series)) i = SERIES_LEN(series);
    }
    // END
    else if (IS_WORD(item) && VAL_WORD_CANON(item) == SYM_END) {
        i = SERIES_LEN(series);
    }
    else if (IS_BLOCK(item)) {
        i = To_Thru(parse, index, item, is_thru);
    }
    else {
        if (IS_ARRAY_INPUT(parse)) {
            REBVAL word; /// !!!Temp, but where can we put it?
            VAL_INIT_WRITABLE_DEBUG(&word);

            if (IS_LIT_WORD(item)) {  // patch to search for word, not lit.
                word = *item;

                // Only set type--don't reset the header, because that could
                // make the word binding inconsistent with the bits.
                //
                VAL_SET_TYPE_BITS(&word, REB_WORD);
                item = &word;
            }

            i = Find_In_Array(
                AS_ARRAY(series),
                index,
                SERIES_LEN(series),
                item,
                1,
                HAS_CASE(parse) ? AM_FIND_CASE : 0,
                1
            );

            if (i != NOT_FOUND && is_thru) i++;
        }
        else {
            // "str"
            if (ANY_BINSTR(item)) {
                if (!IS_STRING(item) && !IS_BINARY(item)) {
                    // !!! Can this be optimized not to use COPY?
                    ser = Copy_Form_Value(item, 0);
                    i = Find_Str_Str(
                        series,
                        0,
                        index,
                        SERIES_LEN(series),
                        1,
                        ser,
                        0,
                        SERIES_LEN(ser),
                        (parse->find_flags & AM_FIND_CASE)
                            ? AM_FIND_CASE
                            : 0
                    );
                    if (i != NOT_FOUND && is_thru) i += SERIES_LEN(ser);
                    Free_Series(ser);
                }
                else {
                    i = Find_Str_Str(
                        series,
                        0,
                        index,
                        SERIES_LEN(series),
                        1,
                        VAL_SERIES(item),
                        VAL_INDEX(item),
                        VAL_LEN_AT(item),
                        (parse->find_flags & AM_FIND_CASE)
                            ? AM_FIND_CASE
                            : 0
                    );
                    if (i != NOT_FOUND && is_thru) i += VAL_LEN_AT(item);
                }
            }
            // #"A"
            else if (IS_CHAR(item)) {
                i = Find_Str_Char(
                    VAL_CHAR(item),
                    series,
                    0,
                    index,
                    SERIES_LEN(series),
                    1,
                    (parse->find_flags & AM_FIND_CASE)
                        ? AM_FIND_CASE
                        : 0
                );
                if (i != NOT_FOUND && is_thru) i++;
            }
            // bitset
            else if (IS_BITSET(item)) {
                i = Find_Str_Bitset(
                    series,
                    0,
                    index,
                    SERIES_LEN(series),
                    1,
                    VAL_BITSET(item),
                    (parse->find_flags & AM_FIND_CASE)
                        ? AM_FIND_CASE
                        : 0
                );
                if (i != NOT_FOUND && is_thru) i++;
            }
            else
                fail (Error(RE_PARSE_RULE, item));
        }
    }

    return i;
}


//
//  Do_Eval_Rule: C
// 
// Evaluate the input as a code block. Advance input if
// rule succeeds. Return new index or failure.
// 
// Examples:
//     do skip
//     do end
//     do "abc"
//     do 'abc
//     do [...]
//     do variable
//     do datatype!
//     do quote 123
//     do into [...]
// 
// Problem: cannot write:  set var do datatype!
//
static REBCNT Do_Eval_Rule(REBPARSE *parse, REBCNT index, const REBVAL **rule)
{
    const REBVAL *item = *rule;
    REBCNT n;
    REBPARSE newparse;

    REBVAL value;
    REBVAL save; // REVIEW: Could this just reuse value?
    VAL_INIT_WRITABLE_DEBUG(&value);
    VAL_INIT_WRITABLE_DEBUG(&save);

    // First, check for end of input:
    if (index >= SERIES_LEN(parse->series)) {
        if (IS_WORD(item) && VAL_CMD(item) == SYM_END) return index;
        else return NOT_FOUND;
    }

    // Evaluate next N input values:
    DO_NEXT_MAY_THROW(index, &value, AS_ARRAY(parse->series), index);

    if (index == THROWN_FLAG) {
        // Value is a THROW, RETURN, BREAK, etc...we have to stop processing
        *parse->out = value;
        return THROWN_FLAG;
    }

    // Get variable or command:
    if (IS_WORD(item)) {

        n = VAL_CMD(item);

        if (n == SYM_SKIP)
            return (IS_SET(&value)) ? index : NOT_FOUND;

        if (n == SYM_QUOTE) {
            item = item + 1;
            (*rule)++;
            if (IS_END(item)) fail (Error(RE_PARSE_END, item - 2));
            if (IS_GROUP(item)) {
                // might GC
                if (DO_ARRAY_THROWS(&save, item)) {
                    *parse->out = save;
                    return THROWN_FLAG;
                }
                item = &save;
            }
        }
        else if (n == SYM_INTO) {
            REBPARSE sub_parse;
            REBCNT i;

            item = item + 1;
            (*rule)++;
            if (IS_END(item)) fail (Error(RE_PARSE_END, item - 2));
            item = Get_Parse_Value(&save, item); // sub-rules
            if (!IS_BLOCK(item)) fail (Error(RE_PARSE_RULE, item - 2));
            if (!ANY_BINSTR(&value) && !ANY_ARRAY(&value)) return NOT_FOUND;

            sub_parse.series = VAL_SERIES(&value);
            sub_parse.type = VAL_TYPE(&value);
            sub_parse.find_flags = parse->find_flags;
            sub_parse.result = 0;
            sub_parse.out = parse->out;

            i = Parse_Rules_Loop(
                &sub_parse, VAL_INDEX(&value), VAL_ARRAY_AT(item), 0
            );

            if (i == THROWN_FLAG) return THROWN_FLAG;

            if (i == VAL_LEN_HEAD(&value)) return index;

            return NOT_FOUND;
        }
        else if (n > 0)
            fail (Error(RE_PARSE_RULE, item));
        else
            item = Get_Parse_Value(&save, item); // variable
    }
    else if (IS_PATH(item)) {
        item = Get_Parse_Value(&save, item); // variable
    }
    else if (IS_SET_WORD(item) || IS_GET_WORD(item) || IS_SET_PATH(item) || IS_GET_PATH(item))
        fail (Error(RE_PARSE_RULE, item));

    if (IS_NONE(item)) {
        return (VAL_TYPE(&value) > REB_NONE) ? NOT_FOUND : index;
    }

    // Copy the value into its own block:
    newparse.series = ARRAY_SERIES(Make_Array(1));
    Append_Value(AS_ARRAY(newparse.series), &value);
    newparse.type = REB_BLOCK;
    newparse.find_flags = parse->find_flags;
    newparse.result = 0;
    newparse.out = parse->out;

    PUSH_GUARD_SERIES(newparse.series);
    n = Parse_Next_Array(&newparse, 0, item, 0);
    DROP_GUARD_SERIES(newparse.series);

    if (n == THROWN_FLAG)
        return THROWN_FLAG;

    if (n == NOT_FOUND)
        return NOT_FOUND;

    return index;
}


//
//  Parse_Rules_Loop: C
//
static REBCNT Parse_Rules_Loop(
    REBPARSE *parse,
    REBCNT index,
    const REBVAL *rules,
    REBCNT depth
) {
    REBSER *series = parse->series;
    const REBVAL *item;     // current rule item
    const REBVAL *word;     // active word to be set
    REBCNT start;       // recovery restart point
    REBCNT i;           // temp index point
    REBCNT begin;       // point at beginning of match
    REBINT count;       // iterated pattern counter
    REBINT mincount;    // min pattern count
    REBINT maxcount;    // max pattern count
    const REBVAL *item_hold;
    REBVAL *val;        // spare
    REBCNT rulen;
    REBFLGS flags;
    REBCNT cmd;
    const REBVAL *rule_head = rules;

    REBVAL save;
    VAL_INIT_WRITABLE_DEBUG(&save);

    if (C_STACK_OVERFLOWING(&flags)) Trap_Stack_Overflow();
    //if (depth > MAX_PARSE_DEPTH) vTrap_Word(RE_LIMIT_HIT, SYM_PARSE, 0);
    flags = 0;
    word = 0;
    mincount = maxcount = 1;
    start = begin = index;

    // For each rule in the rule block:
    while (NOT_END(rules)) {

        //Print_Parse_Index(parse->type, rules, series, index);

        if (--Eval_Count <= 0 || Eval_Signals) {
            //
            // !!! See notes on other invocations about the questions raised by
            // calls to Do_Signals_Throws() by places that do not have a clear
            // path up to return results from an interactive breakpoint.
            //
            REBVAL result;
            VAL_INIT_WRITABLE_DEBUG(&result);

            if (Do_Signals_Throws(&result))
                fail (Error_No_Catch_For_Throw(&result));
            if (IS_SET(&result))
                fail (Error(RE_MISC));
        }

        //--------------------------------------------------------------------
        // Pre-Rule Processing Section
        //
        // For non-iterated rules, including setup for iterated rules.
        // The input index is not advanced here, but may be changed by
        // a GET-WORD variable.
        //--------------------------------------------------------------------

        item = rules++;

        // If word, set-word, or get-word, process it:
        if (VAL_TYPE(item) >= REB_WORD && VAL_TYPE(item) <= REB_GET_WORD) {

            // Is it a command word?
            if ((cmd = VAL_CMD(item))) {

                if (!IS_WORD(item)) {
                    // SET or GET not allowed
                    fail (Error(RE_PARSE_COMMAND, item));
                }

                if (cmd <= SYM_BREAK) { // optimization

                    switch (cmd) {

                    case SYM_BAR:
                        return index;   // reached it successfully

                    // Note: mincount = maxcount = 1 on entry
                    case SYM_WHILE:
                        SET_FLAG(flags, PF_WHILE);
                    case SYM_ANY:
                        mincount = 0;
                    case SYM_SOME:
                        maxcount = MAX_I32;
                        continue;

                    case SYM_OPT:
                        mincount = 0;
                        continue;

                    case SYM_COPY:
                        SET_FLAG(flags, PF_COPY);
                    case SYM_SET:
                        SET_FLAG(flags, PF_SET_OR_COPY);
                        item = rules++;
                        if (!(IS_WORD(item) || IS_SET_WORD(item)))
                            fail (Error(RE_PARSE_VARIABLE, item));
                        if (VAL_CMD(item)) fail (Error(RE_PARSE_COMMAND, item));
                        word = item;
                        continue;

                    case SYM_NOT:
                        SET_FLAG(flags, PF_NOT);
                        flags ^= (1<<PF_NOT2);
                        continue;

                    case SYM_AND:
                        SET_FLAG(flags, PF_AND);
                        continue;

                    case SYM_THEN:
                        SET_FLAG(flags, PF_THEN);
                        continue;

                    case SYM_REMOVE:
                        SET_FLAG(flags, PF_REMOVE);
                        continue;

                    case SYM_INSERT:
                        SET_FLAG(flags, PF_INSERT);
                        goto post;

                    case SYM_CHANGE:
                        SET_FLAG(flags, PF_CHANGE);
                        continue;

                    // There are two RETURNs: one is a matching form, so with
                    // 'parse data [return "abc"]' you are not asking to
                    // return the literal string "abc" independent of input.
                    // it will only return if "abc" matches.  This works for
                    // a rule reference as well, such as 'return rule'.
                    //
                    // The second option is if you put the value in parens,
                    // in which case it will just return whatever that value
                    // happens to be, e.g. 'parse data [return ("abc")]'

                    case SYM_RETURN:
                        if (IS_GROUP(rules)) {
                            REBVAL evaluated;
                            VAL_INIT_WRITABLE_DEBUG(&evaluated);

                            if (DO_ARRAY_THROWS(&evaluated, rules)) {
                                // If the group evaluation result gives a
                                // THROW, BREAK, CONTINUE, etc then we'll
                                // return that
                                *parse->out = evaluated;
                                return THROWN_FLAG;
                            }

                            *parse->out = *ROOT_PARSE_NATIVE;
                            CONVERT_NAME_TO_THROWN(
                                parse->out, &evaluated, FALSE
                            );

                            // Implicitly returns whatever's in parse->out
                            return THROWN_FLAG;
                        }
                        SET_FLAG(flags, PF_RETURN);
                        continue;

                    case SYM_ACCEPT:
                    case SYM_BREAK:
                        parse->result = 1;
                        return index;

                    case SYM_REJECT:
                        parse->result = -1;
                        return index;

                    case SYM_FAIL:
                        index = NOT_FOUND;
                        goto post;

                    case SYM_IF:
                        item = rules++;
                        if (IS_END(item)) goto bad_end;
                        if (!IS_GROUP(item)) fail (Error(RE_PARSE_RULE, item));

                        // might GC
                        if (DO_ARRAY_THROWS(&save, item)) {
                            *parse->out = save;
                            return THROWN_FLAG;
                        }

                        item = &save;
                        if (IS_CONDITIONAL_TRUE(item)) continue;
                        else {
                            index = NOT_FOUND;
                            goto post;
                        }

                    case SYM_LIMIT:
                        fail (Error(RE_NOT_DONE));
                        //val = Get_Parse_Value(&save, rules++);
                    //  if (IS_INTEGER(val)) limit = index + Int32(val);
                    //  else if (ANY_SERIES(val)) limit = VAL_INDEX(val);
                    //  else goto
                        //goto bad_rule;
                    //  goto post;

                    case SYM__Q_Q:
                        Print_Parse_Index(parse->type, rules, series, index);
                        continue;
                    }
                }
                // Any other cmd must be a match command, so proceed...

            } else { // It's not a PARSE command, get or set it:

                // word: - set a variable to the series at current index
                if (IS_SET_WORD(item)) {
                    REBVAL temp;
                    VAL_INIT_WRITABLE_DEBUG(&temp);

                    Val_Init_Series_Index(&temp, parse->type, series, index);

                    *GET_MUTABLE_VAR(item) = temp;

                    continue;
                }

                // :word - change the index for the series to a new position
                if (IS_GET_WORD(item)) {
                    // !!! Should mutability be enforced?
                    item = GET_MUTABLE_VAR(item);
                    if (!ANY_SERIES(item)) // #1263
                        fail (Error(RE_PARSE_SERIES, rules - 1));
                    index = Set_Parse_Series(parse, item);
                    series = parse->series;
                    continue;
                }

                // word - some other variable
                if (IS_WORD(item)) {
                    // !!! Should mutability be enforced?
                    item = GET_MUTABLE_VAR(item);
                }

                // item can still be 'word or /word
            }
        }
        else if (ANY_PATH(item)) {
            if (IS_PATH(item)) {
                if (Do_Path_Throws(&save, NULL, item, NULL))
                    fail (Error_No_Catch_For_Throw(&save));
                item = &save;
            }
            else if (IS_SET_PATH(item)) {
                REBVAL tmp;
                VAL_INIT_WRITABLE_DEBUG(&tmp);

                Val_Init_Series(&tmp, parse->type, parse->series);
                VAL_INDEX(&tmp) = index;
                if (Do_Path_Throws(&save, NULL, item, &tmp))
                    fail (Error_No_Catch_For_Throw(&save));
                item = &save;
            }
            else if (IS_GET_PATH(item)) {
                if (Do_Path_Throws(&save, NULL, item, NULL))
                    fail (Error_No_Catch_For_Throw(&save));
                // CureCode #1263 change
                //      if (parse->type != VAL_TYPE(item) || VAL_SERIES(item) != parse->series)
                if (!ANY_SERIES(&save)) fail (Error(RE_PARSE_SERIES, item));
                index = Set_Parse_Series(parse, &save);
                item = NULL;
            }

            if (index > SERIES_LEN(series)) index = SERIES_LEN(series);
            if (!item) continue; // for SET and GET cases
        }

        if (IS_GROUP(item)) {
            REBVAL evaluated;
            VAL_INIT_WRITABLE_DEBUG(&evaluated);

            // might GC
            if (DO_ARRAY_THROWS(&evaluated, item)) {
                *parse->out = evaluated;
                return THROWN_FLAG;
            }
            // ignore evaluated if it's not THROWN?

            if (index > SERIES_LEN(series)) index = SERIES_LEN(series);
            continue;
        }

        // Counter? 123
        if (IS_INTEGER(item)) { // Specify count or range count
            SET_FLAG(flags, PF_WHILE);
            mincount = maxcount = Int32s(item, 0);
            item = Get_Parse_Value(&save, rules++);
            if (IS_END(item)) fail (Error(RE_PARSE_END, rules - 2));
            if (IS_INTEGER(item)) {
                maxcount = Int32s(item, 0);
                item = Get_Parse_Value(&save, rules++);
                if (IS_END(item)) fail (Error(RE_PARSE_END, rules - 2));
            }
        }
        // else fall through on other values and words

        //--------------------------------------------------------------------
        // Iterated Rule Matching Section:
        //
        // Repeats the same rule N times or until the rule fails.
        // The index is advanced and stored in a temp variable i until
        // the entire rule has been satisfied.
        //--------------------------------------------------------------------

        item_hold = item;   // a command or literal match value

        if (VAL_TYPE(item) <= REB_UNSET || VAL_TYPE(item) >= REB_NATIVE)
            goto bad_rule;

        begin = index;      // input at beginning of match section
        rulen = 0;          // rules consumed (do not use rule++ below)

        //note: rules var already advanced

        for (count = 0; count < maxcount;) {

            item = item_hold;

            if (IS_WORD(item)) {

                switch (cmd = VAL_WORD_CANON(item)) {

                case SYM_SKIP:
                    i = (index < SERIES_LEN(series))
                        ? index + 1
                        : NOT_FOUND;
                    break;

                case SYM_END:
                    i = (index < SERIES_LEN(series))
                        ? NOT_FOUND
                        : SERIES_LEN(series);
                    break;

                case SYM_TO:
                case SYM_THRU:
                    if (IS_END(rules)) goto bad_end;
                    item = Get_Parse_Value(&save, rules);
                    rulen = 1;
                    i = Parse_To(parse, index, item, LOGICAL(cmd == SYM_THRU));
                    break;

                case SYM_QUOTE:
                    if (IS_END(rules)) goto bad_end;
                    rulen = 1;
                    if (IS_GROUP(rules)) {
                        // might GC
                        if (DO_ARRAY_THROWS(&save, rules)) {
                            *parse->out = save;
                            return THROWN_FLAG;
                        }
                        item = &save;
                    }
                    else item = rules;

                    if (0 == Cmp_Value(
                        ARRAY_AT(AS_ARRAY(series), index),
                        item,
                        HAS_CASE(parse)
                    )) {
                        i = index + 1;
                    }
                    else {
                        i = NOT_FOUND;
                    }
                    break;

                case SYM_INTO: {
                    REBPARSE sub_parse;

                    if (IS_END(rules)) goto bad_end;

                    rulen = 1;
                    item = Get_Parse_Value(&save, rules); // sub-rules

                    if (!IS_BLOCK(item)) goto bad_rule;

                    val = ARRAY_AT(AS_ARRAY(series), index);

                    if (IS_END(val) || (!ANY_BINSTR(val) && !ANY_ARRAY(val))) {
                        i = NOT_FOUND;
                        break;
                    }

                    sub_parse.series = VAL_SERIES(val);
                    sub_parse.type = VAL_TYPE(val);
                    sub_parse.find_flags = parse->find_flags;
                    sub_parse.result = 0;
                    sub_parse.out = parse->out;

                    i = Parse_Rules_Loop(
                        &sub_parse,
                        VAL_INDEX(val),
                        VAL_ARRAY_AT(item),
                        depth + 1
                    );

                    if (i == THROWN_FLAG) return THROWN_FLAG;

                    if (i != VAL_LEN_HEAD(val)) {
                        i = NOT_FOUND;
                        break;
                    }

                    i = index + 1;
                    break;
                }

                case SYM_DO:
                    if (!IS_ARRAY_INPUT(parse)) goto bad_rule;

                    i = Do_Eval_Rule(parse, index, &rules);

                    if (i == THROWN_FLAG) return THROWN_FLAG;

                    rulen = 1;
                    break;

                default:
                    goto bad_rule;
                }
            }
            else if (IS_BLOCK(item)) {
                item = VAL_ARRAY_AT(item);
                //if (IS_END(rules) && item == rule_head) {
                //  rules = item;
                //  goto top;
                //}
                i = Parse_Rules_Loop(parse, index, item, depth+1);

                if (i == THROWN_FLAG) return THROWN_FLAG;

                if (parse->result) {
                    index = (parse->result > 0) ? i : NOT_FOUND;
                    parse->result = 0;
                    break;
                }
            }
            // Parse according to datatype:
            else {
                if (IS_ARRAY_INPUT(parse))
                    i = Parse_Next_Array(parse, index, item, depth+1);
                else
                    i = Parse_Next_String(parse, index, item, depth+1);

                // i may be THROWN_FLAG
            }

            if (i == THROWN_FLAG) return THROWN_FLAG;

            // Necessary for special cases like: some [to end]
            // i: indicates new index or failure of the match, but
            // that does not mean failure of the rule, because optional
            // matches can still succeed, if if the last match failed.
            if (i != NOT_FOUND) {
                count++; // may overflow to negative
                if (count < 0) count = MAX_I32; // the forever case
                // If input did not advance:
                if (i == index && !GET_FLAG(flags, PF_WHILE)) {
                    if (count < mincount) index = NOT_FOUND; // was not enough
                    break;
                }
            }
            //if (i >= series->tail) {     // OLD check: no more input
            else {
                if (count < mincount) index = NOT_FOUND; // was not enough
                else if (i != NOT_FOUND) index = i;
                // else keep index as is.
                break;
            }
            index = i;

            // A BREAK word stopped us:
            //if (parse->result) {parse->result = 0; break;}
        }

        rules += rulen;

        //if (index > series->tail && index != NOT_FOUND) index = series->tail;
        if (index > SERIES_LEN(series)) index = NOT_FOUND;

        //--------------------------------------------------------------------
        // Post Match Processing:
        //--------------------------------------------------------------------
post:
        // Process special flags:
        if (flags) {
            // NOT before all others:
            if (GET_FLAG(flags, PF_NOT)) {
                if (GET_FLAG(flags, PF_NOT2) && index != NOT_FOUND) index = NOT_FOUND;
                else index = begin;
            }
            if (index == NOT_FOUND) { // Failure actions:
                // !!! if word isn't NULL should we set its var to NONE! ...?
                if (GET_FLAG(flags, PF_THEN)) {
                    SKIP_TO_BAR(rules);
                    if (NOT_END(rules)) rules++;
                }
            }
            else {  // Success actions:
                count = (begin > index) ? 0 : index - begin; // how much we advanced the input
                if (GET_FLAG(flags, PF_COPY)) {
                    REBVAL temp;
                    VAL_INIT_WRITABLE_DEBUG(&temp);

                    Val_Init_Series(
                        &temp,
                        parse->type,
                        IS_ARRAY_INPUT(parse)
                            ? ARRAY_SERIES(Copy_Array_At_Max_Shallow(
                                AS_ARRAY(series), begin, count
                            ))
                            : Copy_String_Slimming(series, begin, count) // condenses;
                    );
                    *GET_MUTABLE_VAR(word) = temp;
                }
                else if (GET_FLAG(flags, PF_SET_OR_COPY)) {
                    REBVAL *var = GET_MUTABLE_VAR(word); // traps if protected

                    if (IS_ARRAY_INPUT(parse)) {
                        if (count == 0) SET_NONE(var);
                        else *var = *ARRAY_AT(AS_ARRAY(series), begin);
                    }
                    else {
                        if (count == 0) SET_NONE(var);
                        else {
                            i = GET_ANY_CHAR(series, begin);
                            if (parse->type == REB_BINARY) {
                                SET_INTEGER(var, i);
                            } else {
                                SET_CHAR(var, i);
                            }
                        }
                    }

                    // !!! Used to reuse item, so item was set to the var at
                    // the end, but was that actually needed?
                    item = var;
                }
                if (GET_FLAG(flags, PF_RETURN)) {
                    // See notes on PARSE's return in handling of SYM_RETURN

                    REBVAL captured;
                    VAL_INIT_WRITABLE_DEBUG(&captured);

                    Val_Init_Series(
                        &captured,
                        parse->type,
                        IS_ARRAY_INPUT(parse)
                            ? ARRAY_SERIES(Copy_Array_At_Max_Shallow(
                                AS_ARRAY(series), begin, count
                            ))
                            : Copy_String_Slimming(series, begin, count) // condenses
                    );

                    *parse->out = *ROOT_PARSE_NATIVE;
                    CONVERT_NAME_TO_THROWN(parse->out, &captured, FALSE);

                    // Implicitly returns whatever's in parse->out
                    return THROWN_FLAG;
                }
                if (GET_FLAG(flags, PF_REMOVE)) {
                    if (count) Remove_Series(series, begin, count);
                    index = begin;
                }
                if (flags & (1<<PF_INSERT | 1<<PF_CHANGE)) {
                    count = GET_FLAG(flags, PF_INSERT) ? 0 : count;
                    cmd = GET_FLAG(flags, PF_INSERT) ? 0 : (1<<AN_PART);
                    item = rules++;
                    if (IS_END(item)) goto bad_end;
                    // Check for ONLY flag:
                    if (IS_WORD(item) && (cmd = VAL_CMD(item))) {
                        if (cmd != SYM_ONLY) goto bad_rule;
                        cmd |= (1<<AN_ONLY);
                        item = rules++;
                    }
                    // CHECK FOR QUOTE!!
                    item = Get_Parse_Value(&save, item); // new value

                    if (IS_UNSET(item)) fail (Error(RE_NO_VALUE, rules - 1));

                    if (IS_END(item)) goto bad_end;

                    if (IS_ARRAY_INPUT(parse)) {
                        index = Modify_Array(
                            GET_FLAG(flags, PF_CHANGE) ? A_CHANGE : A_INSERT,
                            AS_ARRAY(series),
                            begin,
                            item,
                            cmd,
                            count,
                            1
                        );

                        if (IS_LIT_WORD(item))
                            //
                            // Only set the type, not the whole header (in
                            // order to keep binding information)
                            //
                            VAL_SET_TYPE_BITS(
                                ARRAY_AT(AS_ARRAY(series), index - 1),
                                REB_WORD
                            );
                    }
                    else {
                        if (parse->type == REB_BINARY) cmd |= (1<<AN_SERIES); // special flag
                        index = Modify_String(GET_FLAG(flags, PF_CHANGE) ? A_CHANGE : A_INSERT,
                                series, begin, item, cmd, count, 1);
                    }
                }
                if (GET_FLAG(flags, PF_AND)) index = begin;
            }

            flags = 0;
            word = 0;
        }

        // Goto alternate rule and reset input:
        if (index == NOT_FOUND) {
            SKIP_TO_BAR(rules);
            if (IS_END(rules)) break;
            rules++;
            index = begin = start;
        }

        begin = index;
        mincount = maxcount = 1;

    }
    return index;

bad_rule:
    fail (Error(RE_PARSE_RULE, rules - 1));
bad_end:
    fail (Error(RE_PARSE_END, rules - 1));
    return 0;
}


//
// Shared implementation routine for PARSE? and PARSE.  The difference is that
// PARSE? only returns whether or not a set of rules completed to the end.
// PARSE is more general purpose in terms of the result it provides, and
// it defaults to returning the input.
//
static REB_R Parse_Core(struct Reb_Call *call_, REBOOL logic)
{
    PARAM(1, input);
    PARAM(2, rules);
    REFINE(3, case);
    REFINE(4, all);

    REBCNT index;
    REBPARSE parse;

    if (IS_NONE(ARG(rules)) || IS_STRING(ARG(rules))) {
        //
        // !!! R3-Alpha supported "simple parse", which was cued by the rules
        // being either NONE! or a STRING!.  Though this functionality does
        // not exist in Ren-C, it's more informative to give an error telling
        // where to look for the functionality than a generic "parse doesn't
        // take that type" error.
        //
        fail (Error(RE_USE_SPLIT_SIMPLE));
    }

    assert(IS_BLOCK(ARG(rules)));

    // The native dispatcher should have pre-filled the output slot with a
    // trash value in the debug build.  We double-check the expectation of
    // whether the parse loop overwites this slot with a result or not.
    //
    assert(IS_TRASH_DEBUG(D_OUT));

    parse.series = VAL_SERIES(ARG(input));
    parse.type = VAL_TYPE(ARG(input));

    // We always want "case-sensitivity" on binary bytes, vs. treating as
    // case-insensitive bytes for ASCII characters.
    //
    parse.find_flags = REF(case) || IS_BINARY(ARG(input)) ? AM_FIND_CASE : 0;

    parse.result = 0;
    parse.out = D_OUT;

    index = Parse_Rules_Loop(
        &parse, VAL_INDEX(ARG(input)), VAL_ARRAY_AT(ARG(rules)), 0
    );

    if (index == THROWN_FLAG) {
        assert(!IS_TRASH_DEBUG(D_OUT));
        assert(THROWN(D_OUT));
        if (
            IS_NATIVE(D_OUT)
            && VAL_FUNC_CODE(ROOT_PARSE_NATIVE) == VAL_FUNC_CODE(D_OUT)
        ) {
            // Note the difference:
            //
            //     parse "1020" [(return true) not-seen]
            //     parse "0304" [return [some ["0" skip]]] not-seen]
            //
            // In the first, a parenthesized evaluation ran a `return`, which
            // is aiming to return from a function using a THROWN().  In
            // the second case parse interrupted *itself* with a THROWN_FLAG
            // to evaluate the expression to the result "0304" from the
            // matched pattern.
            //
            // When parse interrupts itself by throwing, it indicates so
            // by using the throw name of its own REB_NATIVE-valued function.
            // This handles that branch and catches the result value.
            //
            CATCH_THROWN(D_OUT, D_OUT);

            // In the logic case, we are only concerned with matching.  If
            // a construct that can return arbitrary values is used, then
            // failure is triggered with a specific error, saying PARSE must
            // be used instead of PARSE?.
            //
            // !!! Review if this is the best semantics for a parsing variant
            // that is committed to only returning logic TRUE or FALSE, in
            // spite of existence of rules that allow the general PARSE to
            // do otherwise.
            //
            if (logic && !IS_LOGIC(D_OUT))
                fail (Error(RE_PARSE_NON_LOGIC, D_OUT));

            return R_OUT;
        }

        // All other throws should just bubble up uncaught.
        //
        return R_OUT_IS_THROWN;
    }

    // If the loop returned to us, it shouldn't have put anything in out.
    //
    assert(IS_TRASH_DEBUG(D_OUT));

    // Parse can fail if the match rule state can't process pending input.
    //
    if (index == NOT_FOUND)
        return logic ? R_FALSE : R_NONE;

    // If the match rules all completed, but the parse position didn't end
    // at (or beyond) the tail of the input series, the parse also failed.
    //
    if (index < VAL_LEN_HEAD(ARG(input)))
        return logic ? R_FALSE : R_NONE;

    // The end was reached...if doing a logic-based PARSE? then return TRUE.
    //
    if (logic) return R_TRUE;

    // Otherwise it's PARSE so return the input (a series, hence conditionally
    // true, yet more informative for chaining.)  See #2165.
    //
    *D_OUT = *ARG(input);
    return R_OUT;
}


//
//  parse?: native [
//
//  ; NOTE: If changing this, also update PARSE
//
//  "Determines if a series matches the given grammar rules or not."
//
//      input [any-series!]
//          "Input series to parse"
//      rules [block! string! none!]
//          "Rules to parse by (STRING! and NONE! are deprecated)"
//      /case
//          "Uses case-sensitive comparison"
//      /all
//          "(ignored refinement left for Rebol2 transitioning)"
//  ]
//
REBNATIVE(parse_q)
{
    return Parse_Core(call_, TRUE);
}


//
//  parse: native [
//
//  ; NOTE: If changing this, also update PARSE?
//
//  "Parses a series according to grammar rules and returns a result."
//
//      input [any-series!]
//          "Input series to parse (default result for successful match)"
//      rules [block! string! none!]
//          "Rules to parse by (STRING! and NONE! are deprecated)"
//      /case
//          "Uses case-sensitive comparison"
//      /all
//          "(ignored refinement left for Rebol2 transitioning)"
//  ]
//
REBNATIVE(parse)
{
    return Parse_Core(call_, FALSE);
}
