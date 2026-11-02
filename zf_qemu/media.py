"""Create empty, synthetic FAT32 cards; no captured card data is distributed."""
import struct
from pathlib import Path


def create_card(path: Path, size=4 * 1024**3):
    """Create a sparse MBR/FAT32 image suitable for both emulated slots."""
    path = Path(path)
    if path.exists():
        return
    start, reserved, spc, copies = 2048, 32, 64, 2
    total = size // 512 - start
    fat_sectors = 1
    while True:
        clusters = (total - reserved - copies * fat_sectors) // spc
        required = ((clusters + 2) * 4 + 511) // 512
        if fat_sectors >= required:
            break
        fat_sectors = required
    if clusters < 65525:
        raise ValueError('Card must be large enough for FAT32')
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.creating')
    with temporary.open('xb') as output:
        output.truncate(size)
        mbr = bytearray(512)
        struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\xfe\xff\xff', 0x0c,
                         b'\xfe\xff\xff', start, total)
        mbr[510:] = b'\x55\xaa'
        output.write(mbr)
        boot = bytearray(512)
        boot[:11] = b'\xeb\x58\x90MSWIN4.1'
        struct.pack_into('<HBHBHHBHHHII', boot, 11, 512, spc, reserved, copies,
                         0, 0, 0xf8, 0, 63, 255, start, total)
        struct.pack_into('<IHHIHH', boot, 36, fat_sectors, 0, 0, 2, 1, 6)
        boot[64], boot[66] = 0x80, 0x29
        struct.pack_into('<I', boot, 67, 0x5af00001)
        boot[71:82], boot[82:90], boot[510:] = b'ZF QEMU    ', b'FAT32   ', b'\x55\xaa'
        info = bytearray(512)
        struct.pack_into('<I', info, 0, 0x41615252)
        struct.pack_into('<III', info, 484, 0x61417272, clusters - 1, 3)
        struct.pack_into('<I', info, 508, 0xaa550000)
        for offset, data in ((0, boot), (1, info), (6, boot), (7, info)):
            output.seek((start + offset) * 512)
            output.write(data)
        for copy in range(copies):
            output.seek((start + reserved + copy * fat_sectors) * 512)
            output.write(struct.pack('<III', 0x0ffffff8, 0xffffffff, 0x0fffffff))
    temporary.replace(path)
