`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================

======================================================

Build deps
---------------

Taken from (https://github.com/termux/termux-packages/issues/10103#issuecomment-1333002785).

  $ pkg update && pkg upgrade

  $ pkg install -y x11-repo

  $ pkg install -y clang lld binutils cmake autoconf automake libtool '*ndk*' make python git libandroid-shmem-static ninja llvm bison flex libx11 xorgproto libdrm libpixman libxfixes libjpeg-turbo xtrans libxxf86vm xorg-xrandr xorg-font-util xorg-util-macros libxfont2 libxkbfile libpciaccess xcb-util-renderutil xcb-util-image xcb-util-keysyms xcb-util-wm xorg-xkbcomp xkeyboard-config libxdamage libxinerama libxshmfence

  $ pip install meson mako pyyaml

Build Mesa Zink
---------------

Build Mesa Zink using this repository (https://github.com/alexvorxx/zink-xlib-termux).

Go to the folder with Mesa code and run the commands:

  $ LDFLAGS='-l:libandroid-shmem.a -llog' meson . build -Dgallium-va=disabled -Dgallium-drivers=virgl,zink,swrast -Ddri3=disabled -Dvulkan-drivers= -Dglx=xlib -Dplatforms=x11 -Dllvm=disabled -Dbuildtype=release
  
  $ ninja -C build install

Usage Zink
---------------

To use Zink with built-in Android driver:

  $ pkg install vulkan-loader-android

To use Zink with Turnip Freedreno driver:

  $ pkg install mesa-vulkan-icd-freedreno

You also may compile it yourself.

======================================================

Source
------

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.

Other repositories are likely forks, and code found there is not supported.  

Support
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://docs.mesa3d.org/bugs.html>`_).


Contributing
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://docs.mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
