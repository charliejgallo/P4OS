# Oficina: prueba en la placa antes del release

Para la sesión de firmware. Oficina (`apps/oficina`, `aos.oficina`, G5 de
`PEDIDOS-APPS-2026-11.md`) tiene las siete fases hechas, probadas en la Mac
(con AddressSanitizer) y en el simulador, **nunca en la placa**. Este
archivo dice qué hay, cómo instalarlo, cómo cargar los archivos de prueba,
qué probar fase por fase, qué medir y qué falta del lado del firmware para
publicarla. El detalle técnico de cada fase está en el
[README](README.md) (secciones "Phase N" y "Checking phase N").

## 0. Resultado de la primera pasada en la placa (2026-11-22)

Hecha por la sesión de firmware con el firmware `v0.31.0-35-g5e95c18`, la
rama mergeada con `main` (`oficina-032`), el pack de fuentes y los 55
archivos de `samples_all.sh`, todo manejado desde el portal.

- **Los 55 archivos abren.** Ninguna caída, ningún "no memory", ningún
  reinicio en 21 minutos de uso. Cada uno se miró en una captura: las
  planillas con sus formatos, los documentos con tabla, imagen (decodificada
  por el HAL) y nota al pie, los lectores de la fase 3, los comprimidos
  como carpetas (también con subcarpetas), los PDF como texto, las
  presentaciones y los binarios de Office 97-2003.
- **PDF cifrados:** RC4 abre solo; AES-128 y AES-256 con su contraseña
  tecleada en el teclado de la app; una equivocada dice "That is not the
  password" y vuelve a la lista.
- **Páginas de PDF dibujadas (fase 7):** `clasico.pdf` 128 y 146 ms por
  página (688×974), `cairo.pdf` 66, `quartz.pdf` 98, `fuentes.pdf` 166,
  `imagenes.pdf` 166, `dibujo.pdf` 278, `escaneo.pdf` 515 ms. En la Mac
  eran 3 a 50. Se ven como en el simulador.
- **Memoria:** el inicio tiene 15 014 KB de PSRAM libres; con la app
  abierta 14 080; con `registro.log` (4 MB) 9 497; con páginas dibujadas
  entre 10 702 y 12 896. Vuelve después de cada documento. La RAM interna
  no se mueve (203 KB), salvo acostada (179). Al salir el sistema la deja
  en segundo plano: retiene 905 KB (las fuentes).
- **PDF escrito por la placa:** `informe.docx` guardado como PDF y bajado a
  la Mac: una página, con títulos, colores, lista, tabla, imagen y nota al
  pie.
- **Acostada:** la lista se reacomoda y vuelve.

Lo que el portal no pudo manejar, y queda para la mano:

- **Todo lo que es gesto:** desplazar y hacer zoom en las páginas de PDF,
  el doble toque, tocar una diapositiva para presentar, el pellizco. La app
  los lee por `aos_gesture_attach`, que toma los dedos del panel y no ve
  los toques del portal (le pasa también a Mapas).
- **Pegar texto desde el portal** (`POST /api/clip` con `paste`) no llega a
  los campos de la app, que usa el teclado y los diálogos de Notas: el
  texto se tecleó tocando las teclas.
- Editar y guardar (celdas, párrafos), ordenar y filtrar, Buscar, el
  índice, "Ver como árbol", extraer de un comprimido, las otras copias
  (odt, docx, txt), el "+" de archivos nuevos, el teclado físico, la copia
  "(sin guardar)" al ser cerrada por el sistema, andar sin el pack, y la
  vista de páginas con el worker ocupado por otra app.

Una cosa a mirar: en `fuentes.pdf` la segunda línea (TrueType simbólica,
cmap 3,0) sale "Canon guiro, 123 anos", sin tildes; las otras seis las
traen. No se comparó con la Mac.

## 1. Qué hay

Rama `claude/app-oficina-564612`, sin push ni tag:

