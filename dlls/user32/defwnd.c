/*
 * Default window procedure
 *
 * Copyright 1993, 1996 Alexandre Julliard
 *	     1995 Alex Korobka
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <string.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winnls.h"
#include "imm.h"
#include "win.h"
#include "user_private.h"
#include "controls.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(win);


/***********************************************************************
 *              DefWindowProcA (USER32.@)
 *
 * See DefWindowProcW.
 */
LRESULT WINAPI DefWindowProcA( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    LRESULT result = 0;
    HWND full_handle;

    if (!(full_handle = WIN_IsCurrentProcess( hwnd )))
    {
        if (!IsWindow( hwnd )) return 0;
        ERR( "called for other process window %p\n", hwnd );
        return 0;
    }
    hwnd = full_handle;

    SPY_EnterMessage( SPY_DEFWNDPROC, hwnd, msg, wParam, lParam );

    switch(msg)
    {
    case WM_SYSCOMMAND:
        result = NC_HandleSysCommand( hwnd, wParam, lParam );
        break;

    case WM_IME_CHAR:
        if (HIBYTE(wParam)) PostMessageA( hwnd, WM_CHAR, HIBYTE(wParam), lParam );
        PostMessageA( hwnd, WM_CHAR, LOBYTE(wParam), lParam );
        break;

    case WM_IME_KEYDOWN:
        result = PostMessageA( hwnd, WM_KEYDOWN, wParam, lParam );
        break;

    case WM_IME_KEYUP:
        result = PostMessageA( hwnd, WM_KEYUP, wParam, lParam );
        break;

    case WM_IME_COMPOSITION:
        if (lParam & GCS_RESULTSTR)
        {
            LONG size, i;
            unsigned char lead = 0;
            char *buf = NULL;
            HIMC himc = ImmGetContext( hwnd );

            if (himc)
            {
                if ((size = ImmGetCompositionStringA( himc, GCS_RESULTSTR, NULL, 0 )))
                {
                    if (!(buf = HeapAlloc( GetProcessHeap(), 0, size ))) size = 0;
                    else size = ImmGetCompositionStringA( himc, GCS_RESULTSTR, buf, size );
                }
                ImmReleaseContext( hwnd, himc );

                for (i = 0; i < size; i++)
                {
                    unsigned char c = buf[i];
                    if (!lead)
                    {
                        if (IsDBCSLeadByte( c ))
                            lead = c;
                        else
                            SendMessageA( hwnd, WM_IME_CHAR, c, 1 );
                    }
                    else
                    {
                        SendMessageA( hwnd, WM_IME_CHAR, MAKEWORD(c, lead), 1 );
                        lead = 0;
                    }
                }
                HeapFree( GetProcessHeap(), 0, buf );
            }
        }
        /* fall through */

    default:
        result = NtUserMessageCall( hwnd, msg, wParam, lParam, 0, NtUserDefWindowProc, TRUE );
        break;
    }

    SPY_ExitMessage( SPY_RESULT_DEFWND, hwnd, msg, result, wParam, lParam );
    return result;
}


/***********************************************************************
 *              DefWindowProcW (USER32.@) Calls default window message handler
 *
 * Calls default window procedure for messages not processed
 *  by application.
 *
 *  RETURNS
 *     Return value is dependent upon the message.
*/
LRESULT WINAPI DefWindowProcW(
    HWND hwnd,      /* [in] window procedure receiving message */
    UINT msg,       /* [in] message identifier */
    WPARAM wParam,  /* [in] first message parameter */
    LPARAM lParam )   /* [in] second message parameter */
{
    LRESULT result = 0;
    HWND full_handle;

    if (!(full_handle = WIN_IsCurrentProcess( hwnd )))
    {
        if (!IsWindow( hwnd )) return 0;
        ERR( "called for other process window %p\n", hwnd );
        return 0;
    }
    hwnd = full_handle;
    SPY_EnterMessage( SPY_DEFWNDPROC, hwnd, msg, wParam, lParam );

    switch(msg)
    {
    case WM_SYSCOMMAND:
        result = NC_HandleSysCommand( hwnd, wParam, lParam );
        break;

    case WM_IME_CHAR:
        PostMessageW( hwnd, WM_CHAR, wParam, lParam );
        break;

    case WM_IME_KEYDOWN:
        result = PostMessageW( hwnd, WM_KEYDOWN, wParam, lParam );
        break;

    case WM_IME_KEYUP:
        result = PostMessageW( hwnd, WM_KEYUP, wParam, lParam );
        break;

    case WM_IME_COMPOSITION:
        if (lParam & GCS_RESULTSTR)
        {
            LONG size, i;
            WCHAR *buf = NULL;
            HIMC himc = ImmGetContext( hwnd );

            if (himc)
            {
                if ((size = ImmGetCompositionStringW( himc, GCS_RESULTSTR, NULL, 0 )))
                {
                    if (!(buf = HeapAlloc( GetProcessHeap(), 0, size * sizeof(WCHAR) ))) size = 0;
                    else size = ImmGetCompositionStringW( himc, GCS_RESULTSTR, buf, size * sizeof(WCHAR) );
                }
                ImmReleaseContext( hwnd, himc );

                for (i = 0; i < size / sizeof(WCHAR); i++)
                    SendMessageW( hwnd, WM_IME_CHAR, buf[i], 1 );
                HeapFree( GetProcessHeap(), 0, buf );
            }
        }
        /* fall through */

    default:
        result = NtUserMessageCall( hwnd, msg, wParam, lParam, 0, NtUserDefWindowProc, FALSE );
        break;
    }
    SPY_ExitMessage( SPY_RESULT_DEFWND, hwnd, msg, result, wParam, lParam );
    return result;
}
