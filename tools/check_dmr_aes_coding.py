# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Check research coding derivations against ETSI TS 102 361-1 V2.6.1.

These are standard-derived protocol vectors, not an AES implementation or a
claim of DMRA interoperability. Run with python3 tools/check_dmr_aes_coding.py.
"""


def cyclic(data, degree, polynomial):
    remainder = data << degree
    while remainder.bit_length() > degree:
        remainder ^= polynomial << (remainder.bit_length() - degree - 1)
    return (data << degree) | remainder


def extend(word):
    return (word << 1) | (word.bit_count() & 1)


def golay(data):
    return extend(cyclic(data, 11, 0xC75))


def hamming(data):
    return extend(cyclic(data, 4, 0x13))


def single_burst(word):
    transmitted = [0] * 32
    for column in range(16):
        for row in range(2):
            index = 2 * column + row
            transmitted[(17 * index) % 32] = (word >> (15 - column)) & 1
    return int("".join(map(str, transmitted)), 2)


def main():
    # Independently transcribed parity suffixes: tables B.11 and B.16,
    # first information bit first. Compare matrices to polynomial division.
    golay_parity = (0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB)
    hamming_parity = (0x13, 0x1A, 0x1F, 0x1C, 0x0E, 0x15,
                      0x0B, 0x16, 0x19, 0x0D, 0x07)
    for row, parity in enumerate(golay_parity):
        data = 1 << (7 - row)
        assert golay(data) == (data << 12) | parity
    for data in range(2048):
        matrix_word = data << 5
        for row, parity in enumerate(hamming_parity):
            if data & (1 << (10 - row)):
                matrix_word ^= parity
        assert hamming(data) == matrix_word

    # Table B.5 labels the transmitted bits. PC(15..0) duplicates the
    # Hamming word, so these are its column numbers in transmission order.
    columns = (0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15,
               8, 0, 9, 1, 10, 2, 11, 3, 12, 4, 13, 5, 14, 6, 15, 7)
    for data in range(2048):
        word = hamming(data)
        table_word = 0
        for column in columns:
            table_word = (table_word << 1) | ((word >> (15 - column)) & 1)
        assert single_burst(word) == table_word
    # Linear codes: minimum nonzero weight is minimum pairwise distance.
    assert min(golay(d).bit_count() for d in range(1, 4096)) == 8
    assert min(hamming(d).bit_count() for d in range(1, 2048)) == 4
    assert min(single_burst(hamming(d)).bit_count() for d in range(1, 2048)) == 8
    print("PASS: ETSI Golay shortening, Hamming matrix, single-burst interleave, distances")
    print("Golay basis (input:codeword):")
    print(" ".join(f"{1 << i:03X}:{golay(1 << i):06X}" for i in range(12)))
    print("Single-burst vectors (input:Hamming:transmitted):")
    for data in (0, 1, 5, 0x0D, 0x123, 0x7FF):
        print(f"{data:03X}:{hamming(data):04X}:{single_burst(hamming(data)):08X}")


if __name__ == "__main__":
    main()