| Commit | Fase |
|---|---|
| `f8d2091`, `731f706` | 1: planillas xlsx, ods y csv (grilla, fórmulas, formatos, ordenar, filtrar) y editor de texto para archivos grandes |
| `1ad4a2b`, `56ec120` | 2: documentos docx y odt editables, guardando sólo lo tocado |
| `fe6744c` | 3: lectores (RTF, HTML, EPUB, FB2, EML, Markdown, vCard, iCal, árboles JSON/XML/YAML/TOML, imágenes, vista previa de iWork) y comprimidos (ZIP, CBZ, TAR, GZ) |
| `48a5da8` | 4: PDF leído como texto acomodado (con cifrado RC4/AES) y PDF escrito desde documentos, planillas y textos |
| `e02e48a` | 5: presentaciones pptx y odp (ver, presentar, copia a PDF) |
| `92c6c0e` | 6: doc, xls y ppt de Office 97-2003; texto de Pages, Numbers y Keynote |
| `5a4d861` y el de esta guía | 7: páginas de PDF dibujadas como se imprimen |

Lo que se instala:

- `oficina.so`: 544 KB, 230 símbolos, ABI 100 (`tools/build_apps.sh oficina`).
- `oficina_p4.pak`: 877 KB, **obligatorio** (`"need": true` en
  `store.json`): Inter regular, negrita, cursiva y negrita cursiva en 20,
  24, 28, 36 y 48 px para los documentos. Sin el pack la app anda con la
  Inter del firmware (negrita dibujada dos veces, cursiva derecha) y lo
  avisa en el log.
- `web/oficina.js`: la página Oficina del portal (subir, bajar, borrar,
  carpetas, vista rápida de csv y texto, recientes).
- Textos: `lang/en|de/aos.oficina.lang` (264) y `web.oficina.lang` (22).

## 2. Instalar

Desde la raíz del repo, en la rama:

```
tools/build_apps.sh oficina                      # ok  oficina.so  544K  ABI 100  230 symbols
python3 apps/oficina/tools/pack_fonts.py         # apps/oficina/build/oficina_p4.pak (lv_font_conv; Inter en ~/.cache/p4os-fonts)
tools/install_apps.sh p4os.local oficina         # el .so, el pack y la página; reinicia la placa
tools/install_lang.sh p4os.local en              # y de
```

Probarla también **en una tarjeta donde no estuvo nunca** (sin
`/sdcard/data/oficina` ni preferencias `of_*`): la carpeta
`/sdcard/documentos` y la de datos se crean solas en el primer uso.

## 3. Los archivos de prueba

Todo inventado, hecho por los scripts de `tools/` (ninguno sale de
documentos de nadie). En la Mac:

```
sh apps/oficina/tools/samples_all.sh /tmp/oficina-pruebas
```

Da 55 archivos (necesita Python 3; fontTools y Pillow para los tres de la
fase 7; en la Mac usa `textutil`, `cupsfilter`, `pango-view`,
`rsvg-convert` y `tesseract` si están, y si falta alguno sigue sin esos
archivos). Para no mezclarlos con los documentos del usuario, subirlos a
una carpeta aparte:

```
B=http://p4os.local
curl -4 -s -X POST "$B/api/fs/mkdir?path=/documentos/pruebas-oficina"
for f in /tmp/oficina-pruebas/*; do
  curl -4 -sS -o /dev/null -w "%{http_code} ${f##*/}\n" -X PUT \
    "$B/api/fs/put?path=/documentos/pruebas-oficina/${f##*/}" --data-binary "@$f"
done
```

(o arrastrarlos a la página Oficina del portal, que también prueba la
subida). Al terminar, borrar la carpeta desde la misma página.

Contraseñas de los PDF cifrados: `clave_aes.pdf` → `1234`,
`clave_256.pdf` → `abc`; `clave_rc4.pdf` abre sin pedirla (contraseña de
usuario vacía).

