///////////////////////////////////////////////////////////////////////////////
// Name:        motif/evtloop.cpp
// Purpose:     implements wxEventLoop for Motif
// Author:      Mattia Barbon
// Modified by:
// Created:     01.11.02
// RCS-ID:      $Id$
// Copyright:   (c) 2002 Mattia Barbon
// License:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

#ifdef __GNUG__
    #pragma implementation "evtloop.h"
#endif

#ifdef __VMS
#define XtParent XTPARENT
#define XtDisplay XTDISPLAY
#endif

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#endif //WX_PRECOMP

#include "wx/evtloop.h"
#include "wx/event.h"
#include "wx/app.h"
#include "wx/window.h"

#ifdef __VMS__
#pragma message disable nosimpint
#endif
#include <Xm/Xm.h>
#include <X11/Xlib.h>
#ifdef __VMS__
#pragma message enable nosimpint
#endif

#include "wx/motif/private.h"

static bool CheckForKeyUp(XEvent* event);
static bool CheckForKeyDown(XEvent* event);
static bool CheckForAccelerator(XEvent* event);
static void ProcessXEvent(XEvent* event);
static void wxBreakDispatch();

// ----------------------------------------------------------------------------
// wxEventLoopImpl
// ----------------------------------------------------------------------------

class WXDLLEXPORT wxEventLoopImpl
{
public:
    // ctor
    wxEventLoopImpl() { SetExitCode(0); }

    // set/get the exit code
    void SetExitCode(int exitcode) { m_exitcode = exitcode; }
    int GetExitCode() const { return m_exitcode; }

    bool SendIdleMessage();
    bool GetKeepGoing() const { return m_keepGoing; }
    void SetKeepGoing(bool keepGoing) { m_keepGoing = keepGoing; }
private:
    // the exit code of the event loop
    int  m_exitcode;
    bool m_keepGoing;
};

// ----------------------------------------------------------------------------
// wxEventLoopImpl idle event processing
// ----------------------------------------------------------------------------

static bool SendIdleMessage()
{
    wxIdleEvent event;

    return wxTheApp->ProcessEvent(event) && event.MoreRequested();
}

bool wxEventLoopImpl::SendIdleMessage()
{
    return ::SendIdleMessage();
}

// ============================================================================
// wxEventLoop implementation
// ============================================================================

// ----------------------------------------------------------------------------
// wxEventLoop running and exiting
// ----------------------------------------------------------------------------

wxEventLoop *wxEventLoop::ms_activeLoop = NULL;

wxEventLoop::~wxEventLoop()
{
    wxASSERT_MSG( !m_impl, _T("should have been deleted in Run()") );
}

bool wxEventLoop::IsRunning() const
{
    return m_impl != NULL;
}

int wxEventLoop::Run()
{
    // event loops are not recursive, you need to create another loop!
    wxCHECK_MSG( !IsRunning(), -1, _T("can't reenter a message loop") );

    wxEventLoop *oldLoop = ms_activeLoop;
    ms_activeLoop = this;

    m_impl = new wxEventLoopImpl;
    m_impl->SetKeepGoing( true );

    for( ;; )
    {
        if( !wxDoEventLoopIteration( *this ) )
            break;
    }

    int exitcode = m_impl->GetExitCode();
    delete m_impl;
    m_impl = NULL;

    ms_activeLoop = oldLoop;

    return exitcode;
}

void wxEventLoop::Exit(int rc)
{
    wxCHECK_RET( IsRunning(), _T("can't call Exit() if not running") );

    m_impl->SetExitCode(rc);
    m_impl->SetKeepGoing( false );

    ::wxBreakDispatch();
}

// ----------------------------------------------------------------------------
// wxEventLoop message processing dispatching
// ----------------------------------------------------------------------------

bool wxEventLoop::Pending() const
{
    return XtAppPending( (XtAppContext)wxTheApp->GetAppContext() ) != 0;
}

