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

enum {
	PendingPosition = 1,
};

enum _SurfaceActionType {
	Sentinel,
	PlaceAboveOther,
	PlaceBelowOther,
};

typedef enum _SurfaceActionType		SurfaceActionType;
typedef struct _Subsurface		Subsurface;
typedef struct _Substate		Substate;
typedef struct _SurfaceAction		SurfaceAction;
typedef struct _SurfaceActionClientData SurfaceActionClientData;

#define SubsurfaceFromRole(role) ((Subsurface *)(role))

struct _SurfaceAction {
	/* What this action is.  */
	SurfaceActionType type;

	/* What subsurface this action applies to.  */
	Subsurface *subsurface;

	/* What surface is the "other" surface.  */
	Surface *other;

	/* Surface destroy listener.  */
	DestroyCallback *destroy_listener;

	/* The next and last surface actions in this list.  */
	SurfaceAction *next, *last;
};

struct _Substate {
	/* The position of the subsurface relative to the parent.  */
	int x, y;

	/* Various flags.  */
	int flags;
};

struct _Subsurface {
	/* The role object itself.  */
	Role role;

	/* The parent surface.  */
	Surface *parent;

	/* The number of references to this subsurface.  */
	int refcount;

	/* Pending substate.  */
	Substate pending_substate;

	/* Current substate.  */
	Substate current_substate;

	/* Commit callback attached to the parent.  */
	CommitCallback *commit_callback;

	/* Whether or not this should be desynchronous.  */
	Bool should_be_desync;

	/* Whether or not this is synchronous.  */
	Bool synchronous;

	/* Whether or not a commit is pending.  */
	Bool pending_commit;

	/* Whether or not this subsurface is mapped.  */
	Bool mapped;

	/* Whether or not this subsurface was just added to a parent that
	   has not yet committed.  */
	Bool pending;

	/* The last dimensions and position that were used to update this
	   surface's outputs.  */
	int output_x, output_y, output_width, output_height;
};

struct _SurfaceActionClientData {
	/* Any pending subsurface actions.  */
	SurfaceAction actions;
};

/* The global wl_subcompositor resource.  */

struct wl_global *global_subcompositor;

static void
UnlinkSurfaceAction(SurfaceAction *subaction)
{
	subaction->last->next = subaction->next;
	subaction->next->last = subaction->last;
}

static void
HandleOtherSurfaceDestroyed(void *data)
{
	SurfaceAction *action;

	action = data;
	UnlinkSurfaceAction(action);
	XLFree(action);
}

static void
DestroySurfaceAction(SurfaceAction *subaction)
{
	XLSurfaceCancelRunOnFree(subaction->destroy_listener);
	UnlinkSurfaceAction(subaction);

	XLFree(subaction);
}

static Bool
CheckSiblingRelationship(Subsurface *subsurface, Surface *other)
{
	Subsurface *other_subsurface;

	if (other->role_type != SubsurfaceType
	    /* The role might've been detached from the other surface.  */
	    || !other->role)
		return False;

	other_subsurface = SubsurfaceFromRole(other->role);

	if (other_subsurface->parent != subsurface->parent)
		return False;

	return True;
}

static void
ParentBelow(View *parent, View *below, Surface *surface)
{
	ViewInsertBefore(parent, surface->view, below);
	ViewInsertBefore(parent, surface->under, surface->view);
}

static void
ParentAbove(View *parent, View *above, Surface *surface)
{
	ViewInsertAfter(parent, surface->under, above);
	ViewInsertAfter(parent, surface->view, surface->under);
}

static void
ParentStart(View *parent, Surface *surface)
{
	ViewInsert(parent, surface->under);
	ViewInsert(parent, surface->view);
}

