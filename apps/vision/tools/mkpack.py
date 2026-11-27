#!/usr/bin/env python3
"""
P4OS - Vision: makes the model packs from Espressif's esp-dl models.

    python3 apps/vision/tools/mkpack.py            the three packs, in apps/vision/build/
    python3 apps/vision/tools/mkpack.py --sim      and copies them to sim/sim_fs/apps/

The models are downloaded once from esp-dl's repository at a fixed commit
(~/.cache/p4os-vision), checked against their SHA-256, and packed as they
come: the engine reads the .espdl files whole (main/ve_api.h).

    vision_p4.pak        faces (MSR + MNP) and people (Pico)        ~0.6 MB, needed
    vision_coco_p4.pak   the 80 COCO objects (YOLO11n, 320 x 320)   ~3.0 MB, extra
    vision_manos_p4.pak  hands (ESPDet-Pico) and their gesture       ~1.5 MB, extra

esp-dl and its models are MIT (Espressif). YOLO11n and ESPDet-Pico were
trained with Ultralytics' code (AGPL-3.0); Espressif publishes the weights
as MIT, but the extras say so in their "info" entry and THIRD-PARTY.md.

The pack (main/vi_pack.c reads it):

    "VEPK" u16 version (1) u16 count
    count x entry (40 bytes): char name[32]  u32 offset  u32 length
    data: each entry 16-byte aligned (the engine runs on it in place)
"""
import hashlib
import os
import shutil
import struct
import sys
import urllib.request

COMMIT = '512ed0b0dc5f5320f3d9ca5a5aea189803f87f31'
BASE = 'https://raw.githubusercontent.com/espressif/esp-dl/%s/models/' % COMMIT

MODELS = {
    'face_msr': ('human_face_detect/models/p4/human_face_detect_msr_s8_v1.espdl',
                 '2bbaaf17f39ce9ee547abf5ffdc6d8fcd1844009a0d26878d7bfb33451f16211'),
    'face_mnp': ('human_face_detect/models/p4/human_face_detect_mnp_s8_v1.espdl',
                 '5b7fedcff59dbe69d1935d6d206bf371fc18b5dd9176320ef69c9f05ee83af55'),
    'person': ('pedestrian_detect/models/p4/pedestrian_detect_pico_s8_v1.espdl',
               '0bc5326cab0f004c9525a2e3e83899b3e7a68796debb2d21c13935329b8006cc'),
    'coco': ('coco_detect/models/p4/coco_detect_yolo11n_320_s8_v1.espdl',
             '383b67556c491b7f5ded02f1f9542d512f454f6339cbd12528048f0ce64977a6'),
    'hand': ('hand_detect/models/p4/espdet_pico_224_224_hand.espdl',
             'a8d1857bab33bfd23bc72e1543a156ef33645b47b9ac130776aacb6a215439ea'),
    'gesture': ('hand_gesture_recognition/models/p4/mobilenetv2_0_5_128_128_gesture.espdl',
                'fc4c4e0b4ba52e651dd802d5e0dc738f9eac899b107a9ce4638635e746be3c5d'),
}

LICENSE_MIT = ('license=MIT (esp-dl, Espressif Systems)\n'
               'source=github.com/espressif/esp-dl %s\n' % COMMIT[:12])
LICENSE_ULTRA = LICENSE_MIT + ('note=trained with Ultralytics YOLO11 code (AGPL-3.0); '
                               'Espressif publishes these weights as MIT\n')

PACKS = {
    'vision_p4.pak': (['face_msr', 'face_mnp', 'person'], LICENSE_MIT),
    'vision_coco_p4.pak': (['coco'], LICENSE_ULTRA),
    'vision_manos_p4.pak': (['hand', 'gesture'], LICENSE_ULTRA),
}


def fetch(name):
    path, sha = MODELS[name]
    cache = os.path.join(os.path.expanduser('~/.cache/p4os-vision'), COMMIT[:12])
    os.makedirs(cache, exist_ok=True)
    local = os.path.join(cache, os.path.basename(path))
    if not os.path.exists(local):
        print('downloading', os.path.basename(path))
        with urllib.request.urlopen(BASE + path, timeout=60) as r:
            data = r.read()
        with open(local + '.part', 'wb') as f:
            f.write(data)
        os.replace(local + '.part', local)
    data = open(local, 'rb').read()
    if hashlib.sha256(data).hexdigest() != sha:
        os.remove(local)
        sys.exit('%s: wrong checksum' % os.path.basename(path))
    return data


def pack(entries):
    n = len(entries)
    head = 8 + 40 * n
    off = (head + 15) & ~15
    dirs, blobs = [], []
    for name, data in entries:
        assert len(name.encode()) < 32
        dirs.append(struct.pack('<32sII', name.encode(), off, len(data)))
        blobs.append((off, data))
        off = (off + len(data) + 15) & ~15
    out = bytearray(off)
    out[0:8] = b'VEPK' + struct.pack('<HH', 1, n)
    out[8:head] = b''.join(dirs)
    for o, d in blobs:
        out[o:o + len(d)] = d
    return bytes(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    app = os.path.dirname(here)
    out = os.path.join(app, 'build')
    os.makedirs(out, exist_ok=True)
    made = []
    for fname, (names, info) in PACKS.items():
        entries = [('info', info.encode())] + [(n, fetch(n)) for n in names]
        blob = pack(entries)
        path = os.path.join(out, fname)
        with open(path, 'wb') as f:
            f.write(blob)
        made.append(path)
        print('%-22s %8.2f MB  %s' % (fname, len(blob) / 1e6, ', '.join(names)))
    if '--sim' in sys.argv:
        dst = os.path.join(app, '..', '..', 'sim', 'sim_fs', 'apps')
        os.makedirs(dst, exist_ok=True)
        for p in made:
            shutil.copy(p, dst)
        print('copied to', os.path.normpath(dst))


if __name__ == '__main__':
    main()
