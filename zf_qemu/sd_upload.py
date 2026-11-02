"""Bounded FAT32 root-file writer for the GUI's private virtual card images."""
from __future__ import annotations

import struct
from pathlib import Path

MAX_UPLOAD = 256 * 1024 * 1024


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from('<H', data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from('<I', data, offset)[0]


def _fat_name(name: str) -> tuple[bytes, list[bytes]]:
    if not name or name in {'.', '..'} or any(c in name for c in '/\\:\0'):
        raise ValueError('文件名不能为空，也不能包含路径分隔符')
    if len(name.encode('utf-16-le')) > 510:
        raise ValueError('文件名过长')
    stem, dot, suffix = name.rpartition('.')
    if not dot:
        stem, suffix = name, ''
    allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$~!#%&-{}()@'`"
    upper_stem, upper_suffix = stem.upper(), suffix.upper()
    simple = (upper_stem and len(upper_stem) <= 8 and len(upper_suffix) <= 3
              and all(c in allowed for c in upper_stem + upper_suffix)
              and name == name.upper())
    if simple:
        short = (upper_stem.ljust(8) + upper_suffix.ljust(3)).encode('ascii')
        return short, []

    clean_stem = ''.join(c for c in upper_stem if c in allowed) or 'FILE'
    clean_ext = ''.join(c for c in upper_suffix if c in allowed)[:3]
    short = (clean_stem[:6] + '~1').ljust(8) + clean_ext.ljust(3)
    encoded = name.encode('utf-16-le')
    units = list(struct.unpack('<' + 'H' * (len(encoded) // 2), encoded)) + [0]
    while len(units) % 13:
        units.append(0xffff)
    checksum = 0
    for byte in short.encode('ascii'):
        checksum = (((checksum & 1) << 7) | (checksum >> 1)) + byte
        checksum &= 0xff
    chunks = [units[i:i + 13] for i in range(0, len(units), 13)]
    entries = []
    for index in range(len(chunks), 0, -1):
        seq = index | (0x40 if index == len(chunks) else 0)
        entry = bytearray(32)
        entry[0], entry[11], entry[13] = seq, 0x0f, checksum
        for offset, unit in zip((1, 3, 5, 7, 9), chunks[index - 1][:5]):
            struct.pack_into('<H', entry, offset, unit)
        for offset, unit in zip((14, 16, 18, 20, 22, 24), chunks[index - 1][5:11]):
            struct.pack_into('<H', entry, offset, unit)
        struct.pack_into('<H', entry, 26, 0)
        for offset, unit in zip((28, 30), chunks[index - 1][11:13]):
            struct.pack_into('<H', entry, offset, unit)
        entries.append(bytes(entry))
    return short.encode('ascii'), entries


def _decode_lfn(entries: list[bytes]) -> str:
    units = []
    for entry in reversed(entries):
        units.extend(struct.unpack_from('<5H', entry, 1))
        units.extend(struct.unpack_from('<6H', entry, 14))
        units.extend(struct.unpack_from('<2H', entry, 28))
    values = []
    for unit in units:
        if unit in (0, 0xffff):
            break
        values.append(unit)
    return bytes().join(struct.pack('<H', unit) for unit in values).decode('utf-16-le')


def add_file(image: Path, name: str, payload: bytes) -> dict:
    image = Path(image)
    if not payload:
        raise ValueError('不接受空文件')
    if len(payload) > MAX_UPLOAD:
        raise ValueError(f'文件超过 {MAX_UPLOAD // (1024 * 1024)} MiB 限制')
    short_name, lfn_entries = _fat_name(name)

    with image.open('r+b', buffering=0) as card:
        mbr = card.read(512)
        if len(mbr) != 512 or mbr[510:512] != b'\x55\xaa':
            raise ValueError('卡片镜像没有有效 MBR')
        partitions = [struct.unpack_from('<B3sB3sII', mbr, 446 + i * 16)
                      for i in range(4)]
        fat_partitions = [p for p in partitions if p[2] in (0x0b, 0x0c)]
        if len(fat_partitions) != 1:
            raise ValueError('只支持单个 FAT32 分区的卡片镜像')
        start_sector, sector_count = fat_partitions[0][4:]
        base = start_sector * 512
        card.seek(base)
        boot = card.read(512)
        bps, spc = _u16(boot, 11), boot[13]
        reserved, fat_count = _u16(boot, 14), boot[16]
        fat_sectors, root_cluster = _u32(boot, 36), _u32(boot, 44)
        if (boot[510:512] != b'\x55\xaa' or boot[82:90] != b'FAT32   '
                or bps != 512 or not spc or spc & (spc - 1) or fat_count not in (1, 2)):
            raise ValueError('卡片分区不是受支持的 FAT32 布局')
        cluster_bytes = bps * spc
        data_sector = reserved + fat_count * fat_sectors
        total_sectors = _u16(boot, 19) or _u32(boot, 32)
        cluster_count = (total_sectors - data_sector) // spc
        if cluster_count < 1 or base + total_sectors * bps > image.stat().st_size:
            raise ValueError('FAT32 分区几何信息越界')
        fat_base = base + reserved * bps
        data_base = base + data_sector * bps
        card.seek(fat_base)
        fat_bytes = bytearray(card.read(fat_sectors * bps))

        def cluster_offset(cluster: int) -> int:
            if cluster < 2 or cluster >= cluster_count + 2:
                raise ValueError('FAT32 簇号越界')
            return data_base + (cluster - 2) * cluster_bytes

        def fat_get(cluster: int) -> int:
            return _u32(fat_bytes, cluster * 4) & 0x0fffffff

        def fat_set(cluster: int, value: int) -> None:
            struct.pack_into('<I', fat_bytes, cluster * 4, value)
            for index in range(fat_count):
                card.seek(fat_base + index * fat_sectors * bps + cluster * 4)
                card.write(struct.pack('<I', value))

        def chain(first: int) -> list[int]:
            result, current = [], first
            while 2 <= current < 0x0ffffff8:
                if current in result or len(result) > cluster_count:
                    raise ValueError('FAT32 簇链循环或超长')
                result.append(current)
                value = fat_get(current)
                if value >= 0x0ffffff8:
                    break
                if value < 2:
                    raise ValueError('FAT32 簇链损坏')
                current = value
            return result

        directory_chain = chain(root_cluster)
        slots = []
        existing_names = set()
        lfn_run = []
        for cluster in directory_chain:
            card.seek(cluster_offset(cluster))
            directory = card.read(cluster_bytes)
            for offset in range(0, cluster_bytes, 32):
                entry = directory[offset:offset + 32]
                if entry[0] == 0:
                    slots.append((cluster, offset, True))
                    continue
                if entry[0] == 0xe5:
                    slots.append((cluster, offset, True))
                    lfn_run = []
                    continue
                slots.append((cluster, offset, False))
                if entry[11] == 0x0f:
                    lfn_run.append(entry)
                    continue
                if entry[11] & 0x08:
                    lfn_run = []
                    continue
                alias = entry[:11].decode('ascii', errors='replace').rstrip()
                existing_names.add(alias.casefold())
                if lfn_run:
                    try:
                        existing_names.add(_decode_lfn(lfn_run).casefold())
                    except (UnicodeError, struct.error):
                        pass
                lfn_run = []
        wanted = name.casefold()
        if wanted in existing_names:
            raise ValueError('卡片根目录已存在同名文件')
        alias = short_name.decode('ascii').casefold()
        if alias in existing_names:
            raise ValueError('文件名生成的 FAT 短别名已存在')

        needed_clusters = (len(payload) + cluster_bytes - 1) // cluster_bytes
        free_clusters = []
        for cluster in range(2, cluster_count + 2):
            if fat_get(cluster) == 0:
                free_clusters.append(cluster)
                if len(free_clusters) == needed_clusters:
                    break
        if len(free_clusters) != needed_clusters:
            raise ValueError('卡片剩余空间不足')

        file_entry = bytearray(32)
        file_entry[:11], file_entry[11] = short_name, 0x20
        struct.pack_into('<H', file_entry, 20, (free_clusters[0] >> 16) if free_clusters else 0)
        struct.pack_into('<H', file_entry, 26, (free_clusters[0] & 0xffff) if free_clusters else 0)
        struct.pack_into('<I', file_entry, 28, len(payload))
        records = lfn_entries + [bytes(file_entry)]
        needed_slots = len(records)

        # Append after the final live directory entry; extend the root chain if needed.
        end_index = max((i + 1 for i, (_, _, free) in enumerate(slots) if not free), default=0)
        available = len(slots) - end_index
        while available < needed_slots + 1:
            new_cluster = next((c for c in range(2, cluster_count + 2)
                                if fat_get(c) == 0 and c not in free_clusters), None)
            if new_cluster is None:
                raise ValueError('卡片没有可扩展根目录的空闲簇')
            fat_set(directory_chain[-1], new_cluster)
            fat_set(new_cluster, 0x0fffffff)
            directory_chain.append(new_cluster)
            card.seek(cluster_offset(new_cluster))
            card.write(bytes(cluster_bytes))
            slots.extend((new_cluster, off, False) for off in range(0, cluster_bytes, 32))
            available += cluster_bytes // 32

        target_slots = slots[end_index:end_index + needed_slots]
        for (cluster, offset, _), entry in zip(target_slots, records):
            card.seek(cluster_offset(cluster) + offset)
            card.write(entry)
        next_slot = slots[end_index + needed_slots]
        card.seek(cluster_offset(next_slot[0]) + next_slot[1])
        card.write(bytes(32))

        for index, cluster in enumerate(free_clusters):
            next_cluster = free_clusters[index + 1] if index + 1 < len(free_clusters) else 0x0fffffff
            fat_set(cluster, next_cluster)
        for index in range(fat_count):
            card.seek(fat_base + index * fat_sectors * bps)
            card.write(fat_bytes)
        for index, cluster in enumerate(free_clusters):
            card.seek(cluster_offset(cluster))
            block = payload[index * cluster_bytes:(index + 1) * cluster_bytes]
            card.write(block.ljust(cluster_bytes, b'\0'))

        fsinfo_sector = _u16(boot, 48)
        if fsinfo_sector not in (0, 0xffff):
            for sector in {fsinfo_sector, _u16(boot, 50) + fsinfo_sector}:
                if sector >= total_sectors:
                    continue
                offset = base + sector * bps
                card.seek(offset)
                info = bytearray(card.read(bps))
                if info[:4] == b'RRaA' and info[484:488] == b'rrAa':
                    free_count = _u32(info, 488)
                    if free_count != 0xffffffff:
                        struct.pack_into('<I', info, 488, max(0, free_count - needed_clusters))
                    struct.pack_into('<I', info, 492, free_clusters[-1] + 1)
                    card.seek(offset)
                    card.write(info)
        card.flush()
    return {'name': name, 'bytes': len(payload), 'clusters': needed_clusters}