static void
RunOneSurfaceAction(Subsurface *subsurface, SurfaceAction *subaction)
{
	View *target;

	if (!subsurface->role.surface || !subsurface->parent)
		return;

	if (subaction->type == PlaceAboveOther) {
		if (subaction->other != subsurface->parent &&
		    !CheckSiblingRelationship(subsurface, subaction->other))
			/* The hierarchy changed in some unacceptable way
			   between the action being recorded and the commit of
			   the parent. Ignore.  */
			return;

		/* Determine the target under which to place the view.  If
		   subaction->other is underneath the parent, then this will
		   actually be subsurface->parent->under.  */
		target = ViewGetParent(subaction->other->view);

		/* After that, unparent the views.  */
		ViewUnparent(subsurface->role.surface->view);
		ViewUnparent(subsurface->role.surface->under);

		if (subaction->other == subsurface->parent)
			/* Re-insert this view at the beginning of the parent.
			 */
			ParentStart(subsurface->parent->view,
			    subsurface->role.surface);
		else
			/* Re-insert this view in front of the other surface. */
			ParentAbove(target, subaction->other->view,
			    subsurface->role.surface);
	} else if (subaction->type == PlaceBelowOther) {
		if (subaction->other != subsurface->parent &&
		    !CheckSiblingRelationship(subsurface, subaction->other))
			return;

		target = ViewGetParent(subaction->other->view);
		ViewUnparent(subsurface->role.surface->view);
		ViewUnparent(subsurface->role.surface->under);

		if (subaction->other != subsurface->parent)
			/* Re-insert this view before the other surface.  */
			ParentBelow(target, subaction->other->under,
			    subsurface->role.surface);
		else
			/* Re-insert this view below the parent surface.  */
			ParentStart(subsurface->parent->under,
			    subsurface->role.surface);
	}
}

static void
FreeSurfaceActions(SurfaceAction *first)
{
	SurfaceAction *action, *last;

	action = first->next;

	while (action != first) {
		last   = action;
		action = action->next;

		DestroySurfaceAction(last);
	}
}

static void
FreeSubsurfaceData(void *data)
{
	SurfaceActionClientData *client;

	client = data;
	FreeSurfaceActions(&client->actions);
}

static SurfaceAction *
AddSurfaceAction(Subsurface *subsurface, Surface *other, SurfaceActionType type)
{
	SurfaceAction		*action;
	SurfaceActionClientData *client;

	action			 = XLMalloc(sizeof *action);
	action->subsurface	 = subsurface;
	action->type		 = type;
	action->other		 = other;
	action->destroy_listener = XLSurfaceRunOnFree(other,
	    HandleOtherSurfaceDestroyed, action);

	client = XLSurfaceGetClientData(subsurface->parent, SubsurfaceData,
	    sizeof *client, FreeSubsurfaceData);

	if (!client->actions.next) {
		/* Client is not yet initialized, so initialize the sentinel
		   node.  */

		client->actions.next = &client->actions;
		client->actions.last = &client->actions;
		client->actions.type = Sentinel;
	}

	action->next = client->actions.next;
	action->last = &client->actions;

	client->actions.next->last = action;
	client->actions.next	   = action;

	return action;
}

static void
RunSurfaceActions(SurfaceAction *first)
{
	SurfaceAction *action, *last;

	action = first->last;

	while (action != first) {
		last = action;
		/* Run the actions backwards so they appear in the right
		   order.  */
		action = action->last;

		RunOneSurfaceAction(last->subsurface, last);
		DestroySurfaceAction(last);
	}
}

static void
DestroySubsurface(struct wl_client *client, struct wl_resource *resource)
{
	Subsurface *subsurface;

	subsurface = wl_resource_get_user_data(resource);

	/* Now detach the role from its surface, which can be reused in the
	   future.  */
	if (subsurface->role.surface)
		XLSurfaceReleaseRole(subsurface->role.surface,
		    &subsurface->role);

	wl_resource_destroy(resource);
}

static void
SetPosition(struct wl_client *client, struct wl_resource *resource, int32_t x,
    int32_t y)
{
	Subsurface *subsurface;

	subsurface = wl_resource_get_user_data(resource);

	subsurface->pending_substate.x = x;
	subsurface->pending_substate.y = y;
	subsurface->pending_substate.flags |= PendingPosition;
}

