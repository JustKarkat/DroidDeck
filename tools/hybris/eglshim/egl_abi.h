/* egl_abi.h — minimal self-contained EGL ABI declarations for the shim.
 *
 * We deliberately do NOT include any system EGL header: the build container
 * carries hybris' Android-flavored egl.h whose native types conflict with the
 * exported signatures, and the real consumers (ANGLE) call by symbol + ABI
 * anyway. Handles are pointers, EGLint is int32, EGLAttrib is intptr,
 * EGLTime is uint64 — the Khronos aarch64 ABI.
 */
#ifndef EGL_ABI_H
#define EGL_ABI_H

#include <stdint.h>

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef void *EGLSync;
typedef void *EGLImage;
typedef void *EGLClientBuffer;
typedef void *__eglMustCastToProperFunctionPointerType;

typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef intptr_t EGLAttrib;
typedef uint64_t EGLTime;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_IMAGE ((EGLImage)0)
#define EGL_NO_SYNC ((EGLSync)0)
#define EGL_DONT_CARE ((EGLint)-1)

#define EGL_FALSE 0u
#define EGL_TRUE 1u

#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_WINDOW_BIT 0x0004
#define EGL_PIXMAP_BIT 0x0002
#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_ES_BIT 0x0001
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_OPENGL_BIT 0x0008
#define EGL_TEXTURE_FORMAT 0x3080
#define EGL_NO_TEXTURE 0x305C
#define EGL_DRAW 0x3059
#define EGL_READ 0x308A
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CLIENT_APIS 0x308D

#endif
