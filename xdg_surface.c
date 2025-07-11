/* Wayland compositor running on top of an X server.

Copyright (C) 2022 to various contributors.

This file is part of 12to11.

12to11 is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

12to11 is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with 12to11.  If not, see <https://www.gnu.org/licenses/>.  */

#include <string.h>

#include <stdlib.h>
#include <stdio.h>

#include "compositor.h"
#include "xdg-shell.h"

#include <X11/extensions/shape.h>

#define XdgRoleFromRole(role) ((XdgRole *) (role))

/* This is the default core event mask used by our windows.  */
#define DefaultEventMask					\
  (ExposureMask | StructureNotifyMask | PropertyChangeMask)

enum
  {
    StatePendingFrameCallback	= 1,
    StatePendingWindowGeometry	= (1 << 2),
    StateWaitingForAckConfigure = (1 << 3),
    StateWaitingForAckCommit	= (1 << 4),
    StateMaybeConfigure		= (1 << 5),
    StateDirtyFrameExtents	= (1 << 6),
    StateTemporaryBounds	= (1 << 7),
    StatePendingBufferRelease   = (1 << 8),
  };

typedef struct _XdgRole XdgRole;
typedef struct _XdgState XdgState;
typedef struct _ReleaseLaterRecord ReleaseLaterRecord;
typedef struct _ReconstrainCallback ReconstrainCallback;
typedef struct _PingEvent PingEvent;

/* Association between XIDs and surfaces.  */

static XLAssocTable *surfaces;

/* The default border color of a window.  Not actually used for
   anything other than preventing BadMatch errors during window
   creation.  */

unsigned long border_pixel;

struct _ReconstrainCallback
{
  /* Function called when a configure event is received.  */
  void (*configure) (void *, XEvent *);

  /* Function called when we are certain a frame moved or resized.  */
  void (*resized) (void *);

  /* Data the functions is called with.  */
  void *data;

  /* The next and last callbacks in this list.  */
  ReconstrainCallback *next, *last;
};

struct _XdgState
{
  int window_geometry_x;
  int window_geometry_y;
  int window_geometry_width;
  int window_geometry_height;
};

struct _XdgRole
{
  /* The role object.  */
  Role role;

  /* The link to the wm_base's list of surfaces.  */
  XdgRoleList link;

  /* The attached XdgWmBase.  Not valid if link->next is NULL.  */
  XdgWmBase *wm_base;

  /* The window backing this role.  */
  Window window;

  /* The render target backing this role.  */
  RenderTarget target;

  /* The subcompositor backing this role.  */
  Subcompositor *subcompositor;

  /* The implementation of this role.  */
  XdgRoleImplementation *impl;

  /* The pending frame ID.  */
  uint64_t pending_frame;

  /* List of pending ping events.  */
  XLList *ping_events;

  /* Number of references to this role.  Used when the client
     terminates and the Wayland library destroys objects out of
     order.  */
  int refcount;

  /* Various role state.  */
  int state;

  /* Buffer release helper.  */
  BufferReleaseHelper *release_helper;

  /* The synchronization helper.  */
  SyncHelper *sync_helper;

  /* The pending xdg_surface state.  */
  XdgState pending_state;

  /* The current xdg_surface state.  */
  XdgState current_state;

  /* List of callbacks run upon a ConfigureNotify event.  */
  ReconstrainCallback reconstrain_callbacks;

  /* Configure event serial.  */
  uint32_t conf_serial, last_specified_serial;

  /* The current bounds of the subcompositor.  */
  int min_x, min_y, max_x, max_y;

  /* The bounds width and bounds height of the subcompositor.  */
  int bounds_width, bounds_height;

  /* The pending root window position of the subcompositor.  */
  int pending_root_x, pending_root_y;

  /* How many synthetic (in the case of toplevels) ConfigureNotify
     events to wait for before ignoring those coordinates.  */
  int pending_synth_configure;

  /* The pending frame time.  */
  uint32_t pending_frame_time;

  /* The input region of the attached subsurface.  */
  pixman_region32_t input_region;

  /* The type of the attached role.  */
  XdgRoleImplementationType type;
};

struct _PingEvent
{
  /* Function called to reply to this event.  */
  void (*reply_func) (XEvent *);

  /* The event.  */
  XEvent event;
};

/* Event base of the XShape extension.  */
int shape_base;

static ReconstrainCallback *
AddCallbackAfter (ReconstrainCallback *start)
{
  ReconstrainCallback *callback;

  callback = XLMalloc (sizeof *callback);
  callback->next = start->next;
  callback->last = start;

  start->next->last = callback;
  start->next = callback;

  return callback;
}

static void
UnlinkReconstrainCallback (ReconstrainCallback *callback)
{
  callback->last->next = callback->next;
  callback->next->last = callback->last;

  XLFree (callback);
}

static void
RunReconstrainCallbacksForXEvent (XdgRole *role, XEvent *event)
{
  ReconstrainCallback *callback;

  callback = role->reconstrain_callbacks.next;

  while (callback != &role->reconstrain_callbacks)
    {
      callback->configure (callback->data, event);
      callback = callback->next;
    }
}

static void
RunReconstrainCallbacks (XdgRole *role)
{
  ReconstrainCallback *callback;

  callback = role->reconstrain_callbacks.next;

  while (callback != &role->reconstrain_callbacks)
    {
      callback->resized (callback->data);
      callback = callback->next;
    }
}

static void
FreeReconstrainCallbacks (XdgRole *role)
{
  ReconstrainCallback *callback, *last;

  callback = role->reconstrain_callbacks.next;

  while (callback != &role->reconstrain_callbacks)
    {
      last = callback;
      callback = callback->next;

      XLFree (last);
    }
}

static void
RunFrameCallbacks (Surface *surface, XdgRole *role, uint32_t frame_time)
{
  /* Surface can be NULL for various reasons, especially events
     arriving after the shell surface is detached.  */

  if (!surface)
    return;

  XLSurfaceRunFrameCallbacksMs (surface, frame_time);
}

static void
RunFrameCallbacksConditionally (XdgRole *role, uint32_t frame_time)
{
  if (!(role->state & StatePendingBufferRelease))
    RunFrameCallbacks (role->role.surface, role, frame_time);
  else if (role->role.surface)
    {
      /* weston-simple-shm seems to assume that a frame callback can
	 only arrive after all buffers have been released.  */
      role->state |= StatePendingFrameCallback;
      role->pending_frame_time = frame_time;
    }
}

