#!/usr/bin/env python3
"""Make a few seconds of speech on this laptop and have a handheld talk it out (D112).

A talk normally comes from a handheld's microphone. To demonstrate and test push-to-talk with real
words and nobody's voice, this makes the speech with Piper (the demo's narrator, build/voices/), turns
it into what a handheld's talk carries - 8 kHz mono, IMA ADPCM, four bits a sample - and gives it to a
handheld over its cable with the console's `clip` command. `clip talk` then sends it through the
ordinary push-to-talk path, and every listener plays it as speech.

  python tools/voiceclip.py fnk0104b all "This is Channi, talking to everyone."
  python tools/voiceclip.py fnk0104b_c 1 "Jolly here, just for you."     # to device 1
  python tools/voiceclip.py fnk0104b all "..." --save clip.wav           # and keep the 8 kHz audio

The clip is at most 16 s (64 KB). Nothing is written on the handheld but its RAM.
"""
import argparse
import base64
import io
import pathlib
import sys
import time
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
VOICES = ROOT / "build" / "voices"
VOICE = "en_US-amy-medium"
RATE = 8000
LINE_BYTES = 144          # 192 base64 characters a line: what the console takes in one piece (tools/lgfw.py)

# IMA ADPCM, as firmware/handheld/main/service/hh_adpcm.c: the coder starts from zero, low nibble first.
INDEX_ADJUST = [-1, -1, -1, -1, 2, 4, 6, 8]
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
         97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
         724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
         4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
         18500, 20350, 22385, 24623, 27086, 29794, 32767]


def ima_encode(samples):
    pred, index, out, low = 0, 0, bytearray(), None
    for s in samples:
        step = STEPS[index]
        diff = int(s) - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        delta = step >> 3
        if diff >= step:
            code |= 4; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 2; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 1; delta += step
        pred = max(-32768, min(32767, pred - delta if code & 8 else pred + delta))
        index = max(0, min(88, index + INDEX_ADJUST[code & 7]))
        if low is None:
            low = code
        else:
            out.append(low | (code << 4))
            low = None
    if low is not None:
        out.append(low)
    return bytes(out)


def speech(text, voice=VOICE):
    """Piper's speech for text, as 8 kHz 16-bit samples, loud but not clipped."""
    import numpy as np
    from piper import PiperVoice
    from scipy.signal import resample_poly
    model = VOICES / f"{voice}.onnx"
    v = PiperVoice.load(str(model))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        (v.synthesize_wav if hasattr(v, "synthesize_wav") else v.synthesize)(text, w)
    buf.seek(0)
    with wave.open(buf, "rb") as w:
        rate = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    from math import gcd
    g = gcd(RATE, rate)
    y = resample_poly(x, RATE // g, rate // g)          # band-limited: nothing above 4 kHz folds back
    peak = np.max(np.abs(y)) or 1.0
    y = y * (26000.0 / peak)                            # the handheld's AGC targets about this
    y = np.concatenate([np.zeros(RATE // 10), y, np.zeros(RATE // 5)])   # a breath before and after
    if len(y) % 2:
        y = np.append(y, 0.0)
    return np.clip(y, -32768, 32767).astype(np.int16)


def clip_bytes(text, voice=VOICE):
    pcm = speech(text, voice)
    data = ima_encode(pcm)
    if len(data) > 64 * 1024:
        raise ValueError(f"{len(pcm) / RATE:.1f} s of speech is longer than a clip may be (16 s)")
    return data, pcm


def lines(data):
    """The console commands that give a clip to a handheld, in order (the last starts nothing)."""
    yield f"clip begin {len(data)}"
    for at in range(0, len(data), LINE_BYTES):
        yield "clip data " + base64.b64encode(data[at:at + LINE_BYTES]).decode("ascii")


def send(port, data, target, log=print):
    """Gives the clip to the handheld on port and has it talk it to target. Returns True once talking."""
    sys.path.insert(0, str(ROOT / "tools"))
    from flash import open_quiet
    s = open_quiet(port, 115200, 0.01)

    def ask(cmd, wait):
        s.reset_input_buffer()
        s.write(b"\r\n" + cmd.encode("ascii") + b"\r\n")
        got, end = b"", time.time() + wait
        while time.time() < end:
            got += s.read(256)
            for line in got.decode("utf-8", "replace").split("\n")[:-1]:
                if line.strip().startswith("CLIP:"):
                    return line.strip()
        return None

    try:
        for cmd in lines(data):
            answer = ask(cmd, 6)
            if answer is None or "refused" in answer or answer.startswith("CLIP: ESP_"):
                log(f"stopped at '{cmd[:16]}': {answer}")
                return False
        answer = ask(f"clip talk {target}", 6)
        log(answer or "no answer to clip talk")
        return bool(answer and "talking" in answer)
    finally:
        s.dtr = s.rts = False
        s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board", help="a handheld from tools/bench_devices.json")
    ap.add_argument("target", help="a device number, a group name, or all")
    ap.add_argument("text", help="what to say")
    ap.add_argument("--voice", default=VOICE)
    ap.add_argument("--save", help="also write the 8 kHz speech to this .wav")
    args = ap.parse_args()
    sys.path.insert(0, str(ROOT / "tools"))
    from flash import load_map
    d = next((x for x in load_map()["devices"] if x.get("name") == args.board), None)
    if d is None or not d.get("port"):
        sys.exit(f"voiceclip: {args.board} is not in the device map, or has no port")
    data, pcm = clip_bytes(args.text, args.voice)
    print(f"{len(pcm) / RATE:.1f} s of speech, {len(data)} bytes")
    if args.save:
        with wave.open(args.save, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(pcm.tobytes())
    t = time.time()
    ok = send(d["port"], data, args.target)
    print(f"given in {time.time() - t:.1f} s" if ok else "not started")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