## 4. Cómo manejarla desde la Mac

`POST /api/open?id=aos.oficina` la abre; `GET /api/screen.bmp` es la
pantalla; `POST /api/touch?x=&y=` toca; `tools/plog.sh p4os.local` lee el
log. Coordenadas útiles, derecha (720 × 1280):

| Qué | Dónde |
|---|---|
| Pestañas Recientes / Archivos | (104, 188) / (272, 188) |
| Filas de la lista de archivos | x 360, y = 372 + 110·k (k desde 0; arriba, en 272, la fila para subir de carpeta) |
| Atrás de un documento | (44, 92) |
| Menú ⋯ de un documento | (674, 92) |
| Menú (hoja de abajo) | ítems cada 92 px, el último en y 1176 |
| Botón OK de una pregunta con texto | (526, 282) |

Los pellizcos y el teclado físico hay que probarlos a mano: el portal no
los manda.

## 5. Qué probar

Cada punto: qué hacer y qué tiene que pasar. Lo marcado **[placa]** no se
pudo probar fuera de ella.

### Fase 1: planillas y texto

1. `ventas.xlsx`: se ve la grilla con paneles fijos, celdas combinadas,
   colores, formatos (moneda, fecha, porcentaje). Dos toques en una celda
   la editan; una fórmula como `=SUMA(...)` sobre una columna de números
   da el total, y tocar celdas mientras se escribe pone sus referencias. Guardar (menú → Guardar)
   y volver a abrir: el valor está. **[placa]** el teclado en pantalla.
2. `presupuesto.ods`: lo mismo; menú → "Ordenar de menor a mayor",
   "Filtrar por este valor" y "Sacar el filtro", deshacer.
3. `gastos.csv` y `temperaturas.csv`: separador y coma decimal detectados;
   guardar no cambia los campos que no se tocaron.
4. `registro.log` (4 MB): abre rápido, se desplaza liviano, los niveles en
   color, Buscar anda. **[placa]** que no trabe la interfaz mientras indexa.
5. `programa.py`, `config.ini`, `datos.json`: resaltado; editar y guardar.
6. "+" en la lista: nuevo xlsx, ods, csv, txt y md.
7. Pellizco en la planilla y en el texto (zoom); acostada.
8. **[placa]** Con teclado USB o Bluetooth: flechas, Shift, Enter/Tab,
   Ctrl+Z/C/V/S/F en la planilla y en el texto.
9. **[placa]** Dejar un documento con cambios sin guardar y abrir otras
   cuatro apps (el sistema cierra la quinta): queda una copia
   "<nombre> (sin guardar)" al lado del archivo, nunca encima.

### Fase 2: documentos

1. `informe.docx` y `carta.odt`: títulos, negritas, cursivas, colores,
   listas, la tabla, la imagen (**[placa]** decodificada por el HAL), la
   nota al pie al final.
2. Editar texto, poner negrita, cambiar el estilo de un párrafo, partir y
   unir párrafos, deshacer. Guardar y abrir de nuevo.
3. El párrafo con control de cambios se ve y no se puede editar.
4. Guardar una copia como odt (desde el docx) y como docx (desde el odt),
   como txt y como PDF.
5. Menú → Letra más grande/más chica; acostada.
6. Con el pack: negrita y cursiva de verdad. Sin el pack (sacarlo de
   `/sdcard/apps/`): sigue andando, con negrita doble.

### Fase 3: lectores y comprimidos

1. Abrir cada uno, ver, copiar, Índice, Buscar, Guardar una copia como
   docx: `receta.rtf`, `club.html`, `cuentos.epub` (capítulos, imágenes),
   `jardin.fb2`, `reunion.eml` (adjuntos listados y guardables),
   `guia.md` y `LEEME.md` (con formato), `contactos.vcf`, `agenda.ics`,
   `paisaje.png`. `datos.json`, `config.xml`, `ajustes.yaml` y
   `proyecto.toml` abren en el editor de texto; menú → "Ver como árbol".