static void
AllBuffersReleased (void *data)
{
  XdgRole *role;
  Surface *surface;

  role = data;
  surface = role->role.surface;

  /* Clear the buffer release flag.  */
  role->state &= ~StatePendingBufferRelease;

  /* Run frame callbacks now, as no more buffers are waiting to be
     released.  */
  if (surface && role->state & StatePendingFrameCallback)
    {
      RunFrameCallbacks (surface, role,
			 role->pending_frame_time);

      role->state &= ~StatePendingFrameCallback;
    }
}

Bool
XLHandleXEventForXdgSurfaces (XEvent *event)
{
  XdgRole *role;
  Window window;

  if (event->type == ClientMessage
      && ((event->xclient.message_type == _NET_WM_FRAME_DRAWN
	   || event->xclient.message_type == _NET_WM_FRAME_TIMINGS)
	  || (event->xclient.message_type == WM_PROTOCOLS
	      && event->xclient.data.l[0] == _NET_WM_SYNC_REQUEST)))
    {
      role = XLLookUpAssoc (surfaces, event->xclient.window);

      if (role)
	{
	  SyncHelperHandleFrameEvent (role->sync_helper, event);
	  return True;
	}

      return False;
    }

  if (event->type == Expose)
    {
      role = XLLookUpAssoc (surfaces, event->xexpose.window);

      if (role)
	{
	  SubcompositorExpose (role->subcompositor, event);

	  return True;
	}

      return False;
    }

  if (event->type == KeyPress || event->type == KeyRelease)
    {
      /* These events are actually sent by the input method library
	 upon receiving XIM_COMMIT messages.  */

      role = XLLookUpAssoc (surfaces, event->xkey.window);

      if (role && role->role.surface)
	{
	  XLTextInputDispatchCoreEvent (role->role.surface, event);
	  return True;
	}

      return False;
    }

  window = XLGetGEWindowForSeats (event);

  if (window != None)
    {
      role = XLLookUpAssoc (surfaces, window);

      if (role && role->role.surface)
	{
	  XLDispatchGEForSeats (event, role->role.surface,
				role->subcompositor);
	  return True;
	}

      return False;
    }

  return False;
}

static void
Destroy (struct wl_client *client, struct wl_resource *resource)
{
  XdgRole *role;

  role = wl_resource_get_user_data (resource);

  if (role->impl)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
			      "trying to destroy xdg surface with role");
      return;
    }

  /* Now detach the role from its surface, which can be reused in the
     future.  */
  if (role->role.surface)
    XLSurfaceReleaseRole (role->role.surface, &role->role);

  wl_resource_destroy (resource);
}

static void
GetToplevel (struct wl_client *client, struct wl_resource *resource,
	     uint32_t id)
{
  XdgRole *role;

  role = wl_resource_get_user_data (resource);

  if (!role->role.surface)
    /* This object is inert.  */
    return;

  if (role->type == TypePopup)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
			      "surface was previously a popup");
      return;
    }

  role->type = TypeToplevel;

  XLGetXdgToplevel (client, resource, id);
}

static void
GetPopup (struct wl_client *client, struct wl_resource *resource,
	  uint32_t id, struct wl_resource *parent_resource,
	  struct wl_resource *positioner_resource)
{
  XdgRole *role;

  role = wl_resource_get_user_data (resource);

  if (!role->role.surface)
    /* This object is inert.  */
    return;

  if (role->type == TypeToplevel)
    {
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
			      "surface was previously a toplevel");
      return;
    }

  role->type = TypePopup;

  XLGetXdgPopup (client, resource, id, parent_resource,
		 positioner_resource);
}

static void
SetWindowGeometry (struct wl_client *client, struct wl_resource *resource,
		   int32_t x, int32_t y, int32_t width, int32_t height)
{
  XdgRole *role;

  role = wl_resource_get_user_data (resource);

  if (x == role->current_state.window_geometry_x
      && y == role->pending_state.window_geometry_y
      && width == role->pending_state.window_geometry_width
      && height == role->pending_state.window_geometry_height)
    return;

  role->state |= StatePendingWindowGeometry;

  role->pending_state.window_geometry_x = x;
  role->pending_state.window_geometry_y = y;
  role->pending_state.window_geometry_width = width;
  role->pending_state.window_geometry_height = height;

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr, "Client requested geometry: [%d %d %d %d]\n",
	   role->pending_state.window_geometry_x,
	   role->pending_state.window_geometry_y,
	   role->pending_state.window_geometry_width,
	   role->pending_state.window_geometry_height);
#endif
}

static void
AckConfigure (struct wl_client *client, struct wl_resource *resource,
	      uint32_t serial)
{
  XdgRole *xdg_role;

  xdg_role = wl_resource_get_user_data (resource);

  if (!xdg_role->role.surface)
    return;

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr, "ack_configure: %"PRIu32"\n", serial);
#endif

  if (serial && serial <= xdg_role->last_specified_serial)
    {
      /* The client specified the same serial twice.  */
      wl_resource_post_error (resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
			      "same serial specified twice");
      return;
    }

  if (serial == xdg_role->conf_serial)
    {
      xdg_role->last_specified_serial = serial;
      xdg_role->state &= ~StateWaitingForAckConfigure;

      /* Garbage the subcompositor too, since contents could be
	 exposed due to changes in bounds.  */
      SubcompositorGarbage (xdg_role->subcompositor);

#ifdef DEBUG_GEOMETRY_CALCULATION
      fprintf (stderr, "Client acknowledged configuration\n");
#endif
    }

  if (xdg_role->impl)
    xdg_role->impl->funcs.ack_configure (&xdg_role->role,
					 xdg_role->impl,
					 serial);
}

static const struct xdg_surface_interface xdg_surface_impl =
  {
    .get_toplevel = GetToplevel,
    .get_popup = GetPopup,
    .destroy = Destroy,
    .set_window_geometry = SetWindowGeometry,
    .ack_configure = AckConfigure,
  };