static void
PlaceAbove(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *surface_resource)
{
	Subsurface *subsurface;
	Surface	   *other;

	subsurface = wl_resource_get_user_data(resource);
	other	   = wl_resource_get_user_data(surface_resource);

	if (other != subsurface->parent &&
	    !CheckSiblingRelationship(subsurface, other)) {
		wl_resource_post_error(resource,
		    WL_SUBSURFACE_ERROR_BAD_SURFACE,
		    "surface is not a sibling or the parent");
		return;
	}

	AddSurfaceAction(subsurface, other, PlaceAboveOther);
}

static void
PlaceBelow(struct wl_client *client, struct wl_resource *resource,
    struct wl_resource *surface_resource)
{
	Subsurface *subsurface;
	Surface	   *other;

	subsurface = wl_resource_get_user_data(resource);
	other	   = wl_resource_get_user_data(surface_resource);

	if (other != subsurface->parent &&
	    !CheckSiblingRelationship(subsurface, other)) {
		wl_resource_post_error(resource,
		    WL_SUBSURFACE_ERROR_BAD_SURFACE,
		    "surface is not a sibling or the parent");
		return;
	}

	AddSurfaceAction(subsurface, other, PlaceBelowOther);
}

static void
SetSync1(Subsurface *subsurface)
{
	Surface	   *child;
	Subsurface *child_subsurface;
	XLList	   *list;

	/* Note that the given subsurface has become synchronous by setting
	   its synchronous flag to True.  */

	subsurface->synchronous = True;

	if (subsurface->role.surface) {
		list = subsurface->role.surface->subsurfaces;

		for (; list; list = list->next) {
			child		 = list->data;
			child_subsurface = SubsurfaceFromRole(child->role);
			SetSync1(child_subsurface);
		}
	}
}

static void
SetSync(struct wl_client *client, struct wl_resource *resource)
{
	Subsurface *subsurface;

	subsurface = wl_resource_get_user_data(resource);

	/* This subsurface should not actually be desynchronous.  */
	subsurface->should_be_desync = False;

	/* Now, make each child synchronous recursively.  */
	SetSync1(subsurface);
}

static Bool
IsParentSynchronous(Subsurface *subsurface)
{
	Surface	   *surface;
	Subsurface *parent;

	surface = subsurface->parent;

	if (!surface || surface->role_type != SubsurfaceType)
		return False;

	parent = SubsurfaceFromRole(surface->role);

	return parent->synchronous;
}

static void
NoteSubsurfaceDesynchronous(Subsurface *subsurface, Bool apply_state)
{
	Surface	   *child;
	Subsurface *child_subsurface;
	XLList	   *list;

	/* Note that the given subsurface has become desynchronous, and
	   apply pending state.  Make each of its children that should be
	   desynchronous desynchronous as well, but avoid applying their
	   pending state.  */

	subsurface->synchronous = False;

	if (subsurface->pending_commit && subsurface->role.surface &&
	    apply_state) {
		XLCommitSurface(subsurface->role.surface, False);

		/* Set pending_commit to False only here, where it is certain
		   that the cached state has been applied.  */
		subsurface->pending_commit = False;
	}

	if (subsurface->role.surface) {
		list = subsurface->role.surface->subsurfaces;

		for (; list; list = list->next) {
			child		 = list->data;
			child_subsurface = SubsurfaceFromRole(child->role);

			if (child_subsurface->should_be_desync)
				NoteSubsurfaceDesynchronous(child_subsurface,
				    False);
		}
	}
}

static void
NoteSubsurfaceTeardown(Subsurface *subsurface)
{
	Surface	   *child;
	Subsurface *child_subsurface;
	XLList	   *list;

	/* The same, but it avoids applying any pending state.  Used during
	   teardown.  */

	list			= subsurface->role.surface->subsurfaces;
	subsurface->synchronous = False;

	for (; list; list = list->next) {
		child		 = list->data;
		child_subsurface = SubsurfaceFromRole(child->role);

		if (child_subsurface->should_be_desync)
			NoteSubsurfaceTeardown(child_subsurface);
	}
}

