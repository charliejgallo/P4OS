# Colour emoji

The phone sends emoji all the time. The watch could only swap a few for
monochrome pictograms (Font Awesome faces, a heart) and the rest for a dot;
the P4 has the card and the PSRAM to draw them all, in colour.

## The pack

`tools/gen_emoji.py` turns Google's Noto Emoji (the 2D set, Apache 2.0, and
its public-domain region flags) into one file, `/fonts/emoji.pak` on the
card: 4012 emoji, each a 48 x 48 RGBA image encoded as QOI, and an index of
their sequences. 15.5 MB. It is not in git: the CI builds it from Noto's
tag `v2026-09-24-unicode18_0` (the `packs` job of
`.github/workflows/build.yml`) and it goes out with the release as
`emoji.pak`. By hand, the same file byte for byte:

    git clone --depth 1 --branch v2026-09-24-unicode18_0 --filter=blob:none --sparse \
        https://github.com/googlefonts/noto-emoji.git
    (cd noto-emoji && git sparse-checkout set 2D/png/72 third_party/region-flags/png)
    tools/gen_emoji.py noto-emoji emoji.pak
    tools/install_emoji.sh p4os.local emoji.pak      # then restart

QOI and not PNG: the board decodes PNG with libpng and the simulator with
lodepng, while a QOI decoder is a page of C (`aos_emoji.c`) that runs the
same in both, and faster.

## How a text gets them

- `aos_text_safe()` (`aos_text_safe.c`), which every phone text goes through
  (banners, the notification centre, the lock screen), looks for the longest
  emoji sequence at each position (`aos_emoji_match`): skin tones, ZWJ
  joiners, flags (regional-indicator pairs and the UK's tag sequences) and
  keycaps, skipping the variation selectors. The whole sequence becomes one
  code point of plane 15's private-use area, U+F0000 + its place in the
  index, because LVGL draws one glyph per code point and shapes nothing.
- A single symbol of the BMP is an emoji only with U+FE0F after it, or when
  Unicode presents it as one by default (`bmp_emoji_by_default`): a sun or a
  copyright sign with no selector stays text, a lone `#` or digit too.
- Each theme font (`aos_font_*`) is replaced by a copy in PSRAM whose
  fallback is an emoji font with its metrics (`aos_emoji_font`). Asked for
  one of those code points, it reads the image from the card, decodes it,
  scales it to 4/5 of the line (area average over premultiplied colour) and
  keeps it as an ARGB8888 image. LVGL draws glyphs of format IMAGE as
  images, in their own colours.

## Memory and threads

- The index, ~200 KB, in PSRAM from boot. The images stay on the card until
  a text needs one: a 28 px emoji takes 3 KB of PSRAM once made.
- Glyphs are asked for from LVGL's task and from its two draw threads, so
  the cache is behind a mutex, and nothing in it is freed (a draw thread may
  be painting it). It stops at 6 MB, thousands of emoji; past that a new one
  is not drawn.
- Without the card or the pack none of this is on: the fonts stay the
  compiled ones and the text filter does what it did on the watch.

The simulator reads `sim/sim_fs/fonts/emoji.pak`, and its first fake
notification (key `x`) carries the hard cases.
