#!/usr/bin/env python3
"""'juguete' icons made of a game's own art: Mila and Monster Hop.

    python3 tools/icons/juguete/sprites.py        (post.py runs it before shrinking)

render.py renders only the tile for these (raw/tile/<id>_<v>_big|mini.png);
this lays the game's sprite on it, with a soft shadow, and writes
raw/<id>_<v>_big|mini.png like any other model.

Where the art comes from (none of it is in git; both are the user's games,
made in Blender by their own scripts):

  Mila         apps/mila/art/mila_hd/ (apps/mila/tools/blender/mila.py renders
               it; full colour already). MILA_ART overrides; a worktree falls
               back to the main checkout's.
  Monster Hop  the MonsterHop repository's art/hd/ (MIT, the user's), beside
               P4OS. MH_ART overrides. Those sprites are light + region id, and
               are painted here the way the game paints them (mh_lut_build in
               apps/monsterhop/main/mh_art.c): colour = palette[id] * light /
               196, glowing ids kept bright. Tommy's colours are the game's
               default outfit (mh_shop_apply in mh_shop.c, the red cap); the
               monsters' are art/hd/monsters/palettes.json.
"""

import json
import os

import numpy as np
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(ICONS))
RAW = os.path.join(ICONS, "out", "juguete", "raw")
MAIN = os.path.normpath(os.path.join(ROOT, "..", "..", ".."))     # a worktree's main checkout


def _first(*dirs):
    for d in dirs:
        if d and os.path.isdir(d):
            return d
    return None


MILA = _first(os.environ.get("MILA_ART"), os.path.join(ROOT, "apps", "mila", "art", "mila_hd"),
              os.path.join(MAIN, "apps", "mila", "art", "mila_hd"))
MH = _first(os.environ.get("MH_ART"), os.path.join(ROOT, "..", "MonsterHop", "art", "hd"),
            os.path.join(MAIN, "..", "MonsterHop", "art", "hd"))

# Tommy's default outfit (mh_shop_apply): skin, hair, dark, white, the
# striped shirt (blue, white, yellow), trousers, shoes, socks, laces, cheeks
TOMMY = {1: 0xF5C9A6, 2: 0x5A341C, 3: 0x1A1414, 4: 0xFFFFFF, 5: 0x2E7CF0, 6: 0xF5F5F5, 7: 0xFFC830,
         8: 0x2E3A6A, 9: 0xE83434, 10: 0xF2F2F2, 11: 0xFFFFFF, 12: 0xF09090}
RED_CAP = {1: 0xE62E2E, 2: 0xB81E22, 3: 0xFFD040, 4: 0x5A1A1A}


def _rgb(v):
    if isinstance(v, (list, tuple)):
        return tuple(int(c) for c in v[:3])
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def paint(img_path, pal, keep=()):
    """A light + id sprite through a palette, as the game does it."""
    im = np.asarray(Image.open(img_path).convert("RGBA")).astype(np.float32)
    ids = np.asarray(Image.open(img_path[:-4] + "_id.png").convert("L")).astype(np.int32) // 16
    light = im[..., 0]
    out = np.zeros_like(im)
    for i in range(16):
        m = ids == i
        if not m.any():
            continue
        r, g, b = _rgb(pal.get(i, pal.get(str(i), 0x808080)))
        li = np.maximum(light[m], 160) if i in keep else light[m]
        f = (li + 2) / 196.0
        out[..., 0][m] = np.clip(r * f, 0, 255)
        out[..., 1][m] = np.clip(g * f, 0, 255)
        out[..., 2][m] = np.clip(b * f, 0, 255)
    out[..., 3] = im[..., 3]
    return Image.fromarray(out.astype(np.uint8), "RGBA")