bool wxEventLoop::Dispatch()
{
    XEvent event;
    XtAppContext context = (XtAppContext)wxTheApp->GetAppContext();

    if( XtAppPeekEvent( context, &event ) != 0 )
    {
        XtAppNextEvent( context, &event );
        ProcessXEvent( &event );
    }
    else
#ifdef __VMS
     XtAppProcessEvent( context, XtIMTimer|XtIMAlternateInput );
#else
     XtAppProcessEvent( context, XtIMTimer|XtIMAlternateInput|XtIMSignal );
#endif

    return m_impl ? m_impl->GetKeepGoing() : true;
}

// ----------------------------------------------------------------------------
// Static functions (originally in app.cpp)
// ----------------------------------------------------------------------------

void ProcessXEvent(XEvent* event)
{
    if (event->type == KeyPress)
    {
        if (CheckForAccelerator(event))
        {
            // Do nothing! We intercepted and processed the event as an
            // accelerator.
            return;
        }
        // It seemed before that this hack was redundant and
        // key down events were being generated by wxCanvasInputEvent.
        // But no longer - why ???
        //
        else if (CheckForKeyDown(event))
        {
            // We intercepted and processed the key down event
            return;
        }
        else
        {
            XtDispatchEvent(event);
            return;
        }
    }
    else if (event->type == KeyRelease)
    {
        // TODO: work out why we still need this !  -michael
        //
        if (::CheckForKeyUp(event))
        {
            // We intercepted and processed the key up event
            return;
        }
        else
        {
            XtDispatchEvent(event);
            return;
        }
    }
    else if (event->type == PropertyNotify)
    {
        wxTheApp->HandlePropertyChange(event);
        return;
    }
    else if (event->type == ResizeRequest)
    {
        /* Terry Gitnick <terryg@scientech.com> - 1/21/98
         * If resize event, don't resize until the last resize event for this
         * window is recieved. Prevents flicker as windows are resized.
         */

        Display *disp = event->xany.display;
        Window win = event->xany.window;
        XEvent report;

        //  to avoid flicker
        report = * event;
        while( XCheckTypedWindowEvent (disp, win, ResizeRequest, &report));

        // TODO: when implementing refresh optimization, we can use
        // XtAddExposureToRegion to expand the window's paint region.

        XtDispatchEvent(event);
    }
    else
    {
        XtDispatchEvent(event);
    }
}

// Returns true if an accelerator has been processed
bool CheckForAccelerator(XEvent* event)
{
    if (event->xany.type == KeyPress)
    {
        // Find a wxWindow for this window
        // TODO: should get display for the window, not the current display
        Widget widget = XtWindowToWidget(event->xany.display,
                                         event->xany.window);
        wxWindow* win = NULL;

        // Find the first wxWindow that corresponds to this event window
        while (widget && !(win = wxGetWindowFromTable(widget)))
            widget = XtParent(widget);

        if (!widget || !win)
            return false;

        wxKeyEvent keyEvent(wxEVT_CHAR);
        wxTranslateKeyEvent(keyEvent, win, (Widget) 0, event);

        // Now we have a wxKeyEvent and we have a wxWindow.
        // Go up the hierarchy until we find a matching accelerator,
        // or we get to the top.
        while (win)
        {
            if (win->ProcessAccelerator(keyEvent))
                return true;
            win = win->GetParent();
        }
        return false;
    }
    return false;
}

// bool wxApp::CheckForKeyDown(WXEvent* event) { wxFAIL; return false; }

bool CheckForKeyDown(XEvent* event)
{
    if (event->xany.type == KeyPress)
    {
        Widget widget = XtWindowToWidget(event->xany.display,
                                         event->xany.window);
        wxWindow* win = NULL;

        // Find the first wxWindow that corresponds to this event window
        while (widget && !(win = wxGetWindowFromTable(widget)))
            widget = XtParent(widget);

        if (!widget || !win)
            return false;

        wxKeyEvent keyEvent(wxEVT_KEY_DOWN);
        wxTranslateKeyEvent(keyEvent, win, (Widget) 0, event);

        return win->GetEventHandler()->ProcessEvent( keyEvent );
    }

    return false;
}

// bool wxApp::CheckForKeyUp(WXEvent* event) { wxFAIL; return false; }

