#!/usr/bin/env python3
"""xwayland-host.py — start Xwayland on a socket we create ourselves.

Why: Xwayland takes /tmp/.X<n>-lock by creating a temp file and link()ing it
into place. In the guest the X socket directory lives on /data, where hard
links are denied for the app uid ("ln: Permission denied"), and -nolock is
refused because the server only honours it for root (the guest runs as
uid 10018). Either one aborts startup with:

    (EE) Linking lock file (/tmp/.X<n>-lock) in place failed: Operation not permitted

gamescope sidesteps exactly this by creating the socket and passing it in via
-listenfd. We do the same: bind /tmp/.X11-unix/X<n>, hand the fd to Xwayland,
and let it serve without ever touching the lock.

Usage: xwayland-host.py <display-number> [extra Xwayland args...]
"""
import os
import socket
import sys

n = sys.argv[1] if len(sys.argv) > 1 else "9"
extra = sys.argv[2:]

sock_dir = "/tmp/.X11-unix"
path = f"{sock_dir}/X{n}"
os.makedirs(sock_dir, exist_ok=True)
try:
    os.unlink(path)
except FileNotFoundError:
    pass
except PermissionError:
    pass

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(64)
os.chmod(path, 0o777)

fd = s.fileno()
if fd != 3:
    os.dup2(fd, 3)
    s.close()
os.set_inheritable(3, True)

argv = ["Xwayland", f":{n}", "-terminate", "-listenfd", "3",
        "-noTouchPointerEmulation", "-force-xrandr-emulation"] + extra
os.execvp(argv[0], argv)
