/* Tests for the Wayland compositor running on the X server.

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

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <string.h>

#include <sys/types.h>

#include <wayland-client.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "12to11-test.h"

struct test_seat
{
  /* The test seat, if any.  */
  struct test_seat_controller *controller;

  /* The device manager, if any.  */
  struct test_device_controller *device_controller;

  /* The seat resource itself.  */
  struct wl_seat *seat;

  /* The wl_pointer resource.  */
  struct wl_pointer *pointer;

  /* The keyboard resource.  */
  struct wl_keyboard *keyboard;

  /* The device ID of the seat.  */
  uint32_t device_id;

  /* The buttons currently held down.  */
  unsigned char buttons;
};

struct test_display
{
  /* The wayland display.  */
  struct wl_display *display;

  /* The X display.  */
  Display *x_display;

  /* List of pixmap formats.  */
  XPixmapFormatValues *pixmap_formats;

  /* Number of pixmap formats.  */
  int num_pixmap_formats;

  /* The registry and various Wayland interfaces.  */
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct test_manager *test_manager;

  /* The test scale lock.  */
  struct test_scale_lock *scale_lock;

  /* Test interfaces.  */
  struct test_interface *interfaces;

  /* The test seat.  */
  struct test_seat *seat;

  /* The number of such interfaces.  */
  int num_test_interfaces;

  /* Internal field used by test_get_serial.  */
  uint32_t serial;
};

struct test_interface
{
  /* The name of the interface.  */
  const char *interface;

  /* Pointer to the interface data.  */
  void *data;

  /* Pointer to the interface.  */
  const struct wl_interface *c_interface;

  /* The wanted version.  */
  uint32_t version;
};

struct test_buffer
{
  /* The associated struct wl_buffer.  */
  struct wl_buffer *buffer;

  /* Whether or not the buffer is busy.  */
  int is_busy;
};

struct image_data_header
{
  /* Currently 1.  High bit is byte order.  */
  unsigned char version;

  /* The data format.  Currently always 0.  */
  unsigned char format;

  /* The width and height.  */
  unsigned short width, height;

  /* Padding.  */
  unsigned short pad1;

  /* The stride.  */
  unsigned int stride;
};

enum image_data_format
  {
    /* Little-endian ARGB8888.  */
    IMAGE_DATA_ARGB8888_LE,
    /* Little-endian XRGB8888.  */
    IMAGE_DATA_XRGB8888_LE,
  };

extern void die (const char *) __attribute__ ((noreturn));
extern struct test_display *open_test_display (struct test_interface *, int);
extern int get_shm_file_descriptor (void);
extern size_t get_image_stride (struct test_display *, int, int);
extern struct wl_buffer *upload_image_data (struct test_display *,
					    const char *, int, int,
					    int);
extern void report_test_failure (const char *, ...)
  __attribute__ ((noreturn, format (gnu_printf, 1, 2)));
extern void test_log (const char *, ...)
  __attribute__ ((format (gnu_printf, 1, 2)));

extern bool make_test_surface (struct test_display *, struct wl_surface **,
			       struct test_surface **);
extern struct wl_buffer *load_png_image (struct test_display *, const char *);
extern unsigned char *load_image_data (const char *,
				       struct image_data_header *);
extern void verify_image_data (struct test_display *, Window, const char *);
extern void test_init (void);
extern void test_complete (void) __attribute__ ((noreturn));
extern void test_set_scale (struct test_display *, int);
extern void test_init_seat (struct test_display *);
extern uint32_t test_get_serial (struct test_display *);
extern void verify_window_size (struct test_display *, Window, int, int);
extern struct test_buffer *get_test_buffer (struct test_display *,
					    struct wl_buffer *);
extern void test_buffer_committed (struct test_buffer *);
extern void verify_buffer_released (struct test_buffer *);

#define ARRAYELTS(arr) (sizeof (arr) / sizeof (arr)[0])

#if __GNUC__ >= 7
#define FALLTHROUGH __attribute__ ((fallthrough))
#else
#define FALLTHROUGH ((void) 0)
#endif
