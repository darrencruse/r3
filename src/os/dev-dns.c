//
//  File: %dev-dns.c
//  Summary: "Device: DNS access"
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
// Calls local DNS services for domain name lookup.
//
// See MS WSAAsyncGetHost* details regarding multiple requests.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reb-host.h"
#include "sys-net.h"

extern DEVICE_CMD Init_Net(REBREQ *); // Share same init
extern DEVICE_CMD Quit_Net(REBREQ *);

extern void Signal_Device(REBREQ *req, REBINT type);

#ifndef WINSYS_WIN32
#undef HAS_ASYNC_DNS
#endif

#ifdef HAS_ASYNC_DNS
// Async DNS requires a window handle to signal completion (WSAASync)
extern HWND Event_Handle;
#endif

//
//  Open_DNS: C
//
DEVICE_CMD Open_DNS(REBREQ *sock)
{
    SET_OPEN(sock);
    return DR_DONE;
}


//
//  Close_DNS: C
//
// Note: valid even if not open.
//
DEVICE_CMD Close_DNS(REBREQ *sock)
{
    // Terminate a pending request:
#ifdef HAS_ASYNC_DNS
    if (GET_FLAG(sock->flags, RRF_PENDING)) {
        CLR_FLAG(sock->flags, RRF_PENDING);
        if (sock->requestee.handle) WSACancelAsyncRequest(sock->requestee.handle);
    }
#endif
    if (sock->special.net.host_info) OS_FREE(sock->special.net.host_info);
    sock->special.net.host_info = 0;
    sock->requestee.handle = 0;
    SET_CLOSED(sock);
    return DR_DONE; // Removes it from device's pending list (if needed)
}


//
//  Read_DNS: C
//
// Initiate the GetHost request and return immediately.
// Note the temporary results buffer (must be freed later).
//
DEVICE_CMD Read_DNS(REBREQ *sock)
{
    char *host;
#ifdef HAS_ASYNC_DNS
    HANDLE handle;
#else
    HOSTENT *he;
#endif

    host = OS_ALLOC_N(char, MAXGETHOSTSTRUCT); // be sure to free it

#ifdef HAS_ASYNC_DNS
    if (!GET_FLAG(sock->modes, RST_REVERSE)) // hostname lookup
        handle = WSAAsyncGetHostByName(Event_Handle, WM_DNS, s_cast(sock->common.data), host, MAXGETHOSTSTRUCT);
    else
        handle = WSAAsyncGetHostByAddr(Event_Handle, WM_DNS, s_cast(&sock->special.net.remote_ip), 4, AF_INET, host, MAXGETHOSTSTRUCT);

    if (handle != 0) {
        sock->special.net.host_info = host;
        sock->requestee.handle = handle;
        return DR_PEND; // keep it on pending list
    }
#else
    // Use old-style blocking DNS (mainly for testing purposes):
    if (GET_FLAG(sock->modes, RST_REVERSE)) {
        he = gethostbyaddr(
            cast(char*, &sock->special.net.remote_ip), 4, AF_INET
        );
        if (he) {
            sock->special.net.host_info = host; //???
            sock->common.data = b_cast(he->h_name);
            SET_FLAG(sock->flags, RRF_DONE);
            return DR_DONE;
        }
    }
    else {
        he = gethostbyname(s_cast(sock->common.data));
        if (he) {
            sock->special.net.host_info = host; // ?? who deallocs?
            memcpy(&sock->special.net.remote_ip, *he->h_addr_list, 4); //he->h_length);
            SET_FLAG(sock->flags, RRF_DONE);
            return DR_DONE;
        }
    }
#endif

    OS_FREE(host);
    sock->special.net.host_info = 0;

    sock->error = GET_ERROR;
    //Signal_Device(sock, EVT_ERROR);
    return DR_ERROR; // Remove it from pending list
}


//
//  Poll_DNS: C
//
// Check for completed DNS requests. These are marked with
// RRF_DONE by the windows message event handler (dev-event.c).
// Completed requests are removed from the pending queue and
// event is signalled (for awake dispatch).
//
DEVICE_CMD Poll_DNS(REBREQ *dr)
{
    REBDEV *dev = (REBDEV*)dr;  // to keep compiler happy
    REBREQ **prior = &dev->pending;
    REBREQ *req;
    REBOOL change = FALSE;
    HOSTENT *host;

    // Scan the pending request list:
    for (req = *prior; req; req = *prior) {

        // If done or error, remove command from list:
        if (GET_FLAG(req->flags, RRF_DONE)) { // req->error may be set
            *prior = req->next;
            req->next = 0;
            CLR_FLAG(req->flags, RRF_PENDING);

            if (!req->error) { // success!
                host = cast(HOSTENT*, req->special.net.host_info);
                if (GET_FLAG(req->modes, RST_REVERSE))
                    req->common.data = b_cast(host->h_name);
                else
                    memcpy(&req->special.net.remote_ip, *host->h_addr_list, 4); //he->h_length);
                Signal_Device(req, EVT_READ);
            }
            else
                Signal_Device(req, EVT_ERROR);
            change = TRUE;
        }
        else prior = &req->next;
    }

    return change ? 1 : 0; // DEVICE_CMD implicitly returns i32
}


/***********************************************************************
**
**  Command Dispatch Table (RDC_ enum order)
**
***********************************************************************/

static DEVICE_CMD_FUNC Dev_Cmds[RDC_MAX] =
{
    Init_Net,   // Shared init - called only once
    Quit_Net,   // Shared
    Open_DNS,
    Close_DNS,
    Read_DNS,
    0,  // write
    Poll_DNS
};

DEFINE_DEV(Dev_DNS, "DNS", 1, Dev_Cmds, RDC_MAX, 0);
