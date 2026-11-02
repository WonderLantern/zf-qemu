/*
 * Nikon Z 6III -- the body's buttons, as the real MCU reads them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * With mcu-image= the body MCU's own switch service builds the key report
 * the EXPEED decodes (family 2 kind 1, sub_8043C2C; decoded by the EXPEED's
 * sub_41011850 into the ID_SW0/1/2 banks of the name table at 0x42b8b27c).
 * The MCU reads its buttons from two places:
 *
 *   - GPIO pins, through pin tables in its flash (sub_803BA3C / table
 *     0x8057adc, sub_803B914 / 0x8057b0c, sub_803B9F4 / 0x8057b3c), each
 *     entry a pin id ((port << 4) | pin) and an active level (0 for all
 *     used here): the shutter's half press PG1 and full press PG13, the
 *     10-pin remote's PG14/PG15, AF-ON PG2, ISO PC2, AF-ON (vertical) PF6,
 *     the 10-pin Fn PB11;
 *   - the two I2C button expanders (nikon-z63-stm32.h): word 0x200141c0
 *     from one, word 0x200141c4 from the other (sub_803D998, sub_803DA14),
 *     a bit per button, active low.
 *
 * Bit -> switch, followed through the switch service: the 61-row table at
 * 0x8056cf8 turns word bits into per-button bytes (sub_803D6F8), those
 * become the report's bytes (sub_804388C; the two selectors through the
 * direction encoder sub_8044348), and the EXPEED's decoder names each byte.
 * At run time the key task (sub_8044960) maps each expander bit to a key
 * byte through the tables at 0x80578be (0x22) and 0x80578d6 (0x23); for
 * 0x23 those tables, not the power-on words, give the names below (MENU is
 * key byte 37 = 0x23 bit 6, which the report carries at 0x43).
 *
 * Without this every one of those inputs read 0 -- every button held,
 * among them MENU, PLAY, OK, DELETE, i and the movie-record button -- and
 * the shutter fully pressed. The board now holds them released (1) from
 * machine init, and presses them by name: -machine mcu-keys=, qom-set
 * /machine mcu-keys at run time, or the display window's keyboard.
 * mcu-pins= still overrides any pin afterwards.
 */

#ifndef HW_ARM_NIKON_Z63_MCUKEYS_H
#define HW_ARM_NIKON_Z63_MCUKEYS_H

typedef struct Z63McuKey {
    const char *name;
    uint8_t expander;           /* I2C address, or 0 for a GPIO pin */
    uint8_t port, bit;          /* GPIO port 0..10 and pin, or expander bit */
    const char *also;           /* pressed with it ("full" holds "half") */
} Z63McuKey;

static const Z63McuKey z63_mcu_key_map[] = {
    /* GPIO, active low */
    { "half",         0, 6, 1 },            /* PG1  RELEASE_HALF */
    { "full",         0, 6, 13, "half" },   /* PG13 RELEASE_ON */
    { "af-on",        0, 6, 2 },            /* PG2  AF_ON */
    { "iso",          0, 2, 2 },            /* PC2  ISO */
    /* expander 0x23 (word 0x200141c4 at power-on) */
    { "ss-center",    0x23, 0, 0 },
    { "ss-down",      0x23, 0, 1 },
    { "ss-right",     0x23, 0, 2 },
    { "ss-up",        0x23, 0, 3 },
    { "ss-left",      0x23, 0, 4 },
    { "play",         0x23, 0, 5 },
    { "menu",         0x23, 0, 6 },
    { "i",            0x23, 0, 7 },
    { "disp",         0x23, 0, 8 },
    { "release-mode", 0x23, 0, 9 },
    { "ok",           0x23, 0, 11 },
    { "left",         0x23, 0, 12 },
    { "down",         0x23, 0, 13 },
    { "right",        0x23, 0, 14 },
    { "up",           0x23, 0, 15 },
    { "zoom-out",     0x23, 0, 16 },        /* THUMBNAIL */
    { "zoom-in",      0x23, 0, 17 },        /* ZOOM */
    { "delete",       0x23, 0, 18 },
    /* expander 0x22 (word 0x200141c0 at power-on). These names are the
     * power-on path's; the run-time table at 0x80578be sends bits 9..13 to
     * key bytes 63, 36, 35, 9, 45, which have not been followed to the
     * EXPEED's names yet. */
    { "illuminate",   0x22, 0, 2 },
    { "movie",        0x22, 0, 3 },         /* movie-record button */
    { "fn-v",         0x22, 0, 9 },         /* FUNCTION_VPOS */
    { "fn2",          0x22, 0, 10 },
    { "fn",           0x22, 0, 11 },
    { "exp-comp",     0x22, 0, 12 },
};

