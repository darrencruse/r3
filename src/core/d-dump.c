//
//  File: %d-dump.c
//  Summary: "various debug output functions"
//  Section: debug
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

//#include <stdio.h> // !!! No <stdio.h> in Ren-C release builds

#include "sys-core.h"
#include "mem-series.h" // low-level series memory access

#if !defined(NDEBUG)

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

//
//  Dump_Series: C
//
void Dump_Series(REBSER *series, const char *memo)
{
    if (!series) return;
    Debug_Fmt(
        "%s Series %x \"%s\":"
            " wide: %2d"
            " size: %6d"
            " bias: %d"
            " tail: %d"
            " rest: %d"
            " flags: %x",
        memo,
        series,
        "-", // !label
        SER_WIDE(series),
        SER_TOTAL(series),
        SER_BIAS(series),
        SER_LEN(series),
        SER_REST(series),
        series->info.bits // flags + width
    );
    if (Is_Array_Series(series)) {
        Dump_Values(ARR_HEAD(AS_ARRAY(series)), SER_LEN(series));
    } else
        Dump_Bytes(
            SER_DATA_RAW(series),
            (SER_LEN(series) + 1) * SER_WIDE(series)
        );
}

//
//  Dump_Bytes: C
//
void Dump_Bytes(REBYTE *bp, REBCNT limit)
{
    const REBCNT max_lines = 120;
    REBYTE buf[2048];
    REBYTE str[40];
    REBYTE *cp, *tp;
    REBYTE c;
    REBCNT l, n;
    REBCNT cnt = 0;

    cp = buf;
    for (l = 0; l < max_lines; l++) {
        cp = Form_Hex_Pad(cp, (REBUPT) bp, 8);

        *cp++ = ':';
        *cp++ = ' ';
        tp = str;

        for (n = 0; n < 16; n++) {
            if (cnt++ >= limit) break;
            c = *bp++;
            cp = Form_Hex2(cp, c);
            if ((n & 3) == 3) *cp++ = ' ';
            if ((c < 32) || (c > 126)) c = '.';
            *tp++ = c;
        }

        for (; n < 16; n++) {
            c = ' ';
            *cp++ = c;
            *cp++ = c;
            if ((n & 3) == 3) *cp++ = ' ';
            if ((c < 32) || (c > 126)) c = '.';
            *tp++ = c;
        }
        *tp++ = 0;

        for (tp = str; *tp;) *cp++ = *tp++;

        *cp = 0;
        Debug_Str(s_cast(buf));
        if (cnt >= limit) break;
        cp = buf;
    }
}


//
//  Dump_Values: C
// 
// Print values in raw hex; If memory is corrupted this still needs to work.
//
void Dump_Values(RELVAL *vp, REBCNT count)
{
    REBYTE buf[2048];
    REBYTE *cp;
    REBCNT l, n;
    REBCNT *bp = (REBCNT*)vp;
    const REBYTE *type;

    cp = buf;
    for (l = 0; l < count; l++) {
        REBVAL *val = cast(REBVAL*, bp);
        cp = Form_Hex_Pad(cp, l, 8);

        *cp++ = ':';
        *cp++ = ' ';

        type = Get_Type_Name((REBVAL*)bp);
        for (n = 0; n < 11; n++) {
            if (*type) *cp++ = *type++;
            else *cp++ = ' ';
        }
        *cp++ = ' ';
        for (n = 0; n < sizeof(REBVAL) / sizeof(REBCNT); n++) {
            cp = Form_Hex_Pad(cp, *bp++, 8);
            *cp++ = ' ';
        }
        n = 0;
        if (IS_WORD(val) || IS_GET_WORD(val) || IS_SET_WORD(val)) {
            const REBYTE *name = STR_HEAD(VAL_WORD_SPELLING(val));
            n = snprintf(
                s_cast(cp), sizeof(buf) - (cp - buf), " (%s)", cs_cast(name)
            );
        }

        *(cp + n) = 0;
        Debug_Str(s_cast(buf));
        cp = buf;
    }
}


//
//  Dump_Info: C
//
void Dump_Info(void)
{
    Debug_Fmt("^/--REBOL Kernel Dump--");

    Debug_Fmt("Evaluator:");
    Debug_Fmt("    Cycles:  %d", cast(REBINT, Eval_Cycles));
    Debug_Fmt("    Counter: %d", Eval_Count);
    Debug_Fmt("    Dose:    %d", Eval_Dose);
    Debug_Fmt("    Signals: %x", Eval_Signals);
    Debug_Fmt("    Sigmask: %x", Eval_Sigmask);
    Debug_Fmt("    DSP:     %d", DSP);

    Debug_Fmt("Memory/GC:");

    Debug_Fmt("    Ballast: %d", GC_Ballast);
    Debug_Fmt("    Disable: %d", GC_Disabled);
    Debug_Fmt("    Guarded Series: %d", SER_LEN(GC_Series_Guard));
    Debug_Fmt("    Guarded Values: %d", SER_LEN(GC_Value_Guard));
}


//
//  Dump_Stack: C
//
// Prints stack counting levels from the passed in number.  Pass 0 to start.
//
void Dump_Stack(REBFRM *f, REBCNT level, REBOOL dump_args)
{
    REBINT n;
    REBVAL *arg;
    REBVAL *param;

    Debug_Fmt(""); // newline.

    if (f == NULL) f = FS_TOP;
    if (f == NULL) {
        Debug_Fmt("*STACK[] - NO FRAMES*");
        return;
    }

    for (; f != NULL; f = f->prior) {
        if (NOT(Is_Any_Function_Frame(f)))
            continue;
        printf(
            "STACK[%d](%s) - %d\n",
            level,
            FRM_LABEL(f) ? STR_HEAD(FRM_LABEL(f)) : NULL,
            f->eval_type // note: this is now an ordinary Reb_Kind, stringify it
        );

        if (dump_args) {
            n = 1;
            arg = FRM_ARG(f, 1);
            param = FUNC_PARAMS_HEAD(f->func);

            for (; NOT_END(param); ++param, ++arg, ++n) {
                Debug_Fmt(
                    "    %s: %72r",
                    STR_HEAD(VAL_PARAM_SPELLING(param)),
                    arg
                );
            }
        }
    }
}

#ifdef TEST_PRINT
    // Simple low-level tests:
    Print("%%d %d", 1234);
    Print("%%d %d", -1234);
    Print("%%d %d", 12345678);
    Print("%%d %d", 0);
    Print("%%6d %6d", 1234);
    Print("%%10d %10d", 123456789);
    Print("%%x %x", 0x1234ABCD);
    Print("%%x %x", -1);
    Print("%%4x %x", 0x1234);
    Print("%%s %s", "test");
    Print("%%s %s", 0);
    Print("%%c %c", (REBINT)'X');
    Print("%s %d %x", "test", 1234, 1234);
    getchar();
#endif

#endif // DUMP is picked up by scan regardless of #ifdef, must be defined


//
//  dump: native [
//
//  "Temporary debug dump"
//
//      value [<opt> any-value!]
//  ]
//
REBNATIVE(dump)
{
#ifdef NDEBUG
    fail (Error(RE_DEBUG_ONLY));
#else
    PARAM(1, value);

    REBVAL *value = ARG(value);

    Dump_Stack(frame_, 0, TRUE);

    if (ANY_SERIES(value))
        Dump_Series(VAL_SERIES(value), "=>");
    else
        Dump_Values(value, 1);

    *D_OUT = *value;
    return R_OUT;
#endif
}