2. `folleto.pages`, `turnos.numbers`, `charla.key`: la vista previa y el
   texto (fase 6).
3. `fotos.zip`, `historieta.cbz`, `notas.tar`, `copia.tar.gz`,
   `carta.txt.gz`: se recorren como carpetas; abrir una entrada; extraer
   con una pulsación larga.
4. `latin1.txt`: los acentos bien (Windows-1252 detectado).

### Fase 4: PDF como texto y PDF escrito

1. `clasico.pdf`: dos columnas en orden, sin encabezados ni números de
   página repetidos, la palabra cortada con guion unida, la lista, las dos
   imágenes (**[placa]** la JPEG).
2. `moderno.pdf`, `girado.pdf`, `roto.pdf` (se reconstruye), `quartz.pdf`,
   `cairo.pdf`, `columnas.pdf`, `escaneo.pdf` (el texto invisible del OCR,
   no la imagen).
3. Los tres cifrados: pide la contraseña (§3), una equivocada dice "La
   contraseña no es esa".
4. Desde un documento, una planilla y un texto: Guardar una copia como PDF;
   abrir el PDF resultante en la app y bajarlo desde el portal para verlo
   en la Mac.

### Fase 5: presentaciones

1. `charla.pptx` y `charla.odp`: la columna de diapositivas con fondos,
   formas, viñetas, la tabla, la imagen; menú → Notas del orador.
2. Tocar una diapositiva: presentar a pantalla completa, toque a la
   derecha o deslizar para avanzar, atrás para salir; la oculta se saltea.
3. Guardar una copia como PDF (una página por diapositiva) y el texto como
   docx.

### Fase 6: Office 97-2003

1. `informe.doc`, `receta.doc`, `club.doc`: texto con formato, tablas,
   el enlace; Guardar una copia como docx.
2. `cuentas.xls`: dos hojas (una oculta), formatos, combinadas, paneles
   fijos; se edita y "Guardar" ofrece copia como xlsx u ods.
3. `charla.ppt`: diapositivas con la imagen, el grupo, las notas, la
   oculta.

### Fase 7: páginas de PDF dibujadas (lo nuevo)

1. Abrir `clasico.pdf` (sale el texto acomodado), menú → "Ver las páginas
   como se imprimen": aparecen las tres páginas en columna, al ancho.
   Volver atrás y abrir otro PDF: ahora abre directamente en páginas (la
   elección se recuerda); menú → "Ver el texto acomodado" la cambia de
   nuevo.
2. Arrastrar (con inercia): el número "2 / 3" aparece abajo y se va solo.
3. **[placa]** Doble toque: zoom ×2 donde se tocó; otro doble toque vuelve
   al ancho. Pellizco hasta ×4. Mientras se dibuja el zoom nuevo se ve la
   imagen anterior estirada, y después nítida.
4. Acostada: la página en el medio sigue en el medio.
5. `dibujo.pdf`: líneas con sus puntas y uniones, la línea punteada, las
   dos estrellas (rellena y con hueco), la curva, el círculo recortado, los
   cuadrados semitransparentes, los colores (CMYK, Lab, tintas), el
   degradé lineal y el radial, los dos patrones (cuadros rojos y azules;
   puntos azules), los dos recuadros "formulario", los triángulos y
   cuadrados verdes y rojos (fuente Type 3) y el recuadro amarillo
   (anotación).
6. `imagenes.pdf`: la imagen de 16 colores, el tablero, la imagen con
   máscara suave sobre la franja azul, **[placa]** la JPEG derecha y girada,
   el círculo magenta (máscara), el hueco verde (color clave) y la imagen
   en línea de 4 × 2.
