# wlroots [![](https://api.travis-ci.org/swaywm/wlroots.svg)](https://travis-ci.org/swaywm/wlroots)

Pluggable, composable modules for building a
[Wayland](http://wayland.freedesktop.org/) compositor.

This is a WIP: [status](https://github.com/swaywm/wlroots/issues/9)

## Contributing

See [CONTRIBUTING.md](https://github.com/swaywm/wlroots/blob/master/CONTRIBUTING.md)

## Building

Install dependencies:

* wayland
* wayland-protocols
* EGL
* GLESv2
* DRM
* GBM
* libinput
* udev
* pixman
* systemd (optional, for logind support)
* elogind (optional, for logind support on systems without systemd)
* libcap (optional, for capability support)
* asciidoc (optional, for man pages)

Run these commands:

    meson build
    ninja -C build
