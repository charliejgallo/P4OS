#!/bin/sh
# P4OS - Office: every invented test file of the seven phases, in one folder,
# to copy to the board's /sdcard/documentos (or upload from the portal).
#
#     sh apps/oficina/tools/samples_all.sh DIR
#
# Python 3 and the standard library for most; fontTools and Pillow for the
# drawing samples (phase 7); on the Mac, textutil makes the .doc files and
# cupsfilter, pango-view, rsvg-convert and tesseract the "external" PDFs
# (each one skipped when missing). Nothing comes from anyone's documents.
set -e
[ -n "$1" ] || { echo "usage: $0 DIR"; exit 2; }
D="$1"
T=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 "$T/make_samples.py" "$D" >/dev/null          # phases 1-2: xlsx, ods, csv, docx, odt, texts
python3 "$T/samples_read.py" "$D" >/dev/null          # phase 3: rtf, html, epub, eml, fb2, md, vcf, ics, json..., zip, tar
python3 "$T/samples_pdf.py" "$TMP/pdf" --external >/dev/null 2>&1 || python3 "$T/samples_pdf.py" "$TMP/pdf" >/dev/null
cp "$TMP"/pdf/*.pdf "$D"/                              # phase 4: PDFs, three with a password
python3 "$T/samples_slides.py" "$D" >/dev/null        # phase 5: pptx, odp
python3 "$T/samples_old.py" "$D" >/dev/null           # phase 6: xls, ppt, pages, numbers, key
if command -v textutil >/dev/null; then
    for f in "$D"/*.rtf "$D"/*.html "$D"/informe.docx; do
        [ -f "$f" ] || continue
        n=$(basename "$f"); n=${n%.*}
        [ -f "$D/$n.doc" ] || textutil -convert doc -output "$D/$n.doc" "$f" 2>/dev/null || true
    done
fi
if python3 -c "import fontTools" 2>/dev/null; then
    python3 "$T/samples_draw.py" "$D" >/dev/null      # phase 7: dibujo, imagenes, fuentes
else
    echo "no fontTools: dibujo.pdf, imagenes.pdf and fuentes.pdf skipped"
fi
ls "$D" | wc -l | tr -d ' ' | sed 's/$/ files in '"$(echo "$D" | sed 's/\//\\\//g')"'/'