7. `fuentes.pdf`: la misma frase "Cañón güiro, 123 años" en siete fuentes
   embebidas (TrueType, simbólica, CID, CFF, OpenType, CIDFontType0,
   Type 1 con "á ó" armadas por seac), y abajo contorno, relleno con
   contorno, invisible, subíndice, escala y texto girado. Las etiquetas
   chicas son Helvetica no embebida: salen con la Inter del firmware.
8. Los PDF de la fase 4 en páginas, en especial `escaneo.pdf` (imagen
   grande), `quartz.pdf` (TrueType de la Mac) y los cifrados (pide la
   contraseña también en esta vista).
9. Salir con una página dibujándose (atrás enseguida) y volver a entrar
   varias veces: no se cuelga ni se cae (el worker se detiene en
   `of_pdfv_close`).
10. **[placa]** Abrir la vista de páginas con otra app que tenga el worker
    del sistema ocupado (Video, por ejemplo): tiene que avisar "Otra app
    está usando el dibujo en segundo plano" y no colgarse.

## 6. Qué medir **[placa]**

- **Tiempo de cada página** (fase 7): la app escribe en el log, por cada
  página o parte dibujada, `pdf page N WxH at S: T ms` (y "(stopped)" si se
  dejó de necesitar, "(no memory)" si no hubo memoria). Con
  `tools/plog.sh p4os.local | grep "pdf page"`. En la Mac son 3 a 50 ms;
  en la placa se espera bastante más (soft-float donde haya `double` del
  parser, PSRAM). Interesa: `clasico.pdf` al ancho, `fuentes.pdf` al ancho
  y ×2, `escaneo.pdf`, y una página acostada.
- **Memoria** (fase 7): hasta 6 millones de píxeles guardados (12 MB de
  PSRAM) más la página que se está dibujando; ver que no falte al abrir
  otras apps.
- **Apertura**: `registro.log` (4 MB), `ventas.xlsx`, `informe.docx`,
  `cuentos.epub`, `clasico.pdf` como texto.
- **Interfaz**: que la grilla, el texto, las diapositivas y las páginas se
  desplacen sin tirones.

## 7. Lo que ya se sabe que no hace

En el README, sección "Left over". En corto: la fase 7 no dibuja
sombreados de malla, máscaras suaves de grupos ni modos de mezcla; JPEG
2000, JBIG2 y fax salen como un recuadro gris; las fuentes no embebidas
son todas Inter; en las páginas no se siguen enlaces ni se selecciona
texto. Los `.xls` y `.ppt` de prueba están hechos a mano (no hay en la Mac
quien los escriba), así que un archivo de Office real puede traer cosas que
no cubren. PDF escrito por la app: sólo WinAnsi.

## 8. Lo que falta del lado del firmware para publicarla

Pedidos ya anotados en `docs/internal/HANDOFF-NUEVAS-APPS.md` (filas de
Oficina, 2026-11-22):

- el nombre `Oficina` en `_sistema.lang` (en `Office`, de `Büro`);
- sumar `oficina` a `P4OS_SIM_APPS` en `sim/CMakeLists.txt`;
- `THIRD-PARTY.md`: miniz `tinfl` (MIT) vendorizado en
  `apps/oficina/third/`, e Inter 4.1 (SIL OFL 1.1) en `oficina_p4.pak`
  (la fase 7 no suma código de terceros: el lector de fuentes y el
  dibujante son propios);
- publicar `oficina_p4.pak` en `store/packs.json` (obligatorio);
- (opcional) que Archivos abra con Oficina los formatos de la lista;
- (opcional) un comando de guion `hwkey` en el simulador.

Esta sesión no tocó `store/`, `CARRIED_PACKS` ni el CI.

## 9. Qué devolver

Por cada punto de §5 que falle: el archivo, qué se hizo, `screen.bmp` y
las líneas del log. Los tiempos de §6. Con eso se corrige en la rama
`claude/app-oficina-564612` antes del release.