/* The GPIO inputs that must idle released: the keys above and the 10-pin
 * remote's half/full press and Fn, and the vertical AF-ON. */
static const uint8_t z63_mcu_key_idle_pins[][2] = {
    { 6, 1 }, { 6, 13 }, { 6, 2 }, { 2, 2 },
    { 6, 14 }, { 6, 15 }, { 1, 11 }, { 5, 6 },
};

static void z63_mcukey_drive(Z63Stm32 *m, const Z63McuKey *k, bool down)
{
    if (k->expander) {
        z63_stm32_expander_set(m, k->expander, k->bit, down);
    } else {
        z63_stm32_gpio_drive(&m->gpio[k->port], k->bit, !down);
    }
}

static const Z63McuKey *z63_mcukey_find(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(z63_mcu_key_map); i++) {
        if (!strcmp(z63_mcu_key_map[i].name, name)) {
            return &z63_mcu_key_map[i];
        }
    }
    return NULL;
}

static void z63_mcukeys_init(Z63Stm32 *m)
{
    for (size_t i = 0; i < ARRAY_SIZE(z63_mcu_key_idle_pins); i++) {
        z63_stm32_gpio_drive(&m->gpio[z63_mcu_key_idle_pins[i][0]],
                             z63_mcu_key_idle_pins[i][1], 1);
    }
}

/*
 * The controls that are not buttons: the two command dials, the mode dial,
 * the photo/video lever and the power switch. They go through the same
 * property as NAME=VALUE tokens beside the keys (-machine mcu-keys=, or
 * qom-set /machine mcu-keys at run time):
 *
 *   dial-main=+N  dial-main=-N  dial-sub=+N  dial-sub=-N
 *                 N detents of a command dial (queued behind a turn
 *                 the same way; one the other way is refused until the
 *                 dial stops)
 *   mode=AUTO|P|S|A|M|U1|U2|U3  (or mode=<0..4095>, the ladder's level)
 *   lever=photo|video
 *   power=on|off
 *
 * A list of such tokens alone leaves the held keys as they are; a list
 * with any key in it holds exactly its keys, as before, and an empty one
 * releases them all. Each lasts until changed: the mode dial, the lever
 * and the power switch keep the position they were given.
 *
 * Command dials. The key service arms a compare callback on its timer
 * every 500 ticks (sub_8042EC8 with sub_80444E4), which reads each dial as
 * two inputs making a 2-bit phase, (a << 1) | b: the main dial PE7 (a) and
 * PE13 (b) (sub_8044D3C, count at 0x20019343), the sub dial PF11 (a) and
 * PF14 (b) (sub_8044D94, count at 0x20019344). The main dial counts -1 on
 * the phase change 0 -> 2 and +1 on 2 -> 0, the sub dial -1 on 1 -> 0 and
 * +1 on 0 -> 1; every other change is ignored. A full cycle through the
 * phases in the order 0, 1, 3, 2 is therefore +1 on either dial, and the
 * reverse order -1, from whichever phase the pins rest at. The counts go
 * out as state bytes 67 and 68 of message 0x20C0081 (sub_8044808 ->
 * sub_8043D60), the switch report's bytes 11 and 12, which the EXPEED adds
 * to ID_ROTSW0/1, the scan object's first two dial words: +N here is +N
 * there, burst for burst (watched at 0xa03367c60, 8 bursts of +-25 on both
 * dials at once, none lost). One detent is modelled as one full cycle, a
 * phase every Z63_MCU_DIAL_PHASE_NS of virtual time -- at 4 ms a phase,
 * now and then one count in a burst went missing (the M4 shares its TCG
 * thread with the four A53s, so its sampling is not always on time).
 * DECLARED: the detent (one cycle) and the rate. Which way a hand would
 * call clockwise the MCU cannot say (the wiring decides it); what the
 * stock firmware does with +1 on the main dial: in M, a shutter speed a
 * third of a stop shorter (item 0x0205 +8); with ISO held, a third of a
 * stop more ISO (0x0208 +8); with exposure compensation held, +1/3 EV
 * (0x0204 +2); with the release-mode button held, the next continuous
 * mode (0x020B 2 -> 6). The sub dial with ISO held turns Auto ISO off
 * (0x0509 1 -> 0); with no lens there is no aperture to turn.
 *
 * Mode dial. A resistor ladder on ADC1 channel 1, converted four times
 * every 8 ms (the cyclic "SwichCy8" -> sub_8044894 -> sub_8044C88); the
 * mean selects a position code by range (below), which goes out as state
 * byte 6 of message 0x20C0083, the report's byte 15. The level this board
 * gives an unset channel, 0x800, is code 5. The names are the EXPEED's
 * reading of each code (store item 0x0000), measured on this model.
 *
 * Photo/video lever. Expander 0x23 bit 10 (state byte 46 through the table
 * at 0x80578d6, sub_8044218), one of the bits the key task passes through
 * as a level rather than latching like a button (mask 0x1ef4f8 at
 * 0x80577c8); the report carries (state == 0) at byte 87.
 *
 * Power switch. PC5, read with PB11 and PC2 into state bytes 0, 66 and 11
 * of message 0x20C0083 (table at 0x80577d4); state byte 0 is the report's
 * byte 4, which the EXPEED decodes as SW0 bit 9 (0x200, the switch name
 * table's power switch). Low is on: this board reads an undriven PC5 as
 * low, and the camera starts switched on. power=off starts the camera's own
 * power-down (family 1 route 3 kind 6 with byte 5 = 0, where the standby
 * timer's has 1) and ends at the power-hold line like a standby does.
 * power=on afterwards changes nothing: the MCU's side of starting the
 * EXPEED again is not modelled (nor is waking from standby).
 */