static void
Commit (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (!xdg_role->impl)
    return;

  if (xdg_role->state & StatePendingWindowGeometry)
    {
      xdg_role->current_state.window_geometry_x
	= xdg_role->pending_state.window_geometry_x;
      xdg_role->current_state.window_geometry_y
	= xdg_role->pending_state.window_geometry_y;
      xdg_role->current_state.window_geometry_width
	= xdg_role->pending_state.window_geometry_width;
      xdg_role->current_state.window_geometry_height
	= xdg_role->pending_state.window_geometry_height;

#ifdef DEBUG_GEOMETRY_CALCULATION
      fprintf (stderr, "Client set window geometry to: [%d %d %d %d]\n"
	       "State is: %d\n",
	       xdg_role->current_state.window_geometry_x,
	       xdg_role->current_state.window_geometry_y,
	       xdg_role->current_state.window_geometry_width,
	       xdg_role->current_state.window_geometry_height,
	       xdg_role->state & StateWaitingForAckConfigure);
#endif

      /* Now, clear the "pending window geometry" flag.  */
      xdg_role->state &= ~StatePendingWindowGeometry;

      /* Next, set the "dirty frame extents" flag; this is then used
	 to update the window geometry the next time the window is
	 resized.  */
      xdg_role->state |= StateDirtyFrameExtents;
    }

  xdg_role->impl->funcs.commit (role, surface,
				xdg_role->impl);

  /* This flag means no commit has happened after an
     ack_configure.  */
  if (!(xdg_role->state & StateWaitingForAckConfigure)
      && xdg_role->state & StateWaitingForAckCommit)
    {
#ifdef DEBUG_GEOMETRY_CALCULATION
      fprintf (stderr, "Client aknowledged commit\n");
#endif
      xdg_role->state &= ~StateWaitingForAckCommit;
    }

  if (!(xdg_role->state & StateWaitingForAckCommit))
    {
      /* Tell the sync helper to update the frame.  This will also
	 complete any resize if necessary.  */
      SyncHelperUpdate (xdg_role->sync_helper);

      /* Run the after_commit function of the role implementation,
	 which peforms actions such as posting pending configure
	 events for built-in resize.  */

      if (xdg_role->impl->funcs.after_commit)
	xdg_role->impl->funcs.after_commit (role, surface,
					    xdg_role->impl);
    }
  else
    /* Now, tell the sync helper to generate a frame.
       Many clients do this:

       wl_surface@1.frame (new id wl_callback@2)
       wl_surface@1.commit ()

       and upon receiving a configure event, potentially call:

       xdg_surface@3.ack_configure (1)

       but do not commit (or even ack_configure) until the frame
       callback is triggered.

       That is problematic because the frame clock is not unfrozen
       until the commit happens.  To work around the problem, tell the
       sync helper to check for this situation, and run frame
       callbacks if necessary.  */
    SyncHelperCheckFrameCallback (xdg_role->sync_helper);

  return;
}

static Bool
Setup (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  /* Set role->surface here, since this is where the refcounting is
     done as well.  */
  role->surface = surface;

  /* Prevent the surface from ever holding another kind of role.  */
  surface->role_type = XdgType;

  xdg_role = XdgRoleFromRole (role);
  ViewSetSubcompositor (surface->view,
		        xdg_role->subcompositor);
  ViewSetSubcompositor (surface->under,
		        xdg_role->subcompositor);

  /* Make sure the under view ends up beneath surface->view.  */
  SubcompositorInsert (xdg_role->subcompositor,
		       surface->under);
  SubcompositorInsert (xdg_role->subcompositor,
		       surface->view);

  /* Retain the backing data.  */
  xdg_role->refcount++;

  return True;
}

static void
ReleaseBacking (XdgRole *role)
{
  if (--role->refcount)
    return;

  /* Unlink the role if it is still linked.  */

  if (role->link.next)
    {
      role->link.next->last = role->link.last;
      role->link.last->next = role->link.next;
    }

  /* Release all buffers pending release.  The sync is necessary
     because the X server does not perform operations immediately
     after the Xlib function is called.  */
  FreeBufferReleaseHelper (role->release_helper);

  /* Now release the reference to any toplevel implementation that
     might be attached.  */
  if (role->impl)
    XLXdgRoleDetachImplementation (&role->role, role->impl);

  /* Release all allocated resources.  */
  RenderDestroyRenderTarget (role->target);
  XDestroyWindow (compositor.display, role->window);

  /* Free associated ping events.  */
  XLListFree (role->ping_events, XLFree);

  /* And the association.  */
  XLDeleteAssoc (surfaces, role->window);

  /* Destroy the sync helper.  */
  FreeSyncHelper (role->sync_helper);

  /* There shouldn't be any children of the subcompositor at this
     point.  */
  SubcompositorFree (role->subcompositor);

  /* Free the input region.  */
  pixman_region32_fini (&role->input_region);

  /* Free reconstrain callbacks.  */
  FreeReconstrainCallbacks (role);

  /* And since there are no C level references to the role anymore, it
     can be freed.  */
  XLFree (role);
}

static void
Teardown (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  /* Clear role->surface here, since this is where the refcounting is
     done as well.  */
  role->surface = NULL;

  xdg_role = XdgRoleFromRole (role);

  /* Unparent the surface's views as well.  */
  ViewUnparent (surface->view);
  ViewUnparent (surface->under);

  /* Detach the surface's views from the subcompositor.  */
  ViewSetSubcompositor (surface->view, NULL);
  ViewSetSubcompositor (surface->under, NULL);

  /* Release the backing data.  */
  ReleaseBacking (xdg_role);
}

static void
ReleaseBuffer (Surface *surface, Role *role, ExtBuffer *buffer)
{
  RenderBuffer render_buffer;
  XdgRole *xdg_role;

  render_buffer = XLRenderBufferFromBuffer (buffer);
  xdg_role = XdgRoleFromRole (role);

  if (RenderIsBufferIdle (render_buffer, xdg_role->target))
    /* If the buffer is already idle, release it now.  */
    XLReleaseBuffer (buffer);
  else
    {
      /* Release the buffer once it is destroyed or becomes idle.  */
      ReleaseBufferWithHelper (xdg_role->release_helper,
			       buffer, xdg_role->target);
      xdg_role->state |= StatePendingBufferRelease;
    }
}