static void
SetDesync(struct wl_client *client, struct wl_resource *resource)
{
	Subsurface *subsurface;

	subsurface = wl_resource_get_user_data(resource);

	/* Set it so that this subsurface should be desynchronous.  If the
	   parent is synchronous, then it does not actually become
	   desynchronous until the pending state is applied.  */
	subsurface->should_be_desync = True;

	/* Return if the parent is synchronous, as Wayland specifies
	   children of synchronous subsurfaces are always synchronous.  */

	if (IsParentSynchronous(subsurface))
		return;

	/* Make subsurface desynchronous and apply its pending state.  If
	   any of its children are supposed to be desynchronous, make them
	   desynchronous as well, but do not apply the pending state.  This
	   is how the documentation for the set_desync request is worded:

	     If cached state exists when wl_surface.commit is called in
	     desynchronized mode, the pending state is added to the cached
	     state, and applied as a whole. This invalidates the cache.

	     Note: even if a sub-surface is set to desynchronized, a parent
	     sub-surface may override it to behave as synchronized. For
	     details, see wl_subsurface.

	     If a surface's parent surface behaves as desynchronized, then
	     the cached state is applied on set_desync.

	  Notice how the last paragraph tries to stress that only surfaces
	  that are made desynchronous at the time of a set_desync request
	  made on them are supposed to have their cached state applied at
	  the time of that request.

	  Normally, applying the cached state of the desynchronous
	  subsurface will cause the cached state of its children to be
	  applied.  However, there could be no cached state at all on the
	  surface specified as the argument to the set_desync request, in
	  which case children should not have their pending state applied.
	  This behavior is subject to tests in subsurface_test.c.  */
	NoteSubsurfaceDesynchronous(subsurface, True);
}

static const struct wl_subsurface_interface wl_subsurface_impl = {
	.destroy      = DestroySubsurface,
	.set_position = SetPosition,
	.place_above  = PlaceAbove,
	.place_below  = PlaceBelow,
	.set_sync     = SetSync,
	.set_desync   = SetDesync,
};

static void
DestroyBacking(Subsurface *subsurface)
{
	if (--subsurface->refcount)
		return;

	XLFree(subsurface);
}

static Bool
EarlyCommit(Surface *surface, Role *role)
{
	Subsurface *subsurface;

	subsurface = SubsurfaceFromRole(role);

	/* If the role is synchronous, don't commit until the parent
	   commits.  */
	if (subsurface->synchronous) {
		subsurface->pending_commit = True;
		return False;
	} else if (subsurface->pending_commit) {
		/* There is still cached state.  Merge the state into the
		   surface first, before SubcompositorUpdate is called by
		   InternalCommit.  */
		XLSurfaceMergeCachedState(surface);

		/* As the state is merged, there is no more cached state.  */
		subsurface->pending_commit = False;
	}

	return True;
}

static void
MaybeUpdateOutputs(Subsurface *subsurface)
{
	int x, y, width, height, base_x, base_y;

	if (subsurface->role.surface->output_x == INT_MIN ||
	    subsurface->role.surface->output_y == INT_MIN)
		/* Valid base coordinates are not yet available.  */
		return;

	if (!subsurface->parent)
		/* A valid scale factor is not available.  */
		return;

	/* Compute the positions relative to the parent.  */
	x = floor(subsurface->current_substate.x * subsurface->parent->factor);
	y = floor(subsurface->current_substate.y * subsurface->parent->factor);

	/* And the base X and Y.  */
	base_x = subsurface->role.surface->output_x;
	base_y = subsurface->role.surface->output_y;

	/* Compute the absolute width and height of the surface
	   contents.  */
	width  = ViewWidth(subsurface->role.surface->view);
	height = ViewHeight(subsurface->role.surface->view);

	/* If nothing really changed, return.  */
	if (x == subsurface->output_x && y == subsurface->output_y &&
	    width == subsurface->output_width &&
	    height == subsurface->output_height)
		return;

	/* Otherwise, recompute the outputs this subsurface overlaps and
	   record those values.  */
	subsurface->output_x	  = x;
	subsurface->output_y	  = y;
	subsurface->output_width  = width;
	subsurface->output_height = height;

	/* Recompute overlaps.  */
	XLUpdateSurfaceOutputs(subsurface->role.surface, x + base_x, y + base_y,
	    width, height);
}

