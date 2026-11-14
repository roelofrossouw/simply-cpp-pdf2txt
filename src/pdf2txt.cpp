// The linearizer: ported from simply-cpp-mupdf's src/linearizer.cpp (read_schedule 1.1.2) to
// sc-mupdf 4.x, by way of read-schedule 1.0. Every threshold, comparison and the float arithmetic are
// kept as they were, so the text is the same byte for byte; only the page, block and line types
// changed.
#include "pdf2txt.h"

#include <timer.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <vector>

namespace {
    using sc::rect;

    struct text_line {
        rect box;
        float size = 0.f;
        std::string text;
    };

    struct text_block {
        rect box;
        std::vector<text_line> lines;
        float first_size = 0.f;
        bool first_bold = false;
        bool is_table = false;
        std::string table_md; // rendered already when is_table
    };

    // Union-find with path halving.
    struct dsu {
        std::vector<size_t> parent;

        explicit dsu(const size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), size_t{0}); }

        size_t find(size_t a) {
            while (parent[a] != a) a = parent[a] = parent[parent[a]];
            return a;
        }

        void unite(const size_t a, const size_t b) { parent[find(a)] = find(b); }
    };

    // The vertical middle (the old sc::rect::middle()).
    double middle(const rect &box) { return box.top() + box.height() / 2; }

    // A line's font size as the old library had it: the modal size in tenths of a point, as a float.
    float float_size(const double size) { return static_cast<float>(std::lround(size * 10.0)) / 10.f; }

    // ---------------------------------------------------------------- collect

    std::vector<text_block> collect(const sc::mupdf::page &page) {
        std::vector<text_block> out;
        int total_lines = 0;
        int filled_lines = 0;
        // Without image blocks, as 1.1.2 read pages (flags 0): kept, an image splits the text around it
        // into separate blocks and the headings and anchors come out differently
        for (const auto block: page.blocks(sc::mupdf::image_blocks::left_out)) {
            if (block.type() != sc::mupdf::block_type::text) continue;
            text_block tb;
            tb.box = block.bbox();
            bool first = true;
            for (const auto line: block) {
                total_lines++;
                std::string text = line.text();
                if (text.empty()) continue;
                const float size = float_size(line.font_size());
                if (first) {
                    tb.first_size = size;
                    tb.first_bold = line.bold();
                    first = false;
                }
                tb.lines.push_back({line.bbox(), size, std::move(text)});
                filled_lines++;
            }
            if (!tb.lines.empty()) out.push_back(std::move(tb));
        }
        // Mostly lines that don't read as text: a font without a usable encoding, so OCR instead
        if (total_lines > filled_lines * 2) return {};
        return out;
    }

    // The body's font size: the most common size weighted by text length (headings are short, body
    // text long).
    double body_font_size(const std::vector<text_block> &blocks) {
        std::map<int, size_t> weight; // size * 10 -> characters
        for (const auto &b: blocks)
            for (const auto &l: b.lines)
                weight[static_cast<int>(l.size * 10.f + 0.5f)] += l.text.size();
        int best_k = 100; // 10 pt when there's nothing
        size_t best_w = 0;
        for (const auto &[k, w]: weight)
            if (w > best_w) {
                best_w = w;
                best_k = k;
            }
        return best_k / 10.0;
    }

    // ------------------------------------------------------------- list items

    bool is_list_start(const std::string &s) {
        if (s.empty()) return false;
        static const char *bullets[] = {"- ", "* ", "•", "◦", "▪", "– ", "— "};
        for (const char *b: bullets)
            if (s.rfind(b, 0) == 0) return true;
        // "(a) ", "(12) ", "(iv) "
        if (s.front() == '(') {
            const auto close = s.find(')');
            if (close != std::string::npos && close >= 2 && close <= 5 && close + 1 < s.size() && s[close + 1] == ' ') return true;
        }
        // "4. ", "4) ", "4.2 ", "4.2.1 ", but not a bare year like "2026 "
        size_t i = 0;
        bool any_digit = false, any_dot = false;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
            any_digit |= std::isdigit(static_cast<unsigned char>(s[i])) != 0;
            any_dot |= s[i] == '.';
            ++i;
        }
        if (any_digit) {
            const bool closed = i < s.size() && s[i] == ')';
            const size_t after = closed ? i + 1 : i;
            const bool spaced = after < s.size() && s[after] == ' ';
            if (spaced && (any_dot || closed)) return true;
        }
        // "a. ", "b) "
        if (s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && (s[1] == '.' || s[1] == ')') && s[2] == ' ') return true;
        return false;
    }

    // ---------------------------------------------------------------- tables
    //
    // Rows: lines grouped by their vertical middle within y_tol. A row of at least min_cols cells is a
    // candidate; a run of at least min_rows candidate rows, close together and lining up column by
    // column, is a table. Columns match by x-interval overlap, not left edges: amounts are
    // right-aligned and headers centred.

    struct band {
        double y = 0;
        std::vector<std::pair<size_t, size_t> > cells; // (block, line)
    };

    const rect &cell_box(const std::vector<text_block> &blocks, const std::pair<size_t, size_t> &c) {
        return blocks[c.first].lines[c.second].box;
    }

    bool bands_align(const std::vector<text_block> &blocks, const band &a, const band &b, double x_tol, int min_cols) {
        int matches = 0;
        for (const auto &ca: a.cells)
            for (const auto &cb: b.cells)
                if (cell_box(blocks, ca).gap_x(cell_box(blocks, cb)) <= x_tol) {
                    ++matches;
                    break;
                }
        return matches >= min_cols;
    }

    std::string escape_md_cell(std::string s) {
        for (size_t p = 0; (p = s.find('|', p)) != std::string::npos; p += 2) s.replace(p, 1, "\\|");
        std::replace(s.begin(), s.end(), '\n', ' ');
        return s;
    }

    std::string render_table(const std::vector<text_block> &blocks, const std::vector<band> &rows, double x_tol) {
        std::vector<std::pair<size_t, size_t> > cells;
        std::vector<size_t> row_of;
        for (size_t r = 0; r < rows.size(); ++r)
            for (const auto &c: rows[r].cells) {
                cells.push_back(c);
                row_of.push_back(r);
            }

        // Columns: cells linked by x-interval proximity
        dsu ds(cells.size());
        for (size_t i = 0; i < cells.size(); ++i)
            for (size_t j = i + 1; j < cells.size(); ++j)
                if (cell_box(blocks, cells[i]).gap_x(cell_box(blocks, cells[j])) <= x_tol) ds.unite(i, j);

        // Left to right by their leftmost cell
        std::map<size_t, double> col_left; // root -> leftmost
        for (size_t i = 0; i < cells.size(); ++i) {
            const size_t r = ds.find(i);
            const double l = cell_box(blocks, cells[i]).left();
            auto it = col_left.find(r);
            if (it == col_left.end() || l < it->second) col_left[r] = l;
        }
        std::vector<std::pair<double, size_t> > ordered; // (left, root)
        for (const auto &[root, l]: col_left) ordered.push_back({l, root});
        std::sort(ordered.begin(), ordered.end());
        std::map<size_t, size_t> col_of; // root -> column
        for (size_t c = 0; c < ordered.size(); ++c) col_of[ordered[c].second] = c;
        const size_t ncols = ordered.size();

        std::vector<std::vector<std::string> > grid(rows.size(), std::vector<std::string>(ncols));
        for (size_t i = 0; i < cells.size(); ++i) {
            auto &slot = grid[row_of[i]][col_of[ds.find(i)]];
            if (!slot.empty()) slot += ' ';
            slot += blocks[cells[i].first].lines[cells[i].second].text;
        }

        std::string md;
        for (size_t r = 0; r < grid.size(); ++r) {
            md += '|';
            for (auto &cell: grid[r]) md += ' ' + escape_md_cell(cell) + " |";
            md += '\n';
            if (r == 0) {
                md += '|';
                for (size_t c = 0; c < ncols; ++c) md += "---|";
                md += '\n';
            }
        }
        return md;
    }

    // Finds tables and replaces their lines with a block each holding the table.
    void detect_tables(std::vector<text_block> &blocks, const sc::pdf2txt::layout_options &opt, double body) {
        const double y_tol = opt.table_y_tol_em * body;
        const double x_tol = opt.table_x_tol_em * body;

        std::vector<std::pair<double, std::pair<size_t, size_t> > > flat;
        for (size_t bi = 0; bi < blocks.size(); ++bi)
            for (size_t li = 0; li < blocks[bi].lines.size(); ++li) flat.push_back({middle(blocks[bi].lines[li].box), {bi, li}});
        std::sort(flat.begin(), flat.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

        std::vector<band> bands;
        for (const auto &[y, id]: flat) {
            if (bands.empty() || y - bands.back().y > y_tol) bands.push_back({y, {}});
            bands.back().y = y; // the band's last middle
            bands.back().cells.push_back(id);
        }
        for (auto &bd: bands)
            std::sort(bd.cells.begin(), bd.cells.end(), [&](const auto &a, const auto &b) { return cell_box(blocks, a).left() < cell_box(blocks, b).left(); });

        std::vector<std::vector<size_t> > runs;
        std::vector<size_t> run;
        for (size_t i = 0; i < bands.size(); ++i) {
            const bool multi = static_cast<int>(bands[i].cells.size()) >= opt.table_min_cols;
            const bool continues = !run.empty() && multi && bands[i].y - bands[run.back()].y <= 3.0 * body &&
                                   bands_align(blocks, bands[run.back()], bands[i], x_tol, opt.table_min_cols);
            if (continues) {
                run.push_back(i);
            } else {
                if (static_cast<int>(run.size()) >= opt.table_min_rows) runs.push_back(run);
                run.clear();
                if (multi) run.push_back(i);
            }
        }
        if (static_cast<int>(run.size()) >= opt.table_min_rows) runs.push_back(run);
        if (runs.empty()) return;

        std::vector<std::vector<char> > used(blocks.size());
        for (size_t bi = 0; bi < blocks.size(); ++bi) used[bi].assign(blocks[bi].lines.size(), 0);

        std::vector<text_block> tables;
        for (const auto &r: runs) {
            std::vector<band> rows;
            rect bbox;
            bool first = true;
            for (size_t bandix: r) {
                rows.push_back(bands[bandix]);
                for (const auto &[bi, li]: bands[bandix].cells) {
                    used[bi][li] = 1;
                    if (first) {
                        bbox = blocks[bi].lines[li].box;
                        first = false;
                    } else bbox.include(blocks[bi].lines[li].box);
                }
            }
            text_block tb;
            tb.box = bbox;
            tb.is_table = true;
            tb.table_md = render_table(blocks, rows, x_tol);
            tables.push_back(std::move(tb));
        }

        // The tables' lines out, emptied blocks dropped, the tables after the rest
        std::vector<text_block> next;
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            text_block kept;
            kept.box = blocks[bi].box;
            kept.first_size = blocks[bi].first_size;
            kept.first_bold = blocks[bi].first_bold;
            for (size_t li = 0; li < blocks[bi].lines.size(); ++li)
                if (!used[bi][li]) kept.lines.push_back(std::move(blocks[bi].lines[li]));
            if (!kept.lines.empty()) next.push_back(std::move(kept));
        }
        for (auto &t: tables) next.push_back(std::move(t));
        blocks = std::move(next);
    }

    // ---------------------------------------------------------- reading order

    std::vector<size_t> reading_order(const std::vector<text_block> &blocks, const sc::pdf2txt::layout_options &opt) {
        if (blocks.empty()) return {};
        rect content = blocks.front().box;
        for (const auto &b: blocks) content.include(b.box);
        const double cw = std::max(1.0, content.width());

        std::vector<size_t> by_y(blocks.size());
        for (size_t i = 0; i < by_y.size(); ++i) by_y[i] = i;
        std::stable_sort(by_y.begin(), by_y.end(), [&](size_t a, size_t b) { return blocks[a].box.top() < blocks[b].box.top(); });

        std::vector<size_t> out;
        std::vector<size_t> segment;

        auto flush = [&] {
            if (segment.empty()) return;
            std::vector<rect> boxes;
            boxes.reserve(segment.size());
            for (size_t idx: segment) boxes.push_back(blocks[idx].box);
            auto cols = rect::group_adjacent(boxes, opt.column_gutter_min, 1e18);
            std::sort(cols.begin(), cols.end(), [](const auto &a, const auto &b) { return a.first.left() < b.first.left(); });
            for (auto &[cbox, members]: cols) {
                std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
                    const rect &ra = blocks[segment[a]].box;
                    const rect &rb = blocks[segment[b]].box;
                    return ra.top() < rb.top() || (ra.top() == rb.top() && ra.left() < rb.left());
                });
                for (size_t m: members) out.push_back(segment[m]);
            }
            segment.clear();
        };

        for (size_t idx: by_y) {
            if (blocks[idx].box.width() >= opt.spanning_fraction * cw) {
                flush();
                out.push_back(idx); // a full-width block is a section break
            } else {
                segment.push_back(idx);
            }
        }
        flush();
        return out;
    }

    // ---------------------------------------------------------------- render

    struct piece {
        std::string md; // without the anchor
        bool anchored = false;
        rect box;
    };

    piece render_block(const text_block &b, double body, const sc::pdf2txt::layout_options &opt) {
        piece p;
        p.box = b.box;
        if (b.is_table) {
            p.md = b.table_md;
            p.anchored = true;
            return p;
        }

        size_t start = 0;
        const double ratio = b.first_size / body;
        const bool small_block = b.lines.size() <= 2;
        std::string heading_prefix;
        if (ratio >= opt.heading_ratio_h1) heading_prefix = "# ";
        else if (ratio >= opt.heading_ratio_h2) heading_prefix = "## ";
        else if (b.first_bold && small_block && b.lines.front().text.size() < 120) heading_prefix = "### ";

        if (!heading_prefix.empty()) {
            p.md += heading_prefix + b.lines.front().text + "\n";
            start = 1;
            if (start >= b.lines.size()) return p; // a heading alone: no anchor
        }

        std::string flow;
        for (size_t i = start; i < b.lines.size(); ++i) {
            flow += b.lines[i].text;
            if (i + 1 < b.lines.size()) {
                const bool brk = is_list_start(b.lines[i + 1].text) || (b.box.right() - b.lines[i].box.right()) > 2.0 * body;
                flow += brk ? "\n" : " ";
            }
        }
        if (!flow.empty()) {
            p.md += flow + "\n";
            p.anchored = true;
        }
        return p;
    }

    std::vector<piece> render_page(const sc::mupdf::page &page, const sc::pdf2txt::layout_options &opt) {
        auto blocks = collect(page);
        if (blocks.empty()) return {};
        const double body = body_font_size(blocks);
        detect_tables(blocks, opt, body);
        std::vector<piece> pieces;
        for (size_t idx: reading_order(blocks, opt)) pieces.push_back(render_block(blocks[idx], body, opt));
        return pieces;
    }
}

