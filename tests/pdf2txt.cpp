// sc::pdf2txt: PDFs written with sc::mupdf::writer (in the temporary directory, not kept) read back:
// pages, headings, anchors, a table, OCR for pages without enough text, the options and the limits.
#include <pdf2txt.h>

#include "sc_test.h"

#include <mupdf_writer.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

namespace {
    const std::string paragraph[] = {
        "The insured named in this schedule is covered for the sections shown below, subject to the terms,",
        "conditions and exclusions of the policy wording, which forms part of this contract of insurance and",
        "must be read together with it. Premiums are payable monthly in advance by debit order on the date",
        "agreed with the broker. Cover lapses when a premium is not paid within the days of grace allowed,",
        "and claims must be reported within thirty days of the event that gives rise to them.",
    };

    // A schedule-like page: a big title, a bold heading, a paragraph and a three-column table.
    void schedule_page(sc::mupdf::writer &pdf) {
        pdf.draw(sc::text{"Policy Schedule", {72, 80}, {"sans-serif", 24}});
        pdf.draw(sc::text{"Insured details", {72, 130}, sc::font::bold("sans-serif", 10)});
        double y = 160;
        for (const auto &line: paragraph) {
            pdf.draw(sc::text{line, {72, y}, {"sans-serif", 10}});
            y += 13;
        }
        const std::vector<std::vector<std::string> > rows{{"Section", "Sum insured", "Premium"},
                                                          {"Motor", "R 250 000", "R 1 200"},
                                                          {"Buildings", "R 1 500 000", "R 850"}};
        y += 30;
        for (const auto &row: rows) {
            double x = 72;
            for (const auto &cell: row) {
                pdf.draw(sc::text{cell, {x, y}, {"sans-serif", 10}});
                x += 160;
            }
            y += 16;
        }
    }

    bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
}

int main() {
    using namespace sc::pdf2txt;
    const auto directory = std::filesystem::temp_directory_path() / "sc-pdf2txt-test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto file = directory / "schedule.pdf";
    {
        sc::mupdf::writer pdf{file};
        schedule_page(pdf);
        pdf.new_page(); // nothing on it
        pdf.new_page();
        pdf.draw(sc::text{"Page three has a line, too little to go on.", {72, 100}, {"sans-serif", 10}});
        pdf.new_page();
        schedule_page(pdf);
        pdf.close();
    }

    std::vector<int> ocr_pages;
    const page_reader ocr = [&](const sc::mupdf::page &page) {
        ocr_pages.push_back(page.number());
        return std::string{"OCR of page "} + std::to_string(page.number()) + "\n";
    };
    std::vector<std::string> said;
    const progress_report progress = [&](const std::string &message) { said.push_back(message); };

    SECTION("Pages with text as markdown with anchors, the rest by OCR");
    figures counted;
    const auto text = document_text(file, {}, ocr, progress, &counted);
    if (std::getenv("SC_PDF2TXT_SHOW")) std::cout << text << std::endl;
    CHECK(text.starts_with("<page 1>\n# Policy Schedule\n"));
    CHECK(contains(text, "### Insured details\n"));
    CHECK(contains(text, "[b1] The insured named in this schedule is covered"));
    CHECK(contains(text, "contract of insurance and must be read")); // the lines reflowed
    CHECK(contains(text, "[b2] | Section | Sum insured | Premium |\n|---|---|---|\n| Motor | R 250 000 | R 1 200 |\n| Buildings | R 1 500 000 | R 850 |\n"));
    CHECK(contains(text, "<page 2>\nOCR of page 2\n<page 3>\nOCR of page 3\n<page 4>\n# Policy Schedule\n"));
    CHECK((ocr_pages == std::vector<int>{2, 3}));
    CHECK(contains(text.substr(text.find("<page 4>")), "[b1] The insured")); // anchors start again on each page
    CHECK_EQ(counted.pages, 4);
    CHECK_EQ(counted.ocr_pages, 2);
    CHECK((said.size() == 4 && said[0] == "Doing OCR on page 2" && said[1].starts_with("Done OCR on page 2 -> ")));

    SECTION("Without OCR: a page without text is left out, a short one kept");
    const auto plain = document_text(file);
    CHECK(!contains(plain, "<page 2>"));
    CHECK(contains(plain, "<page 3>\n[b1] Page three has a line, too little to go on.\n\n<page 4>\n"));

    SECTION("Anchors through the document, no anchors, no page marks");
    const auto numbered = document_text(file, {.numbering = anchor_numbering::document});
    CHECK(contains(numbered.substr(numbered.find("<page 4>")), "[b4] The insured")); // b1, b2 on page 1, b3 on page 3
    const auto bare = document_text(file, {.layout = {.anchors = false}, .page_marks = false});
    CHECK(bare.starts_with("# Policy Schedule\n"));
    CHECK(!contains(bare, "[b") && !contains(bare, "<page"));

    SECTION("One page's markdown, and a document in memory");
    const sc::mupdf::document document{file};
    int block = 5;
    const auto page_one = page_markdown(document.page(0), {}, &block);
    CHECK(page_one.starts_with("# Policy Schedule\n"));
    CHECK(contains(page_one, "[b6] The insured"));
    CHECK_EQ(block, 7);
    CHECK(page_markdown(document.page(1)).empty());
    std::ifstream in{file, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK_EQ(document_text(sc::mupdf::document::from_memory(bytes)), plain);

    SECTION("max_pages: pages after it aren't read");
    ocr_pages.clear();
    const auto first = document_text(file, {.max_pages = 1}, ocr);
    CHECK(!contains(first, "<page 2>"));
    CHECK(ocr_pages.empty());

    SECTION("max_size: no new page once reached, and a page stops at a block");
    const auto short_text = document_text(file, {.max_size = 100}, ocr);
    CHECK(short_text.starts_with("<page 1>\n"));
    CHECK(!contains(short_text, "<page 2>"));
    CHECK(!contains(short_text, "| Motor"));

    SECTION("A file that isn't a PDF throws");
    CHECK_THROWS_AS((void) document_text(directory / "missing.pdf"), std::exception);

    std::filesystem::remove_all(directory);
    TEST_SUMMARY();
}
