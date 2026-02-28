"""Convert bitmap assets into assembler files that use `.fill`.

Purpose:
- Convert Dioptase OS bitmap sheets into `.s` files that can be assembled into
  the kernel or other images.
- Emit raw pixel bytes via `.fill` words (two 16-bit pixels packed per word),
  so the output uses only assembler directives and preserves byte layout.

Inputs:
- One or more bitmap paths, or no paths (which means "all .bmp files in this
  directory").

Outputs:
- One `.s` file per input bitmap in the selected output directory.

Important assumptions:
- Pixels are emitted in cell-major order, then row-major within each cell.
- Known sprite/tile sheets use fixed cell sizes (32x32 or 8x8).
- Pixel encoding preserves the legacy channel ordering used by the previous
  script (`0x0BGR` nibble layout from an `(R, G, B)` tuple).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable, Iterator, Sequence


@dataclass(frozen=True)
class SheetLayout:
    """Describe how to linearize a bitmap sheet into cells.

    Purpose:
    - Define the sub-image cell size used to flatten sprite/tile sheets into a
      contiguous stream.

    Inputs:
    - `cell_width`, `cell_height`: dimensions of each cell in pixels.
    - `transparent_from_top_left`: if true, pixels matching the top-left pixel
      of each cell are replaced with `0xFFFF`.

    Outputs:
    - Used by `iter_sheet_pixels()` to determine pixel ordering and optional
      transparency replacement.

    Invariants:
    - Cell dimensions must be positive.
    """

    cell_width: int
    cell_height: int
    transparent_from_top_left: bool = False


# Known sheets in this repo. These layouts preserve the expected "frame/tile
# contiguous" order when loading assets into sprite/tile memory.
KNOWN_LAYOUTS: dict[str, SheetLayout] = {
    "spritemap.bmp": SheetLayout(cell_width=32, cell_height=32),
    "dinorunsheet.bmp": SheetLayout(cell_width=32, cell_height=32),
    "sunsheet.bmp": SheetLayout(cell_width=32, cell_height=32),
    "tilemap.bmp": SheetLayout(cell_width=8, cell_height=8),
}


def _import_pillow_image():
    """Import Pillow lazily so `--help` works even if Pillow is missing."""

    try:
        from PIL import Image  # type: ignore
    except ModuleNotFoundError as exc:
        raise SystemExit(
            "Pillow is required to convert images (pip install pillow)."
        ) from exc
    return Image


def encode_pixel_16bit(pixel: tuple[int, int, int]) -> int:
    """Encode an RGB pixel into the legacy 16-bit Dioptase asset format.

    Purpose:
    - Match the nibble encoding previously emitted by this script so existing
      assets retain the same channel ordering.

    Inputs:
    - `pixel`: `(R, G, B)` tuple from Pillow.

    Outputs:
    - 16-bit integer in the form `0x0BGR` (legacy script behavior).

    Invariants:
    - Values are truncated to the top 4 bits per channel.
    """

    r, g, b = pixel
    return ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4)


def iter_sheet_pixels(img, layout: SheetLayout) -> Iterator[int]:
    """Yield encoded pixels from a bitmap in the required sheet order.

    Purpose:
    - Flatten a sprite/tile sheet into a contiguous pixel stream where each
      cell is emitted completely before the next cell.

    Inputs:
    - `img`: Pillow image converted to RGB.
    - `layout`: cell size and transparency behavior.

    Outputs:
    - Iterator of encoded 16-bit pixel values.

    Invariants:
    - Image dimensions must be divisible by the cell size.
    """

    if layout.cell_width <= 0 or layout.cell_height <= 0:
        raise ValueError("cell size must be positive")
    if img.width % layout.cell_width != 0 or img.height % layout.cell_height != 0:
        raise ValueError(
            f"image size {img.width}x{img.height} is not divisible by "
            f"cell size {layout.cell_width}x{layout.cell_height}"
        )

    cols = img.width // layout.cell_width
    rows = img.height // layout.cell_height

    for cell_row in range(rows):
        for cell_col in range(cols):
            x0 = cell_col * layout.cell_width
            y0 = cell_row * layout.cell_height
            transparent_pixel = img.getpixel((x0, y0))

            for y in range(layout.cell_height):
                for x in range(layout.cell_width):
                    pixel = img.getpixel((x0 + x, y0 + y))
                    if layout.transparent_from_top_left and pixel == transparent_pixel:
                        yield 0xFFFF
                    else:
                        yield encode_pixel_16bit(pixel)


def pack_pixels_into_fill_words(pixels: Iterable[int]) -> list[int]:
    """Pack 16-bit pixels into 32-bit words for `.fill`.

    Purpose:
    - Preserve the original byte stream while using `.fill` (32-bit directive)
      by packing two little-endian 16-bit pixels into each word.

    Inputs:
    - `pixels`: iterable of 16-bit pixel values.

    Outputs:
    - List of 32-bit `.fill` words where low 16 bits are the first pixel and
      high 16 bits are the second pixel.

    Invariants:
    - Pixel values are masked to 16 bits.
    """

    words: list[int] = []
    pending: int | None = None

    for pixel in pixels:
        pixel &= 0xFFFF
        if pending is None:
            pending = pixel
        else:
            words.append(pending | (pixel << 16))
            pending = None

    if pending is not None:
        # Pad the final halfword with zero if the pixel count is odd.
        words.append(pending)

    return words


def symbol_name_for(path: Path) -> str:
    """Return a stable assembler symbol name for an asset file."""

    stem = re.sub(r"[^A-Za-z0-9_]", "_", path.stem).upper()
    if not stem:
        stem = "ASSET"
    if stem[0].isdigit():
        stem = "_" + stem
    return f"{stem}_DATA"


def asm_text_for_bitmap(
    source_path: Path,
    layout: SheetLayout,
    words: Sequence[int],
    image_width: int,
    image_height: int,
) -> str:
    """Render the assembler source text for one converted bitmap.

    Purpose:
    - Produce a `.s` file that defines one label containing packed bitmap data.

    Inputs:
    - Source file path, layout metadata, packed `.fill` words, image dimensions.

    Outputs:
    - UTF-8/ASCII text for an assembler source file.

    Invariants:
    - `words` is the packed representation of the converted bitmap.
    """

    symbol = symbol_name_for(source_path)
    cells_x = image_width // layout.cell_width
    cells_y = image_height // layout.cell_height
    pixel_count = image_width * image_height

    lines: list[str] = []
    lines.append(f"# Auto-generated from {source_path.name} by img_to_hex.py")
    lines.append("# Pixel format: legacy 16-bit 0x0BGR (matches previous script output)")
    lines.append("# Packing: two 16-bit pixels per .fill word (low half = first pixel)")
    lines.append(
        f"# Sheet: {image_width}x{image_height} px, cell {layout.cell_width}x{layout.cell_height} px, "
        f"{cells_x}x{cells_y} cells, {pixel_count} pixels, {len(words)} .fill words"
    )
    lines.append("")
    lines.append("  .align 4")
    lines.append("  .data")
    lines.append(f"  .global {symbol}")
    lines.append(f"{symbol}:")
    for word in words:
        lines.append(f"  .fill 0x{word:08X}")
    lines.append("")
    return "\n".join(lines)


def convert_bitmap_to_asm(source_path: Path, out_path: Path, layout: SheetLayout) -> None:
    """Convert one bitmap file to one assembler file.

    Purpose:
    - Read a bitmap, flatten it according to `layout`, pack pixels into `.fill`
      words, and write the resulting `.s` file.

    Inputs:
    - `source_path`: bitmap file path.
    - `out_path`: destination `.s` path.
    - `layout`: conversion layout.

    Outputs:
    - Writes `out_path`.

    Invariants:
    - Output directory exists (created by caller).
    """

    Image = _import_pillow_image()
    with Image.open(source_path) as src_img:
        img = src_img.convert("RGB")
    img_width = img.width
    img_height = img.height
    try:
        pixels = list(iter_sheet_pixels(img, layout))
    finally:
        try:
            img.close()
        except Exception:
            pass

    words = pack_pixels_into_fill_words(pixels)
    asm = asm_text_for_bitmap(source_path, layout, words, img_width, img_height)
    out_path.write_text(asm, encoding="ascii")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    """Parse command-line arguments for the bitmap converter."""

    parser = argparse.ArgumentParser(
        description="Convert bitmap assets into assembler .s files using .fill"
    )
    parser.add_argument(
        "images",
        nargs="*",
        help=(
            "Bitmap file(s) to convert. If omitted, converts all .bmp files in "
            "the script directory."
        ),
    )
    parser.add_argument(
        "--out-dir",
        default=None,
        help="Output directory for generated .s files (defaults to script directory).",
    )
    parser.add_argument(
        "--transparent-top-left",
        action="store_true",
        help=(
            "Treat the top-left pixel of each cell as transparent and encode it "
            "as 0xFFFF (applies to all files, overriding default layout setting)."
        ),
    )
    return parser.parse_args(argv)


def resolve_inputs(script_dir: Path, image_args: Sequence[str]) -> list[Path]:
    """Resolve requested input bitmap paths.

    Purpose:
    - Support both explicit file paths and the default "all BMPs in this
      directory" mode.

    Inputs:
    - `script_dir`: directory containing this script.
    - `image_args`: command-line image arguments.

    Outputs:
    - Sorted list of bitmap paths to convert.
    """

    if not image_args:
        return sorted(script_dir.glob("*.bmp"))

    paths: list[Path] = []
    for arg in image_args:
        p = Path(arg)
        if p.is_absolute():
            paths.append(p)
            continue

        # Prefer the caller's relative path semantics, but keep compatibility
        # with the old workflow that passed names relative to this script.
        if p.exists():
            paths.append(p.resolve())
        else:
            paths.append((script_dir / p).resolve())
    return paths


def layout_for(path: Path, force_transparent_top_left: bool) -> SheetLayout:
    """Pick the conversion layout for a bitmap file."""

    base = KNOWN_LAYOUTS.get(path.name, SheetLayout(cell_width=1, cell_height=1))
    if force_transparent_top_left:
        return SheetLayout(
            cell_width=base.cell_width,
            cell_height=base.cell_height,
            transparent_from_top_left=True,
        )
    return base


def main(argv: Sequence[str]) -> int:
    """Program entry point.

    Purpose:
    - Convert one or more bitmaps into `.s` files and report generated paths.

    Inputs:
    - `argv`: CLI args excluding program name.

    Outputs:
    - Process exit status.

    Invariants:
    - Fails fast with informative messages for missing inputs or unsupported
      image dimensions for known sheet layouts.
    """

    args = parse_args(argv)
    script_dir = Path(__file__).resolve().parent
    out_dir = Path(args.out_dir).resolve() if args.out_dir else script_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    inputs = resolve_inputs(script_dir, args.images)
    if not inputs:
        print("No .bmp files found to convert.", file=sys.stderr)
        return 1

    for source in inputs:
        if not source.exists():
            print(f"Input file not found: {source}", file=sys.stderr)
            return 1
        if source.suffix.lower() != ".bmp":
            print(f"Unsupported input (expected .bmp): {source}", file=sys.stderr)
            return 1

    for source in inputs:
        out_path = out_dir / f"{source.stem}.s"
        layout = layout_for(source, args.transparent_top_left)
        try:
            convert_bitmap_to_asm(source, out_path, layout)
        except Exception as exc:
            print(f"Failed to convert {source}: {exc}", file=sys.stderr)
            return 1
        print(f"Wrote {out_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