#define Z63_MCU_DIAL_PHASE_NS   (8 * SCALE_MS)

typedef struct Z63McuDial {
    const char *name;
    uint8_t port, pin_a, pin_b;
    int pending;                /* detents still to turn, signed */
    uint8_t phases;             /* phases already done of the current one */
} Z63McuDial;

/* The ladder, in rising level: each position's range of the 12-bit mean
 * (sub_8044C88), the MCU's code for it, and what the EXPEED makes of that
 * code -- store item 0x0000 (also 0x0001 and 0x2101) and the shooting
 * screen's mode icon, seen on this model with the stock firmware. The
 * level sent is the middle of the range. */
static const struct {
    const char *name;
    uint8_t code;               /* state byte 6, report byte 15 */
    uint8_t item;               /* store item 0x0000 */
    uint16_t lo, hi;
} z63_mcu_mode_ladder[] = {
    { "S",     1, 1,    0,  282 },
    { "A",     2, 2,  283,  846 },
    { "M",     3, 3,  847, 1435 },
    { "U3",    6, 9, 1436, 2028 },
    { "U2",    5, 8, 2029, 2603 },      /* 0x800, the unset level */
    { "U1",    4, 7, 2604, 3187 },
    { "AUTO", 10, 4, 3188, 3789 },
    { "P",     0, 0, 3790, 4095 },
};

static struct {
    QEMUTimer *timer;
    Z63McuDial dial[2];
} z63_mcuin = {
    .dial = {
        { "dial-main", 4, 7, 13 },      /* PE7, PE13 */
        { "dial-sub", 5, 11, 14 },      /* PF11, PF14 */
    },
};

#define Z63_MCU_MODE_ADC        0       /* ADC1 */
#define Z63_MCU_MODE_CHANNEL    1
#define Z63_MCU_LEVER_EXPANDER  0x23
#define Z63_MCU_LEVER_BIT       10
#define Z63_MCU_POWER_PORT      2       /* PC5 */
#define Z63_MCU_POWER_PIN       5

static unsigned z63_mcudial_phase(Z63Stm32 *m, const Z63McuDial *d)
{
    Z63McuDial own = *d;
    if (m->zf_clocks) {
        bool main = d == &z63_mcuin.dial[0];
        own.port = main ? 0 : 4;
        own.pin_a = main ? 10 : 7;
        own.pin_b = main ? 11 : 13;
        d = &own;
    }
    uint16_t idr = z63_stm32_gpio_idr(&m->gpio[d->port]);

    return ((idr >> d->pin_a) & 1) << 1 | ((idr >> d->pin_b) & 1);
}

