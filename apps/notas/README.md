# Notas (Notes)

Notes with styles, and task lists, for P4OS. Id `aos.notas`.

- **Notes** in a rich-text editor of its own: four sizes, bold, italic,
  underline, strikethrough, nine text colours and six highlighters; title,
  heading, subheading, body and quote blocks; a rule; eight kinds of bullets
  (disc, circle, square, dash, arrow, star, tick, diamond), five of numbering
  (1. a) A) i. I.) and checkboxes, nested up to five levels. Undo and redo,
  copy and paste with the formatting, selection with handles, and Markdown
  shortcuts while typing (`- `, `1. `, `[] `, `# `, `> `, `---`).
- **Task lists**: round boxes, priority (`!` to `!!!`), subtasks that are
  ticked with their parent, drag to reorder, a folding "Completed" section,
  and Enter to type the next task.
- Cards in a grid or a list, pinned first, a colour per note, search, sort
  by date or title, a bin. Share a note as a QR code, or type it into a
  computer through the USB or Bluetooth keyboard.
- Dark (the default) or light background, from the menu of the notes
  screen.
- A keyboard of its own, Spanish: ñ, a dead accent key (´ then a vowel, ¨
  then u), shift by itself at the start of a sentence, shift twice for caps
  lock, and the space bar as a trackpad for the caret.

## Files

One Markdown file per note in `/sdcard/notas`, named after its title, with
the metadata as front matter and a little inline HTML for what Markdown has
no syntax for:

```
---
title: Compras
type: note
color: yellow
pinned: true
created: 2026-10-04 11:20
modified: 2026-10-04 11:42
---
# Para el sábado
Algo con **negrita**, *cursiva*, <u>subrayado</u> y ~~tachado~~.
<span style="color:red;background:yellow;font-size:large">Esto</span> en grande.
- un punto
  ◦ otro, adentro
1. primero
- [x] hecho
- [ ] !! pendiente, prioridad media
> una cita
---
```

One line per block, two spaces per level of indent, the bullet's own glyph
for the styles Markdown has no marker for. Any text file works too: a line
the reader cannot make out is a paragraph, and a hand-written note's first
`# heading` becomes its title. They are edited from the portal too (below). Deleted notes go to `/sdcard/notas/papelera`.

A note is saved two seconds after the last change and when it is closed; a
note left empty is not kept.

## The portal

Page **Notas** (`#notas`), in `web/notas.js`: a page of the card
(docs/PORTAL-PAGES.md) that `tools/install_apps.sh` puts in `/web`, using
only `window.P4OS` version 1 and bringing its own styles. The notes as
cards, a visual editor with the same formatting as the app, a task list
editor, the raw Markdown, the bin, and uploading `.md`/`.txt` files. Its
parser and writer follow `nt_doc.c` rule for rule: a note saved from the
browser without changes comes back byte for byte (only `modified:` moves).
Before writing it checks that the file did not change on the card since it
was read.

While the firmware still carries its own copy (`pageNotas` in
`components/aos_portal/web/app.js`, with its menu entry in `index.html`),
`notas.js` sees the portal's Notas entry in the menu and does not register:
the firmware's goes on working. Once the firmware drops both, this one takes
the place by itself.

The app watches the card every two seconds: the open note, if it changed and
there is nothing unsaved in the app, is read again (closed if it went to the
bin), and the notes screen follows the folder. Not while the keyboard, a
panel or a menu is up.

## Fonts

The firmware has Inter Medium only. Bold, italic and bold italic (and the
regular weight again, with the dashes and curly quotes the firmware's
lacks), in the app's four sizes, travel in `notas_p4.pak` (802 KB), which the
app reads a font at a time when a note first draws with it:

```
python3 apps/notas/tools/pack_fonts.py          # -> apps/notas/build/notas_p4.pak
python3 apps/notas/tools/pack_fonts.py --sim    # and a copy in sim/sim_fs/apps
```

It needs `lv_font_conv` and Inter 4.1 (SIL OFL 1.1; the script uses the zip
`tools/gen_fonts.py` keeps in `~/.cache/p4os-fonts`). Without the pack the
app works in the firmware's Medium: bold is drawn twice a pixel apart and
italic is upright.

## Build and install

```
tools/build_apps.sh notas
python3 apps/notas/tools/pack_fonts.py
tools/install_apps.sh p4os.local notas         # the .so, the pack and the page; restarts the board
tools/install_lang.sh p4os.local en            # and de
```

## Code

| File | What |
|---|---|
| `notas.c` | the app: the notes, a note's screen with its formatting bar and panels, the bin, menus |
| `nt_editor.c` | the rich-text editor: layout, drawing, caret, selection, undo, clipboard |
| `nt_doc.c` | the document, its Markdown both ways, fragments (undo and clipboard) |
| `nt_tasks.c` | the task list screen |
| `nt_kb.c` | the keyboard |
| `nt_store.c` | the folder: index, load, save, rename, bin, the welcome note |
| `nt_fonts.c` | the font pack |
| `nt_ui.c`, `nt_theme.c` | drawn icons, sheets, swatches; the two palettes |
