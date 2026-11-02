/*
 * Nikon Z 6III -- the I2C masters at 0x0e005000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Four Cadence-style I2C masters, 0x400 apart, interrupts SPI 137..140
 * (I2C_0..3 in the image's name table). Until now this was part of the
 * plain-storage window EMIO-E0, so a transfer was written and never
 * happened: the posture task (POSTSERV) never got an accelerometer sample,
 * CameraState's device readiness lacked its orientation bit, CAPST never
 * reported ready and APPCTL stayed in state 1 -- the chain is in
 * docs/plan.md.
 *
 * The controller below is ../n-research/z8-qemu's src/i2c-controller.h
 * (GPL-2.0-or-later), ported with only renames and this board's stop
 * reporting; its notes (docs/native-i2c-controller.md) give the Z 8
 * driver evidence (40c16df8/40c16f68/40c17134). Re-checked here: the
 * posture task's handle is 0x00030080, bank 3 (0x0e005c00) as on the Z 8,
 * and the registers this image had left in that bank -- control 0x0b6e,
 * address 0x68, timeout 0x1f, mask 0x2e7, disable 0x2ff -- are that
 * controller's layout and bits. What is attached to a bank is the
 * board's (nikon-z63-imu.h); an address nothing answers NACKs, never
 * invented data, and each such address is reported once.
 */

#ifndef HW_ARM_NIKON_Z63_I2C_H
#define HW_ARM_NIKON_Z63_I2C_H


/* Functional Cadence master controller; original 40c16df8/40c16f68/40c17134.
 * Four banks, IRQ a9..ac, FIFO16. The 100us boundary is virtual timing, not
 * a measured I2C rate (-machine i2c-byte-ns= changes it; the driver's
 * divisors, DIV_A 0 / DIV_B 11, give pclk/264, about 24 us a byte at an
 * unmeasured 100 MHz pclk). No endpoint means address NACK, never invented data.
 * See docs/native-i2c-controller.md for native ISR and callback evidence. */
#define Z63_I2C_BASE 0x0e005000ULL
#define Z63_I2C_BANKS 4
#define Z63_I2C_LENGTH 0x1000
#define Z63_I2C_FIFO_DEPTH 16
#define Z63_I2C_STEP_NS 100000
#define Z63_I2C_CR_RW 0x01U
#define Z63_I2C_CR_MS 0x02U
#define Z63_I2C_CR_NEA 0x04U
#define Z63_I2C_CR_ACK 0x08U
#define Z63_I2C_CR_HOLD 0x10U
#define Z63_I2C_CR_CLR_FIFO 0x40U
/* Original 40c16d74 uses bit7 only for object+4 == 3 and config+10 == 1.
 * Its electrical meaning is unproven; configuration can latch it, but an
 * active transfer in this special mode is not implemented. Bit5 is retained
 * for the observed ordinary native 0x22 master configuration. */
#define Z63_I2C_CR_UNSUPPORTED_BANK3_MODE 0x80U
#define Z63_I2C_IRQ_COMP 0x01U
#define Z63_I2C_IRQ_DATA 0x02U
#define Z63_I2C_IRQ_NACK 0x04U
#define Z63_I2C_IRQ_MASK 0x2ffU

/* Explicit board bus router. start/write return the addressed device ACK;
 * read supplies its byte or a bus failure. stop is used only after an ACKed
 * start. These are electrical endpoint methods, never firmware callbacks. */
typedef struct Z63I2cEndpoint {
    bool (*start)(void *opaque, unsigned bank, uint16_t address,
                  bool receive, bool repeated);
    bool (*write)(void *opaque, unsigned bank, uint8_t byte);
    bool (*read)(void *opaque, unsigned bank, uint8_t *byte);
    void (*stop)(void *opaque, unsigned bank);
} Z63I2cEndpoint;

