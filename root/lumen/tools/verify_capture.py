"""Compare guest MMIO readback with the native renderer, then export PNG previews."""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib


def read_ppm(path):
    """Read the fixed P6 format emitted by both LUMEN diagnostic programs."""
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or len(pixels) != width * height * 3:
        raise ValueError(f"{path}: expected a complete 8-bit RGB P6 image")
    return width, height, pixels


def double_pixels(width, height, data):
    """Expand each pixel twice on both axes without filtering the guest palette."""
    output = bytearray()
    for y in range(height):
        row = bytearray()
        for x in range(width):
            offset = (y * width + x) * 3
            row.extend(data[offset:offset + 3] * 2)
        output.extend(row * 2)
    return bytes(output)


def png_chunk(kind, payload):
    """Encode one standard PNG chunk with its payload length and CRC."""
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload)))


def write_png(path, width, height, pixels):
    """Write a lossless RGB PNG using only the host Python standard library."""
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    header = struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
                     png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b""))


def main():
    """Fail on any guest/native pixel difference before producing review images."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest-dir", type=Path, required=True)
    parser.add_argument("--preview", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for scene in ("title", "game", "map", "action", "lift"):
        native = args.output / f"native-{scene}.ppm"
        subprocess.run([str(args.preview.resolve()), str(native), scene,
                        "35" if scene == "action" else "0"], check=True)
        width, height, pixels = read_ppm(args.guest_dir / f"lumen-{scene}.ppm")
        nw, nh, expected = read_ppm(native)
        enlarged = double_pixels(width, height, pixels)
        if nw != width * 2 or nh != height * 2 or enlarged != expected:
            differences = sum(a != b for a, b in zip(enlarged, expected))
            raise RuntimeError(f"{scene}: guest/native display differs ({differences} RGB bytes)")
        target = args.output / f"lumen-{scene}.png"
        write_png(target, nw, nh, enlarged)
        print(f"PASS {scene}: guest MMIO matches native pixels exactly -> {target}")


if __name__ == "__main__":
    main()
