/*
 * Nikon Z 6III -- the serial-NOR controller at 0x0e007000, for writes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The RTOS reads the NOR through its memory window (the image at
 * 0x40160000, SNOR_USER at 0x45980000) and never touches the controller
 * until it saves something. Then it runs helpers it keeps in RAM
 * (`.data`, flat 0x455cb01c..0x455cd1d8, executed at 0xa02bda01c..) --
 * the window is unusable while the flash is busy -- and those drive the
 * controller with SPI-NOR opcodes in the top byte of +0x90:
 *
 *   0x06000001 write enable          0x04000001 write disable
 *   0x50000001 clear flag status     0x05930401 read status (+0xa0 bit 0
 *   0x70930401 read flag status                 busy, bit 1 write-enabled)
 *              (+0xa0 bit 7 ready, bit 5 erase error)
 *   0xdc0b0001 / 0x5c0b0001 / 0x210b0001  erase 64 / 32 / 4 KiB at +0x94
 *
 * each followed by polling +0x90 bit 1 until the controller has taken it
 * (sub_455CBF44, erase; sub_455CC0F4, the 4/32 KiB variant). A page
 * program (sub_455CBCD0) sets the controller up (+0x00 = 0x010000c9,
 * +0x38 = 0x0c010005, +0x44 = 4, +0x58 = 0), puts the flash offset in +0x1c
 * and +0x78, the byte count (at most 256, inside one page) in +0x7c and 6
 * in +0x80, writes +0x70 = 1, stores the words through the memory window at
 * 0x40000000 + offset, waits for +0x70 bit 5 and clears it by writing it.
 *
 * This is ../z9-qemu's NOR model (WonderLantern, GPL-2.0-or-later; its
 * nor-model, "functional controller contract recovered from the original
 * copied helper"), whose firmware runs the same helpers; the contract was
 * re-read from this image (the program set-up above is word for word the
 * same check) and is moved into the board, which owns the window. The z9
 * attachment still refuses its nor-model.
 *
 * What it writes: the SNOR_USER body (-machine snor-user=), in memory --
 * the file is never written back. An erase or program anywhere else in the
 * 128 MiB (the loader or the firmware image) stops the machine: nothing
 * here has been seen to want it, and it would rewrite the image.
 *
 * DECLARED CHOICES, all z9's: a command is taken 1 us after it is written,
 * an erase or a page program completes 200 us later, and the flash is
 * otherwise ideal (no failures). Opcodes not listed above, and the
 * controller's other registers (+0xb8, +0xbc: a calibration sequence in
 * sub_455CB8CC, not reached so far), still stop with their PC.
 */

#ifndef HW_ARM_NIKON_Z63_NOR_H
#define HW_ARM_NIKON_Z63_NOR_H

#define Z63_NOR_BASE        0x0e007000ULL
#define Z63_NOR_SIZE        0x100
#define Z63_NOR_FLASH_VA    0x40000000ULL    /* the memory window */
#define Z63_NOR_FLASH_SIZE  (128 * MiB)
#define Z63_NOR_CMD_NS      1000
#define Z63_NOR_MEDIA_NS    200000
#define Z63_NOR_LOG         32

typedef struct Z63Nor {
    MemoryRegion mr;
    uint32_t regs[Z63_NOR_SIZE / 4];
    QEMUTimer *cmd_timer, *media_timer;
    /* The writable part of the flash: SNOR_USER's body. */
    uint8_t **media;
    uint32_t media_offset, media_size;
    bool wel, busy, program_active;
    uint32_t flags, response, command, cmd_address;
    uint32_t offset, length, received, erase_length;
    uint8_t program[256];
    uint64_t commands, erases, programs, logged;
} Z63Nor;

static bool z63_nor_config_offset(unsigned off)
{
    switch (off) {
    case 0x00: case 0x04: case 0x08: case 0x0c: case 0x10: case 0x14:
    case 0x1c: case 0x38: case 0x40: case 0x44: case 0x50: case 0x54:
    case 0x58: case 0x78: case 0x7c: case 0x80: case 0x94: case 0xa8:
    case 0xb0: case 0xb4:
        return true;
    default:
        return false;
    }
}