static void z63_mcudial_tick(void *opaque)
{
    static const uint8_t gray[4] = { 0, 1, 3, 2 };
    Z63Stm32 *m = opaque;
    bool more = false;

    for (unsigned i = 0; i < ARRAY_SIZE(z63_mcuin.dial); i++) {
        Z63McuDial *d = &z63_mcuin.dial[i];
        unsigned k = 0, at, next;

        if (!d->pending) {
            continue;
        }
        at = z63_mcudial_phase(m, d);
        while (gray[k] != at) {
            k++;
        }
        bool forward = d->pending > 0;
        if (m->zf_clocks) { forward = !forward; }
        next = gray[(k + (forward ? 1 : 3)) & 3];
        unsigned port = d->port, pin_a = d->pin_a, pin_b = d->pin_b;
        if (m->zf_clocks) {
            port = i == 0 ? 0 : 4;
            pin_a = i == 0 ? 10 : 7;
            pin_b = i == 0 ? 11 : 13;
        }
        z63_stm32_gpio_drive(&m->gpio[port], pin_a, next >> 1);
        z63_stm32_gpio_drive(&m->gpio[port], pin_b, next & 1);
        if (++d->phases == 4) {
            d->phases = 0;
            d->pending += d->pending > 0 ? -1 : 1;
        }
        more |= d->pending != 0;
    }
    if (more) {
        timer_mod(z63_mcuin.timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  Z63_MCU_DIAL_PHASE_NS);
    }
}

/* One NAME=VALUE token; with `apply` false it is only checked. */
static bool z63_mcuin_token(Z63Stm32 *m, const char *token, bool apply,
                            Error **errp)
{
    g_autofree char *name = g_strdup(token);
    char *value = strchr(name, '=');
    long n;

    *value++ = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(z63_mcuin.dial); i++) {
        Z63McuDial *d = &z63_mcuin.dial[i];

        if (strcmp(name, d->name)) {
            continue;
        }
        if (qemu_strtol(value, NULL, 10, &n) || !n || n < -127 || n > 127) {
            error_setg(errp, "mcu-keys: %s wants +N or -N detents, 1..127",
                       name);
            return false;
        }
        /* Turns the same way queue up; a turn back while one is still
         * playing would cut a detent short, so it waits its turn. */
        if (d->pending && (n > 0) != (d->pending > 0)) {
            error_setg(errp, "mcu-keys: %s is still turning the other way "
                       "(%d detents left)", name, d->pending);
            return false;
        }
        if (apply) {
            if (!z63_mcuin.timer) {
                z63_mcuin.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                               z63_mcudial_tick, m);
            }
            d->pending += n;
            if (!timer_pending(z63_mcuin.timer)) {
                z63_mcudial_tick(m);
            }
        }
        return true;
    }
    if (!strcmp(name, "mode")) {
        long level = -1;

        for (unsigned i = 0; i < ARRAY_SIZE(z63_mcu_mode_ladder); i++) {
            if (!g_ascii_strcasecmp(value, z63_mcu_mode_ladder[i].name)) {
                level = (z63_mcu_mode_ladder[i].lo +
                         z63_mcu_mode_ladder[i].hi) / 2;
            }
        }
        if (level < 0 && (qemu_strtol(value, NULL, 0, &level) ||
                          level < 0 || level > 4095)) {
            error_setg(errp, "mcu-keys: mode=%s is not a position or a "
                       "level 0..4095", value);
            return false;
        }
        if (apply) {
            m->adc.input[Z63_MCU_MODE_ADC][Z63_MCU_MODE_CHANNEL] = level;
        }
        return true;
    }
    if (!strcmp(name, "lever")) {
        bool video = !strcmp(value, "video");

        if (!video && strcmp(value, "photo")) {
            error_setg(errp, "mcu-keys: lever=photo or lever=video");
            return false;
        }
        if (apply) {
            z63_stm32_expander_set(m, Z63_MCU_LEVER_EXPANDER,
                                   Z63_MCU_LEVER_BIT, video);
        }
        return true;
    }
    if (!strcmp(name, "power")) {
        bool off = !strcmp(value, "off");

        if (!off && strcmp(value, "on")) {
            error_setg(errp, "mcu-keys: power=on or power=off");
            return false;
        }
        if (apply) {
            z63_stm32_gpio_drive(&m->gpio[Z63_MCU_POWER_PORT],
                                 Z63_MCU_POWER_PIN, off);
        }
        return true;
    }
    error_setg(errp, "mcu-keys: unknown control '%s'", name);
    return false;
}

