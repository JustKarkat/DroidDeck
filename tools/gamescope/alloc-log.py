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
import re
import sys

PATH = "src/rendervulkan.cpp"
MARK = "0130-diag"

with open(PATH) as f:
    src = f.read()

if MARK in src:
    print("alloc-log: already instrumented")
    sys.exit(0)


def insert_after(pattern, block, what):
    """Insert `block` after the first line matching `pattern` (regex)."""
    global src
    rx = re.compile(pattern)
    for m in rx.finditer(src):
        end = src.find("\n", m.start())
        if end < 0:
            break
        src = src[: end + 1] + block + src[end + 1 :]
        return
    sys.exit(f"alloc-log: anchor not found for {what}: {pattern!r}")


# 1) Texture memory: which dimensions/format produce the big allocations.
insert_after(
    r"GetImageMemoryRequirements\s*\(\s*g_device\.device\(\)\s*,\s*m_vkImage",
    '\tif ( memRequirements.size > ( 8u << 20 ) )\n'
    '\t\tvk_log.infof( "0130-diag: tex %ux%ux%u fmt=%#x size=%lluMB align=%llu typeBits=%#x",\n'
    "\t\t\twidth, height, depth, (uint32_t)m_format,\n"
    "\t\t\t(unsigned long long)( memRequirements.size >> 20 ),\n"
    "\t\t\t(unsigned long long)memRequirements.alignment,\n"
    "\t\t\t(uint32_t)memRequirements.memoryTypeBits );\n",
    "texture memory",
)

# 2) The raw Vulkan allocation, in case it is not texture-driven.
insert_after(
    r"vk\.AllocateMemory\(\s*g_device\.device\(\)\s*,\s*&allocInfo\s*,\s*nullptr\s*,\s*&memoryHandle",
    '\tif ( allocInfo.allocationSize > ( 8u << 20 ) )\n'
    '\t\tvk_log.infof( "0130-diag: vkAllocateMemory size=%lluMB type=%u",\n'
    "\t\t\t(unsigned long long)( allocInfo.allocationSize >> 20 ),\n"
    "\t\t\t(uint32_t)allocInfo.memoryTypeIndex );\n",
    "vkAllocateMemory",
)

with open(PATH, "w") as f:
    f.write(src)

if MARK not in src:
    sys.exit("alloc-log: instrumentation missing after write")
print("alloc-log: instrumented", PATH)
