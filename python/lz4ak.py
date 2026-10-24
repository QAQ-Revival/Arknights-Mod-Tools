# Adapted from isHarryh/Ark-Unpacker src/lz4ak/Block.py (BSD-3-Clause).
# Copyright (c) 2022-2026, Harry Huang. See licenses/Ark-Unpacker-LICENSE.txt.
# The original implementation credits Kengxxiao and MooncellWiki/UnityPy.
import lz4.block


def decompress_lz4ak(data, size):
    if size < 0 or size > 256 * 1024 * 1024:
        raise ValueError("Invalid LZ4AK block size")
    fixed = bytearray(data)
    ip = op = 0

    def extra(position):
        length = 0
        while True:
            if position >= len(fixed):
                raise ValueError("Truncated LZ4AK length")
            value = fixed[position]
            position += 1
            length += value
            if value != 255:
                return length, position

    while ip < len(fixed):
        literal = fixed[ip] & 15
        match = fixed[ip] >> 4
        fixed[ip] = (literal << 4) | match
        ip += 1
        if literal == 15:
            length, ip = extra(ip)
            literal += length
        ip += literal
        op += literal
        if ip > len(fixed) or op > size:
            raise ValueError("Invalid LZ4AK literal")
        if op == size:
            break
        if ip + 2 > len(fixed):
            raise ValueError("Truncated LZ4AK offset")
        offset = (fixed[ip] << 8) | fixed[ip + 1]
        if offset == 0 or offset > op:
            raise ValueError("Invalid LZ4AK offset")
        fixed[ip], fixed[ip + 1] = fixed[ip + 1], fixed[ip]
        ip += 2
        if match == 15:
            length, ip = extra(ip)
            match += length
        op += match + 4
        if op > size:
            raise ValueError("Invalid LZ4AK match")
    if op != size or ip != len(fixed):
        raise ValueError("Invalid LZ4AK output size")
    return lz4.block.decompress(fixed, uncompressed_size=size)