/* Hold exactly the named keys (':'-separated; empty releases all), and set
 * the controls named as NAME=VALUE. */
static bool z63_mcukeys_set(Z63Stm32 *m, const char *list, Error **errp)
{
    g_auto(GStrv) names = g_strsplit_set(list ? list : "", ":,", -1);
    bool held[ARRAY_SIZE(z63_mcu_key_map)] = { false };
    bool keys = !list || !*list;

    for (char **n = names; *n; n++) {
        const Z63McuKey *k;

        if (!**n) {
            continue;
        }
        if (strchr(*n, '=')) {
            if (!z63_mcuin_token(m, *n, false, errp)) {
                return false;
            }
            continue;
        }
        keys = true;
        k = z63_mcukey_find(*n);
        if (!k) {
            error_setg(errp, "mcu-keys: unknown key '%s'", *n);
            return false;
        }
        held[k - z63_mcu_key_map] = true;
        if (k->also) {
            held[z63_mcukey_find(k->also) - z63_mcu_key_map] = true;
        }
    }
    for (char **n = names; *n; n++) {
        if (strchr(*n, '=')) {
            z63_mcuin_token(m, *n, true, &error_abort);
        }
    }
    if (!keys) {
        error_report("Z63_MCUKEYS: controls: %s", list);
        return true;
    }
    for (size_t i = 0; i < ARRAY_SIZE(z63_mcu_key_map); i++) {
        z63_mcukey_drive(m, &z63_mcu_key_map[i], held[i]);
    }
    error_report("Z63_MCUKEYS: held: %s", list && *list ? list : "(none)");
    return true;
}

/*
 * The shutter button's two contacts reach the EXPEED as well, not only the
 * MCU. The RTOS's runtime line descriptors (0xa02dc3dd8 + 8 * (INTID - 55),
 * installed by sub_40C15DFC) put EXT_INT_4 (INTID 59) on pin 35 = GPIO
 * port 3 bit 4 and EXT_INT_5 (INTID 60) on pin 36 = port 3 bit 5, both
 * mode 2 (each edge, the ISR sub_40C16194.. re-reading the pin) and both
 * with callback sub_40FC7CDC(pin, level). That callback takes the pin in
 * 0xa03d8a910 as the half press -- level 1 starts it, sub_40FC4140(0) --
 * and the pin in 0xa03d8a914 as the full press, which with half already
 * held releases the shutter, sub_40FC4140(1). The MCU's key report alone
 * (family 2 kind 1, SW0 bit 30) shows the press on screen but releases
 * nothing. ../z9-qemu (WonderLantern, GPL-2.0-or-later) drives the same
 * pair on the Z 9 -- bank 3 bits 4 and 5, INTIDs 59 and 60, high while
 * closed -- through its capture-gpio model.
 *
 * DECLARED: that the button closes these alongside the MCU's PG1 and PG13,
 * high while closed (the pin descriptors' active-level byte +12 is 1, and
 * sub_40FC7CDC treats a 1 as pressed). Contact bounce and the order in
 * which the two halves close are not modelled. The contacts follow PG1 and
 * PG13 as driven, so mcu-keys=, the keyboard and mcu-pins= all reach them.
 */
#define Z63_RELEASE_HALF_BIT    4       /* EXPEED port 3, EXT_INT_4 */
#define Z63_RELEASE_FULL_BIT    5       /* EXPEED port 3, EXT_INT_5 */

static uint16_t z63_mcukeys_contacts(Z63Stm32 *m)
{
    Z63Stm32Gpio *g = &m->gpio[6];      /* port G */
    uint16_t closed = g->driven & ~g->level;

    return (((closed >> 1) & 1) << Z63_RELEASE_HALF_BIT) |
           (((closed >> 13) & 1) << Z63_RELEASE_FULL_BIT);
}