typedef struct Z63I2c Z63I2c;
typedef struct Z63I2cBlock {
    Z63I2c *owner;
    unsigned index;
    QEMUTimer *timer;
    uint32_t control, address, slave_pause, timeout;
    uint32_t isr, imr, receive_left;
    uint8_t tx[Z63_I2C_FIFO_DEPTH], rx[Z63_I2C_FIFO_DEPTH];
    unsigned tx_head, tx_count, rx_head, rx_count;
    bool active, address_pending, bus_started, repeated;
    bool segment_done, tx_empty_announced, irq_level;
    uint64_t starts, tx_bytes, rx_bytes, completions, nacks;
    int64_t step_due;             /* virtual time the pending step is for */
} Z63I2cBlock;

struct Z63I2c {
    MemoryRegion mmio;
    void *owner;
    Z63I2cBlock blocks[Z63_I2C_BANKS];
    qemu_irq irqs[Z63_I2C_BANKS];
    const Z63I2cEndpoint *endpoint;
    void *peripheral;
    uint32_t configuration_writes, acknowledgements, rejected_accesses;
    bool halted;
    uint32_t step_ns;          /* virtual time per bus step, 0: the default */
};

static void z63_i2c_schedule(Z63I2cBlock *b);

static void z63_i2c_update_irq(Z63I2c *s, unsigned bank)
{
    Z63I2cBlock *b = &s->blocks[bank];
    b->irq_level = !s->halted && !!(b->isr & ~b->imr & Z63_I2C_IRQ_MASK);
    if (s->irqs[bank]) {
        qemu_set_irq(s->irqs[bank], b->irq_level);
    }
}

static void z63_i2c_fault(Z63I2c *s, const char *kind, hwaddr offset,
                         uint64_t value, unsigned size)
{
    s->rejected_accesses++;
    s->halted = true;
    for (unsigned bank = 0; bank < Z63_I2C_BANKS; bank++) {
        if (s->blocks[bank].timer) {
            timer_del(s->blocks[bank].timer);
        }
        z63_i2c_update_irq(s, bank);
    }
    error_report("Z63_I2C: %s at 0x%" HWADDR_PRIx " (value 0x%" PRIx64
                 ", size %u) -- not modelled; see nikon-z63-i2c.h", kind,
                 Z63_I2C_BASE + offset, value, size);
    z63_stop_write("I2C", Z63_I2C_BASE + offset, size, value);
}

static bool z63_i2c_register(hwaddr offset, unsigned size)
{
    if (size != 4 || (offset & 3) || offset >= Z63_I2C_LENGTH) {
        return false;
    }
    return (offset & 0x3ff) <= 0x28;
}

static void z63_i2c_stop_bus(Z63I2cBlock *b)
{
    Z63I2c *s = b->owner;
    if (b->bus_started) {
        s->endpoint->stop(s->peripheral, b->index);
    }
    b->bus_started = false;
    b->active = false;
    b->address_pending = false;
}

static void z63_i2c_nack(Z63I2cBlock *b)
{
    b->isr |= Z63_I2C_IRQ_NACK;
    b->nacks++;
    z63_i2c_stop_bus(b);
    z63_i2c_update_irq(b->owner, b->index);
}

static void z63_i2c_complete(Z63I2cBlock *b)
{
    if (!b->segment_done) {
        b->isr |= Z63_I2C_IRQ_COMP;
        b->completions++;
        b->segment_done = true;
    }
    if (!(b->control & Z63_I2C_CR_HOLD)) {
        z63_i2c_stop_bus(b);
    }
    z63_i2c_update_irq(b->owner, b->index);
}

/* Held TX empty first requests refill. At the following boundary, retained
 * HOLD completes this segment while preserving the bus for a repeated start.
 * Original software decides whether to refill, release HOLD or start RX. */
