<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Key check values

A key check value (KCV) is three octets computed from a key, shown as six hex digits (`3FA2C1`). Two radios, or a
radio and the Codeplug Editor, show the same KCV for the same key, so their owners can see they hold the same key
without either key being shown. Status: built and host-tested (`keyCryptoTest`, the UI flow tests, `kcv.test.mjs`);
untested on a radio.

Sources: FIPS-197 (AES), FIPS 180-4 (SHA-256). The AES form is the check value key management tools commonly show for
a block cipher key. The hashed form is ours.

## 1. Files

| Item | Where |
|---|---|
| Check values | `core/support/kcv.c` (`kcv.h`) |
| The stored keys as one list | `core/codeplug/key_store.c` (`key_store.h`) |
| Radio UI | Settings > Privacy: Key check, Stored keys (`core/ui/ui_menus.c`, `ui_svc_privacy_kcv`, `ui_svc_key`) |
| Codeplug Editor | `site/static/js/codeplug/kcv.js`; the Key check column of a system's traffic key table (`site/static/js/editor/modesview.js`) |
| Vectors | `tests/vectors/key-check.txt`, read by the C and the JS tests |

No switch: the code is about 0.6 KB of flash and no RAM, and SHA-256 is in the image already (support info).

## 2. The hashed check value

For every key that is not a block cipher key (3):

```
KCV = first 3 octets of SHA-256( "RFKCV1" | class | a | b | length | key )
```

| Field | Octets | Value |
|---|---|---|
| Label | 6 | ASCII `RFKCV1` |
| Class | 1 | 1 a privacy key slot (2.1), 2 a traffic key (2.2) |
| a | 1 | The privacy type, or the system type |
| b | 1 | 0, or the traffic key's algorithm id |
| Length | 2 | The key's octets, big endian |
| Key | length | 2.1, 2.2 |

The label keeps this value apart from any other SHA-256 of the same octets. The class, type and algorithm keep two
kinds of key with the same octets apart: an NXDN seed and a basic privacy key of the same number give different
values.

### 2.1 A privacy key slot

A slot of the settings store ([settings-store.md](settings-store.md) 3.1): a = its type (`dmrPrivacyType_t`: 1
Motorola BP, 2 Anytone BP), b = 0, the key = its value as 4 octets, big endian. Motorola key 1 is `9507A7`.

### 2.2 A traffic key

A record of the traffic key family ([channel-modes.md](channel-modes.md) 5.4): a = its system's type, b = its
algorithm id, the key = the record's octets from offset 5. The key ID and the system's number are not covered: the
same key under another ID, or on another radio's system n, has the same KCV.

## 3. The AES check value

A traffic key whose algorithm is AES: the first 3 octets of AES under the key of 16 zero octets. Only 32-octet
(AES-256) keys; another length has no KCV. `kcv.c` lists the (system type, algorithm) pairs that are AES. A mode whose
keys are AES adds its row when it claims its ids ([channel-modes.md](channel-modes.md) 4), with its AES code
(`CONFIG_DMR_AES`). A build without AES shows `-` for such a key. The Codeplug Editor marks a type's traffic keys as AES
with `traffic.kcv = "aes"`.

## 4. Where it shows

| Where | What |
|---|---|
| Settings > Privacy > Key check | Slot 1's KCV, `-` with no key. OK says how to use it. Dimmed until a key type is picked |
| Settings > Privacy > Stored keys | Every stored key, one row each: `Privacy 1 Moto` or the system's name and `#` key ID, its KCV as the value. The privacy slots that hold a valid key first, then the traffic keys whose system exists, in the area's order. A key with no KCV in this build shows `-` |
| Codeplug Editor, a system's traffic keys | A Key check column, computed from the row as typed, `-` while a field doesn't parse. Shown when the key itself is hidden |

## 5. What a KCV gives away

A KCV of a key with few possible values names the key: Motorola BP has 255, Anytone BP and NXDN seeds 65,536 and
32,767, and anyone can try them all. For these keys the KCV is a convenience, not a secret; the radio shows the key
itself in Settings anyway. For a 256-bit key, three octets narrow nothing an attacker can use: about one key in 16.7
million shares a KCV, and finding one still means searching the whole key space. Two different keys can share a KCV
(1 in 16.7 million): a match says the keys are almost surely the same, a mismatch that they differ.

## 6. Tests

- `tests/host/keyCryptoTest.c`: the vectors, the AES form with `CONFIG_DMR_AES`, and that the AES form is refused
  without it.
- `tests/host/uiFlowTest.c`: Key check after a key is typed, its why-not, the Stored keys screen.
- `site/tests/kcv.test.mjs`: the same vectors through `kcv.js`, and the traffic key record form.