namespace sc::pdf2txt {
    std::string page_markdown(const mupdf::page &page, const layout_options &layout, int *block_number) {
        int numbered = block_number ? *block_number : 0;
        std::string out;
        for (const auto &p: render_page(page, layout)) {
            if (p.md.empty()) continue;
            if (layout.anchors && p.anchored) out += "[b" + std::to_string(++numbered) + "] ";
            out += p.md + "\n";
        }
        if (block_number) *block_number = numbered;
        return out;
    }

    bool append_page(const mupdf::page &page, std::string &out, int &block_number, const options &options) {
        const auto full = [&] { return options.max_size && out.size() >= options.max_size; };
        if (full()) return false;
        const auto pieces = render_page(page, options.layout);
        if (pieces.empty()) return false;

        // Too little text: OCR reads the page instead
        size_t page_size = 0;
        for (const auto &p: pieces) page_size += p.md.size();
        if (page_size == 0 || page_size < options.min_page_text) return false;

        if (options.page_marks) out += "<page " + std::to_string(page.number()) + ">\n";
        for (const auto &p: pieces) {
            if (full()) break; // at a block
            if (p.md.empty()) continue;
            if (options.layout.anchors && p.anchored) out += "[b" + std::to_string(++block_number) + "] ";
            out += p.md + "\n";
        }
        return true;
    }

