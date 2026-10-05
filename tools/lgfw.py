#!/usr/bin/env python3
"""Make, sign and check LocalGrid firmware update files (.lgfw).

An update file is what a grid accepts as new firmware: a signed header and the image behind it.
`site/sign/index.html` does the same in a browser, with nothing installed; this is the command-line
form, for a build machine, and the reference the browser page is checked against. The layout is in
docs/firmware-updates.md and in HEADER below, and the two tools must agree byte for byte.

  python tools/lgfw.py keygen release                     # release.key.pem (keep it) and release.pub.hex
  python tools/lgfw.py sign  --key release.key.pem --role N --chip esp32 --version 2026.10.5 \
         firmware/node/build-esp32/lg_node.bin -o lg_node-esp32.lgfw
  python tools/lgfw.py check --pub release.pub.hex lg_node-esp32.lgfw
  python tools/lgfw.py show  lg_node-esp32.lgfw

Needs the `cryptography` package, which the ESP-IDF Python environment already has.
The private key is the grid's whole defence against a forged update: keep it off the bench boards,
out of git, and backed up. A grid trusts the public half, and only that.
"""
import argparse
import hashlib
import pathlib
import struct
import sys
import time

MAGIC = b"LGFW"
FORMAT = 1
HEADER_LEN = 128
SIG_LEN = 64            # ECDSA P-256 over SHA-256, raw r || s (IEEE P1363), as WebCrypto and PSA both use
CHIPS = {"esp32": 1, "esp32s3": 2, "esp32c6": 3}
ROLES = {"N": "access point", "H": "handheld"}
#   0  4s  magic "LGFW"
#   4  B   format (1)
#   5  c   role: N access point, H handheld
#   6  B   chip: 1 esp32, 2 esp32s3, 3 esp32c6
#   7  B   reserved, 0
#   8  I   serial: the number newer-wins compares; the signer's clock in seconds by default
#  12  I   image length in bytes
#  16  32s SHA-256 of the image
#  48  8s  key id: the first 8 bytes of SHA-256 of the signer's public key (65 bytes, uncompressed)
#  56  32s version text, UTF-8, NUL padded
#  88  40s reserved, 0
# 128      signature over bytes 0..127, 64 bytes
# 192      the image
HEADER = struct.Struct("<4sBcBBII32s8s32s40s")
assert HEADER.size == HEADER_LEN


def crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec, utils
    except ImportError:
        sys.exit("lgfw: the 'cryptography' package is missing; run inside the ESP-IDF Python environment")
    return hashes, serialization, ec, utils


def public_bytes(key):
    _, serialization, _, _ = crypto()
    return key.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def key_id(pub):
    return hashlib.sha256(pub).digest()[:8]


def parse(blob):
    if len(blob) < HEADER_LEN + SIG_LEN:
        raise ValueError("too short to be an update file")
    magic, fmt, role, chip, _, serial, length, digest, kid, text, _ = HEADER.unpack(blob[:HEADER_LEN])
    if magic != MAGIC or fmt != FORMAT:
        raise ValueError("not a LocalGrid update file (format 1)")
    image = blob[HEADER_LEN + SIG_LEN:]
    return {
        "role": role.decode("ascii", "replace"), "chip": chip, "serial": serial, "length": length,
        "sha256": digest, "key_id": kid, "version": text.rstrip(b"\0").decode("utf-8", "replace"),
        "signature": blob[HEADER_LEN:HEADER_LEN + SIG_LEN], "image": image, "header": blob[:HEADER_LEN],
    }


def cmd_keygen(args):
    _, serialization, ec, _ = crypto()
    key_path = pathlib.Path(args.name + ".key.pem")
    pub_path = pathlib.Path(args.name + ".pub.hex")
    if key_path.exists():
        sys.exit(f"lgfw: {key_path} exists; a release key is never overwritten")
    key = ec.generate_private_key(ec.SECP256R1())
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    pub = public_bytes(key.public_key())
    pub_path.write_text(pub.hex() + "\n")
    print(f"private key: {key_path}   (keep it safe and out of git)")
    print(f"public key:  {pub_path}   key id {key_id(pub).hex()}")


