import ctypes, os, sys

os.environ["DDEGL_DEBUG"] = "1"
lib = ctypes.CDLL("/usr/local/hybris/usr/lib/angle/libEGL.so")

for fn, rest, args in [
    ("eglGetPlatformDisplay", ctypes.c_void_p, [ctypes.c_uint, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]),
    ("eglInitialize", ctypes.c_uint, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]),
    ("eglGetError", ctypes.c_int, []),
    ("eglQueryString", ctypes.c_char_p, [ctypes.c_void_p, ctypes.c_int]),
]:
    f = getattr(lib, fn)
    f.restype, f.argtypes = rest, args

x11 = ctypes.CDLL("libX11.so.6")
x11.XOpenDisplay.restype = ctypes.c_void_p
x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
xdpy = x11.XOpenDisplay(None)
print("XOpenDisplay ->", hex(xdpy or 0))

PAA = 0x3202
NONE_ = 0x3038
def probe(name, attribs, native):
    a = (ctypes.c_void_p * len(attribs))(*[ctypes.c_void_p(x) for x in attribs])
    lib.eglGetError()
    d = lib.eglGetPlatformDisplay(PAA, ctypes.c_void_p(native), a)
    e1 = lib.eglGetError()
    if not d:
        print(f"{name}: display=NULL getDisplay_err=0x{e1:04x}")
        return
    maj, mino = ctypes.c_int(), ctypes.c_int()
    lib.eglGetError()
    r = lib.eglInitialize(d, ctypes.byref(maj), ctypes.byref(mino))
    e2 = lib.eglGetError()
    v = lib.eglQueryString(d, 0x3053) if r else b""
    print(f"{name}: dpy={d:#x} init={r} {maj.value}.{mino.value} init_err=0x{e2:04x} vendor={v}")

# Attempt 1 from the DT16 case: {TYPE=OPENGLES(0x320e), DEVICE_TYPE=0x3209,EGL_ANGLE(0x348e)}
A1 = [0x3203, 0x320E, 0x3209, 0x348E, NONE_]
# ...with DEBUG_LAYERS=0 appended (Chromium appends when not official)
# EGL_PLATFORM_ANGLE_DEBUG_LAYERS_ANGLE = 0x345B
A1b = [0x3203, 0x320E, 0x3209, 0x348E, 0x345B, 0, NONE_]
# Attempts 2/3 from the case: DEVICE_TYPE 0x345e with worker types 0x320d / 0x3489
A2 = [0x3203, 0x320D, 0x3209, 0x345E, NONE_]
A3 = [0x3203, 0x3489, 0x3209, 0x345E, NONE_]
# Raw enum sanity: what does DEVICE_TYPE value 0x345e validate as?
A4 = [0x3203, 0x320E, 0x3209, 0x345E, NONE_]

for name, attrs in [("A1+X11", A1), ("A1+NULL", A1), ("A1b+X11", A1b), ("A2+X11", A2), ("A3+X11", A3), ("A4+X11", A4)]:
    try:
        probe(name, attrs, xdpy if name.endswith("X11") else 0)
    except Exception as e:
        print(name, "EXC", e)

ext = lib.eglQueryString(None, 0x3055)
print("client_ext len:", len(ext) if ext else -1)
print("has device_type_egl:", b"device_type_egl" in (ext or b""))