    std::string document_text(const mupdf::document &document, const options &options, const page_reader &ocr,
                              const progress_report &progress, figures *figures) {
        auto page_options = options;
        if (!ocr) page_options.min_page_text = 0; // nothing reads it otherwise: what there is stays
        std::string out;
        int block_number = 0;
        for (const auto page: document.pages()) {
            if (options.max_pages && page.number() > options.max_pages) break;
            if (options.max_size && out.size() >= options.max_size) break;
            if (figures) ++figures->pages;
            if (options.numbering == anchor_numbering::per_page) block_number = 0;
            if (append_page(page, out, block_number, page_options) || !ocr) continue;
            if (figures) ++figures->ocr_pages;
            const auto ocr_started = std::chrono::steady_clock::now();
            if (progress) progress("Doing OCR on page " + std::to_string(page.number()));
            sc::timer stopwatch;
            const auto read = ocr(page);
            if (options.page_marks) out += "<page " + std::to_string(page.number()) + ">\n";
            out += read;
            if (figures) figures->ocr_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - ocr_started).count();
            if (progress) progress("Done OCR on page " + std::to_string(page.number()) + " -> " + static_cast<std::string>(stopwatch));
        }
        return out;
    }

    std::string document_text(const std::filesystem::path &file, const options &options, const page_reader &ocr,
                              const progress_report &progress, figures *figures) {
        const mupdf::document document{file};
        return document_text(document, options, ocr, progress, figures);
    }
}