static void
SubsurfaceUpdate (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (xdg_role->state & StateWaitingForAckCommit)
    {
      /* Updates are being postponed until the next commit after
	 ack_configure. 

	 Now, tell the sync helper to generate a frame.
	 Many clients do this:

	 wl_surface@1.frame (new id wl_callback@2)
	 wl_surface@1.commit ()

	 and upon receiving a configure event, potentially call:

	 xdg_surface@3.ack_configure (1)

	 but do not commit (or even ack_configure) until the frame
	 callback is triggered.

	 That is problematic because the frame clock is not unfrozen
	 until the commit happens.  To work around the problem, tell
	 the sync helper to check for this situation, and run frame
	 callbacks if necessary.  */
      SyncHelperCheckFrameCallback (xdg_role->sync_helper);
      return;
    }

  /* Tell the sync helper to do an update.  */
  SyncHelperUpdate (xdg_role->sync_helper);
}

static Window
GetWindow (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  return xdg_role->window;
}

static void
HandleResourceDestroy (struct wl_resource *resource)
{
  XdgRole *role;

  role = wl_resource_get_user_data (resource);
  role->role.resource = NULL;

  /* Release the backing data.  */
  ReleaseBacking (role);
}

static void
OpaqueRegionChanged (Subcompositor *subcompositor,
		     void *client_data,
		     pixman_region32_t *opaque_region)
{
  XdgRole *role;
  long *data;
  int nrects, i;
  pixman_box32_t *boxes;

  boxes = pixman_region32_rectangles (opaque_region, &nrects);
  role = client_data;

  if (nrects < 64)
    data = alloca (4 * sizeof *data * nrects);
  else
    data = XLMalloc (4 * sizeof *data * nrects);

  for (i = 0; i < nrects; ++i)
    {
      data[i * 4 + 0] = BoxStartX (boxes[i]);
      data[i * 4 + 1] = BoxStartY (boxes[i]);
      data[i * 4 + 2] = BoxWidth (boxes[i]);
      data[i * 4 + 3] = BoxHeight (boxes[i]);
    }

  XChangeProperty (compositor.display, role->window,
		   _NET_WM_OPAQUE_REGION, XA_CARDINAL,
		   32, PropModeReplace,
		   (unsigned char *) data, nrects * 4);

  if (nrects >= 64)
    XLFree (data);
}

static void
InputRegionChanged (Subcompositor *subcompositor,
		    void *data,
		    pixman_region32_t *input_region)
{
  XdgRole *role;
  int nrects, i;
  pixman_box32_t *boxes;
  XRectangle *rects;

  role = data;
  boxes = pixman_region32_rectangles (input_region, &nrects);

  /* If the number of rectangles is small (<= 256), allocate them on
     the stack.  Otherwise, use the heap instead.  */

  if (nrects < 256)
    rects = alloca (sizeof *rects * nrects);
  else
    rects = XLMalloc (sizeof *rects * nrects);

  /* Convert the boxes into proper XRectangles and make them the input
     region of the window.  */

  for (i = 0; i < nrects; ++i)
    {
      rects[i].x = BoxStartX (boxes[i]);
      rects[i].y = BoxStartY (boxes[i]);
      rects[i].width = BoxWidth (boxes[i]);
      rects[i].height = BoxHeight (boxes[i]);
    }

  XShapeCombineRectangles (compositor.display,
			   role->window, ShapeInput,
			   0, 0, rects, nrects,
			   /* pixman uses the same region
			      representation as the X server, which is
			      YXBanded.  */
			   ShapeSet, YXBanded);

  if (nrects >= 256)
    XLFree (rects);

  /* Also save the input region for future use.  */
  pixman_region32_copy (&role->input_region, input_region);
}

static void
NoteConfigure (XdgRole *role, XEvent *event)
{
  if (role->pending_synth_configure)
    role->pending_synth_configure--;

  if (role->role.surface)
    {
      /* Update the list of outputs that the surface is inside.  */
      XLUpdateSurfaceOutputs (role->role.surface,
			      event->xconfigure.x + role->min_x,
			      event->xconfigure.y + role->min_y,
			      -1, -1);

      /* Update pointer constraints.  */
      XLPointerConstraintsSurfaceMovedTo (role->role.surface,
					  event->xconfigure.x,
					  event->xconfigure.y);
    }

  /* Tell the frame clock how many WM-generated configure events have
     arrived.  */
  SyncHelperNoteConfigureEvent (role->sync_helper);

  /* Run reconstrain callbacks.  */
  RunReconstrainCallbacksForXEvent (role, event);
}

static void
CurrentRootPosition (XdgRole *role, int *root_x, int *root_y)
{
  Window child_return;

  if (role->pending_synth_configure)
    {
      *root_x = role->pending_root_x;
      *root_y = role->pending_root_y;

      return;
    }

  XTranslateCoordinates (compositor.display, role->window,
			 DefaultRootWindow (compositor.display),
			 0, 0, root_x, root_y, &child_return);
}