def tommy():
    meta = json.load(open(os.path.join(MH, "chars", "meta.json")))
    body = paint(os.path.join(MH, "chars", "tommy_idle_s_00.png"), TOMMY)
    cap = paint(os.path.join(MH, "chars", "cap_cap_idle_s_00.png"), RED_CAP)
    mb, mc = meta["tommy_idle_s_00"], meta["cap_cap_idle_s_00"]
    # both are placed by their anchor pixel (ax, ay): put the anchors together
    x0 = min(-mb["ax"], -mc["ax"])
    y0 = min(-mb["ay"], -mc["ay"])
    x1 = max(body.width - mb["ax"], cap.width - mc["ax"])
    y1 = max(body.height - mb["ay"], cap.height - mc["ay"])
    out = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
    out.alpha_composite(body, (-mb["ax"] - x0, -mb["ay"] - y0))
    out.alpha_composite(cap, (-mc["ax"] - x0, -mc["ay"] - y0))
    return out


def monster_card(name):
    pals = json.load(open(os.path.join(MH, "monsters", "palettes.json")))
    return paint(os.path.join(MH, "monsters", f"card_{name}.png"), pals[name], keep=(5,))


def mila(frame):
    return Image.open(os.path.join(MILA, f"mila_c_{frame}_s_00.png")).convert("RGBA")


SPRITES = {
    ("demo.mila", "d"): lambda: mila("sit"),
    ("demo.mila", "e"): lambda: mila("meow"),
    ("demo.mila", "f"): lambda: mila("purr"),
    ("demo.monsterhop", "d"): tommy,
    ("demo.monsterhop", "e"): lambda: monster_card("vampire"),
    ("demo.monsterhop", "f"): lambda: monster_card("bat"),
}


def place(tile, sprite, fill):
    """The sprite centred on the tile, fit in fill x the side, with a soft
    shadow down and to the right (the scene's sun is up and to the left)."""
    W = tile.width
    bbox = sprite.getbbox()
    sprite = sprite.crop(bbox)
    k = fill * W / max(sprite.width, sprite.height)
    sw, sh = max(1, round(sprite.width * k)), max(1, round(sprite.height * k))
    sp = sprite.convert("RGBa").resize((sw, sh), Image.LANCZOS).convert("RGBA")
    x, y = (W - sw) // 2, (W - sh) // 2
    out = tile.copy()
    a = sp.split()[3]
    sh_img = Image.new("RGBA", out.size, (0, 0, 0, 0))
    shadow = Image.new("RGBA", sp.size, (10, 10, 30, 0))
    shadow.putalpha(a.point(lambda v: int(v * 0.4)))
    sh_img.alpha_composite(shadow, (x + round(0.02 * W), y + round(0.035 * W)))
    sh_img = sh_img.filter(ImageFilter.GaussianBlur(0.02 * W))
    # the shadow only falls on the tile
    sh_img.putalpha(Image.fromarray(np.minimum(np.asarray(sh_img.split()[3]), np.asarray(out.split()[3]))))
    out.alpha_composite(sh_img)
    out.alpha_composite(sp, (x, y))
    return out


def compose_all():
    tiles = os.path.join(RAW, "tile")
    if not os.path.isdir(tiles):
        return 0
    n = 0
    for (app_id, v), make in SPRITES.items():
        for kind, fill in (("big", 0.74), ("mini", 0.78)):
            src = os.path.join(tiles, f"{app_id}_{v}_{kind}.png")
            dst = os.path.join(RAW, f"{app_id}_{v}_{kind}.png")
            if not os.path.exists(src):
                continue
            if os.path.exists(dst) and os.path.getmtime(dst) > max(os.path.getmtime(src), os.path.getmtime(__file__)):
                continue
            if (app_id == "demo.mila" and not MILA) or (app_id == "demo.monsterhop" and not MH):
                print("no art for", app_id)
                continue
            place(Image.open(src).convert("RGBA"), make(), fill).save(dst)
            n += 1
    return n


if __name__ == "__main__":
    print("composed:", compose_all())