static bool z63_nor_known_command(uint32_t command)
{
    switch (command | 1) {
    case 0x06000001: case 0x04000001: case 0x50000001:
    case 0x05930401: case 0x70930401:
    case 0xdc0b0001: case 0x5c0b0001: case 0x210b0001:
        return true;
    default:
        return false;
    }
}

/* The SNOR_USER bytes behind flash [offset, offset + len), or NULL with a
 * stop if the range is anywhere else. */
static uint8_t *z63_nor_media(Z63Nor *n, uint32_t offset, uint32_t len,
                              const char *what)
{
    if (offset < n->media_offset ||
        offset + (uint64_t)len > n->media_offset + (uint64_t)n->media_size) {
        error_report("Z63_NOR: %s of flash +0x%x (%u bytes) is outside "
                     "SNOR_USER (+0x%x..+0x%x) -- it would rewrite the "
                     "loader or the firmware image; not modelled, stopping",
                     what, offset, len, n->media_offset,
                     n->media_offset + n->media_size);
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_PANIC, 3);
        return NULL;
    }
    if (!*n->media) {
        /* No body loaded: the region reads zero; give the flash its
         * erased state only where it is written. */
        *n->media = g_malloc0(n->media_size);
    }
    return *n->media + (offset - n->media_offset);
}

static void z63_nor_log(Z63Nor *n, const char *what)
{
    if (n->logged++ < Z63_NOR_LOG) {
        error_report("Z63_NOR: %s flash +0x%x, %u bytes (erases %" PRIu64
                     ", programs %" PRIu64 ")%s", what, n->offset, n->length,
                     n->erases, n->programs,
                     n->logged == Z63_NOR_LOG ? " -- further operations not "
                     "logged" : "");
    }
}

static void z63_nor_media_complete(void *opaque)
{
    Z63Nor *n = opaque;
    uint8_t *m;

    if (n->erase_length) {
        m = z63_nor_media(n, n->offset, n->erase_length, "erase");
        if (m) {
            memset(m, 0xff, n->erase_length);
        }
        n->erases++;
        z63_nor_log(n, "erased");
        n->erase_length = 0;
    } else if (n->program_active && n->received == n->length) {
        m = z63_nor_media(n, n->offset, n->length, "program");
        if (m) {
            for (unsigned i = 0; i < n->length; i++) {
                m[i] &= n->program[i];
            }
        }
        n->programs++;
        if (n->programs == 1 || !(n->programs % 256)) {
            z63_nor_log(n, "programmed");
        }
        n->program_active = false;
        n->regs[0x70 / 4] |= 0x20;
    }
    n->busy = false;
    n->wel = false;
    n->flags |= 0x80;
}

static void z63_nor_command_complete(void *opaque)
{
    Z63Nor *n = opaque;
    uint32_t command = n->command;
    unsigned bytes;

    switch (command) {
    case 0x06000001:
        n->wel = true;
        break;
    case 0x04000001:
        n->wel = false;
        break;
    case 0x50000001:
        n->flags = n->busy ? 0 : 0x80;
        break;
    case 0x05930401:
        n->response = n->busy | (n->wel ? 2 : 0);
        break;
    case 0x70930401:
        n->response = n->flags;
        break;
    case 0xdc0b0001:
    case 0x5c0b0001:
    case 0x210b0001:
        bytes = command == 0xdc0b0001 ? 64 * KiB :
                command == 0x5c0b0001 ? 32 * KiB : 4 * KiB;
        if (!n->wel || n->busy || (n->cmd_address & (bytes - 1)) ||
            n->cmd_address > Z63_NOR_FLASH_SIZE - bytes) {
            n->flags |= 0x20;
            break;
        }
        n->wel = false;
        n->busy = true;
        n->offset = n->cmd_address;
        n->flags &= ~0x80;
        n->erase_length = n->length = bytes;
        timer_mod(n->media_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  Z63_NOR_MEDIA_NS);
        break;
    default:
        g_assert_not_reached();
    }
    n->regs[0x90 / 4] = command & ~3u;
    n->commands++;
}