static void z63_i2c_advance(Z63I2cBlock *b)
{
    Z63I2c *s = b->owner;
    if (s->halted || !b->active) {
        return;
    }
    if (b->address_pending) {
        b->address_pending = false;
        b->starts++;
        if (!s->endpoint ||
            !s->endpoint->start(s->peripheral, b->index, b->address,
                                !!(b->control & Z63_I2C_CR_RW), b->repeated)) {
            z63_i2c_nack(b);
            return;
        }
        b->bus_started = true;
        return;
    }
    if (b->segment_done) {
        if (!(b->control & Z63_I2C_CR_HOLD)) {
            z63_i2c_stop_bus(b);
        }
        return;
    }
    if (b->control & Z63_I2C_CR_RW) {
        if (b->receive_left && b->rx_count < Z63_I2C_FIFO_DEPTH) {
            uint8_t byte;
            if (!s->endpoint->read(s->peripheral, b->index, &byte)) {
                z63_i2c_nack(b);
                return;
            }
            unsigned tail = (b->rx_head + b->rx_count) % Z63_I2C_FIFO_DEPTH;
            b->rx[tail] = byte;
            b->rx_count++;
            b->receive_left--;
            b->rx_bytes++;
            if (b->rx_count == Z63_I2C_FIFO_DEPTH || !b->receive_left) {
                b->isr |= Z63_I2C_IRQ_DATA;
            }
        }
        if (!b->receive_left) {
            z63_i2c_complete(b);
        }
    } else {
        if (b->tx_count) {
            uint8_t byte = b->tx[b->tx_head];
            if (!s->endpoint->write(s->peripheral, b->index, byte)) {
                z63_i2c_nack(b);
                return;
            }
            b->tx_head = (b->tx_head + 1) % Z63_I2C_FIFO_DEPTH;
            b->tx_count--;
            b->tx_bytes++;
            if (b->tx_count) {
                return;
            }
        }
        if (!(b->control & Z63_I2C_CR_HOLD) || b->tx_empty_announced) {
            z63_i2c_complete(b);
        } else {
            b->tx_empty_announced = true;
            b->isr |= Z63_I2C_IRQ_DATA;
        }
    }
    z63_i2c_update_irq(s, b->index);
}

static bool z63_i2c_needs_step(const Z63I2cBlock *b)
{
    if (!b->active || b->owner->halted) {
        return false;
    }
    if (b->address_pending) {
        return true;
    }
    if (b->segment_done) {
        return !(b->control & Z63_I2C_CR_HOLD);
    }
    return !(b->control & Z63_I2C_CR_RW) || !b->receive_left ||
           b->rx_count < Z63_I2C_FIFO_DEPTH;
}

static void z63_i2c_schedule(Z63I2cBlock *b)
{
    if (!b->timer) {
        return;
    }
    if (z63_i2c_needs_step(b)) {
        if (!timer_pending(b->timer)) {
            uint32_t step = b->owner->step_ns ? b->owner->step_ns
                                              : Z63_I2C_STEP_NS;
            b->step_due = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + step;
            timer_mod(b->timer, b->step_due);
        }
    } else {
        timer_del(b->timer);
    }
}

static void z63_i2c_timer_tick(void *opaque)
{
    Z63I2cBlock *b = opaque;
    z63_i2c_advance(b);
    /* With i2c-byte-ns= given, the bus keeps its rate even when the host
     * runs the timer late: every step the virtual time already covers is
     * taken now, as long as the FIFO lets the bus go on. Without it, one
     * step per tick, as always. */
    uint32_t step = b->owner->step_ns;
    if (step) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        for (unsigned n = 0; n < 64 && z63_i2c_needs_step(b) &&
                             b->step_due + step <= now; n++) {
            b->step_due += step;
            z63_i2c_advance(b);
        }
    }
    z63_i2c_schedule(b);
}

static void z63_i2c_init_timers(Z63I2c *s)
{
    for (unsigned bank = 0; bank < Z63_I2C_BANKS; bank++) {
        Z63I2cBlock *b = &s->blocks[bank];
        b->owner = s;
        b->index = bank;
        b->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, z63_i2c_timer_tick, b);
    }
}