/* The display window's keyboard. */
static void z63_mcukeys_kbd(Z63Stm32 *m, int qcode, bool down)
{
    if (m->zf_clocks) {
        static const struct { int code; const char *name; } keys[] = {
            { Q_KEY_CODE_P, "play" }, { Q_KEY_CODE_BACKSPACE, "delete" },
            { Q_KEY_CODE_DELETE, "delete" }, { Q_KEY_CODE_EQUAL, "zoom-in" },
            { Q_KEY_CODE_MINUS, "zoom-out" }, { Q_KEY_CODE_R, "movie" },
            { Q_KEY_CODE_A, "ae-af-lock" }, { Q_KEY_CODE_F1, "fn" },
            { Q_KEY_CODE_RET, "ok" }, { Q_KEY_CODE_KP_ENTER, "ok" },
            { Q_KEY_CODE_I, "i" }, { Q_KEY_CODE_D, "disp" },
            { Q_KEY_CODE_V, "display-mode" },
        };
        static const int arrows[] = { Q_KEY_CODE_UP, Q_KEY_CODE_DOWN,
                                      Q_KEY_CODE_LEFT, Q_KEY_CODE_RIGHT };
        if (qcode == Q_KEY_CODE_M) { m->zf_menu_pressed = down; return; }
        if (qcode == Q_KEY_CODE_H || qcode == Q_KEY_CODE_SPC) {
            z63_stm32_gpio_drive(&m->gpio[6], 1, !down);
            if (qcode == Q_KEY_CODE_SPC) {
                z63_stm32_gpio_drive(&m->gpio[6], 13, !down);
            }
            return;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(arrows); i++) {
            if (qcode == arrows[i]) {
                if (down || m->zf_navigation == i + 1) {
                    m->zf_navigation = down ? i + 1 : 0;
                }
                return;
            }
        }
        for (unsigned i = 0; i < ARRAY_SIZE(keys); i++) {
            if (qcode != keys[i].code) { continue; }
            for (unsigned j = 1; j < ARRAY_SIZE(zf_buttons); j++) {
                if (!strcmp(keys[i].name, zf_buttons[j].name)) {
                    if (down || m->zf_button == j) { m->zf_button = down ? j : 0; }
                    return;
                }
            }
        }
        return; /* Unbound Zf keys must not drive Z63 I2C expanders. */
    }

    static const struct { int qcode; const char *name; } map[] = {
        { Q_KEY_CODE_UP, "up" }, { Q_KEY_CODE_DOWN, "down" },
        { Q_KEY_CODE_LEFT, "left" }, { Q_KEY_CODE_RIGHT, "right" },
        { Q_KEY_CODE_RET, "ok" }, { Q_KEY_CODE_KP_ENTER, "ok" },
        { Q_KEY_CODE_M, "menu" }, { Q_KEY_CODE_P, "play" },
        { Q_KEY_CODE_BACKSPACE, "delete" }, { Q_KEY_CODE_DELETE, "delete" },
        { Q_KEY_CODE_H, "half" }, { Q_KEY_CODE_SPC, "full" },
        { Q_KEY_CODE_D, "disp" }, { Q_KEY_CODE_I, "i" },
        { Q_KEY_CODE_F1, "fn" }, { Q_KEY_CODE_F2, "fn2" },
        { Q_KEY_CODE_R, "movie" }, { Q_KEY_CODE_A, "af-on" },
        { Q_KEY_CODE_EQUAL, "zoom-in" }, { Q_KEY_CODE_MINUS, "zoom-out" },
        { Q_KEY_CODE_W, "ss-up" }, { Q_KEY_CODE_S, "ss-down" },
        { Q_KEY_CODE_Q, "ss-left" }, { Q_KEY_CODE_E, "ss-right" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
        if (map[i].qcode == qcode) {
            const Z63McuKey *k = z63_mcukey_find(map[i].name);

            z63_mcukey_drive(m, k, down);
            if (k->also) {
                z63_mcukey_drive(m, z63_mcukey_find(k->also), down);
            }
            return;
        }
    }
}

#endif /* HW_ARM_NIKON_Z63_MCUKEYS_H */
