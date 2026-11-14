# simply-cpp-pdf2txt

`sc-pdf2txt` turns a PDF into text the way a language model reads it best: page by page, each page
as markdown in reading order, with the layout kept where it carries meaning. Pages without usable
text (scans, or fonts without a usable encoding) can be read by OCR through a reader you give it.
It reads PDFs with [sc-mupdf](https://github.com/roelofrossouw/simply-cpp-mupdf), and so is
AGPL-3.0 like MuPDF.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has the [sc-pdf2txt reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-pdf2txt).

```cpp
#include <pdf2txt.h>

// Every page with text; pages without any left out.
const auto text = sc::pdf2txt::document_text("letter.pdf");

// Pages without enough text read by OCR, at most 100 pages and 300 KB.
const auto scanned = sc::pdf2txt::document_text("scan.pdf", {.max_size = 300 * 1024, .max_pages = 100},
        [&](const sc::mupdf::page &page) { return my_ocr(page.png(1.5)) + "\n"; });
```

## What a page becomes

```
<page 1>
# Policy Schedule
### Insured details
[b1] The insured named in this schedule is covered for the sections shown below, subject to the terms, ...

[b2] | Section | Sum insured | Premium |
|---|---|---|
| Motor | R 250 000 | R 1 200 |
```

- **Reading order:** blocks are grouped into columns, read left to right and top to bottom. A
  block spanning most of the text's width is a section break.
- **Headings:** `#` and `##` from the font size against the body's, which is the most common size
  weighted by text length. `###` is a short bold block.
- **Paragraphs:** the visual lines are joined. A list item (`- `, `•`, `(a) `, `4.2 `, `b) ` ...)
  and a line ending well short of the block's edge keep their own lines.
- **Tables:** rows of at least 3 cells, lining up column by column over at least 2 rows, become a
  markdown table. Columns match by overlap, so right-aligned amounts and centred headers line up.
- **Anchors:** `[bN] ` before each body block and table (not before headings), so an answer can
  say where it read something. They are numbered per page (from b1 on each page) or through the
  whole document.

A page needs OCR when it gives no text, when most of its lines don't read as text, or, with an OCR
reader, when it gives less than `min_page_text` (500) bytes. Without a reader, short pages are kept
as they are and empty ones are left out.

## API

| | |
|---|---|
| `document_text(file or document, options, ocr, progress, figures)` | the whole document; `ocr` (`page_reader`) gets a page that needs OCR and gives its text; `progress` hears "Doing OCR on page 3"; `figures` counts pages, OCR'd pages and OCR seconds |
| `append_page(page, out, block_number, options)` | one page added to `out`; false when it needs OCR |
| `page_markdown(page, layout, &block_number)` | one page's markdown, without `<page N>` |

`options`: `layout` (`anchors`, the heading ratios 1.30/1.60, `spanning_fraction` 0.65,
`column_gutter_min` 8 pt, the table tolerances and minimum rows and columns), `numbering`
(`anchor_numbering::per_page` or `document`), `page_marks` (`<page N>` lines), `max_size` (bytes:
no new page once reached, and a page stops at a block; 0 means no limit), `max_pages` (0 means all)
and `min_page_text`.

The layout is the linearizer of read_schedule 1.1.2 (simply-cpp-mupdf's `sc::pdf::linearizer`).
Its thresholds, comparisons and float arithmetic are kept, and the output is the same byte for
byte.

## pdf2txt, the program

```bash
pdf2txt invoice.pdf                     # every page with text, anchors per page
pdf2txt -p 5 -s 100000 report.pdf       # the first 5 pages, stopping at 100 000 bytes
pdf2txt -A -P letter.pdf                # plain markdown: no anchors, no <page N> lines
pdf2txt -d contract.pdf                 # anchors numbered through the document
```

It does no OCR: pages without text are left out.

## Building

```bash
scripts/install.sh      # build, test, package, install
```

It needs sc-core 2.0.0 and sc-mupdf 4.3.0 (pages read with `image_blocks::left_out`).
