#!/usr/bin/env python3
"""Synthetic smooth (non-pixel-art) avatar clips for testing gif2pixel.py.

    make_test_clips.py OUT_DIR

Writes, in Muse's naming:
  OUT_DIR/mp4/testmuse.mp4, testmuse-working.mp4, testmuse-making_something.mp4      (light stage)
  OUT_DIR/mp4/testmuse-darkmode-{idle,working,making_something}.mp4                 (dark stage)
    720x720, 24 fps, 3 s, H.264 yuv420p, opaque; the face in the top half.
  OUT_DIR/gif400/idle.gif, thinking.gif, happy.gif
    400x400, 20 fps, on a flat slate stage (neither black nor white).
The character: a gradient blob (teal to violet) with a soft highlight, eyes that
blink, cheeks and a mouth; working looks down with a spinner, making_something
bounces and holds a growing star.
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SS = 2   # supersampling for anti-aliased shape edges


def gradient_sprite(n=256):
    """Body colour: vertical teal -> violet, plus a soft top-left highlight."""
    top, bot = (64, 214, 200), (128, 84, 230)
    lin = Image.linear_gradient("L").resize((n, n))
    chans = [lin.point(lambda v, a=top[i], b=bot[i]: a + (b - a) * v // 255) for i in range(3)]
    base = Image.merge("RGB", chans)
    hl = Image.radial_gradient("L").resize((n, n))
    hl = ImageChops.invert(hl).point(lambda v: max(0, v - 120) * 255 // 135)
    hl = hl.transform((n, n), Image.Transform.AFFINE, (1, 0, n * 0.18, 0, 1, n * 0.22))
    return Image.composite(Image.new("RGB", (n, n), (255, 255, 255)), base, hl.point(lambda v: v * 6 // 10))


SPRITE = gradient_sprite()


def ellipse_mask(size, box):
    m = Image.new("L", (size[0] * SS, size[1] * SS), 0)
    ImageDraw.Draw(m).ellipse([c * SS for c in box], fill=255)
    return m.resize(size, Image.Resampling.LANCZOS)


def frame(S, t, clip, stage):
    k = S / 720.0
    img = Image.new("RGB", (S, S), stage)
    breathe = math.sin(t * 2 * math.pi / 1.5) * 0.02
    bob = math.sin(t * 2 * math.pi / 1.5) * 8
    if clip == "working":
        bob = math.sin(t * 2 * math.pi / 0.5) * 4
    if clip in ("making_something", "happy"):
        bob = -abs(math.sin(t * 2 * math.pi / 0.75)) * 40
    cx, base_y = 360, 700 + bob
    # body + head as one silhouette
    body = (cx - 230 * (1 + breathe), base_y - 380 * (1 - breathe), cx + 230 * (1 + breathe), base_y)
    head = (cx - 200 * (1 + breathe), base_y - 600, cx + 200 * (1 + breathe), base_y - 250)
    box = [min(body[0], head[0]), head[1], max(body[2], head[2]), body[3]]
    sk = [v * k for v in box]
    mask = ImageChops.lighter(ellipse_mask((S, S), [v * k for v in body]), ellipse_mask((S, S), [v * k for v in head]))
    bw, bh = int(sk[2] - sk[0]) + 2, int(sk[3] - sk[1]) + 2
    fill = Image.new("RGB", (S, S), stage)
    fill.paste(SPRITE.resize((bw, bh), Image.Resampling.BICUBIC), (int(sk[0]), int(sk[1])))
    img = Image.composite(fill, img, mask)
    d = ImageDraw.Draw(img)
    # cheeks (soft)
    cheeks = Image.new("L", (S, S), 0)
    cy = base_y - 400
    for sx in (-1, 1):
        ImageDraw.Draw(cheeks).ellipse([(cx + sx * 120 - 38) * k, (cy + 40 - 22) * k, (cx + sx * 120 + 38) * k,
                                        (cy + 40 + 22) * k], fill=150)
    cheeks = cheeks.filter(ImageFilter.GaussianBlur(10 * k))
    img = Image.composite(Image.new("RGB", (S, S), (255, 120, 160)), img, cheeks)
    # eyes
    blink = (t % 2.2) < 0.12
    look = 18 if clip == "working" else 0
    eye = Image.new("L", (S * SS, S * SS), 0)
    pupil = Image.new("L", (S * SS, S * SS), 0)
    de, dp = ImageDraw.Draw(eye), ImageDraw.Draw(pupil)
    for sx in (-1, 1):
        ex, ey = cx + sx * 80, cy - 30
        h = 6 if blink else 42
        de.ellipse([v * k * SS for v in (ex - 34, ey - h, ex + 34, ey + h)], fill=255)
        if not blink:
            dp.ellipse([v * k * SS for v in (ex - 18, ey - 18 + look, ex + 18, ey + 18 + look)], fill=255)
    img = Image.composite(Image.new("RGB", (S, S), (250, 250, 255)), img, eye.resize((S, S), Image.Resampling.LANCZOS))
    img = Image.composite(Image.new("RGB", (S, S), (30, 24, 50)), img, pupil.resize((S, S), Image.Resampling.LANCZOS))
    # mouth
    mouth = Image.new("L", (S * SS, S * SS), 0)
    open_ = 0.5 + 0.5 * math.sin(t * 2 * math.pi / 0.75) if clip in ("making_something", "happy") else 0
    ImageDraw.Draw(mouth).arc([v * k * SS for v in (cx - 40, cy + 20 - 10 * open_, cx + 40, cy + 70 + 20 * open_)],
                              20, 160, fill=255, width=int(10 * k * SS))
    img = Image.composite(Image.new("RGB", (S, S), (60, 20, 50)), img, mouth.resize((S, S), Image.Resampling.LANCZOS))
    d = ImageDraw.Draw(img)
    if clip in ("working", "thinking"):
        # spinner dot orbiting beside the head
        a = t * 2 * math.pi / 0.8
        x, y = cx + 250 + 40 * math.cos(a), cy - 180 + 40 * math.sin(a)
        d.ellipse([(x - 16) * k, (y - 16) * k, (x + 16) * k, (y + 16) * k], fill=(255, 190, 80))
    if clip in ("making_something", "happy"):
        # a star between the paws, growing and turning
        g = 0.4 + 0.6 * ((t / 3.0) % 1.0)
        a0 = t * 2.0
        sx, sy = cx, base_y - 170
        pts = []
        for i in range(10):
            r = (60 if i % 2 == 0 else 26) * g
            a = a0 + i * math.pi / 5
            pts.append(((sx + r * math.cos(a)) * k, (sy + r * math.sin(a)) * k))
        d.polygon(pts, fill=(255, 220, 90))
    return img


def write_mp4(path, frames, fps):
    import imageio_ffmpeg
    w, h = frames[0].size
    gen = imageio_ffmpeg.write_frames(path, (w, h), fps=fps, codec="libx264", pix_fmt_in="rgb24",
                                      pix_fmt_out="yuv420p", quality=7)
    gen.send(None)
    for f in frames:
        gen.send(f.tobytes())
    gen.close()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test-clips"
    os.makedirs(os.path.join(out, "mp4"), exist_ok=True)
    os.makedirs(os.path.join(out, "gif400"), exist_ok=True)
    fps, secs = 24, 3.0
    for stage, prefix in (((240, 240, 244), "testmuse"), ((14, 14, 20), "testmuse-darkmode")):
        for clip in ("idle", "working", "making_something"):
            name = (f"{prefix}.mp4" if prefix == "testmuse" else f"{prefix}-idle.mp4") if clip == "idle" \
                else f"{prefix}-{clip}.mp4"
            frames = [frame(720, i / fps, clip, stage) for i in range(int(fps * secs))]
            write_mp4(os.path.join(out, "mp4", name), frames, fps)
            print("wrote", name, len(frames), "frames")
    for clip in ("idle", "thinking", "happy"):
        frames = [frame(400, i / 20, clip, (58, 66, 84)) for i in range(60)]
        frames[0].save(os.path.join(out, "gif400", clip + ".gif"), save_all=True, append_images=frames[1:],
                       duration=50, loop=0)
        print("wrote", clip + ".gif", len(frames), "frames")


if __name__ == "__main__":
    main()