static void
MoveFractional(Subsurface *subsurface)
{
	double x, y;
	int    x_int, y_int;

	/* Move the surface to a fractional window (subcompositor)
	   coordinate relative to the parent.  This is done by placing the
	   surface at the floor of the coordinates, and then offsetting the
	   image and input by the remainder during rendering.  */
	SurfaceToWindow(subsurface->parent, subsurface->current_substate.x,
	    subsurface->current_substate.y, &x, &y);

	x_int = floor(x);
	y_int = floor(y);

	/* Move the subsurface to x_int, y_int.  */
	ViewMove(subsurface->role.surface->view, x_int, y_int);
	ViewMove(subsurface->role.surface->under, x_int, y_int);

	/* Apply the fractional offset.  */
	ViewMoveFractional(subsurface->role.surface->view, x - x_int,
	    y - y_int);
	ViewMoveFractional(subsurface->role.surface->under, x - x_int,
	    y - y_int);

	/* And set the fractional offset on the surface for input handling
	   purposes.  */
	subsurface->role.surface->input_delta_x = x - x_int;
	subsurface->role.surface->input_delta_y = y - y_int;

	/* Apply pointer constraints.  */
	XLPointerConstraintsSubsurfaceMoved(subsurface->role.surface);
}

static void
AfterParentCommit(Surface *surface, void *data)
{
	Subsurface *subsurface;

	subsurface = data;

	/* The surface might've been destroyed already.  */
	if (!subsurface->role.surface)
		return;

	/* Apply pending state.  */

	if (subsurface->pending_substate.flags & PendingPosition) {
		/* Apply the new position.  */
		subsurface->current_substate.x = subsurface->pending_substate.x;
		subsurface->current_substate.y = subsurface->pending_substate.y;

		/* And move the views.  */
		MoveFractional(subsurface);
	}

	/* Attach the views to the subcompositor if they have not yet been
	   attached, as the parent's state has been applied.  This must come
	   before XLCommitSurface, as doing so will apply the pending state,
	   which will fail to update the subcompositor bounds if the
	   subsurface is not present.  */

	if (subsurface->pending) {
		/* Set the subcompositor here.  If the role providing the
		   subcompositor hasn't been attached to the parent, then when
		   it is it will call ViewSetSubcompositor on the parent's
		   view.  */

		ViewSetSubcompositor(subsurface->role.surface->under,
		    ViewGetSubcompositor(surface->view));
		ViewInsert(surface->view, subsurface->role.surface->under);
		ViewSetSubcompositor(subsurface->role.surface->view,
		    ViewGetSubcompositor(surface->view));
		ViewInsert(surface->view, subsurface->role.surface->view);
		subsurface->pending = False;
	}

	/* And any cached surface state too.  */
	if (subsurface->pending_commit) {
		XLCommitSurface(subsurface->role.surface, False);

		/* If the size changed, update the outputs this surface is in
		   the scanout area of.  */
		MaybeUpdateOutputs(subsurface);
	}

	subsurface->pending_commit	   = False;
	subsurface->pending_substate.flags = 0;
}

static void
SubsurfaceUpdate(Surface *surface, Role *role)
{
	Subsurface *subsurface;

	subsurface = SubsurfaceFromRole(role);

	if (!subsurface->parent || !subsurface->parent->role ||
	    !subsurface->parent->role->funcs.subsurface_update)
		return;

	subsurface->parent->role->funcs.subsurface_update(subsurface->parent,
	    subsurface->parent->role);
}

