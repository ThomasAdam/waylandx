# Waylandx

Go from Wayland -> X (waylandx), rather than X -> Wayland (xwayland)

```
THIS IS A FORK OF THE FOLLOWING PROJECT:

https://sourceforge.net/projects/twelveto11/

I will be making changes, but for now, wanting to mirror it here, primarily s
I don't have to use SVN.

-- Thomas Adam, 2025-06-29
```

Heed the above.  This not my project originally, but I've decided to see how
far I can support it.

## Original README

The following is the original README file which still applies for now.  I've
formatted it to better fit markdown:

### Building

```
meson setup build
meson compile -C build
```

After which it can be installed:

```
meson install -C build
```

... and the binary:

```
waylandx
```

can be run.  This should be done on top of an existing X11 session.  Assuming
this program doesn't crash, it will then allow you to run wayland
applications, such as `foot`, as a native X11 application.`

***Be sure to configure your system so that idle inhibition is reported
correctly.  For more details, see the description of the
idleInhibitCommand resource in the manual page.***



## About / Info

This is a tool for running Wayland applications on an X server,
preferably with a compositing manager running.

It is not yet complete.  What is not yet implemented includes support
for touchscreens, and device switching in dmabuf feedback.

It is not portable to systems other than recent versions of GNU/Linux
running the X.Org server 1.20 or later, and has not been tested on
window (and compositing) managers other than GNOME Shell.

It will not work very well unless the compositing manager supports the
EWMH frame synchronization protocol.

Building and running this tool requires the following X protocol
extensions:

|Extension|Version|
|---------|-------|
|Nonrectangular Window Shape Extension|>=1.1|
|MIT Shared Memory Extension|>=1.2|
|X Resize, Rotate and Reflect Extension|>1.4|
|X Synchronization Extension|>=1.0|
|X Rendering Extension|>=1.2|
|X Input Extension|>=2.3|
|Direct Rendering Interface 3|>=1.2|
|X Fixes Extension|>=1.5|
|X Presentation Extension|>=1.0|

In addition, it requires Xlib to be built with the XCB transport, and
the XCB bindings for MIT-SHM and DRI3 to be available.

### EGL/GLES suport

EGL/GLES support can be enabled with:

```
meson setup build -Degl=enabled
```

After building with EGL support, the renderer must be enabled by
setting the environment variable "RENDERER" to "egl", or by setting
the "renderer" resource (class "Renderer") to "egl".

### Wayland Protocols

The following Wayland protocols are implemented to a more-or-less
complete degree:

|Protocol Name|Version|
|-------------|-------|
|wl_output                                  |4|
|wl_compositor                              |5|
|wl_shm                                     |1|
|xdg_wm_base                                |5|
|wl_subcompositor                           |1|
|wl_seat                                    |8|
|wl_data_device_manager                     |3|
|zwp_linux_dmabuf_v1                        |4|
|zwp_primary_selection_device_manager_v1    |1|
|wp_viewporter                              |1|
|zxdg_decoration_manager_v1                 |1|
|zwp_text_input_manager_v3                  |1|
|wp_single_pixel_buffer_manager_v1          |1|
|zwp_pointer_constraints_v1                 |1|
|zwp_relative_pointer_manager_v1            |1|
|zwp_idle_inhibit_manager_v1                |1|
|xdg_activation_v1                          |1|
|wp_tearing_control_manager_v1	            |1|

When built with EGL, the following Wayland protocol is also supported:

|Protocol Name|Version|
|-------------|-------|
|zwp_linux_explicit_synchronization_v1|2|

When the X server supports version 1.6 or later of the X Resize,
Rotate and Reflect Extension, the following Wayland protocol is also
supported:

|Protocol Name|Version|
|-------------|-------|
|wp_drm_lease_device_v1|1|

When the X server supports version 2.4 or later of the X Input
Extension, the following Wayland protocol is also supported:

|Protocol Name|Version|
|-------------|-------|
|zwp_pointer_gestures_v1|3|