static void
NoteBounds (void *data, int min_x, int min_y,
	    int max_x, int max_y)
{
  XdgRole *role;
  int bounds_width, bounds_height, root_x, root_y;
  Bool run_reconstrain_callbacks, root_position_initialized;

  role = data;
  run_reconstrain_callbacks = False;
  root_position_initialized = False;

  if (role->state & StateWaitingForAckCommit)
    /* Don't resize the window until all configure events are
       acknowledged.  We wait for a commit on the xdg_toplevel to do
       this, because Firefox updates subsurfaces while the old size is
       still in effect.  */
    return;

  if (role->state & StateTemporaryBounds)
    return;

  /* Avoid resizing the window should its actual size not have
     changed.  */

  bounds_width = max_x - min_x + 1;
  bounds_height = max_y - min_y + 1;

  if (role->bounds_width != bounds_width
      || role->bounds_height != bounds_height)
    {
#ifdef DEBUG_GEOMETRY_CALCULATION
      fprintf (stderr, "Resizing to: %d %d (from: %d %d)\n",
	       bounds_width, bounds_height, role->bounds_width,
	       role->bounds_height);
#endif

      /* Update the list of outputs that the surface is inside.
	 First, get the root window position.  */
      CurrentRootPosition (role, &root_x, &root_y);
      root_position_initialized = True;

      /* Next, update the output set.  */
      XLUpdateSurfaceOutputs (role->role.surface, root_x + min_x,
			      root_y + min_y, -1, -1);
      
      if (role->impl->funcs.note_window_pre_resize)
	role->impl->funcs.note_window_pre_resize (&role->role,
						  role->impl,
						  bounds_width,
						  bounds_height);

      XResizeWindow (compositor.display, role->window,
		     bounds_width, bounds_height);
      run_reconstrain_callbacks = True;

      if (role->impl->funcs.note_window_resized)
	role->impl->funcs.note_window_resized (&role->role,
					       role->impl,
					       bounds_width,
					       bounds_height);
    }

  if (role->state & StateDirtyFrameExtents)
    {
      /* Only handle window geometry changes once a commit happens and
	 the window is really resized.  */

      if (role->impl->funcs.handle_geometry_change)
	role->impl->funcs.handle_geometry_change (&role->role,
						  role->impl);

      role->state &= ~StateDirtyFrameExtents;
    }

  /* Now, make sure the window stays at the same position relative to
     the origin of the view.  */

  if (min_x != role->min_x || min_y != role->min_y)
    {
      /* Move the window by the opposite of the amount the min_x and
	 min_y changed.  */

      if (!root_position_initialized)
	CurrentRootPosition (role, &root_x, &root_y);

      XMoveWindow (compositor.display, role->window,
		   root_x + min_x + role->min_x,
		   root_y + min_y + role->min_y);
      run_reconstrain_callbacks = True;

      /* Set pending root window positions.  These positions will be
	 used until the movement really happens, to avoid outdated
	 positions being used after the minimum positions change in
	 quick succession.  */
      role->pending_root_x = root_x + min_x + role->min_x;
      role->pending_root_y = root_y + min_y + role->min_y;
      role->pending_synth_configure++;
    }

  /* Finally, record the current bounds.  */

  role->min_x = min_x;
  role->max_x = max_x;
  role->min_y = min_y;
  role->max_y = max_y;

  role->bounds_width = bounds_width;
  role->bounds_height = bounds_height;

  /* Tell the role implementation about the change in window size.  */

  if (role->impl && role->impl->funcs.note_size)
    role->impl->funcs.note_size (&role->role, role->impl,
				 max_x - min_x + 1,
				 max_y - min_y + 1);

  /* Run reconstrain callbacks if a resize happened.  */
  if (run_reconstrain_callbacks)
    RunReconstrainCallbacks (role);
}

static void
WriteRedirectProperty (XdgRole *role)
{
  unsigned long bypass_compositor;

  bypass_compositor = 2;

  XChangeProperty (compositor.display, role->window,
		   _NET_WM_BYPASS_COMPOSITOR, XA_CARDINAL,
		   32, PropModeReplace,
		   (unsigned char *) &bypass_compositor, 1);
}

static void
ResizeForMap (XdgRole *role)
{
  int min_x, min_y, max_x, max_y;

  SubcompositorBounds (role->subcompositor, &min_x,
		       &min_y, &max_x, &max_y);

  /* At this point, we are probably still waiting for ack_commit; as a
     result, NoteBounds will not really resize the window.  */
  NoteBounds (role, min_x, min_y, max_x, max_y);

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr, "ResizeForMap: %d %d\n",
	   max_x - min_x + 1, max_y - min_y + 1);
#endif

  if (role->state & StateDirtyFrameExtents)
    {
      /* Only handle window geometry changes once a commit
	 happens.  */

      if (role->impl->funcs.handle_geometry_change)
	role->impl->funcs.handle_geometry_change (&role->role,
						  role->impl);

      role->state &= ~StateDirtyFrameExtents;
    }

  /* Resize the window pre-map.  This should generate a
     ConfigureNotify event once the resize completes.  */
  XResizeWindow (compositor.display, role->window,
		 max_x - min_x + 1, max_y - min_y + 1);

  if (role->impl->funcs.note_window_resized)
    role->impl->funcs.note_window_resized (&role->role,
					   role->impl,
					   max_x - min_x + 1,
					   max_y - min_y + 1);
}

static void
GetResizeDimensions (Surface *surface, Role *role, int *x_out,
		     int *y_out)
{
  XLXdgRoleGetCurrentGeometry (role, NULL, NULL, x_out, y_out);

  /* Scale these surface-local dimensions to window-local ones.  */
  TruncateSurfaceToWindow (surface, *x_out, *y_out, x_out, y_out);
}

static void
PostResize (Surface *surface, Role *role, int west_motion,
	    int north_motion, int new_width, int new_height)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (!xdg_role->impl || !xdg_role->impl->funcs.post_resize)
    return;

  xdg_role->impl->funcs.post_resize (role, xdg_role->impl,
				     west_motion, north_motion,
				     new_width, new_height);
}

static void
MoveBy (Surface *surface, Role *role, int west, int north)
{
  XLXdgRoleMoveBy (role, west, north);
}

static void
Rescale (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  /* The window geometry actually applied to the X window (in the form
     of frame extents, etc) heavily depends on the output scale.  */

  if (xdg_role->impl->funcs.handle_geometry_change)
    xdg_role->impl->funcs.handle_geometry_change (role, xdg_role->impl);

  /* Also update the configure bounds if necessary.  */

  if (xdg_role->impl->funcs.rescale)
    xdg_role->impl->funcs.rescale (role, xdg_role->impl);
}

static void
HandleResize (void *data, Bool only_frame)
{
  XdgRole *role;

  role = data;

  if (only_frame)
    {
      SyncHelperCheckFrameCallback (role->sync_helper);
      return;
    }

  /* _NET_WM_SYNC_REQUEST events should be succeeded by a
     ConfigureNotify event.  */
  role->state |= StateWaitingForAckConfigure;
  role->state |= StateWaitingForAckCommit;

  /* Cancel any pending frame.  Nothing should be displayed while an
     ack_configure is pending.  */
  SyncHelperClearPendingFrame (role->sync_helper);

  /* This flag means the WaitingForAckConfigure was caused by a
     _NET_WM_SYNC_REQUEST, and the following ConfigureNotify event
     might not lead to a configure event being sent.  */
  role->state |= StateMaybeConfigure;

  /* If a freeze comes between commit and configure, then clients will
     hang indefinitely waiting for _NET_WM_FRAME_DRAWN.  Make the sync
     helper check for this situation.  */
  SyncHelperCheckFrameCallback (role->sync_helper);

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr, "Waiting for ack_configure (?)...\n");
#endif
}