def cmd_sign(args):
    hashes, serialization, ec, utils = crypto()
    key = serialization.load_pem_private_key(pathlib.Path(args.key).read_bytes(), password=None)
    image = pathlib.Path(args.image).read_bytes()
    if not image or image[0] != 0xE9:
        sys.exit("lgfw: that does not look like an ESP application image (it should begin with 0xE9)")
    text = args.version.encode("utf-8")
    if len(text) > 32:
        sys.exit("lgfw: the version text is longer than 32 bytes")
    pub = public_bytes(key.public_key())
    serial = args.serial if args.serial is not None else int(time.time())
    header = HEADER.pack(MAGIC, FORMAT, args.role.encode(), CHIPS[args.chip], 0, serial, len(image),
                         hashlib.sha256(image).digest(), key_id(pub), text, b"")
    r, s = utils.decode_dss_signature(key.sign(header, ec.ECDSA(hashes.SHA256())))
    out = pathlib.Path(args.out or pathlib.Path(args.image).with_suffix(".lgfw").name)
    out.write_bytes(header + r.to_bytes(32, "big") + s.to_bytes(32, "big") + image)
    print(f"{out}: {ROLES[args.role]} for {args.chip}, version {args.version}, serial {serial}, "
          f"{len(image)} bytes, key id {key_id(pub).hex()}")


def cmd_check(args):
    hashes, _, ec, utils = crypto()
    pub = bytes.fromhex(pathlib.Path(args.pub).read_text().strip())
    try:
        f = parse(pathlib.Path(args.file).read_bytes())
    except ValueError as e:
        sys.exit(f"lgfw: REFUSED: {e}")
    problems = []
    if f["key_id"] != key_id(pub):
        problems.append("signed by a different key")
    if f["length"] != len(f["image"]) or hashlib.sha256(f["image"]).digest() != f["sha256"]:
        problems.append("the image does not match the header")
    try:
        key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), pub)
        sig = utils.encode_dss_signature(int.from_bytes(f["signature"][:32], "big"),
                                         int.from_bytes(f["signature"][32:], "big"))
        key.verify(sig, f["header"], ec.ECDSA(hashes.SHA256()))
    except Exception:
        problems.append("the signature is not this key's")
    if problems:
        sys.exit("lgfw: REFUSED: " + "; ".join(problems))
    print(f"ok: {ROLES.get(f['role'], f['role'])}, chip {f['chip']}, version {f['version']}, serial {f['serial']}")


def cmd_show(args):
    try:
        f = parse(pathlib.Path(args.file).read_bytes())
    except ValueError as e:
        sys.exit(f"lgfw: {e}")
    chip = next((k for k, v in CHIPS.items() if v == f["chip"]), f"chip {f['chip']}")
    print(f"{ROLES.get(f['role'], f['role'])} for {chip}\nversion {f['version']}\nserial  {f['serial']}\n"
          f"image   {f['length']} bytes, sha256 {f['sha256'].hex()}\nkey id  {f['key_id'].hex()}\n"
          f"(not checked against a key: use `check --pub`)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("keygen", help="make a release key pair")
    p.add_argument("name")
    p.set_defaults(func=cmd_keygen)
    p = sub.add_parser("sign", help="sign an application image into an update file")
    p.add_argument("image")
    p.add_argument("--key", required=True)
    p.add_argument("--role", required=True, choices=sorted(ROLES))
    p.add_argument("--chip", required=True, choices=sorted(CHIPS))
    p.add_argument("--version", required=True)
    p.add_argument("--serial", type=int, help="the number newer-wins compares (default: now, in seconds)")
    p.add_argument("-o", "--out")
    p.set_defaults(func=cmd_sign)
    p = sub.add_parser("check", help="check an update file against a public key")
    p.add_argument("file")
    p.add_argument("--pub", required=True)
    p.set_defaults(func=cmd_check)
    p = sub.add_parser("show", help="print an update file's header")
    p.add_argument("file")
    p.set_defaults(func=cmd_show)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
