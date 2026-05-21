#!/usr/bin/env python3
"""Generate the private QWeather JWT header used by Tomato Glow Clock."""

from __future__ import annotations

import argparse
import base64
import pathlib
import re
import sys


def read_pem_der(path: pathlib.Path) -> bytes:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        r"-----BEGIN PRIVATE KEY-----(.*?)-----END PRIVATE KEY-----",
        text,
        re.DOTALL,
    )
    if not match:
        raise ValueError("expected a PKCS#8 PEM private key with BEGIN PRIVATE KEY")
    return base64.b64decode(re.sub(r"\s+", "", match.group(1)))


def read_der_length(data: bytes, pos: int) -> tuple[int, int]:
    if pos >= len(data):
        raise ValueError("truncated DER length")
    first = data[pos]
    pos += 1
    if first < 0x80:
        return first, pos
    n = first & 0x7F
    if n == 0 or n > 4 or pos + n > len(data):
        raise ValueError("invalid DER length")
    return int.from_bytes(data[pos : pos + n], "big"), pos + n


def read_der_tlv(data: bytes, pos: int) -> tuple[int, bytes, int]:
    if pos >= len(data):
        raise ValueError("truncated DER tag")
    tag = data[pos]
    length, value_pos = read_der_length(data, pos + 1)
    end = value_pos + length
    if end > len(data):
        raise ValueError("truncated DER value")
    return tag, data[value_pos:end], end


def extract_ed25519_seed(der: bytes) -> bytes:
    # OpenSSL Ed25519 PKCS#8 private keys contain the 32-byte seed inside
    # the outer privateKey OCTET STRING as: 04 20 <seed>.
    pos = 0
    tag, seq, end = read_der_tlv(der, pos)
    if tag != 0x30 or end != len(der):
        raise ValueError("expected DER SEQUENCE")

    pos = 0
    tag, _, pos = read_der_tlv(seq, pos)
    if tag != 0x02:
        raise ValueError("expected PKCS#8 version")
    tag, alg, pos = read_der_tlv(seq, pos)
    if tag != 0x30:
        raise ValueError("expected PKCS#8 algorithm identifier")
    if b"\x06\x03\x2b\x65\x70" not in alg:
        raise ValueError("private key is not Ed25519")
    tag, private_key, _ = read_der_tlv(seq, pos)
    if tag != 0x04:
        raise ValueError("expected PKCS#8 private key octet string")

    tag, seed, end = read_der_tlv(private_key, 0)
    if tag != 0x04 or end != len(private_key) or len(seed) != 32:
        raise ValueError("expected a 32-byte Ed25519 seed")
    return seed


def format_seed(seed: bytes) -> str:
    lines = []
    for offset in range(0, len(seed), 8):
        chunk = seed[offset : offset + 8]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-key", required=True, type=pathlib.Path)
    parser.add_argument("--credential-id", required=True)
    parser.add_argument("--project-id", required=True)
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("main/APP/qweather_jwt_secret.h"),
    )
    args = parser.parse_args()

    seed = extract_ed25519_seed(read_pem_der(args.private_key))
    output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        f"""#pragma once

/*
 * Private QWeather JWT credential for Tomato Glow Clock.
 * Do not commit this file.
 */

#define TOMATO_QWEATHER_JWT_CREDENTIAL_ID "{args.credential_id}"
#define TOMATO_QWEATHER_JWT_PROJECT_ID "{args.project_id}"

static const unsigned char TOMATO_QWEATHER_ED25519_SEED[32] = {{
{format_seed(seed)}
}};
""",
        encoding="utf-8",
    )
    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
