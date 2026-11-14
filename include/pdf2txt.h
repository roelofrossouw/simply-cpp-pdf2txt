#pragma once

#include <mupdf.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

// A PDF's text the way a language model reads it best: page by page, "<page N>" and the page as
// markdown:
//   - reading order: column-aware, a block spanning most of the width is a section break
//   - headings from the font size against the body's (# and ##) or a short bold block (###), but not
//     a "Label: value" line
//   - what every page repeats at its top or bottom only once
//   - paragraphs reflowed (visual lines joined; list items keep their own lines)
//   - aligned columns written as markdown tables, rows wrapping over several lines included
//   - a "[bN] " anchor before each body block, so an answer can point at where it read something
// A page without usable text (scanned, or a font without a usable encoding) can be read by OCR
// instead: the caller's page_reader gets the page (draw it with page.png()) and gives its text.
//
//     const auto text = sc::pdf2txt::document_text("letter.pdf");
//     const auto with_ocr = sc::pdf2txt::document_text("scan.pdf", {}, [&](const sc::mupdf::page &page) {
//         return my_ocr(page.png(1.5)) + "\n";
//     });
//
// The layout started as read_schedule 1.1.2's linearizer (simply-cpp-mupdf's sc::pdf::linearizer);
// since 1.1.0 it also reads tables whose rows wrap over several lines, leaves out what every page
// repeats in its margins, tidies leader dots and doesn't take "Label: value" for a heading. The 1.0
// text comes back with layout.table_wrapped_rows, tidy_text and repeated_margins false and
// label_headings true.
namespace sc::pdf2txt {
    // How a page's blocks become markdown.
    struct layout_options {
        bool anchors = true;             // "[bN] " before body blocks (tables included, headings not)
        double heading_ratio_h2 = 1.30;  // font size against the body's
        double heading_ratio_h1 = 1.60;
        double spanning_fraction = 0.65; // a block this wide (of the text's width) breaks the columns
        double column_gutter_min = 8.0;  // points: a wider gap between blocks starts a new column
        double table_x_tol_em = 0.8;     // a table column's tolerance, in body sizes
        double table_y_tol_em = 0.6;     // a table row's tolerance, in body sizes
        int table_min_rows = 2;
        int table_min_cols = 3;          // 3, so two columns of prose aren't taken for a table
        bool table_wrapped_rows = true;  // a table goes on through bands of fewer cells that sit in its
                                         // columns (rows wrapping onto more lines, empty cells); false:
                                         // every band needs table_min_cols cells (1.0's tables)
        double table_row_gap_em = 4.5;   // with table_wrapped_rows, a table goes on over a gap this tall
                                         // (in body sizes; 3 before the first full band, as in 1.0)
        bool tidy_text = true;           // leader dots (....., _____) as " … ", runs of spaces as one
        bool label_headings = false;     // "Label: value" may be a heading (1.0); false: it's text
    };

    enum class anchor_numbering {
        per_page, // each page starts again at b1 (read_schedule's)
        document  // b1, b2, ... through the whole document
    };

    struct options {
        layout_options layout;
        anchor_numbering numbering = anchor_numbering::per_page;
        bool page_marks = true;           // "<page N>\n" before each page's text
        std::size_t max_size = 0;         // bytes: no new page once the text is this long, and a page stops
                                          // at a block; 0: no limit
        int max_pages = 0;                // pages after this one aren't read; 0: all
        std::size_t min_page_text = 500;  // with an OCR reader, a page giving less markdown than this is
                                          // read by OCR instead; without one it's kept as it is
        bool repeated_margins = true;     // document_text(): a line at the top or bottom of 3 pages or more
                                          // (and of 40% of the pages with text), digits aside, is kept the
                                          // first time and left out after (running headers, "Page 3 of 9",
                                          // the insurer's registration footer)
        bool repeated_text = true;        // document_text(): a page the same as an earlier one is
                                          // "(the same text as page N)", and a block (80 bytes or more)
                                          // given before is left out
    };

    // A page's text from OCR, for a page without usable text: the text to add after "<page N>\n",
    // ending in a newline. What it throws goes to the caller of document_text().
    using page_reader = std::function<std::string(const mupdf::page &page)>;

    // Progress: "Doing OCR on page 3", then "Done OCR on page 3 -> 2.4s".
    using progress_report = std::function<void(const std::string &message)>;

    // What reading a document took.
    struct figures {
        int pages = 0;           // read, OCR'd ones included
        int ocr_pages = 0;
        double ocr_seconds = 0;
        int repeated_lines = 0;  // margin lines left out as repeats
        int repeated_blocks = 0; // blocks left out as given before (repeated_text)
        int repeated_pages = 0;  // pages the same as an earlier one
    };

    // One page's markdown, without "<page N>": the blocks in reading order, each followed by a blank
    // line, the anchors numbered on from *block_number (from b1 without it), which is left at the
    // last. Empty when the page has no usable text (none, or mostly lines that don't read as text).
    [[nodiscard]] std::string page_markdown(const mupdf::page &page, const layout_options &layout = {}, int *block_number = nullptr);

    // Adds the page ("<page N>" when page_marks, then its blocks) to out, stopping at a block once out
    // has max_size bytes. block_number is the last anchor's number. False, with out and block_number
    // unchanged, when the page gave no usable text or less than min_page_text bytes of it (when
    // min_page_text isn't 0): then it needs OCR.
    bool append_page(const mupdf::page &page, std::string &out, int &block_number, const options &options = {});

    // The document's text. Pages without usable text are read by ocr when given, else left out.
    // Throws mupdf::error when the file can't be opened, and what
    // ocr throws.
    [[nodiscard]] std::string document_text(const mupdf::document &document, const options &options = {}, const page_reader &ocr = {},
                                            const progress_report &progress = {}, figures *figures = nullptr);

    [[nodiscard]] std::string document_text(const std::filesystem::path &file, const options &options = {}, const page_reader &ocr = {},
                                            const progress_report &progress = {}, figures *figures = nullptr);
}