static Bool
CheckFastForward (void *data)
{
  XdgRole *role;

  /* Return whether or not it is ok to fast forward the frame
     counter while ending a frame.  */

  role = data;
  return !(role->state & StateWaitingForAckCommit);
}

static void
SelectExtraEvents (Surface *surface, Role *role,
		   unsigned long event_mask)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  /* Select extra events for the input method.  */
  XSelectInput (compositor.display, xdg_role->window,
		DefaultEventMask | event_mask);

  /* Set the target standard event mask.  */
  RenderSetStandardEventMask (xdg_role->target,
			      DefaultEventMask | event_mask);
}

static void
NoteFocus (Surface *surface, Role *role, FocusMode focus)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (xdg_role->impl && xdg_role->impl->funcs.note_focus)
    xdg_role->impl->funcs.note_focus (role, xdg_role->impl,
				      focus);
}

static void
OutputsChanged (Surface *surface, Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (xdg_role->impl && xdg_role->impl->funcs.outputs_changed)
    xdg_role->impl->funcs.outputs_changed (role, xdg_role->impl);
}

static void
Activate (Surface *surface, Role *role, int deviceid,
	  Timestamp timestamp, Surface *activator_surface)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (xdg_role->impl && xdg_role->impl->funcs.activate)
    xdg_role->impl->funcs.activate (role, xdg_role->impl,
				    deviceid,
				    timestamp.milliseconds,
				    activator_surface);
}

static void
HandleFrameCallback (void *data, uint32_t frame_time)
{
  XdgRole *role;

  role = data;
  RunFrameCallbacksConditionally (role, frame_time);
}

void
XLGetXdgSurface (struct wl_client *client, struct wl_resource *resource,
		 uint32_t id, struct wl_resource *surface_resource)
{
  XdgRole *role;
  XSetWindowAttributes attrs;
  unsigned int flags;
  Surface *surface;
  XdgWmBase *wm_base;

  surface = wl_resource_get_user_data (surface_resource);
  wm_base = wl_resource_get_user_data (resource);

  if (surface->role || (surface->role_type != AnythingType
			&& surface->role_type != XdgType))
    {
      /* A role already exists on that surface.  */
      wl_resource_post_error (resource, XDG_WM_BASE_ERROR_ROLE,
			      "surface already has attached role");
      return;
    }

  role = XLSafeMalloc (sizeof *role);

  if (!role)
    {
      wl_client_post_no_memory (client);
      return;
    }

  memset (role, 0, sizeof *role);

  role->role.resource = wl_resource_create (client, &xdg_surface_interface,
					    wl_resource_get_version (resource),
					    id);

  if (!role->role.resource)
    {
      XLFree (role);
      wl_client_post_no_memory (client);

      return;
    }

  wl_resource_set_implementation (role->role.resource, &xdg_surface_impl,
				  role, HandleResourceDestroy);

  /* Link the role onto the wm base.  */
  role->link.next = wm_base->list.next;
  role->link.last = &wm_base->list;
  role->link.role = &role->role;
  wm_base->list.next->last = &role->link;
  wm_base->list.next = &role->link;
  role->wm_base = wm_base;

  /* Add a reference to this role struct since a wl_resource now
     refers to it.  */
  role->refcount++;

  role->role.funcs.commit = Commit;
  role->role.funcs.teardown = Teardown;
  role->role.funcs.setup = Setup;
  role->role.funcs.release_buffer = ReleaseBuffer;
  role->role.funcs.subsurface_update = SubsurfaceUpdate;
  role->role.funcs.get_window = GetWindow;
  role->role.funcs.get_resize_dimensions = GetResizeDimensions;
  role->role.funcs.post_resize = PostResize;
  role->role.funcs.move_by = MoveBy;
  role->role.funcs.rescale = Rescale;
  role->role.funcs.select_extra_events = SelectExtraEvents;
  role->role.funcs.note_focus = NoteFocus;
  role->role.funcs.outputs_changed = OutputsChanged;
  role->role.funcs.activate = Activate;

  attrs.colormap = compositor.colormap;
  attrs.border_pixel = border_pixel;
  attrs.event_mask = DefaultEventMask;
  attrs.cursor = InitDefaultCursor ();
  flags = CWColormap | CWBorderPixel | CWEventMask | CWCursor;

  role->window = XCreateWindow (compositor.display,
				DefaultRootWindow (compositor.display),
				0, 0, 20, 20, 0, compositor.n_planes,
				InputOutput, compositor.visual, flags,
				&attrs);
  role->target = RenderTargetFromWindow (role->window, DefaultEventMask);
  role->release_helper = MakeBufferReleaseHelper (AllBuffersReleased,
						  role);

  /* Set the client.  */
  RenderSetClient (role->target, client);

  role->subcompositor = MakeSubcompositor ();
  role->sync_helper = MakeSyncHelper (role->subcompositor,
				      role->window,
				      role->target,
				      HandleFrameCallback,
				      &role->role);
  SyncHelperSetResizeCallback (role->sync_helper, HandleResize,
			       CheckFastForward);

  SubcompositorSetTarget (role->subcompositor, &role->target);
  SubcompositorSetInputCallback (role->subcompositor,
				 InputRegionChanged, role);
  SubcompositorSetOpaqueCallback (role->subcompositor,
				  OpaqueRegionChanged, role);
  SubcompositorSetBoundsCallback (role->subcompositor,
				  NoteBounds, role);
  XLSelectStandardEvents (role->window);
  XLMakeAssoc (surfaces, role->window, role);

  /* Tell the compositing manager to never un-redirect this window.
     If it does, frame synchronization will not work.  */
  WriteRedirectProperty (role);

  if (!XLSurfaceAttachRole (surface, &role->role))
    abort ();

  /* Initialize the input region.  */
  pixman_region32_init (&role->input_region);

  /* Initialize the sentinel node of the reconstrain callbacks
     list.  */
  role->reconstrain_callbacks.next = &role->reconstrain_callbacks;
  role->reconstrain_callbacks.last = &role->reconstrain_callbacks;
}