static uint64_t z63_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    Z63I2c *s = opaque;
    if (!z63_i2c_register(offset, size)) {
        z63_i2c_fault(s, "I2C register read", offset, 0, size);
        return 0;
    }
    unsigned bank = offset / 0x400;
    unsigned reg = offset & 0x3ff;
    Z63I2cBlock *b = &s->blocks[bank];
    switch (reg) {
    case 0x00: return b->control;
    case 0x04:
        return (b->active ? 1U << 8 : 0) |
               (b->tx_count ? 1U << 6 : 0) |
               (b->rx_count ? 1U << 5 : 0);
    case 0x08: return b->address;
    case 0x0c: {
        if (!b->rx_count) {
            z63_i2c_fault(s, "I2C empty RX FIFO read", offset, 0, size);
            return 0;
        }
        uint8_t byte = b->rx[b->rx_head];
        b->rx_head = (b->rx_head + 1) % Z63_I2C_FIFO_DEPTH;
        b->rx_count--;
        z63_i2c_schedule(b);
        return byte;
    }
    case 0x10: return b->isr;
    case 0x14:
        return b->control & Z63_I2C_CR_RW ? b->receive_left : b->tx_count;
    case 0x18: return b->slave_pause;
    case 0x1c: return b->timeout;
    case 0x20: return b->imr;
    case 0x28:
        /* Native ISR reads IDR before ORing bit1 and writing back.
         * Its observed RMW reads the current inverse mask. Linux labels
         * IDR write-only; this is the native compatible readback extension. */
        return b->imr;
    default:
        z63_i2c_fault(s, "I2C write-only register read", offset, 0, size);
        return 0;
    }
}

static bool z63_i2c_endpoint_complete(const Z63I2c *s)
{
    return !s->endpoint || (s->endpoint->start && s->endpoint->write &&
                            s->endpoint->read && s->endpoint->stop);
}

static void z63_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    Z63I2c *s = opaque;
    if (!z63_i2c_register(offset, size) || value > UINT32_MAX) {
        z63_i2c_fault(s, "I2C register write", offset, value, size);
        return;
    }
    unsigned bank = offset / 0x400;
    unsigned reg = offset & 0x3ff;
    Z63I2cBlock *b = &s->blocks[bank];
    uint32_t word = value;
    /* Validate the entire access before register/FIFO/endpoint mutation.
     * Invalid widths reach one callback, not split partial word stores. */
    switch (reg) {
    case 0x00:
        if (word & ~0xffffU ||
            (b->active && !b->segment_done &&
             !(word & Z63_I2C_CR_CLR_FIFO) &&
             ((word ^ b->control) & 0x87))) {
            z63_i2c_fault(s, "I2C unsupported control transition", offset, value, size);
            return;
        }
        b->control = word & ~Z63_I2C_CR_CLR_FIFO;
        if (word & Z63_I2C_CR_CLR_FIFO) {
            b->tx_head = b->tx_count = b->rx_head = b->rx_count = 0;
            b->receive_left = 0;
            b->tx_empty_announced = false;
            if (b->active) {
                b->segment_done = true;
            }
            b->address_pending = false;
            if (!(word & Z63_I2C_CR_HOLD)) {
                z63_i2c_stop_bus(b);
            }
        }
        /* Completion has already reached the wire. Releasing HOLD now
         * performs STOP, so the original next request's BA guard sees an
         * idle bus. Waiting another 100us step here creates a false busy
         * error after a valid RX completion. In-flight bytes still use the
         * transfer timer and retain their real completion/IRQ semantics. */
        if (b->segment_done && !(b->control & Z63_I2C_CR_HOLD)) {
            z63_i2c_stop_bus(b);
        }
        s->configuration_writes++;
        break;
    case 0x04: case 0x20:
        z63_i2c_fault(s, "I2C read-only register write", offset, value, size);
        return;
    case 0x08: {
        uint32_t mask = b->control & Z63_I2C_CR_NEA ? 0x7f : 0x3ff;
        if (word & ~mask || !(b->control & Z63_I2C_CR_MS) ||
            (b->control & Z63_I2C_CR_UNSUPPORTED_BANK3_MODE) ||
            (b->active && !b->segment_done) ||
            !z63_i2c_endpoint_complete(s)) {
            z63_i2c_fault(s, "I2C unsupported address transfer", offset, value, size);
            return;
        }
        b->address = word;
        b->repeated = b->bus_started;
        b->active = b->address_pending = true;
        b->segment_done = b->tx_empty_announced = false;
        break;
    }
    case 0x0c: {
        if (word & ~0xffU || b->tx_count == Z63_I2C_FIFO_DEPTH ||
            (b->control & Z63_I2C_CR_RW)) {
            z63_i2c_fault(s, "I2C TX sample, mode or FIFO capacity", offset, value, size);
            return;
        }
        unsigned tail = (b->tx_head + b->tx_count) % Z63_I2C_FIFO_DEPTH;
        b->tx[tail] = word;
        b->tx_count++;
        b->tx_empty_announced = false;
        if (b->active && b->segment_done) {
            b->segment_done = false;
        }
        break;
    }
    case 0x10:
        if (word & ~Z63_I2C_IRQ_MASK) {
            z63_i2c_fault(s, "I2C unknown IRQ status bits", offset, value, size);
            return;
        }
        b->isr &= ~word;
        s->acknowledgements++;
        break;
    case 0x14:
        if (word & ~0xffU || !(b->control & Z63_I2C_CR_RW) ||
            (b->active && !b->segment_done && b->receive_left)) {
            z63_i2c_fault(s, "I2C unsupported receive window", offset, value, size);
            return;
        }
        b->receive_left = word;
        break;
    case 0x18: case 0x1c:
        if (word & ~0xffU) {
            z63_i2c_fault(s, "I2C unknown timing bits", offset, value, size);
            return;
        }
        if (reg == 0x18) {
            b->slave_pause = word;
        } else {
            b->timeout = word;
        }
        s->configuration_writes++;
        break;
    case 0x24: case 0x28:
        if (word & ~Z63_I2C_IRQ_MASK) {
            z63_i2c_fault(s, "I2C unknown IRQ mask bits", offset, value, size);
            return;
        }
        if (reg == 0x24) {
            b->imr &= ~word;
        } else {
            b->imr |= word;
        }
        break;
    default:
        z63_i2c_fault(s, "I2C register write", offset, value, size);
        return;
    }
    z63_i2c_update_irq(s, bank);
    z63_i2c_schedule(b);
}

