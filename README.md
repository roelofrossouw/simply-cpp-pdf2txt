# simply-cpp-pdf2txt

`sc-pdf2txt` turns a PDF into text the way a language model reads it best: page by page, each page
as markdown in reading order, with the layout kept where it carries meaning. Pages without usable
text (scans, or fonts without a usable encoding) can be read by OCR through a reader you give it.
It reads PDFs with [sc-mupdf](https://github.com/roelofrossouw/simply-cpp-mupdf), and so is
AGPL-3.0 like MuPDF.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has the [sc-pdf2txt reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-pdf2txt).

```cpp
#include <sc/pdf2txt/pdf2txt.h>

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
  weighted by text length. `###` is a short bold block. A "Label: value" line (`Plan number:
  4005417293`) is a field, not a heading.
- **Paragraphs:** the visual lines are joined. A list item (`- `, `•`, `(a) `, `4.2 `, `b) ` ...)
  and a line ending well short of the block's edge keep their own lines. Text after a gap on the
  same row (a bullet and its item, `1.` and its heading, `DATE:` and the date) is one line, and a
  word broken at a line end (`low-` / `interest`) is joined again (`low-interest`). The bullets
  `•`, `◦` and `▪` are written as markdown's `- `.
- **Tables:** rows of at least 3 cells, lining up column by column over at least 2 rows, become a
  markdown table. A table goes on through rows with fewer cells that sit in its columns, so a row
  whose cells wrap over several lines (a cover description over three lines, its premium on the
  first) stays in the table, line by line. Between two columns the boundary is the middle of the gap
  that separates them in every row with the most cells, and a cell goes to the column its middle is
  in, so amounts right-aligned under their heading (or wider than it) stay under it.
- **Repeats left out:** a line at the top or bottom of 3 pages or more (and of 40% of the pages),
  digits aside, is given the first time only: running headers, "Page 3 of 9", the registration
  footer. A bare page number (`2`, `2/9`, `- 2 -`, `Page 2`) alone in its block is left out every
  time. A page the same as an earlier one is `(the same text as page N)`, and a block of 80
  bytes or more given before is left out.
- **Tidied text:** leader dots and underscores (`........`) become ` … `, runs of spaces one.
- **Anchors:** `[bN] ` before each body block and table (not before headings), so an answer can
  say where it read something. They are numbered per page (from b1 on each page) or through the
  whole document.

A page needs OCR when it gives no text, when most of its lines don't read as text, or, with an OCR
reader, when it gives less than `min_page_text` (500) bytes, or more than `max_junk_share` of
characters that aren't text (`junk_share()`: a font that maps its glyphs to the wrong characters, as
some FOP and BI Publisher PDFs have; 1, never, by default). Without a reader, short pages are kept
as they are and empty ones are left out.

## API

| | |
|---|---|
| `document_text(file or document, options, ocr, progress, figures)` | the whole document; `ocr` (`page_reader`) gets a page that needs OCR and gives its text; `progress` hears "Doing OCR on page 3"; `figures` counts pages, OCR'd pages and OCR seconds |
| `append_page(page, out, block_number, options)` | one page added to `out`; false when it needs OCR |
| `page_markdown(page, layout, &block_number)` | one page's markdown, without `<page N>` |
| `junk_share(text)` | the share of its characters (white space aside) outside ASCII, Latin, punctuation, currency and shapes: 0.7 or more for a page read through a broken font |

`options`: `layout` (`anchors`, the heading ratios 1.30/1.60, `spanning_fraction` 0.65,
`column_gutter_min` 8 pt, the table tolerances and minimum rows and columns, `table_wrapped_rows`,
`table_row_gap_em` 4.5, `tidy_text`, `label_headings`), `numbering`
(`anchor_numbering::per_page` or `document`), `page_marks` (`<page N>` lines), `max_size` (bytes:
no new page once reached, and a page stops at a block; 0 means no limit), `max_pages` (0 means all),
`min_page_text`, `max_junk_share`, `repeated_margins` and `repeated_text`. `figures` also counts the margin lines,
blocks and pages left out as repeats.

The layout started as the linearizer read_schedule 1.1.2 used (then in sc-mupdf,
which no longer has it), which 1.0 kept byte for byte. 1.1 changes the text; 1.0's comes back with
`table_wrapped_rows`, `tidy_text`, `repeated_margins` and `repeated_text` false and
`label_headings` true. On 173 insurance schedules 1.1's text is 11% shorter, has 42% fewer amounts
standing alone in a block away from their label, and takes the same time (about 55 ms a document). 1.2
joins the text on one row and words broken at a line end, writes bullets as `- ` and leaves out bare
page numbers: on 150 schedules with text it has 14% fewer lines and 7% fewer anchors, no bullet alone
on a line (1,011 before), 80% fewer headings split from their number, and 69% fewer labels apart
from their value.

## pdf2txt, the program

```bash
pdf2txt invoice.pdf                     # every page with text, anchors per page
pdf2txt -p 5 -s 100000 report.pdf       # the first 5 pages, stopping at 100 000 bytes
pdf2txt -A -P letter.pdf                # plain markdown: no anchors, no <page N> lines
pdf2txt -d contract.pdf                 # anchors numbered through the document
pdf2txt -k schedule.pdf                 # repeated headers, footers, blocks and pages kept
```

It does no OCR: pages without text are left out.

## Building

```bash
scripts/install.sh      # build, test, package, install
```

It needs sc-core 2.0.0 and sc-mupdf 4.3.0 (pages read with `image_blocks::left_out`).