static uint64_t z63_nor_read(void *opaque, hwaddr off, unsigned size)
{
    Z63Nor *n = opaque;

    if (size == 4 && !(off & 3) &&
        (z63_nor_config_offset(off) || off == 0x70 || off == 0x90)) {
        return n->regs[off / 4];
    }
    if (size == 4 && off == 0xa0) {
        return n->response;
    }
    z63_stop_read("NOR controller", Z63_NOR_BASE + off, size);
    return 0;
}

static void z63_nor_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    Z63Nor *n = opaque;

    if (size != 4 || (off & 3)) {
        goto refuse;
    }
    if (off == 0x90) {
        if (!z63_nor_known_command(value) || timer_pending(n->cmd_timer)) {
            goto refuse;
        }
        n->regs[off / 4] = value;
        if (value & 1) {
            n->command = value;
            n->cmd_address = n->regs[0x94 / 4];
            n->regs[off / 4] = (value & ~1u) | 2;      /* taken: busy */
            timer_mod(n->cmd_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      Z63_NOR_CMD_NS);
        }
        return;
    }
    if (off == 0x70) {
        uint32_t offset = n->regs[0x78 / 4], length = n->regs[0x7c / 4];

        if (value & ~0x21u) {
            goto refuse;
        }
        if (value & 0x20) {
            n->regs[off / 4] &= ~0x20u;
        }
        if (!(value & 1)) {
            return;
        }
        /* The page-program set-up sub_455CBCD0 makes, and nothing else. */
        if (n->busy || n->program_active || !length || length > 256 ||
            (length & 3) || (offset & 3) || (offset & 255) + length > 256 ||
            offset > Z63_NOR_FLASH_SIZE - length ||
            offset != n->regs[0x1c / 4] || n->regs[0] != 0x010000c9 ||
            n->regs[0x38 / 4] != 0x0c010005 || n->regs[0x44 / 4] != 4 ||
            n->regs[0x58 / 4] != 0 || n->regs[0x80 / 4] != 6) {
            goto refuse;
        }
        n->program_active = n->busy = n->wel = true;
        n->offset = offset;
        n->length = length;
        n->received = 0;
        n->flags &= ~0x80;
        n->regs[off / 4] &= ~0x20u;
        return;
    }
    if (z63_nor_config_offset(off)) {
        if (n->busy &&
            (off == 0x94 || off == 0x78 || off == 0x7c || off == 0x1c)) {
            goto refuse;
        }
        n->regs[off / 4] = value;
        return;
    }
refuse:
    z63_stop_write("NOR controller", Z63_NOR_BASE + off, size, value);
}

static const MemoryRegionOps z63_nor_ops = {
    .read = z63_nor_read,
    .write = z63_nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* A write through the memory window at flash offset `offset`: taken only
 * as the next word of an active page program. */
static bool z63_nor_window_write(Z63Nor *n, uint32_t offset, uint64_t value,
                                 unsigned size)
{
    if (!n->program_active || size != 4 || n->received >= n->length ||
        offset != n->offset + n->received) {
        return false;
    }
    stl_le_p(n->program + n->received, value);
    n->received += 4;
    if (n->received == n->length) {
        timer_mod(n->media_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  Z63_NOR_MEDIA_NS);
    }
    return true;
}

static void z63_nor_init(Object *owner, MemoryRegion *sysmem, Z63Nor *n,
                         uint8_t **media, uint32_t media_offset,
                         uint32_t media_size)
{
    n->media = media;
    n->media_offset = media_offset;
    n->media_size = media_size;
    n->flags = 0x80;
    n->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_nor_command_complete,
                                n);
    n->media_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_nor_media_complete,
                                  n);
    memory_region_init_io(&n->mr, owner, &z63_nor_ops, n, "z63-nor-ctrl",
                          Z63_NOR_SIZE);
    memory_region_add_subregion(sysmem, Z63_NOR_BASE, &n->mr);
}

#endif /* HW_ARM_NIKON_Z63_NOR_H */
