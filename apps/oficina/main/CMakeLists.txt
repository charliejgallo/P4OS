idf_component_register(
    SRCS "oficina.c" "of_ui.c" "of_kb.c" "of_sheetview.c" "of_textview.c"
         "of_util.c" "of_deflate.c" "of_tinfl.c" "of_zip.c" "of_xml.c" "of_sheet.c" "of_formula.c"
         "of_numfmt.c" "of_csv.c" "of_xlsx.c" "of_ods.c" "of_text.c"
         "of_doc.c" "of_docx.c" "of_odt.c" "of_docview.c" "of_fonts.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
