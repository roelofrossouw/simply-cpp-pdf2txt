## TESTS ##
set(SC_TEST_LINK_LIBRARIES sc-pdf2txt)

# PDFs written with sc::mupdf::writer, read back.
add_sc_test(pdf2txt LABELS unit)