static Window
GetWindow(Surface *surface, Role *role)
{
	Subsurface *subsurface;

	subsurface = SubsurfaceFromRole(role);

	if (!subsurface->parent || !subsurface->parent->role ||
	    !subsurface->parent->role->funcs.get_window)
		return None;

	return subsurface->parent->role->funcs.get_window(subsurface->parent,
	    subsurface->parent->role);
}

static void
Commit(Surface *surface, Role *role)
{
	Subcompositor *subcompositor;
	Subsurface    *subsurface;

	subcompositor = ViewGetSubcompositor(surface->view);
	subsurface    = SubsurfaceFromRole(role);

	if (!subcompositor)
		return;

	/* If no buffer is attached, unmap the views.  */
	if (!surface->current_state.buffer) {
		ViewUnmap(surface->under);
		ViewUnmap(surface->view);

		if (subsurface->mapped)
			/* Check for idle inhibition changes.  */
			XLDetectSurfaceIdleInhibit();

		subsurface->mapped = False;
	} else {
		/* Once a buffer is attached to the view, it is automatically
		   mapped.  */
		ViewMap(surface->under);

		if (!subsurface->mapped)
			/* Check if this subsurface being mapped would cause
			   idle inhibitors to change.  */
			XLDetectSurfaceIdleInhibit();

		subsurface->mapped = True;
	}

	if (!subsurface->synchronous) {
		/* Tell the parent that a subsurface changed.  It should then do
		   whatever is appropriate to update the subsurface.  */
		SubsurfaceUpdate(surface, role);

		/* If the size changed, update the outputs this surface is in
		   the scanout area of.  */
		MaybeUpdateOutputs(subsurface);
	}
}

static Bool
Setup(Surface *surface, Role *role)
{
	Subsurface *subsurface;

	surface->role_type = SubsurfaceType;

	subsurface = SubsurfaceFromRole(role);

	subsurface->refcount++;
	subsurface->output_x = INT_MIN;
	subsurface->output_y = INT_MIN;
	role->surface	     = surface;

	/* Now move the subsurface to its initial location (0, 0) */
	MoveFractional(subsurface);

	/* Now add the subsurface to the parent's list of subsurfaces.  */
	subsurface->parent->subsurfaces =
	    XLListPrepend(subsurface->parent->subsurfaces, surface);

	/* And mark the subsurface as pending.  A pending subsurface is not
	   inserted into any subcompositor, but will be inserted upon the
	   parent commit callback being run.

	   The specification states that the "effect of adding a subsurface"
	   will take effect after its parent is applied.

	   The interpretation previously used was that the the subsurface
	   would be made visible upon the parent's state being applied.  But
	   that interpretation led to ambiguities, and contradicted with
	   common sense and the implementation in Weston.  */
	subsurface->pending = True;

	/* Subsurfaces are synchronous by default.  Make every child
	   synchronous.  */
	SetSync1(subsurface);

	return True;
}

static void
Rescale(Surface *surface, Role *role)
{
	Subsurface *subsurface;

	subsurface = SubsurfaceFromRole(role);

	/* If the parent has been detached, return immediately.  */

	if (!subsurface->parent)
		return;

	/* The scale factor changed; move the subsurface to the new correct
	   position.  */

	MoveFractional(subsurface);
}

static void
ParentRescale(Surface *surface, Role *role)
{
	/* This is called when the scale factor of the parent changes.  */
	Rescale(surface, role);
}

