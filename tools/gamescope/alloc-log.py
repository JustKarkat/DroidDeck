#!/usr/bin/env python3
"""0130-diag: name gamescope's large allocations at build time.

gamescope settles at ~830 MB resident on the Shield TV (software Vulkan /
lavapipe) in guard-paged regions whose contents are heap data. Everything
externally observable has been ruled out (library mmap via LD_PRELOAD,
glibc arenas via MALLOC_ARENA_MAX, thread stacks, shm output buffers), so
the next step is to log the allocations from inside: every texture whose
VkMemoryRequirements exceed 8 MiB, with the dimensions that produced it.

Run from the gamescope source root (the PKGBUILD prepare step does this
after the patch series). Idempotent; exits non-zero if the anchors move so
the build fails loudly instead of shipping a silent binary.
"""
import sys

PATH = "src/rendervulkan.cpp"
MARK = "0130-diag"

with open(PATH) as f:
    src = f.read()

if MARK in src:
    print("alloc-log: already instrumented")
    sys.exit(0)


def insert_after(anchor, block):
    """Insert `block` after the line containing `anchor`."""
    global src
    idx = src.find(anchor)
    if idx < 0:
        sys.exit(f"alloc-log: anchor not found: {anchor!r}")
    end = src.find("\n", idx)
    if end < 0:
        sys.exit("alloc-log: anchor at EOF")
    src = src[: end + 1] + block + src[end + 1 :]


# 1) Texture memory: which dimensions/format produce the big allocations.
insert_after(
    "GetImageMemoryRequirements( g_device.device(), m_vkImage, &memRequirements )",
    '\tif ( memRequirements.size > ( 8u << 20 ) )\n'
    '\t\tvk_log.infof( "0130-diag: tex %ux%ux%u size=%lluMB align=%llu typeBits=%#x",\n'
    "\t\t\twidth, height, depth,\n"
    "\t\t\t(unsigned long long)( memRequirements.size >> 20 ),\n"
    "\t\t\t(unsigned long long)memRequirements.alignment,\n"
    "\t\t\t(uint32_t)memRequirements.memoryTypeBits );\n",
)

# 2) The raw Vulkan allocation, in case it is not texture-driven.
insert_after(
    "res = g_device.vk.AllocateMemory( g_device.device(), &allocInfo, nullptr, &memoryHandle )",
    '\tif ( allocInfo.allocationSize > ( 8u << 20 ) )\n'
    '\t\tvk_log.infof( "0130-diag: vkAllocateMemory size=%lluMB type=%u",\n'
    "\t\t\t(unsigned long long)( allocInfo.allocationSize >> 20 ),\n"
    "\t\t\t(uint32_t)allocInfo.memoryTypeIndex );\n",
)

with open(PATH, "w") as f:
    f.write(src)

if MARK not in src:
    sys.exit("alloc-log: instrumentation missing after write")
print("alloc-log: instrumented", PATH)
