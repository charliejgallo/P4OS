idf_component_register(
    SRCS "oficina.c" "of_ui.c" "of_kb.c" "of_sheetview.c" "of_textview.c"
         "of_util.c" "of_deflate.c" "of_tinfl.c" "of_zip.c" "of_xml.c" "of_sheet.c" "of_formula.c"
         "of_numfmt.c" "of_csv.c" "of_xlsx.c" "of_ods.c" "of_text.c"
         "of_doc.c" "of_docx.c" "of_odt.c" "of_docview.c" "of_fonts.c"
         "of_import.c" "of_rtf.c" "of_text2.c" "of_archive.c"
         "of_crypt.c" "of_pdf.c" "of_pdftext.c" "of_pdfw.c"
         "of_slides.c" "of_pptx.c" "of_odp.c" "of_slideview.c"
         "of_cfb.c" "of_doc97.c" "of_xls.c" "of_ppt.c" "of_iwa.c" "of_pdffont.c" "of_pdfdraw.c" "of_pdfview.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl
)
