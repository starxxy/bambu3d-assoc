# Dev tool: build resources/app.ico from a source artwork image.
# Usage: python tools/make_icon.py <source-image> <output.ico>
# - trims a uniform background border (if any)
# - applies rounded-corner transparency so the tile reads well on any desktop
# - emits one .ico with 256/128/64/48/32/24/16 px frames (LANCZOS)
import sys
from PIL import Image, ImageDraw, ImageOps


def trim_uniform_border(img, tol=6):
    """Crop away a near-uniform background border; return (img, cropped?)."""
    rgb = img.convert("RGB")
    corner = rgb.getpixel((0, 0))
    # mask of pixels that differ from the corner color beyond tolerance
    from PIL import ImageChops
    bg = Image.new("RGB", rgb.size, corner)
    diff = ImageChops.difference(rgb, bg).convert("L")
    mask = diff.point(lambda p: 255 if p > tol else 0)
    box = mask.getbbox()
    if box and (box[2] - box[0]) >= 32 and (box[3] - box[1]) >= 32:
        return rgb.crop(box), True
    return rgb, False


def round_corners(img, radius_ratio=0.2):
    s = img.size[0]
    r = max(8, int(s * radius_ratio))
    mask = Image.new("L", (s, s), 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask)
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    img = Image.open(src)
    img, cropped = trim_uniform_border(img)
    side = min(img.size)
    img = ImageOps.fit(img, (side, side), Image.LANCZOS, centering=(0.5, 0.5))
    img = round_corners(img)

    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = {x: img.resize((x, x), Image.LANCZOS) for x in sizes}
    img.save(dst, format="ICO",
             sizes=[(x, x) for x in sizes],
             append_images=[frames[x] for x in sizes[1:]])
    # preview png (largest frame) for eyeballing
    frames[256].save(dst.rsplit(".", 1)[0] + "_preview.png")
    print(f"ok cropped={cropped} side={side} -> {dst}")


if __name__ == "__main__":
    main()