static void
Teardown(Surface *surface, Role *role)
{
	Subsurface		*subsurface;
	SurfaceActionClientData *client;
	SurfaceAction		*action, *last;
	Subcompositor		*subcompositor;

	subsurface = SubsurfaceFromRole(role);

	/* Make each of the surface's children that should be desynchronous
	   desynchronous.  */
	NoteSubsurfaceTeardown(subsurface);

	role->surface = NULL;
	subcompositor = NULL;

	if (subsurface->parent) {
		if (!subsurface->pending) {
			subcompositor = ViewGetSubcompositor(surface->view);

			/* Detach the views if the subsurface is not pending. */

			ViewUnparent(surface->view);
			ViewSetSubcompositor(surface->view, NULL);
			ViewUnparent(surface->under);
			ViewSetSubcompositor(surface->under, NULL);
		}

		client = XLSurfaceFindClientData(subsurface->parent,
		    SubsurfaceData);

		if (client) {
			/* Free all subsurface actions involving this
			   subsurface.  */

			action = client->actions.next;

			while (action != &client->actions) {
				last   = action;
				action = action->next;

				if (last->subsurface == subsurface)
					DestroySurfaceAction(last);
			}
		}

		subsurface->parent->subsurfaces =
		    XLListRemove(subsurface->parent->subsurfaces, surface);
		XLSurfaceCancelCommitCallback(subsurface->commit_callback);

		/* According to the spec, this removal should take effect
		   immediately.  */
		if (subcompositor)
			SubsurfaceUpdate(surface, role);
	}

	/* Destroy the backing data of the subsurface.  */
	DestroyBacking(subsurface);

	/* Update whether or not idle inhibition should continue.  */
	XLDetectSurfaceIdleInhibit();
}

static void
ReleaseBuffer(Surface *surface, Role *role, ExtBuffer *buffer)
{
	Subsurface *subsurface;

	subsurface = SubsurfaceFromRole(role);

	if (!subsurface->parent || !subsurface->parent->role) {
		XLReleaseBuffer(buffer);
		return;
	}

	subsurface->parent->role->funcs.release_buffer(subsurface->parent,
	    subsurface->parent->role, buffer);
}

static void
HandleSubsurfaceResourceDestroy(struct wl_resource *resource)
{
	Subsurface *subsurface;

	subsurface = wl_resource_get_user_data(resource);
	DestroyBacking(subsurface);
}

static Surface *
GetRootSurface(Surface *surface)
{
	Subsurface *subsurface;

	if (surface->role_type != SubsurfaceType || !surface->role)
		return surface;

	subsurface = SubsurfaceFromRole(surface->role);

	if (!subsurface->parent)
		return surface;

	return GetRootSurface(subsurface->parent);
}

static void
GetSubsurface(struct wl_client *client, struct wl_resource *resource,
    uint32_t id, struct wl_resource *surface_resource,
    struct wl_resource *parent_resource)
{
	Surface	   *surface, *parent;
	Subsurface *subsurface;

	surface = wl_resource_get_user_data(surface_resource);
	parent	= wl_resource_get_user_data(parent_resource);

	/* If the surface already has a role, don't attach this subsurface.
	   Likewise if the surface previously held some other role.  */

	if (surface->role ||
	    (surface->role_type != AnythingType &&
		surface->role_type != SubsurfaceType)) {
		wl_resource_post_error(resource,
		    WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
		    "trying to attach subsurface to surface with role");
		return;
	}

	/* Check that a parent loop won't happen.  */

	if (parent == surface) {
		wl_resource_post_error(resource,
		    WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
		    "trying to attach subsurface to itself");
		return;
	}

	if (GetRootSurface(parent) == surface) {
		wl_resource_post_error(resource,
		    WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
		    "specified parent is ancestor of subsurface");
		return;
	}

	subsurface = XLSafeMalloc(sizeof *subsurface);

	if (!subsurface) {
		wl_resource_post_no_memory(resource);
		return;
	}

	memset(subsurface, 0, sizeof *subsurface);
	subsurface->role.resource = wl_resource_create(client,
	    &wl_subsurface_interface, wl_resource_get_version(resource), id);

	if (!subsurface->role.resource) {
		XLFree(subsurface);
		wl_resource_post_no_memory(resource);
		return;
	}

	wl_resource_set_implementation(subsurface->role.resource,
	    &wl_subsurface_impl, subsurface, HandleSubsurfaceResourceDestroy);

	/* Now the wl_resource holds a reference to the subsurface.  */
	subsurface->refcount++;