static const MemoryRegionOps z63_i2c_ops = {
    .read = z63_i2c_read, .write = z63_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 8,
    .valid.unaligned = true,
    .impl.min_access_size = 1, .impl.max_access_size = 8,
    .impl.unaligned = true,
};

static void z63_i2c_reset(Z63I2c *s)
{
    s->halted = false;
    s->configuration_writes = s->acknowledgements = s->rejected_accesses = 0;
    for (unsigned bank = 0; bank < Z63_I2C_BANKS; bank++) {
        Z63I2cBlock *b = &s->blocks[bank];
        QEMUTimer *timer = b->timer;
        if (timer) {
            timer_del(timer);
        }
        z63_i2c_stop_bus(b);
        memset(b, 0, sizeof(*b));
        b->owner = s;
        b->index = bank;
        b->timer = timer;
        b->imr = Z63_I2C_IRQ_MASK;
        z63_i2c_update_irq(s, bank);
    }
}


static void z63_i2c_init(Object *owner, MemoryRegion *sysmem, DeviceState *gic,
                         Z63I2c *s, const Z63I2cEndpoint *ep, void *peripheral)
{
    s->endpoint = ep;
    s->peripheral = peripheral;
    z63_i2c_init_timers(s);
    z63_i2c_reset(s);
    for (unsigned bank = 0; bank < Z63_I2C_BANKS; bank++) {
        s->irqs[bank] = qdev_get_gpio_in(gic,
                                         Z63_GIC_SPI(Z63_SPI_I2C_0 + bank));
    }
    memory_region_init_io(&s->mmio, owner, &z63_i2c_ops, s, "I2C",
                          Z63_I2C_LENGTH);
    /* Over EMIO-E0's last page, which it used to be storage for. */
    memory_region_add_subregion_overlap(sysmem, Z63_I2C_BASE, &s->mmio, 1);
}

#endif /* HW_ARM_NIKON_Z63_I2C_H */
