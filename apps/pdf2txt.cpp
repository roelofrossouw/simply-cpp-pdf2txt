// pdf2txt, the program: a PDF's text as sc::pdf2txt gives it, on standard output. No OCR: pages
// without usable text are left out.

#include <sc/pdf2txt/pdf2txt.h>

#include <sc/core/params.h>

#include <algorithm>
#include <iostream>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "pdf2txt"},
        {"description", "Writes a PDF's text for a language model: each page as \"<page N>\" and markdown in reading order, with "
                        "headings, reflowed paragraphs, tables and a [bN] anchor before each block."},
        {"version", SC_PDF2TXT_VERSION},
        {"epilog", "Pages without usable text (scans, fonts without a usable encoding) are left out: the library reads them by "
                   "OCR when the program using it gives a reader."},
        {"options", {
            {"max-pages", {{"short", "p"}, {"type", "integer"}, {"default", 0}, {"value", "N"},
                           {"help", "read only the first N pages (0: all)"}}},
            {"max-size", {{"short", "s"}, {"type", "integer"}, {"default", 0}, {"value", "BYTES"},
                          {"help", "stop at a block once the text is this long (0: no limit)"}}},
            {"no-anchors", {{"short", "A"}, {"type", "boolean"}, {"help", "no [bN] anchors"}}},
            {"document-anchors", {{"short", "d"}, {"type", "boolean"},
                                  {"help", "number the anchors through the document, not from b1 on each page"}}},
            {"no-page-marks", {{"short", "P"}, {"type", "boolean"}, {"help", "no \"<page N>\" lines"}}},
            {"keep-repeats", {{"short", "k"}, {"type", "boolean"},
                              {"help", "keep what repeats: headers and footers on every page, blocks and pages given before"}}},
            {"password", {{"value", "PASSWORD"}, {"help", "the password of an encrypted PDF"}}}}},
        {"positional", {{"file", {{"help", "the PDF"}}}}}}};

    sc::pdf2txt::options options;
    options.max_pages = args.as<int>("max-pages");
    options.max_size = static_cast<std::size_t>(std::max(0LL, args.as<long long>("max-size")));
    options.layout.anchors = !args["no-anchors"].get<bool>();
    if (args["document-anchors"].get<bool>()) options.numbering = sc::pdf2txt::anchor_numbering::document;
    options.page_marks = !args["no-page-marks"].get<bool>();
    if (args["keep-repeats"].get<bool>()) options.repeated_margins = options.repeated_text = false;
    try {
        const sc::mupdf::document document{args.as<std::string>("file"), args.as<std::string>("password", "")};
        std::cout << sc::pdf2txt::document_text(document, options);
    } catch (const std::exception &error) {
        std::cerr << "pdf2txt: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