	subsurface->role.funcs.commit		 = Commit;
	subsurface->role.funcs.teardown		 = Teardown;
	subsurface->role.funcs.setup		 = Setup;
	subsurface->role.funcs.release_buffer	 = ReleaseBuffer;
	subsurface->role.funcs.subsurface_update = SubsurfaceUpdate;
	subsurface->role.funcs.early_commit	 = EarlyCommit;
	subsurface->role.funcs.get_window	 = GetWindow;
	subsurface->role.funcs.rescale		 = Rescale;
	subsurface->role.funcs.parent_rescale	 = ParentRescale;

	subsurface->parent = parent;

	/* Note that for subsurfaces to be attached in the correct order,
	   commit callbacks must be run in the order they were created.  */
	subsurface->commit_callback = XLSurfaceRunAtCommit(parent,
	    AfterParentCommit, subsurface);
	subsurface->synchronous	    = True;

	if (!XLSurfaceAttachRole(surface, &subsurface->role))
		abort();
}

static void
DestroySubcompositor(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_subcompositor_interface wl_subcompositor_impl = {
	.destroy	= DestroySubcompositor,
	.get_subsurface = GetSubsurface,
};

static void
HandleBind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_subcompositor_interface,
	    version, id);

	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &wl_subcompositor_impl, NULL,
	    NULL);
}

void
XLInitSubsurfaces(void)
{
	global_subcompositor = wl_global_create(compositor.wl_display,
	    &wl_subcompositor_interface, 1, NULL, HandleBind);
}

void
XLSubsurfaceParentDestroyed(Role *role)
{
	Subsurface *subsurface;

	subsurface = SubsurfaceFromRole(role);

	/* The callback is freed with the parent.  */
	subsurface->commit_callback = NULL;

	if (subsurface->role.surface) {
		/* Unparent the view.  The parent is responsible for clearing
		   the subcompositor.  */
		ViewUnparent(subsurface->role.surface->view);
		ViewUnparent(subsurface->role.surface->under);
	}

	subsurface->parent = NULL;
}

void
XLSubsurfaceHandleParentCommit(Surface *parent)
{
	SurfaceActionClientData *client;

	/* Note that these actions will also work for pending subsurfaces,
	   as they will be attached by the time this is called.  */

	client = XLSurfaceFindClientData(parent, SubsurfaceData);

	if (client)
		RunSurfaceActions(&client->actions);
}

void
XLUpdateOutputsForChildren(Surface *parent, int base_x, int base_y)
{
	XLList	   *item;
	Subsurface *subsurface;
	Surface	   *child;
	int	    output_x, output_y, output_width, output_height;

	for (item = parent->subsurfaces; item; item = item->next) {
		child	   = item->data;
		subsurface = SubsurfaceFromRole(child->role);
		output_x   = (subsurface->current_substate.x * parent->factor);
		output_y   = (subsurface->current_substate.y * parent->factor);
		output_width  = ViewWidth(child->view);
		output_height = ViewHeight(child->view);

		XLUpdateSurfaceOutputs(child, base_x + output_x,
		    base_y + output_y, output_width, output_height);

		/* Record those values in the child.  */
		subsurface->output_x	  = output_x;
		subsurface->output_y	  = output_y;
		subsurface->output_width  = output_width;
		subsurface->output_height = output_height;
	}
}

void
XLUpdateDesynchronousChildren(Surface *parent, int *n_children)
{
	XLList	   *item;
	Subsurface *subsurface;
	Surface	   *child;

	for (item = parent->subsurfaces; item; item = item->next) {
		child	   = item->data;
		subsurface = SubsurfaceFromRole(child->role);

		if (!subsurface->synchronous)
			/* The subsurface is desynchronous, so add it to the
			   number of desynchronous children.  */
			*n_children += 1;

		/* Update these numbers recursively as well.  */
		XLUpdateDesynchronousChildren(child, n_children);
	}
}

Surface *
XLSubsurfaceGetRoot(Surface *surface)
{
	return GetRootSurface(surface);
}
