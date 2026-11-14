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
    CHECK(contains(text, "<page 2>\nOCR of page 2\n<page 3>\nOCR of page 3\n<page 4>\n(the same text as page 1)\n"));
    CHECK((ocr_pages == std::vector<int>{2, 3}));
    CHECK_EQ(counted.repeated_pages, 1);
    CHECK_EQ(counted.pages, 4);
    CHECK_EQ(counted.ocr_pages, 2);
    CHECK((said.size() == 4 && said[0] == "Doing OCR on page 2" && said[1].starts_with("Done OCR on page 2 -> ")));

    SECTION("repeated_text false: a page the same as an earlier one is given again");
    const auto again = document_text(file, {.repeated_text = false}, ocr);
    CHECK(contains(again, "<page 4>\n# Policy Schedule\n"));
    CHECK(contains(again.substr(again.find("<page 4>")), "[b1] The insured")); // anchors start again on each page

    SECTION("Without OCR: a page without text is left out, a short one kept");
    const auto plain = document_text(file);
    CHECK(!contains(plain, "<page 2>"));
    CHECK(contains(plain, "<page 3>\n[b1] Page three has a line, too little to go on.\n\n<page 4>\n"));

    SECTION("Anchors through the document, no anchors, no page marks");
    const auto numbered = document_text(file, {.numbering = anchor_numbering::document, .repeated_text = false});
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

    SECTION("A table whose rows wrap over several lines, label lines, leader dots, what repeats in the margins");
    const auto form = directory / "form.pdf";
    {
        sc::mupdf::writer pdf{form};
        for (int n = 1; n <= 4; ++n) {
            if (n > 1) pdf.new_page();
            pdf.draw(sc::text{"Policy number: P12345", {72, 30}, {"sans-serif", 8}}); // the running header
            pdf.draw(sc::text{"Page " + std::to_string(n) + " of 4", {480, 30}, {"sans-serif", 8}});
            pdf.draw(sc::text{"Plan number: 4005417293", {72, 80}, sc::font::bold("sans-serif", 10)});
            pdf.draw(sc::text{"Total premium ........................ R " + std::to_string(100 * n), {72, 110}, {"sans-serif", 10}});
            // Cover section | Details | Sum insured | Premium, the second row's details over three lines
            const double x[] = {72, 180, 360, 460};
            const std::vector<std::vector<std::string> > rows{{"Cover section", "Details", "Sum insured", "Premium"},
                                                              {"Motor", "Primary driver: A Driver", "R 250 000", "R 1 200"},
                                                              {"", "FORD RANGER 3.2", "", ""},
                                                              {"", "Car hire", "", "R 145"},
                                                              {"Building", "12 Main Road", "R 1 500 000", "R 850"}};
            double y = 150;
            for (const auto &row: rows) {
                for (size_t c = 0; c < row.size(); ++c)
                    if (!row[c].empty()) pdf.draw(sc::text{row[c], {x[c], y}, {"sans-serif", 10}});
                y += 14;
            }
            // a second table: a header of short words right-aligned over wider amounts, which start
            // further left than their heading
            const std::vector<std::vector<std::pair<double, std::string> > > dues{{{72, "Section"}, {430, "renewal"}, {530, "due"}},
                                                                                {{72, "Household contents"}, {400, "2,229.26"}, {497, "2,229.26"}},
                                                                                {{72, "Motor vehicles"}, {400, "1,561.21"}, {497, "1,071.00"}}};
            double dy = 240;
            for (const auto &row: dues) {
                for (const auto &[x, text]: row) pdf.draw(sc::text{text, {x, dy}, {"sans-serif", 10}});
                dy += 14;
            }
            pdf.draw(sc::text{"Page " + std::to_string(n) + " text that differs: " + std::string(static_cast<size_t>(n), 'x'), {72, 340}, {"sans-serif", 10}});
        }
        pdf.close();
    }
    figures form_figures;
    const auto read = document_text(form, {}, {}, {}, &form_figures);
    if (std::getenv("SC_PDF2TXT_SHOW")) std::cout << read << std::endl;
    CHECK(contains(read, "| Cover section | Details | Sum insured | Premium |\n|---|---|---|---|\n| Motor | Primary driver: A Driver | R 250 000 | R 1 200 |\n"
                         "|  | FORD RANGER 3.2 |  |  |\n|  | Car hire |  | R 145 |\n| Building | 12 Main Road | R 1 500 000 | R 850 |\n"));
    CHECK(contains(read, "| Section | renewal | due |\n|---|---|---|\n| Household contents | 2,229.26 | 2,229.26 |\n| Motor vehicles | 1,561.21 | 1,071.00 |\n"));
    CHECK(!contains(read, "# Plan number")); // a field, not a heading
    CHECK(contains(read, "Total premium \u2026 R 100"));
    CHECK(contains(read, "Policy number: P12345")); // the first time
    CHECK_EQ(read.find("Policy number: P12345"), read.rfind("Policy number: P12345"));
    CHECK(!contains(read, "Page 2 of 4"));
    CHECK(form_figures.repeated_lines >= 6);
    const auto as_before = document_text(form, {.layout = {.table_wrapped_rows = false, .tidy_text = false, .label_headings = true},
                                                .repeated_margins = false});
    CHECK(contains(as_before, "Page 2 of 4"));
    CHECK(contains(as_before, "### Plan number: 4005417293"));
    CHECK(contains(as_before, "........................"));

    SECTION("A file that isn't a PDF throws");
    CHECK_THROWS_AS((void) document_text(directory / "missing.pdf"), std::exception);

    std::filesystem::remove_all(directory);
    TEST_SUMMARY();
}