Window
XLWindowFromXdgRole (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  return xdg_role->window;
}

Subcompositor *
XLSubcompositorFromXdgRole (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  return xdg_role->subcompositor;
}

void
XLXdgRoleAttachImplementation (Role *role, XdgRoleImplementation *impl)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  XLAssert (!xdg_role->impl && role->surface);
  impl->funcs.attach (role, impl);

  xdg_role->impl = impl;
}

void
XLXdgRoleDetachImplementation (Role *role, XdgRoleImplementation *impl)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  XLAssert (xdg_role->impl == impl);
  impl->funcs.detach (role, impl);

  xdg_role->impl = NULL;
}

void
XLXdgRoleSendConfigure (Role *role, uint32_t serial)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  xdg_role->conf_serial = serial;
  xdg_role->state |= StateWaitingForAckConfigure;
  xdg_role->state |= StateWaitingForAckCommit;

  /* Cancel any pending frame.  Nothing should be displayed while an
     ack_configure is pending.  */
  SyncHelperClearPendingFrame (xdg_role->sync_helper);

  /* See the comment under XLXdgRoleSetBoundsSize.  */
  xdg_role->state &= ~StateTemporaryBounds;

  /* We know know that the ConfigureNotify event following any
     _NET_WM_SYNC_REQUEST event was accepted, so clear the maybe
     configure flag.  */
  xdg_role->state &= ~StateMaybeConfigure;

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr, "Waiting for ack_configure (%"PRIu32")...\n",
	   xdg_role->conf_serial);
#endif

  xdg_surface_send_configure (role->resource, serial);
}

void
XLXdgRoleCalcNewWindowSize (Role *role, int width, int height,
			    int *new_width, int *new_height)
{
  XdgRole *xdg_role;
  int temp, temp1, geometry_width, geometry_height;
  int current_width, current_height, min_x, min_y, max_x, max_y;

  xdg_role = XdgRoleFromRole (role);

  if (!xdg_role->current_state.window_geometry_width
      /* If no surface exists, we might as well return immediately,
	 since the scale factor will not be obtainable.  */
      || !role->surface)
    {
      *new_width = width;
      *new_height = height;

      return;
    }

  SubcompositorBounds (xdg_role->subcompositor,
		       &min_x, &min_y, &max_x, &max_y);

  /* Calculate the current width and height.  */
  current_width = (max_x - min_x + 1);
  current_height = (max_y - min_y + 1);

  /* Adjust the current_width and current_height by the scale
     factor.  */
  TruncateScaleToSurface (role->surface, current_width,
			  current_height, &current_width,
			  &current_height);

  XLXdgRoleGetCurrentGeometry (role, NULL, NULL, &geometry_width,
			       &geometry_height);

  /* Now, temp and temp1 become the difference between the current
     window geometry and the size of the surface (incl. subsurfaces)
     in both axes.  */

  temp = current_width - geometry_width;
  temp1 = current_height - geometry_height;

  *new_width = width - temp;
  *new_height = height - temp1;

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr,
	   "Configure event width, height: %d %d\n"
	   "Generated width, height:       %d %d\n",
	   width, height, *new_width, *new_height);
#endif
}

int
XLXdgRoleGetWidth (Role *role)
{
  XdgRole *xdg_role;
  int x, y, x1, y1;

  xdg_role = XdgRoleFromRole (role);

  SubcompositorBounds (xdg_role->subcompositor,
		       &x, &y, &x1, &y1);

  return x1 - x + 1;
}

int
XLXdgRoleGetHeight (Role *role)
{
  XdgRole *xdg_role;
  int x, y, x1, y1;

  xdg_role = XdgRoleFromRole (role);

  SubcompositorBounds (xdg_role->subcompositor,
		       &x, &y, &x1, &y1);

  return y1 - y + 1;
}

void
XLXdgRoleSetBoundsSize (Role *role, int bounds_width, int bounds_height)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  xdg_role->bounds_width = bounds_width;
  xdg_role->bounds_height = bounds_height;

#ifdef DEBUG_GEOMETRY_CALCULATION
  fprintf (stderr, "Set new bounds size: %d %d\n", bounds_width,
	   bounds_height);
#endif

  /* Now, a temporary bounds_width and bounds_height has been
     recorded.  This means that if a configure event has not yet been
     delivered, then any subsequent SubcompositorUpdate will cause
     NoteBounds to resize back to the old width and height, confusing
     the window manager and possibly causing it to maximize us.

     Set a flag that tells NoteBounds to abstain from resizing the
     window.  This flag is then cleared once a configure event is
     delivered, or the next time the role is mapped.  */
  xdg_role->state |= StateTemporaryBounds;
}

void
XLXdgRoleGetCurrentGeometry (Role *role, int *x_return, int *y_return,
			     int *width, int *height)
{
  XdgRole *xdg_role;
  int x, y, x1, y1, min_x, max_x, min_y, max_y;

  xdg_role = XdgRoleFromRole (role);

  SubcompositorBounds (xdg_role->subcompositor,
		       &min_x, &min_y, &max_x, &max_y);

  if (!xdg_role->current_state.window_geometry_width)
    {
      if (x_return)
	*x_return = min_x;
      if (y_return)
	*y_return = min_y;

      if (width)
	*width = max_x - min_x + 1;
      if (height)
	*height = max_y - min_y + 1;

      return;
    }

  x = xdg_role->current_state.window_geometry_x;
  y = xdg_role->current_state.window_geometry_y;
  x1 = (xdg_role->current_state.window_geometry_x
	+ xdg_role->current_state.window_geometry_width - 1);
  y1 = (xdg_role->current_state.window_geometry_y
	+ xdg_role->current_state.window_geometry_height - 1);

  x1 = MIN (x1, max_x);
  y1 = MIN (y1, max_y);
  x = MAX (min_x, x);
  y = MAX (min_y, y);

  if (x_return)
    *x_return = x;
  if (y_return)
    *y_return = y;

  if (width)
    *width = x1 - x + 1;
  if (height)
    *height = y1 - y + 1;
}

void
XLXdgRoleNoteConfigure (Role *role, XEvent *event)
{
  NoteConfigure (XdgRoleFromRole (role), event);
}

void
XLRetainXdgRole (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  xdg_role->refcount++;
}

