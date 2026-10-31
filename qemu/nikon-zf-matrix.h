/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_ZF_MATRIX_H
#define NIKON_ZF_MATRIX_H

/* Own EX scanner0803fa48, matrix row1/column3: PF1 -> PF10, logical37.
 * Model one ideal low-going contact only. In other scan rows the column
 * retains its normal pull/external-input resolution. No multi-key/diode,
 * contention, EXTI edge or analogue electrical model is claimed.
 */
static inline bool zf_menu_contact_low(bool pressed, unsigned port,
                                      unsigned pin, uint32_t moder,
                                      uint32_t odr)
{
    return pressed && port == 5 && pin == 10 &&
           ((moder >> 20) & 3) == 0 && /* PF10 is an input */
           ((moder >> 2) & 3) == 1 &&  /* PF1 is an output */
           !(odr & (1u << 1));
}

/* Own row3: UP/DOWN/LEFT/RIGHT are PF7/PF8/PF9/PF10 columns.
 * key0 releases the contact; keys1..4 select one physical direction.
 * OK is the separate active-low PG10 input, handled by mcu-pins. */
static inline bool zf_navigation_contact_low(unsigned key, unsigned port,
                                            unsigned pin, uint32_t moder,
                                            uint32_t odr)
{
    return key >= 1 && key <= 4 && port == 5 && pin == key + 6 &&
           ((moder >> (pin * 2)) & 3) == 0 &&
           ((moder >> 6) & 3) == 1 && !(odr & (1u << 3));
}

/* Own row4: the three-position mode lever's two lines are matrix cells
 * (row4,col0) and (row4,col4), both logical46 -- body byte 0x57 carries
 * their state as a level (Z63 mcukeys: state byte 46 -> report byte 87).
 * pos 1 = col0/F7 line, pos 2 = col4/F11 line; 0 releases both. Row4's
 * select output is PF4 (row N = PF(N) like rows 1 and 3). Segment names
 * on the GUI side are unconfirmed: position tests via whole-column drives
 * are confounded by latched keys in the same column. */
static inline bool zf_lever_contact_low(unsigned pos, unsigned port,
                                        unsigned pin, uint32_t moder,
                                        uint32_t odr)
{
    if (pos != 1 && pos != 2) {
        return false;
    }
    unsigned col = pos == 1 ? 7 : 11;
    return port == 5 && pin == col &&
           ((moder >> (pin * 2)) & 3) == 0 &&
           ((moder >> 8) & 3) == 1 && !(odr & (1u << 4));
}
/* A single ideal contact. Matrix contacts only close in their selected row;
 * direct contacts only pull input-mode pins low. No diode/ghosting model. */
typedef struct ZfButton {
    const char *name;
    unsigned port, pin, row;
} ZfButton;

static const ZfButton zf_buttons[] = {
    { "none", 0, 0, 16 },
    { "play", 5, 7, 1 },
    { "delete", 5, 8, 4 },
    { "zoom-in", 5, 9, 4 },
    { "zoom-out", 5, 10, 4 },
    { "ae-af-lock", 6, 2, 16 },
    { "fn", 1, 15, 16 },
    { "movie", 1, 1, 16 },
    { "ok", 6, 10, 16 },
    { "matrix-l39", 5, 8, 4 },
    { "matrix-l42", 5, 9, 4 },
    { "matrix-l45", 5, 12, 4 },
    { "i", 5, 8, 1 },
    { "disp", 5, 9, 1 },
    { "display-mode", 5, 12, 4 }, /* own L45 -> monitor/eye policy */
};

static inline bool zf_button_contact_low(unsigned key, unsigned port,
                                         unsigned pin, uint32_t moder,
                                         uint32_t odr)
{
    if (!key || key >= sizeof(zf_buttons) / sizeof(zf_buttons[0])) {
        return false;
    }
    const ZfButton *b = &zf_buttons[key];
    return port == b->port && pin == b->pin &&
           ((moder >> (pin * 2)) & 3) == 0 &&
           (b->row == 16 || (((moder >> (b->row * 2)) & 3) == 1 &&
                             !(odr & (1u << b->row))));
}
/* Own 0803f78c samples seven columns, ignoring empty67 entries. Rows0/5
 * have six cells; row2 has seven. A mask bit1 opens that contact. Native
 * pulls resolve the open level; only the selected row can pull low. */
static inline bool zf_rotary_contact_low(unsigned enabled, const uint8_t masks[3],
                                         unsigned port, unsigned pin,
                                         uint32_t moder, uint32_t odr)
{
    static const unsigned rows[] = { 0, 2, 5 };
    if (port != 5 || pin < 7 || pin > 13 || ((moder >> (pin * 2)) & 3) != 0) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        unsigned row = rows[i];
        if ((enabled & (1u << i)) && (pin < 13 || i == 1) &&
            ((moder >> (row * 2)) & 3) == 1 && !(odr & (1u << row)) &&
            !(masks[i] & (1u << (pin - 7)))) {
            return true;
        }
    }
    return false;
}
#endif
