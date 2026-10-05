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


def cmd_send(args):
    """Types an update file at a board's console, 600 bytes a line, and waits for each to be taken."""
    import base64
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    import flash
    data = flash.load_map()
    device = next((d for d in data["devices"] if args.board in (d.get("name"), d.get("port"), d.get("id"))), None)
    if device is None or not device.get("port"):
        sys.exit(f"lgfw: {args.board} is not in the device map, or has no port")
    blob = pathlib.Path(args.file).read_bytes()
    # A short read timeout: each of about eight thousand lines is answered in a few milliseconds, and
    # waiting a fifth of a second for more that is not coming made one file take half an hour.
    s = flash.open_quiet(device["port"], 115200, 0.01)

    def ask(command, wait):
        s.reset_input_buffer()
        s.write(b"\r\n" + command.encode("ascii") + b"\r\n")
        reply, end = b"", time.time() + wait
        while time.time() < end:
            reply += s.read(256)
            # A reply is whole once the line after it has begun: the console prints its prompt next,
            # with no newline, so waiting for the buffer to end in one would wait for ever.
            lines = reply.decode("utf-8", "replace").split("\n")
            for i in range(len(lines) - 2, -1, -1):
                if lines[i].strip().startswith("LGFWU:"):
                    return lines[i].strip()
        return None

    started = time.time()
    try:
        print(f"{device['name']}: {ask('update', 4) or 'no answer (is this firmware older than D97?)'}")
        answer = ask("update begin", 4)
        if answer != "LGFWU: ready":
            sys.exit(f"lgfw: {device['name']} would not start: {answer}")
        # 144 bytes is 192 characters of base64: with the command in front, a line the console's serial
        # buffer takes in one piece. 600-byte lines arrived with pieces missing.
        for at in range(0, len(blob), 144):
            answer = ask("update data " + base64.b64encode(blob[at:at + 144]).decode("ascii"), 20)
            if answer is None or "refused" in answer or not answer.startswith("LGFWU: at"):
                sys.exit(f"lgfw: {device['name']} stopped at byte {at}: {answer}")
            if at % (144 * 1000) == 0:
                print(f"  {at * 100 // len(blob)} %", flush=True)
        answer = ask("update end", 30)
        print(f"{device['name']}: {answer}  ({time.time() - started:.0f} s)")
        return 0 if answer and "ok" in answer else 1
    finally:
        s.rts = False
        s.dtr = False
        time.sleep(0.1)
        s.close()


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
    p = sub.add_parser("send", help="send an update file to a board over its serial cable")
    p.add_argument("file")
    p.add_argument("board", help="a name, port or ID from the device map")
    p.set_defaults(func=cmd_send)
    p = sub.add_parser("show", help="print an update file's header")
    p.add_argument("file")
    p.set_defaults(func=cmd_show)
    args = ap.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
