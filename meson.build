project('waylandx',
  'c',
  version : '0.1',
  meson_version: '>=1.6.0', # Wayland module.
  license: 'GPL-3.0-or-later',
  default_options : [
    'c_std=c99',
    'warning_level=2',
    'werror=false'
  ]
)

host = host_machine.system()
if host_machine.system() != 'linux'
  error('Your host purports to be @0@, however only linux is supported'.format(
    host_machine.system())
  )
endif

add_project_arguments(
  [
    '-fno-common',
    '-Warith-conversion',
    '-Wdate-time',
    '-Wdisabled-optimization',
    '-Wdouble-promotion',
    '-Wduplicated-cond',
    '-Wformat-signedness',
    '-Winit-self',
    '-Winvalid-pch',
    '-Wlogical-op',
    '-Wmissing-declarations',
    '-Wmissing-include-dirs',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wnull-dereference',
    '-Wold-style-definition',
    '-Wopenmp-simd',
    '-Wpacked',
    '-Wpointer-arith',
    '-Wstrict-prototypes',
    '-Wsuggest-attribute=format',
    '-Wsuggest-attribute=noreturn',
    '-Wsuggest-final-methods',
    '-Wsuggest-final-types',
    '-Wuninitialized',
    '-Wunknown-pragmas',
    '-Wunused-macros',
    '-Wvariadic-macros',
    '-Wvector-operation-performance',
    '-Wwrite-strings',
    '-Warray-bounds=2',
    '-Wattribute-alias=2',
    '-Wformat=2',
    '-Wformat-truncation=2',
    '-Wimplicit-fallthrough=5',
    '-Wshift-overflow=2',
    '-Wuse-after-free=3',
    '-Wvla-larger-than=4031',
    '-Wredundant-decls',
    '-Wno-missing-field-initializers',
    '-Wno-override-init',
    '-Wno-pedantic',
    '-Wno-sign-compare',
    '-Wno-type-limits',
    '-Wno-unused-parameter',
    '-Wno-format-nonliteral',
    '-g3',
  ],
  language: 'c',
)

cc = meson.get_compiler('c')

m_dep = cc.find_library('m', required: false)
if m_dep.found()
    add_project_link_arguments('-lm', language: 'c')
endif

add_project_arguments([
  '-DPortFile="port_gnu.h"',
  '-D_GNU_SOURCE',
  ], language: 'c')

prefix = get_option('prefix')

# Sources
waylandx_sources = [
  '12to11.c',
  'alloc.c',
  'atoms.c',
  'buffer.c',
  'buffer_release.c',
  'compositor.c',
  'data_device.c',
  'decoration.c',
  'dmabuf.c',
  'dnd.c',
  'drm_lease.c',
  'ewmh.c',
  'explicit_synchronization.c',
  'fence_ring.c',
  'fns.c',
  'frame_clock.c',
  'icon_surface.c',
  'idle_inhibit.c',
  'keyboard_shortcuts_inhibit.c',
  'output.c',
  'picture_renderer.c',
  'pointer_constraints.c',
  'pointer_gestures.c',
  'positioner.c',
  'primary_selection.c',
  'process.c',
  'region.c',
  'relative_pointer.c',
  'renderer.c',
  'run.c',
  'seat.c',
  'select.c',
  'shm.c',
  'single_pixel_buffer.c',
  'subcompositor.c',
  'subsurface.c',
  'surface.c',
  'sync_source.c',
  'tearing_control.c',
  'test.c',
  'text_input.c',
  'time.c',
  'timer.c',
  'transform.c',
  'wp_viewporter.c',
  'xdata.c',
  'xdg_activation.c',
  'xdg_popup.c',
  'xdg_surface.c',
  'xdg_toplevel.c',
  'xdg_wm.c',
  'xerror.c',
  'xsettings.c',
]

# Headers
headers = [
  'fcntl.h',
  'float.h',
  'limits.h',
  'stdlib.h',
  'string.h',
  'time.h',
  'sys/stat.h',
  'sys/wait.h',
  'unistd.h',
]

foreach h : headers
  if not cc.has_header(h)
    error('Cannot find header file: ' + h)
  endif
endforeach

mandatory_deps_found = []
summary_dep_found = {}

mandatory_deps = [
  'libdrm',
  'pixman-1',
  'wayland-client',
  'wayland-scanner',
  'wayland-server',
  'x11',
  'x11-xcb',
  'xcb-dri3',
  'xcb-randr',
  'xcb-shape',
  'xcb-shm',
  'xext',
  'xi',
  'xkbfile',
  'xfixes',
  'xpresent',
  'xrandr',
  'xrender',
  'xshmfence',
  'xwaylandproto',
]

