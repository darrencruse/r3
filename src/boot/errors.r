REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "Error objects"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0.
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Purpose: {
        Specifies error categories and default error messages.
    }
]

Special: [
    code: 0
    type: "special"     ; Not really "errors"
    null:               {invalid error code zero}
    halt:               {halted by user or script}
]

Internal: [
    ; Some of these internal errors can happen prior to when the error
    ; catalog (e.g. the blocks in this file) have been loaded as Rebol data.
    ; Those strings need to be accessible another way, so %make-boot.r takes
    ; the string data and duplicates it as C literals usable by Panic_Core()
    ;
    ; Note: Because these are flattened into printf-style format strings
    ; instead of using the ordinary error-building mechanism, argument
    ; ordering is not honored when an error is raised using the mechanism
    ; from before when booting is ready.  It's also probably a bad idea to
    ; use sophisticated REBVALs as arguments, because it may be too early
    ; in the boot process for them to be molded properly.
    ;
    code: 100
    type: "internal"

    ; Because adding an error code has the overhead of modifying this file and
    ; coming up with a good name, errors for testing or that you don't think
    ; will happen can be put here.  A debug build will identify the line
    ; number and file source of the error, so provides some info already.
    ;
    misc:               {RE_MISC error (if actually happens, add to %errors.r)}

    ; !!! Should there be a distinction made between different kinds of
    ; stack overflows?  (Call stack, Data stack?)
    ;
    stack-overflow:     {stack overflow}

    not-done:           {reserved for future use (or not yet implemented)}

    ; !!! Should boot errors be their own category?  Even if they were,
    ; there will still be some errors that are not boot-specific which can
    ; happen during boot (such as out of memory) which can't rely on boot
    ;
    no-memory:          [{not enough memory:} :arg1 {bytes}]
    corrupt-memory:     {Check_Memory() found a problem}
    boot-data:          {no boot.r text found}
    native-boot:        {bad boot.r native ordering}
    bad-boot-string:    {boot strings area is invalid}
    bad-boot-type-block: {boot block is wrong size}
    max-natives:        {too many natives}
    action-overflow:    {more actions than we should have}
    bad-trash-canon:    {TRASH! was not found}
    bad-true-canon:     {TRUE was not found}
    bad-trash-type:     {the TRASH! word is not correct}
    rebval-alignment:   {sizeof(REBVAL) not 4x 32-bits or 4x 64-bits}
    pool-alignment:     {Memory pool width not 64-bit aligned}

    io-error:           {problem with IO}
    max-words:          {too many words}
    locked-series:      {locked series expansion}
    max-events:         {event queue overflow}
    unexpected-case:    {no case in switch statement}
    bad-size:           {expected size did not match}
    no-buffer:          {buffer not yet allocated}
    invalid-datatype:   [{invalid datatype #} :arg1]
    bad-path:           [{bad path:} :arg1]
    not-here:           [:arg1 {not supported on your system}]
    globals-full:       {no more global variable space}
    limit-hit:          [{internal limit reached:} :arg1]
    bad-sys-func:       [{invalid or missing system function:} :arg1]
    invalid-error:      [{error object or fields were not valid:} :arg1]
    bad-evaltype:       {invalid datatype for evaluation}
    hash-overflow:      {Hash ran out of space}
    no-print-ptr:       {print is missing string pointer}

    bad-utf8:           {invalid UTF-8 byte sequence found during decoding}
    codepoint-too-high: [{codepoint} :arg1 {too large (or data is not UTF-8)}]

    debug-only:         {Feature available only in DEBUG builds}

    host-no-breakpoint: {Interpreter host code has no breakpoint handler}

    invalid-exit:       {Frame does not exist on the stack to EXIT from}
]

Syntax: [
    code: 200
    type: "syntax error"
    invalid:            [{invalid} :arg1 {--} :arg2]
    missing:            [{missing} :arg2 {at} :arg1]
    no-header:          [{script is missing a REBOL header:} :arg1]
    bad-header:         [{script header is not valid:} :arg1]
    bad-checksum:       [{script checksum failed:} :arg1]
    malconstruct:       [{invalid construction spec:} :arg1]
    bad-char:           [{invalid character in:} :arg1]
    needs:              [{this script needs} :arg1 :arg2 {or better to run correctly}]
]

Script: [
    code: 300
    type: "script error"
    no-value:           [:arg1 {has no value}]
    need-value:         [:arg1 {needs a value}]
    not-bound:          [:arg1 {word is not bound to a context}]
    no-relative:        [:arg1 {word is bound relative to context not on stack}]
    not-in-context:     [:arg1 {is not in the specified context}]

    no-arg:             [:arg1 {is missing its} :arg2 {argument}]
    expect-arg:         [:arg1 {does not allow} :arg2 {for its} :arg3 {argument}]
    expect-val:         [{expected} :arg1 {not} :arg2]
    expect-type:        [:arg1 :arg2 {field must be of type} :arg3]
    cannot-use:         [{cannot use} :arg1 {on} :arg2 {value}]
    apply-too-many:     {Too many values in processed argument block of APPLY.}
    local-injection:    [{Attempt to inject value to local} :arg1 {in} :arg2]

    expression-barrier: {Expression barrier hit while processing arguments}
    bar-hit-mid-case:   {Expression barrier hit in middle of CASE pairing}

    break-not-continue: {Use BREAK/WITH when body is the breaking condition}

    ; !!! Temporary errors while faulty constructs are still outstanding
    ; (more informative than just saying "function doesn't take that type")
    use-eval-for-eval:  {Use EVAL or APPLY to call functions arity > 0, not DO}
    use-fail-for-error: [{Use FAIL (not THROW or DO) to raise} :arg1]
    use-split-simple:   {Use SPLIT (instead of PARSE) for "simple" parsing}
    apply-has-changed:  {APPLY takes frame def block (or see r3-alpha-apply)}

    apply-non-function: [:arg1 {needs to be a function for APPLY/SPECIALIZE}]
    limited-fail-input: {FAIL requires complex expressions to be in a GROUP!}

    invalid-arg:        [{invalid argument:} :arg1]
    invalid-type:       [:arg1 {type is not allowed here}]
    invalid-op:         [{invalid operator:} :arg1]
    no-op-arg:          [:arg1 {operator is missing an argument}]
    invalid-data:       [{data not in correct format:} :arg1]
    not-same-type:      {values must be of the same type}
    not-related:        [{incompatible argument for} :arg1 {of} :arg2]
    bad-func-def:       [{invalid function definition:} :arg1]
    bad-func-arg:       [{function argument} :arg1 {is not valid}] ; can be a number

    no-refine:          [:arg1 {has no refinement called} :arg2]
    bad-refines:        {incompatible or invalid refinements}
    bad-refine:         [{incompatible or duplicate refinement:} :arg1]
    bad-refine-revoke:  {refinement's args must be either all unset or all set}

    invalid-path:       [{cannot access} :arg2 {in path} :arg1]
    bad-path-type:      [{path} :arg1 {is not valid for} :arg2 {type}]
    bad-path-set:       [{cannot set} :arg2 {in path} :arg1]
    bad-field-set:      [{cannot set} :arg1 {field to} :arg2 {datatype}]
    dup-vars:           [{duplicate variable specified:} :arg1]

    past-end:           {out of range or past end}
    missing-arg:        {missing a required argument or refinement}
    out-of-range:       [{value out of range:} :arg1]
    too-short:          {content too short (or just whitespace)}
    too-long:           {content too long}
    invalid-chars:      {contains invalid characters}
    invalid-compare:    [{cannot compare} :arg1 {with} :arg2]
    assert-failed:      [{assertion failed for:} :arg1]
    wrong-type:         [{datatype assertion failed for:} :arg1]

    invalid-part:       [{invalid /part count:} :arg1]
    type-limit:         [:arg1 {overflow/underflow}]
    size-limit:         [{maximum limit reached:} :arg1]

    no-return:          {block did not return a value}
    block-lines:        {expected block of lines}
    no-catch:           [{Missing CATCH for THROW of} :arg1]
    no-catch-named:     [{Missing CATCH for THROW of} :arg1 {with /NAME:} :arg2]

    locked-word:        [{variable} :arg1 {locked by PROTECT - cannot modify}]
    locked:             {value or series locked by PROTECT - cannot modify}
    hidden:             {not allowed - would expose or modify hidden values}
    bad-bad:            [:arg1 {error:} :arg2]

    bad-make-arg:       [{cannot MAKE/TO} :arg1 {from:} :arg2]
;   no-decode:          [{cannot decode} :arg1 {encoding}]
    wrong-denom:        [:arg1 {not same denomination as} :arg2]
;   bad-convert:        [{invalid conversion value:} :arg1]
    bad-compression:    [{invalid compressed data - problem:} :arg1]
    dialect:            [{incorrect} :arg1 {dialect usage at:} :arg2]
    bad-command:        {invalid command format (extension function)}

    parse-rule:         [{PARSE - invalid rule or usage of rule:} :arg1]
    parse-end:          [{PARSE - unexpected end of rule after:} :arg1]
    parse-variable:     [{PARSE - expected a variable, not:} :arg1]
    parse-command:      [{PARSE - command cannot be used as variable:} :arg1]
    parse-series:       [{PARSE - input must be a series:} :arg1]
    parse-non-logic:    [{Non-logic PARSE? result:} :arg1 {use PARSE instead}]

    not-ffi-build:      {This Rebol build wasn't linked with libffi features}
    bad-library:        {bad library (already closed?)}
    only-callback-ptr:  {Only callback functions may be passed by FFI pointer}
    free-needs-routine: {Function to destroy struct storage must be routine}

    block-skip-wrong:   {Block is not even multiple of skip size}
;   bad-prompt:         [{Error executing prompt block}]
;   bad-port-action:    [{Cannot use} :arg1 {on this type port}]
;   face-error:         [{Invalid graphics face object}]
;   face-reused:        [{Face object reused (in more than one pane):} :arg1]

    frame-already-used: [{Frame currently in use by a function call} :arg1]

    recursive-varargs:  {VARARGS! chained into itself (maybe try <durable>?)}
    varargs-no-stack:   {Call originating VARARGS! has finished running}
    varargs-make-only:  {MAKE *shared* BLOCK! supported on VARARGS! (not TO)}
    varargs-no-look:    {VARARGS! may only lookahead by 1 if "hard quoted"}
    varargs-take-last:  {VARARGS! does not support TAKE-ing only /LAST item}
]

Math: [
    code: 400
    type: "math error"
    zero-divide:        {attempt to divide by zero}
    overflow:           {math or number overflow}
    positive:           {positive number required}
]

Access: [
    code: 500
    type: "access error"

    cannot-open:        [{cannot open:} :arg1 {reason:} :arg2]
    not-open:           [{port is not open:} :arg1]
    already-open:       [{port is already open:} :arg1]
;   already-closed:     [{port} :arg1 {already closed}]
    no-connect:         [{cannot connect:} :arg1 {reason:} :arg2]
    not-connected:      [{port is not connected:} :arg1]
;   socket-open:        [{error opening socket:} :arg1]
    no-script:          [{script not found:} :arg1]

    no-scheme-name:     {Scheme has no `name:` field (must be WORD!)}
    no-scheme:          [{missing port scheme:} :arg1]

    invalid-spec:       [{invalid spec or options:} :arg1]
    invalid-port:       [{invalid port object (invalid field values)}]
    invalid-actor:      [{invalid port actor (must be native or object)}]
    invalid-port-arg:   [{invalid port argument:} :arg1]
    no-port-action:     [{this port does not support:} :arg1]
    protocol:           [{protocol error:} :arg1]
    invalid-check:      [{invalid checksum (tampered file):} :arg1]

    write-error:        [{write failed:} :arg1 {reason:} :arg2]
    read-error:         [{read failed:} :arg1 {reason:} :arg2]
    read-only:          [{read-only - write not allowed:} :arg1]
    timeout:            [{port action timed out:} :arg1]

    no-create:          [{cannot create:} :arg1]
    no-delete:          [{cannot delete:} :arg1]
    no-rename:          [{cannot rename:} :arg1]
    bad-file-path:      [{bad file path:} :arg1]
    bad-file-mode:      [{bad file mode:} :arg1]
;   protocol:           [{protocol error} :arg1]

    security:           [{security violation:} :arg1 { (refer to SECURE function)}]
    security-level:     [{attempt to lower security to} :arg1]
    security-error:     [{invalid} :arg1 {security policy:} :arg2]

    no-codec:           [{cannot decode or encode (no codec):} :arg1]
    bad-media:          [{bad media data (corrupt image, sound, video)}]
;   would-block:        [{operation on port} :arg1 {would block}]
;   no-action:          [{this type of port does not support the} :arg1 {action}]
;   serial-timeout:     {serial port timeout}
    no-extension:       [{cannot open extension:} :arg1]
    bad-extension:      [{invalid extension format:} :arg1]
    extension-init:     [{extension cannot be initialized (check version):} :arg1]

    call-fail:          [{external process failed:} :arg1]

    permission-denied:  [{permission denied}]
    process-not-found:  [{process not found:} :arg1]

    symbol-not-found:   [{symbol not found:} :arg1]
    bad-memory:         [{non-accessible memory at} :arg1 {in} :arg2]
    no-external-storage: [{no external storage in the series}]
    already-destroyed:  [{storage at} :arg1 {already destroyed}]
]

Command: [
    code: 600
    type: "command error"
    bad-cmd-args:       ["Bad command arguments"]
    no-cmd:             ["No command"]
;   fmt-too-short:      {Format string is too short}
;   fmt-no-struct-size: [{Missing size spec for struct at arg#} :arg1]
;   fmt-no-struct-align: [{Missing align spec for struct at arg#} :arg1]
;   fmt-bad-word:       [{Bad word in format string at arg#} :arg1]
;   fmt-type-mismatch:  [{Type mismatch in format string at arg#} :arg1]
;   fmt-size-mismatch:  [{Size mismatch in format string at arg#} :arg1]
;   dll-arg-count:      {Number of arguments exceeds 25}
;   empty-command:      {Command is empty}
;   db-not-open:        {Database is not open}
;   db-too-many:        {Too many open databases}
;   cant-free:          [{Cannot free} :arg1]
;   nothing-to-free:    {Nothing to free}
;   ssl-error:          [{SSL Error: } :arg1]
    command-fail:       ["Command failed"]
]

; If new category added, be sure to update RE_MAX in %make-boot.r
; (currently RE_COMMAND_MAX because `Command: [...]` is the last category)

; Note that 1000 is the hardcoded constant in %make-boot.r used for RE_USER
