"""Render one timer frame for the Muse Charm's round screen.

The screen is 466x466 and round, so everything that matters stays
inside the inscribed circle: a countdown ring rides the rim, the time
left sits in the middle, and the preset chips stay near the centre
where the corners cannot cut them off.

The ring shows the time left: it starts full and is eaten away as the
timer runs. Its colour follows the time left, green first, then
yellow inside the last minute (or the last third of a short timer),
then red for the last seconds.
"""

from PIL import Image, ImageDraw, ImageFont

SIZE = 466
CENTER = SIZE // 2
RING_RADIUS = 208
RING_WIDTH = 20

BG = (10, 12, 16)
TRACK = (38, 42, 52)
TEXT = (240, 242, 245)
MUTED = (150, 158, 170)
GREEN = (46, 204, 113)
YELLOW = (241, 196, 15)
RED = (231, 76, 60)

PRESETS = ("2m", "3m", "5m", "10m", "15m")

_FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
)


def _font(size):
    for path in _FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def phase_color(remaining, total):
    """Green, then yellow, then red as the end approaches."""
    if remaining <= 0:
        return RED
    fraction = remaining / total if total else 0
    if remaining <= 10 or fraction <= 0.10:
        return RED
    if remaining <= 60 or fraction <= 0.35:
        return YELLOW
    return GREEN


def format_time(seconds):
    seconds = max(0, int(round(seconds)))
    minutes, secs = divmod(seconds, 60)
    if minutes >= 60:
        hours, minutes = divmod(minutes, 60)
        return f"{hours}:{minutes:02d}:{secs:02d}"
    return f"{minutes}:{secs:02d}"


def _centered(draw, y, text, font, fill):
    box = draw.textbbox((0, 0), text, font=font)
    width = box[2] - box[0]
    draw.text(((SIZE - width) / 2 - box[0], y), text, font=font, fill=fill)


def _chips(draw, active=None):
    """The preset row, drawn as it would look on the device."""
    font = _font(24)
    gap = 8
    widths = []
    for label in PRESETS:
        box = draw.textbbox((0, 0), label, font=font)
        widths.append(box[2] - box[0] + 22)
    total = sum(widths) + gap * (len(PRESETS) - 1)
    x = (SIZE - total) / 2
    y = 324
    for label, w in zip(PRESETS, widths):
        fill = (52, 58, 70) if label != active else (84, 94, 112)
        draw.rounded_rectangle([x, y, x + w, y + 42], radius=21, fill=fill)
        box = draw.textbbox((0, 0), label, font=font)
        draw.text((x + (w - (box[2] - box[0])) / 2 - box[0], y + 6),
                  label, font=font, fill=TEXT)
        x += w + gap


def render(remaining, total, state="running", active_preset=None, flash=False):
    """One frame. state is one of: ready, running, paused, done."""
    img = Image.new("RGB", (SIZE, SIZE), BG)
    draw = ImageDraw.Draw(img)

    color = RED if state == "done" else phase_color(remaining, total)
    if state == "done" and flash:
        color = (255, 120, 100)

    # The track, then the time left as an arc that starts at the top
    # and is eaten clockwise.
    box = [CENTER - RING_RADIUS, CENTER - RING_RADIUS,
           CENTER + RING_RADIUS, CENTER + RING_RADIUS]
    draw.arc(box, start=0, end=360, fill=TRACK, width=RING_WIDTH)
    if state == "done":
        draw.arc(box, start=0, end=360, fill=color, width=RING_WIDTH)
    elif total > 0:
        sweep = 360.0 * max(0.0, min(1.0, remaining / total))
        if sweep > 0:
            draw.arc(box, start=-90, end=-90 + sweep, fill=color, width=RING_WIDTH)

    if state == "done":
        _centered(draw, 168, "TIME", _font(84), TEXT)
        _centered(draw, 252, "UP", _font(84), TEXT)
        _centered(draw, 356, "tap to dismiss", _font(26), MUTED)
        return img

    label = {"ready": "READY", "running": "", "paused": "PAUSED"}[state]
    _centered(draw, 150, format_time(remaining), _font(104), TEXT)
    if label:
        _centered(draw, 268, label, _font(30), MUTED)
    _chips(draw, active=active_preset)
    return img


def save_frame(img, path):
    img.save(path, "JPEG", quality=88)
    return path
