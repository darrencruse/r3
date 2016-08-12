REBOL [
    System: "REBOL [R3] Language Interpreter and Run-time Environment"
    Title: "REBOL 3 Boot Sys: Encoder and Decoder"
    Rights: {
        Copyright 2012 REBOL Technologies
        REBOL is a trademark of REBOL Technologies
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Context: sys
    Note: {
        The boot binding of this module is SYS then LIB deep.
        Any non-local words not found in those contexts WILL BE
        UNBOUND and will error out at runtime!
    }
]

;-- Setup Codecs -------------------------------------------------------------

for-each [codec handler] system/codecs [
    if handle? handler [
        ; Change boot handle into object:
        codec: set codec construct [] [
            entry: handler
            title: form reduce ["Internal codec for" codec "media type"]
            name: codec
            type: 'image!
            suffixes: select [
                text [%.txt]
                utf-16le [%.txt]
                utf-16be [%.txt]
                markup [%.html %.htm %.xml %.xsl %.wml %.sgml %.asp %.php %.cgi]
                bmp  [%.bmp]
                gif  [%.gif]
                jpeg [%.jpg %.jpeg]
                png  [%.png]
            ] codec
        ]
        ; Media-types block format: [.abc .def type ...]
        append append system/options/file-types codec/suffixes codec/name
    ]
]

; Special import case for extensions:
append system/options/file-types switch/default fourth system/version [
    3 [[%.rx %.dll extension]]  ; Windows
    2 [[%.rx %.dylib %.so extension]]  ; OS X
    4 7 [[%.rx %.so extension]]  ; Other Posix
] [[%.rx extension]]


decode: function [
    {Decodes a series of bytes into the related datatype (e.g. image!).}
    type [word!] {Media type (jpeg, png, etc.)}
    data [binary!] {The data to decode}
][
    unless all [
        cod: select system/codecs type
        data: do-codec cod/entry 'decode data
    ][
        cause-error 'access 'no-codec type
    ]
    data
]

encode: function [
    {Encodes a datatype (e.g. image!) into a series of bytes.}
    type [word!] {Media type (jpeg, png, etc.)}
    data [image! binary! string!] {The data to encode}
    /options opts [block!] {Special encoding options}
][
    unless all [
        cod: select system/codecs type
        ;encode patch replacing internal PNG encoder crash for now
        data: switch/default cod/name [
            png [
                lib/to-png data
            ]
        ][
            do-codec cod/entry 'encode data
        ]
    ][
        cause-error 'access 'no-codec type
    ]
    data
]

encoding?: function [
    ; !!! Functions ending in ? should only return TRUE or FALSE
    "Returns the media codec name for given binary data. (identify)"
    data [binary!]
][
    for-each [name codec] system/codecs [
        if do-codec codec/entry 'identify data [
            return name
        ]
    ]
    blank
]

export [decode encode encoding?]