void
XLReleaseXdgRole (Role *role)
{
  ReleaseBacking (XdgRoleFromRole (role));
}

void
XLXdgRoleCurrentRootPosition (Role *role, int *root_x, int *root_y)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  CurrentRootPosition (xdg_role, root_x, root_y);
}

XdgRoleImplementationType
XLTypeOfXdgRole (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  return xdg_role->type;
}

XdgRoleImplementation *
XLImplementationOfXdgRole (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  return xdg_role->impl;
}

Bool
XLXdgRoleInputRegionContains (Role *role, int x, int y)
{
  XdgRole *xdg_role;
  pixman_box32_t dummy_box;

  xdg_role = XdgRoleFromRole (role);
  return pixman_region32_contains_point (&xdg_role->input_region,
					 x, y, &dummy_box);
}

void
XLXdgRoleResizeForMap (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  /* Clear the StateTemporaryBounds flag; it should not persist after
     mapping, as a configure event is no longer guaranteed to be sent
     if the toplevel is unmapped immediately after
     XLXdgRoleSetBoundsSize.  */
  xdg_role->state &= ~StateTemporaryBounds;
  ResizeForMap (xdg_role);
}

void *
XLXdgRoleRunOnReconstrain (Role *role, void (*configure_func) (void *,
							       XEvent *),
			   void (*resize_func) (void *), void *data)
{
  ReconstrainCallback *callback;
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  callback = AddCallbackAfter (&xdg_role->reconstrain_callbacks);
  callback->configure = configure_func;
  callback->resized = resize_func;
  callback->data = data;

  return callback;
}

void
XLXdgRoleCancelReconstrainCallback (void *key)
{
  ReconstrainCallback *callback;

  callback = key;
  UnlinkReconstrainCallback (callback);
}

void
XLXdgRoleReconstrain (Role *role, XEvent *event)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);
  RunReconstrainCallbacksForXEvent (xdg_role, event);

  /* If event is a configure event, tell the frame clock about it.  */
  if (event->type == ConfigureNotify)
    SyncHelperNoteConfigureEvent (xdg_role->sync_helper);
}

void
XLXdgRoleMoveBy (Role *role, int west, int north)
{
  int root_x, root_y;
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  /* Move the window by the opposite of west and north.  */

  CurrentRootPosition (xdg_role, &root_x, &root_y);
  XMoveWindow (compositor.display, xdg_role->window,
	       root_x - west, root_y - north);

  /* Set pending root window positions.  These positions will be
     used until the movement really happens, to avoid outdated
     positions being used after the minimum positions change in
     quick succession.  */
  xdg_role->pending_root_x = root_x - west;
  xdg_role->pending_root_y = root_y - north;
  xdg_role->pending_synth_configure++;
}

void
XLInitXdgSurfaces (void)
{
  XColor alloc;
  int shape_minor, shape_major, shape_error;

  surfaces = XLCreateAssocTable (1024);

  alloc.red = 0;
  alloc.green = 65535;
  alloc.blue = 0;

  if (!XAllocColor (compositor.display, compositor.colormap,
		    &alloc))
    {
      fprintf (stderr, "Failed to allocate green pixel\n");
      exit (1);
    }

  border_pixel = alloc.pixel;

  /* Now initialize the nonrectangular window shape extension.  We
     need a version that supports input shapes, which means 1.1 or
     later.  */

  if (!XShapeQueryExtension (compositor.display, &shape_base,
			     &shape_error))
    {
      fprintf (stderr, "The Nonrectangular Window Shape extension is not"
	       " present on the X server\n");
      exit (1);
    }

  if (!XShapeQueryVersion (compositor.display,
			   &shape_major, &shape_minor))
    {
      fprintf (stderr, "A supported version of the Nonrectangular Window"
	       " Shape extension is not present on the X server\n");
      exit (1);
    }

  if (shape_major < 1 || (shape_major == 1 && shape_minor < 1))
    {
      fprintf (stderr, "The version of the Nonrectangular Window Shape"
	       " extension is too old\n");
      exit (1);
    }
}

XdgRoleImplementation *
XLLookUpXdgToplevel (Window window)
{
  XdgRole *role;

  role = XLLookUpAssoc (surfaces, window);

  if (!role)
    return NULL;

  if (role->type != TypeToplevel)
    return NULL;

  return role->impl;
}

XdgRoleImplementation *
XLLookUpXdgPopup (Window window)
{
  XdgRole *role;

  role = XLLookUpAssoc (surfaces, window);

  if (!role)
    return NULL;

  if (role->type != TypePopup)
    return NULL;

  return role->impl;
}

void
XLXdgRoleNoteRejectedConfigure (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  if (xdg_role->state & StateMaybeConfigure)
    {
      /* A configure event immediately following _NET_WM_SYNC_REQUEST
	 was rejected, meaning that we do not have to change anything
	 before unfreezing the frame clock.  */
      xdg_role->state &= ~StateWaitingForAckConfigure;
      xdg_role->state &= ~StateWaitingForAckCommit;
      xdg_role->state &= ~StateMaybeConfigure;
    }
}

void
XLXdgRoleHandlePing (Role *role, XEvent *event,
		     void (*reply_func) (XEvent *))
{
  XdgRole *xdg_role;
  PingEvent *record;

  xdg_role = XdgRoleFromRole (role);

  /* If the role's xdg_wm_base is detached, just reply to the ping
     message.  */
  if (!xdg_role->link.next)
    reply_func (event);
  else
    {
      /* Otherwise, save the event and ping the client.  Then, send
	 replies once the client replies.  */
      record = XLMalloc (sizeof *record);
      record->event = *event;
      record->reply_func = reply_func;
      xdg_role->ping_events = XLListPrepend (xdg_role->ping_events,
					     record);
      XLXdgWmBaseSendPing (xdg_role->wm_base);
    }
}

static void
ReplyPingEvent (void *data)
{
  PingEvent *event;

  event = data;
  event->reply_func (&event->event);
  XLFree (event);
}

void
XLXdgRoleReplyPing (Role *role)
{
  XdgRole *xdg_role;

  xdg_role = XdgRoleFromRole (role);

  /* Free the ping event list, calling the reply functions along the
     way.  */
  XLListFree (xdg_role->ping_events, ReplyPingEvent);
  xdg_role->ping_events = NULL;
}