bool CheckForKeyUp(XEvent* event)
{
    if (event->xany.type == KeyRelease)
    {
        Widget widget = XtWindowToWidget(event->xany.display,
                                         event->xany.window);
        wxWindow* win = NULL;

        // Find the first wxWindow that corresponds to this event window
        while (widget && !(win = wxGetWindowFromTable(widget)))
                widget = XtParent(widget);

        if (!widget || !win)
            return false;

        wxKeyEvent keyEvent(wxEVT_KEY_UP);
        wxTranslateKeyEvent(keyEvent, win, (Widget) 0, event);

        return win->GetEventHandler()->ProcessEvent( keyEvent );
    }

    return false;
}

// ----------------------------------------------------------------------------
// executes one main loop iteration (declared in include/wx/motif/private.h)
// ----------------------------------------------------------------------------

bool wxDoEventLoopIteration( wxEventLoop& evtLoop )
{
    bool moreRequested, pendingEvents;

    for(;;)
    {
        pendingEvents = evtLoop.Pending();
        if( pendingEvents ) break;
        moreRequested = ::SendIdleMessage();
        if( !moreRequested ) break;
    }

#if wxUSE_THREADS
    if( !pendingEvents && !moreRequested )
    {
        // leave the main loop to give other threads a chance to
        // perform their GUI work
        wxMutexGuiLeave();
        wxUsleep(20);
        wxMutexGuiEnter();
    }
#endif

    if( !evtLoop.Dispatch() )
        return false;

    return true;
}

// ----------------------------------------------------------------------------
// ::wxWakeUpIdle implementation
// ----------------------------------------------------------------------------

// As per Vadim's suggestion, we open a pipe, and XtAppAddInputSource it;
// writing a single byte to the pipe will cause wxEventLoop::Pending
// to return true, and wxEventLoop::Dispatch to dispatch an input handler
// that simply removes the byte(s), and to return, which will cause
// an idle event to be sent

// also wxEventLoop::Exit is implemented that way, so that exiting an
// event loop won't require an event being in the queue

#include "wx/module.h"

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

static XtInputId inputId;
static int idleFds[2] = { -1, -1 };

class wxIdlePipeModule : public wxModule
{
public:
    wxIdlePipeModule() {};

    virtual bool OnInit()
    {
        // Must be done before modules are initialized
#if 0
        if( pipe(idleFds) != 0 )
            return false;
#endif
        return true;
    }

    virtual void OnExit()
    {
        close( idleFds[0] );
        close( idleFds[1] );
    }
private:
    DECLARE_DYNAMIC_CLASS(wxIdlePipeModule);
};

IMPLEMENT_DYNAMIC_CLASS(wxIdlePipeModule, wxModule);

static void wxInputCallback( XtPointer, int* fd, XtInputId* )
{
    char buffer[128];

    // wxWakeUpIdle may have been called more than once
    for(;;)
    {
        fd_set in;
        struct timeval timeout;

        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        FD_ZERO( &in );
        FD_SET( *fd, &in );

        if( select( *fd + 1, &in, NULL, NULL, &timeout ) <= 0 )
            break;
        if( read( *fd, buffer, sizeof(buffer) - 1 ) != sizeof(buffer) - 1 )
            break;
    }
}

static void wxBreakDispatch()
{
    char dummy = 0; // for valgrind

    // check if wxWakeUpIdle has already been called
    fd_set in;
    struct timeval timeout;

    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    FD_ZERO( &in );
    FD_SET( idleFds[0], &in );

    if( select( idleFds[0] + 1, &in, NULL, NULL, &timeout ) > 0 )
        return;

    // write a single byte
    write( idleFds[1], &dummy, 1 );
}

void wxApp::WakeUpIdle()
{
    ::wxBreakDispatch();
}

bool wxInitIdleFds()
{
    if( pipe(idleFds) != 0 )
        return false;
    return true;
}

bool wxAddIdleCallback()
{
    if (!wxInitIdleFds())
        return false;
    
    // install input handler for wxWakeUpIdle
    inputId = XtAppAddInput( (XtAppContext) wxTheApp->GetAppContext(),
                             idleFds[0],
                             (XtPointer)XtInputReadMask,
                             wxInputCallback,
                             NULL );

    return true;
}