foreach rd : mandatory_deps
  this = dependency(rd, required: true)
  summary_dep_found += {rd: this}
  mandatory_deps_found += this

  if rd == 'libdrm'
    pc = run_command(
      'pkg-config',
      '--cflags-only-I',
      rd,
      capture: true,
      check: true,
    )

    # In libdrm's case, there's only one.
    path = pc.stdout().strip().split()[0]
    drm_dir = path.replace('-I', '')
    inc_dirs_drm = include_directories(drm_dir)
  endif
endforeach

thread_dep = dependency('threads', required: true)
summary_dep_found += {'pthread': thread_dep}
mandatory_deps_found += thread_dep

# FIXME: as of meson 1.8.0, this module is considered "stable":
# https://mesonbuild.com/Wayland-module.html
#wl_mod = import('unstable-wayland')

wl_protos = [
  'protocol/12to11-test',
  'protocol/drm-lease-v1',
  'protocol/idle-inhibit-unstable-v1',
  'protocol/keyboard-shortcuts-inhibit-unstable-v1',
  'protocol/linux-dmabuf-unstable-v1',
  'protocol/linux-explicit-synchronization-unstable-v1',
  'protocol/pointer-constraints-unstable-v1',
  'protocol/pointer-gestures-unstable-v1',
  'protocol/primary-selection-unstable-v1',
  'protocol/relative-pointer-unstable-v1',
  'protocol/single-pixel-buffer-v1',
  'protocol/tearing-control-v1',
  'protocol/text-input-unstable-v3',
  'protocol/viewporter',
  'protocol/xdg-activation-v1',
  'protocol/xdg-decoration-unstable-v1',
  'protocol/xdg-shell',
]

wayland_scanner = find_program('wayland-scanner')

if not wayland_scanner.found()
  error('wayland-scanner not found')
endif

wayland_scanner_code = generator(
  wayland_scanner,
  output: '@BASENAME@.c',
  arguments: [
    'private-code',
    '@INPUT@',
    '@OUTPUT@',
  ],
)

wayland_scanner_server = generator(
  wayland_scanner,
  output: '@BASENAME@.h',
  arguments: [
    'server-header',
    '@INPUT@',
    '@OUTPUT@'
  ],
)

foreach x : wl_protos
  waylandx_sources += wayland_scanner_code.process(x + '.xml')
  waylandx_sources += wayland_scanner_server.process(x + '.xml')
endforeach

waylandx_cust_targets = []

egl = dependency('egl', required: get_option('egl'))
gles = ''
if egl.found()

  mandatory_deps_found += egl
  gles = dependency('glesv2', required: true)
  if not gles.found()
    error('elg found, but not gles...')
  endif

  add_project_arguments('-DHaveEglSupport', language: 'c')
  mandatory_deps_found += gles
  generate_shaders = custom_target(
    'generate_shaders',
    input: ['shaders.awk', 'shaders.txt'],
    output: 'shaders.h',
    command: ['awk', '-f', '@INPUT0@', '@INPUT1@'],
    capture: true
  )
  waylandx_sources += ['egl.c']
  waylandx_cust_targets += [generate_shaders]
endif

short_types_txt = custom_target(
  'short_types_txt',
  input: ['meson-scripts/filter-vnd.sed', 'media_types.txt'],
  output: 'short_types.txt',
  command: ['sed', '-f', '@INPUT0@', '@INPUT1@'],
  capture: true
)
waylandx_cust_targets += [short_types_txt]

transfer_atoms_h = custom_target(
  'transfer_atoms_h',
  input: [
    short_types_txt,
    'meson-scripts/mime0.awk',
    'meson-scripts/mime1.awk',
    'meson-scripts/mime2.awk',
    'meson-scripts/mime3.awk',
    'meson-scripts/mime4.awk',
    'meson-scripts/transfer_atoms.sh'
  ],
  output: 'transfer_atoms.h',
  command: ['sh', '@INPUT6@', '@OUTPUT@'],

)
waylandx_cust_targets += [transfer_atoms_h]

drm_modifiers_h = custom_target(
  'drm_modifiers_h',
  input: ['meson-scripts/modifiers.awk', '@0@/drm_fourcc.h'.format(drm_dir)],
  output: 'drm_modifiers.h',
  command: ['awk', '-f', '@INPUT0@', '@INPUT1@'],
  capture: true
)
waylandx_cust_targets += [drm_modifiers_h]

summary(
    summary_dep_found,
    bool_yn: true,
    section: 'Dependencies',
)

featurevals = {
    'EGL support': egl.found() ? egl : false,
    'GLES support': egl.found() and gles.found() ? gles : false,
}
summary(
    featurevals,
    bool_yn: true,
    section: 'Features',
)

waylandx = executable(
  meson.project_name(),
  waylandx_sources + waylandx_cust_targets,
  dependencies: mandatory_deps_found,
  include_directories: [inc_dirs_drm],
  install: true
)
