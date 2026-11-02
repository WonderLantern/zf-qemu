"""Retro dial contact masks and provisional label mappings.

Electrical codes are modeled; engraved position mapping remains incomplete.
ISO 512000 has no independently verified contact code.
"""

from __future__ import annotations

EV_LABELS = ['+3', '+2 2/3', '+2 1/3', '+2', '+1 2/3', '+1 1/3', '+1',
             '+2/3', '+1/3', '0', '-1/3', '-2/3', '-1', '-1 1/3', '-1 2/3',
             '-2', '-2 1/3', '-2 2/3', '-3', 'C']
CODE_MASKS = {
    0: {
        1: 49, 2: 51, 3: 50, 4: 54, 5: 38, 6: 39, 7: 37, 8: 45, 9: 47,
        10: 15, 11: 14, 12: 46, 13: 62, 14: 63, 15: 61, 16: 57, 17: 59,
        18: 58, 19: 55,
    },
    2: {
        0: 117, 1: 113, 2: 115, 3: 114, 4: 122, 5: 123, 6: 121, 7: 57,
        8: 59, 9: 58, 10: 62, 11: 63, 12: 61, 13: 29, 14: 31, 15: 30,
        16: 94, 17: 95, 18: 93, 19: 77, 20: 79, 21: 78, 22: 110, 23: 111,
        24: 109, 25: 101, 26: 103, 27: 102, 30: 118,
    },
    5: {
        1: 38, 2: 39, 3: 37, 4: 45, 5: 47, 6: 15, 7: 14, 8: 46, 9: 62,
        10: 63, 11: 61, 12: 57, 13: 59, 14: 58, 15: 50, 16: 51, 17: 49,
        18: 53, 19: 55,
    },
}
DEFAULT_MASK = {0: 53, 2: 119, 5: 61}
DEFAULT_CODE = {0: 0, 2: 31, 5: 11}

LABELS = {
    0: ['1/3 STEP', '1/8000 s', '1/4000 s', '1/2000 s', '1/1000 s',
        '1/500 s', '1/250 s', '1/125 s', '1/60 s', '1/30 s', '1/15 s',
        '1/8 s', '1/4 s', '1/2 s', '1 s', '2 s', '4 s', 'B', 'T', 'X'],
    2: ['C', '100', '125', '160', '200', '250', '320', '400', '500', '640',
        '800', '1000', '1250', '1600', '2000', '2500', '3200', '4000', '5000',
        '6400', '8000', '10000', '12800', '16000', '20000', '25600', '32000',
        '40000', '51200', '64000', '512000'],
    5: EV_LABELS,
}

# Engraved position (index into LABELS) -> firmware position code.
# row0: preserve the existing label/contact assignments while ordering the UI
#       clockwise from STEP. Code 0 is STEP, 19 is 8000, 3/2/1 are B/T/X.
#       row2: 'C' is the default (code 31), the 29 numbered stops run
#       codes 0..27 then 30; '512000' has no verified contact code.  row5: the 19
#       compensation marks are codes 1..19, 'C' is the default mask.
POSITION_CODE = {
    0: {0: DEFAULT_CODE[0], 1: 19, 17: 3, 18: 2, 19: 1}
       | {i: i + 2 for i in range(2, 17)},
    2: {0: DEFAULT_CODE[2]} | {i: (i - 1) if i <= 28 else 30 for i in range(1, 30)},
    5: {i: i + 1 for i in range(19)} | {19: DEFAULT_CODE[5]},
}

UNBOUND = {2: {30: 'ISO 512000 has no separately verified Zf contact code'}}

ROW_NOTE = {
    0: 'shutter speed dial (row0)',
    2: 'ISO dial (row2, 7-bit row)',
    5: 'exposure compensation dial (row5)',
}


def labels(row):
    return list(LABELS[row])


def positions(row):
    return len(LABELS[row])


def describe(row, position):
    """Return the mask, code and caveat for one engraved position."""
    position = int(position)
    if row not in LABELS:
        raise ValueError('invalid rotary row')
    if not 0 <= position < len(LABELS[row]):
        raise ValueError('position %d outside 0..%d' % (position, len(LABELS[row]) - 1))
    label = LABELS[row][position]
    if position in UNBOUND.get(row, {}):
        return {
            'row': row, 'position': position, 'label': label,
            'mask': None, 'code': None, 'confirmed': False,
            'warning': UNBOUND[row][position],
        }
    code = POSITION_CODE[row][position]
    mask = DEFAULT_MASK[row] if code == DEFAULT_CODE[row] and code not in CODE_MASKS[row] else CODE_MASKS[row][code]
    return {
        'row': row, 'position': position, 'label': label,
        'mask': mask, 'code': code, 'confirmed': False,
        'warning': 'label-to-detent order is the engraved order, not yet anchored on a camera',
    }


def mask_for(row, position):
    info = describe(row, position)
    if info['mask'] is None:
        raise ValueError(info['warning'])
    return info['mask']


def table():
    out = {}
    for row in LABELS:
        out[str(row)] = {
            'note': ROW_NOTE[row],
            'default_mask': DEFAULT_MASK[row],
            'default_code': DEFAULT_CODE[row],
            'positions': [
                describe(row, p) for p in range(len(LABELS[row]))
            ],
        }
    return {'confirmed': False, 'rows': out,
            'caveat': 'masks and codes are verified; the label on each detent is not'}
