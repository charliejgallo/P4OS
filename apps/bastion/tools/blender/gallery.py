"""
BASTION - the gallery's portraits: the aliens and the towers again, bigger,
for the cards the player unlocks (SPEC.md has the game's own sheets).

    /Applications/Blender.app/Contents/MacOS/Blender -b -P gallery.py -- [--only grub,laser] [--samples 64]

The same models as aliens.py and towers.py, through the same camera with
more pixels a metre (the kit's scale is turned up for the render, so
nothing is enlarged afterwards):

    gallery/al_<id>.png   4 cells: the frames of its walk, looking down and
                          to the left (yaw 225), 4.5 times the game's size
                          (2.6 for the brute and the carrier, 1.6 a boss)
    gallery/tw_<id>.png   1 cell of 256 x 352: tier 3, base and head
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bs_common as B  # noqa: E402
import aliens as A  # noqa: E402
import towers as T  # noqa: E402

# how much bigger than in the game each size of alien is drawn: enough for
# the smallest of each size to fill its card (pack_p4.py brings the big
# ones down to the card's box)
ZOOM = {'n': 4.5, 'm': 2.6, 'b': 1.6}
YAW = 225.0


def zoom(k):
    B.PX = 40.0 * k
    B.ROW_Y = B.PX * B.SIN_E
    B.ROW_Z = B.PX * B.COS_E


def main():
    out = B.arg('--out', os.path.join(B.ASSETS, 'gallery'))
    only = B.arg('--only')
    only = only.split(',') if only else None
    B.setup(int(B.arg('--samples', 64)))
    B.studio()
    for aid, a in A.ALIENS.items():
        if only and aid not in only:
            continue
        s = A.SIZES[a['size']]
        k = ZOOM[a['size']]
        zoom(k)
        build = a['build']
        cell = (int(s['cell'][0] * k), int(s['cell'][1] * k))
        anchor = (int(s['anchor'][0] * k), int(s['anchor'][1] * k))
        B.render_sheet('al_' + aid, lambda p, c, r: build(p, c), out, 4, cell=cell, anchor=anchor, yaws=[YAW] * 4)
    zoom(2.0)
    for tid, t in T.TOWERS.items():
        if only and tid not in only:
            continue
        base, head = t['build'](3)
        spin = bool(t.get('spin'))

        def both(p, c, r, base=base, head=head, spin=spin):
            base(p)
            # the head turns about the pad's centre; the base does not
            g = B.group('head', p, rot=(0, 0, 0 if spin else YAW))
            if spin:
                head(g, 0.0)
            else:
                head(g)
        B.render_sheet('tw_' + tid, both, out, 1, cell=(T.CELL[0] * 2, T.CELL[1] * 2),
                       anchor=(T.ANCHOR[0] * 2, T.ANCHOR[1] * 2))
    zoom(1.0)


if __name__ == '__main__':
    main()
