// The layout ("linearizer"): it started as the one read_schedule 1.1.2 used, then in sc-mupdf (which
// no longer has it); sc-pdf2txt 1.0.0 kept it byte for byte. 1.1.0 reads tables whose rows wrap over several
// lines, leaves out what repeats in the margins of every page, tidies leader dots and spaces, and
// doesn't take "label: value" lines for headings. 1.2.0 joins text after a gap on one row and words
// broken at a line end, writes bullets as "- " and leaves out bare page numbers.
#include <sc/pdf2txt/pdf2txt.h>

#include <sc/core/timer.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <unordered_map>
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

    // Runs of 4 or more leader characters (. _ ·) as " … ", runs of spaces as one, no space at the ends.
    std::string tidy(const std::string &text) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size();) {
            const char c = text[i];
            if (c == '.' || c == '_') {
                size_t j = i;
                while (j < text.size() && text[j] == c) ++j;
                if (j - i >= 4) {
                    out += " \u2026 ";
                    i = j;
                    continue;
                }
            } else if (text.compare(i, 2, "\u00b7") == 0) { // · as UTF-8
                size_t j = i, n = 0;
                while (text.compare(j, 2, "\u00b7") == 0) j += 2, ++n;
                if (n >= 4) {
                    out += " \u2026 ";
                    i = j;
                    continue;
                }
            }
            out += c;
            ++i;
        }
        std::string squeezed;
        squeezed.reserve(out.size());
        for (const char c: out) {
            if ((c == ' ' || c == '\t') && (squeezed.empty() || squeezed.back() == ' ')) continue;
            squeezed += c == '\t' ? ' ' : c;
        }
        while (!squeezed.empty() && squeezed.back() == ' ') squeezed.pop_back();
        // only leaders: nothing
        if (squeezed == "\u2026") squeezed.clear();
        return squeezed;
    }

    std::vector<text_block> collect(const sc::mupdf::page &page, const bool tidy_text) {
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
                if (tidy_text) {
                    text = tidy(text);
                    if (text.empty()) {
                        filled_lines++; // read, but only leader dots
                        continue;
                    }
                }
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

    std::string render_table(const std::vector<text_block> &blocks, const std::vector<band> &rows, double x_tol, const bool wrapped_rows) {
        std::vector<std::pair<size_t, size_t> > cells;
        std::vector<size_t> row_of;
        for (size_t r = 0; r < rows.size(); ++r)
            for (const auto &c: rows[r].cells) {
                cells.push_back(c);
                row_of.push_back(r);
            }

        std::vector<size_t> col_of_cell(cells.size());
        size_t ncols = 0;
        if (!wrapped_rows) {
            // 1.0: columns are cells linked by x-interval proximity
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
            ncols = ordered.size();
            for (size_t i = 0; i < cells.size(); ++i) col_of_cell[i] = col_of[ds.find(i)];
        } else {
            // The columns are those of the fullest rows (the most cells): between two neighbouring columns
            // the boundary is the middle of the gap that separates them in every such row (from the
            // rightmost right edge of the one to the leftmost left edge of the next), and a cell goes to
            // the column its middle is in. So amounts right-aligned under a heading, or short words of a
            // header right-aligned over wider amounts, stay in their column, and a cell MuPDF made of
            // words from two columns on one baseline goes to one of them instead of joining them.
            size_t most = 0;
            for (const auto &r: rows) most = std::max(most, r.cells.size());
            std::vector<double> rights(most, -1e18), lefts(most, 1e18);
            for (const auto &r: rows) {
                if (r.cells.size() != most) continue;
                for (size_t c = 0; c < most; ++c) { // the band's cells are left to right
                    const auto &box = cell_box(blocks, r.cells[c]);
                    rights[c] = std::max(rights[c], box.right());
                    lefts[c] = std::min(lefts[c], box.left());
                }
            }
            std::vector<double> boundaries; // between column c and c + 1
            for (size_t c = 0; c + 1 < most; ++c) boundaries.push_back((rights[c] + lefts[c + 1]) / 2);
            ncols = most;
            for (size_t i = 0; i < cells.size(); ++i) {
                const auto &box = cell_box(blocks, cells[i]);
                const double mid = (box.left() + box.right()) / 2;
                size_t c = 0;
                while (c < boundaries.size() && mid >= boundaries[c]) ++c;
                col_of_cell[i] = c;
            }
        }

        std::vector<std::vector<std::string> > grid(rows.size(), std::vector<std::string>(ncols));
        for (size_t i = 0; i < cells.size(); ++i) {
            auto &slot = grid[row_of[i]][col_of_cell[i]];
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

        // A run: bands close together whose cells line up in columns. Bands of min_cols cells or more
        // start and carry it (lining up with the columns so far); a band with fewer cells (a row's
        // cell wrapping onto more lines, a row with empty cells) carries it on when each of its cells
        // sits in one column and none straddles two. A run is a table with min_rows full bands.
        struct columns {
            std::vector<std::pair<double, double> > spans;
            double tol = 0, straddle = 0;

            static double gap(const std::pair<double, double> &a, const rect &b) { return std::max(a.first - b.right(), b.left() - a.second); }
            static double overlap(const std::pair<double, double> &a, const rect &b) { return std::min(a.second, b.right()) - std::max(a.first, b.left()); }

            [[nodiscard]] int near(const rect &cell) const {
                int n = 0;
                for (const auto &c: spans) n += gap(c, cell) <= tol;
                return n;
            }
            [[nodiscard]] int straddled(const rect &cell) const {
                int n = 0;
                for (const auto &c: spans) n += overlap(c, cell) > straddle;
                return n;
            }
            void add(const rect &cell) {
                for (auto &c: spans)
                    if (gap(c, cell) <= tol) {
                        c.first = std::min(c.first, cell.left());
                        c.second = std::max(c.second, cell.right());
                        return;
                    }
                spans.emplace_back(cell.left(), cell.right());
            }
        };
        const auto fresh = [&] { return columns{{}, x_tol, 0.5 * body}; };

        std::vector<std::vector<size_t> > runs;
        std::vector<size_t> run;
        int full = 0; // bands of min_cols cells in the run
        auto cols = fresh();
        const auto take = [&](const size_t i) {
            run.push_back(i);
            for (const auto &c: bands[i].cells) cols.add(cell_box(blocks, c));
        };
        const auto close_run = [&] {
            // what the run ends with after its last full band is kept only when it fits (it does by
            // construction); a run of too few full bands isn't a table
            if (full >= opt.table_min_rows) runs.push_back(run);
            run.clear();
            full = 0;
            cols = fresh();
        };
        for (size_t i = 0; i < bands.size(); ++i) {
            const bool multi = static_cast<int>(bands[i].cells.size()) >= opt.table_min_cols;
            const double allowed = (opt.table_wrapped_rows && full > 0 ? opt.table_row_gap_em : 3.0) * body;
            const bool close = !run.empty() && bands[i].y - bands[run.back()].y <= allowed;
            bool continues = false;
            if (close && multi && !opt.table_wrapped_rows) {
                continues = bands_align(blocks, bands[run.back()], bands[i], x_tol, opt.table_min_cols); // 1.0's rule
            } else if (close && multi) {
                int lined_up = 0;
                bool straddles = false;
                for (const auto &c: bands[i].cells) {
                    lined_up += cols.near(cell_box(blocks, c)) > 0;
                    straddles |= cols.straddled(cell_box(blocks, c)) > 1;
                }
                continues = lined_up >= opt.table_min_cols && !straddles;
            } else if (close && opt.table_wrapped_rows && full > 0) {
                continues = std::ranges::all_of(bands[i].cells, [&](const auto &c) {
                    const auto &box = cell_box(blocks, c);
                    return cols.near(box) > 0 && cols.straddled(box) <= 1;
                });
            }
            if (continues) {
                take(i);
                full += multi;
            } else {
                close_run();
                if (multi) {
                    take(i);
                    full = 1;
                }
            }
        }
        close_run();
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
            tb.table_md = render_table(blocks, rows, x_tol, opt.table_wrapped_rows);
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

    // "Label: value": a colon after some text with a letter or digit after it ("Plan number: 4005417293",
    // "Commencement date:01/08/2026"), not a time ("10:30") or a heading ending in a colon.
    bool is_label_value(const std::string &s) {
        for (size_t p = s.find(':'); p != std::string::npos; p = s.find(':', p + 1)) {
            if (p == 0) continue;
            const bool digit_before = std::isdigit(static_cast<unsigned char>(s[p - 1])) != 0;
            const bool digit_after = p + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[p + 1])) != 0;
            if (digit_before && digit_after) continue; // 10:30
            for (size_t q = p + 1; q < s.size(); ++q)
                if (std::isalnum(static_cast<unsigned char>(s[q]))) return true;
        }
        return false;
    }

    struct piece {
        std::string md; // without the anchor
        bool anchored = false;
        rect box;
    };

    bool blank(const std::string &s) {
        return std::all_of(s.begin(), s.end(), [](const unsigned char c) { return std::isspace(c) != 0; });
    }

    // The lines of a block with those on one row joined: MuPDF gives text after a tab gap on the same
    // baseline as a line of its own ("•" and its item, "1." and its heading, "DATE:" and the date).
    // A line is on the row of the one before when their middles are within half a body size and it
    // starts to the right of it. Blank lines are left out.
    std::vector<text_line> rows_joined(const std::vector<text_line> &lines, const double body) {
        std::vector<text_line> out;
        out.reserve(lines.size());
        for (const auto &l: lines) {
            if (blank(l.text)) continue;
            if (!out.empty()) {
                auto &row = out.back();
                if (std::abs(middle(l.box) - middle(row.box)) <= 0.5 * body && l.box.left() >= row.box.right() - 0.25 * body) {
                    row.text += ' ';
                    row.text += l.text;
                    row.box.include(l.box);
                    continue;
                }
            }
            out.push_back(l);
        }
        return out;
    }

    // A list marker (•, ◦, ▪) at the start as markdown's "- "
    std::string markdown_bullet(std::string s) {
        for (const std::string_view b: {std::string_view{"•"}, std::string_view{"◦"}, std::string_view{"▪"}}) {
            if (s.starts_with(b)) {
                size_t rest = b.size();
                while (rest < s.size() && s[rest] == ' ') ++rest;
                return "- " + s.substr(rest);
            }
        }
        return s;
    }

    // A word broken at the end of a line ("low-" then "interest"): the line ends in a letter and a
    // hyphen and the next starts with a lowercase letter, so they join without a space ("low-interest")
    bool broken_word(const std::string &line, const std::string &next) {
        return line.size() >= 2 && line.back() == '-' && std::isalpha(static_cast<unsigned char>(line[line.size() - 2])) != 0 && !next.empty() &&
               std::islower(static_cast<unsigned char>(next.front())) != 0;
    }

    piece render_block(const text_block &block, double body, const sc::pdf2txt::layout_options &opt) {
        piece p;
        p.box = block.box;
        if (block.is_table) {
            p.md = block.table_md;
            p.anchored = true;
            return p;
        }
        const auto lines = rows_joined(block.lines, body);
        if (lines.empty()) return p;

        size_t start = 0;
        const double ratio = block.first_size / body;
        const bool small_block = lines.size() <= 2;
        std::string heading_prefix;
        if (opt.label_headings || !is_label_value(lines.front().text)) { // "Plan number: 4005417293" is a field
            if (ratio >= opt.heading_ratio_h1) heading_prefix = "# ";
            else if (ratio >= opt.heading_ratio_h2) heading_prefix = "## ";
            else if (block.first_bold && small_block && lines.front().text.size() < 120) heading_prefix = "### ";
        }

        if (!heading_prefix.empty()) {
            p.md += heading_prefix + lines.front().text + "\n";
            start = 1;
            if (start >= lines.size()) return p; // a heading alone: no anchor
        }

        std::string flow;
        for (size_t i = start; i < lines.size(); ++i) {
            flow += markdown_bullet(lines[i].text);
            if (i + 1 < lines.size()) {
                if (broken_word(lines[i].text, lines[i + 1].text)) continue;
                const bool brk = is_list_start(lines[i + 1].text) || (block.box.right() - lines[i].box.right()) > 2.0 * body;
                flow += brk ? "\n" : " ";
            }
        }
        if (!flow.empty()) {
            p.md += flow + "\n";
            p.anchored = true;
        }
        return p;
    }

    std::vector<piece> render_blocks(std::vector<text_block> blocks, const sc::pdf2txt::layout_options &opt) {
        if (blocks.empty()) return {};
        const double body = body_font_size(blocks);
        detect_tables(blocks, opt, body);
        std::vector<piece> pieces;
        for (size_t idx: reading_order(blocks, opt)) pieces.push_back(render_block(blocks[idx], body, opt));
        return pieces;
    }

    std::vector<piece> render_page(const sc::mupdf::page &page, const sc::pdf2txt::layout_options &opt) {
        return render_blocks(collect(page, opt.tidy_text), opt);
    }

    // ------------------------------------------------------ repeated margins
    //
    // What a document prints at the top or bottom of every page (its name, the policy number, "Page 3
    // of 12", the insurer's registration paragraph): a line in the top or bottom tenth of a page, by
    // its text with the digits taken out, found on 3 pages or more and on 40% of the pages with text.
    // It's kept where it's first seen and left out after that.

    std::string margin_key(const std::string &text) {
        std::string key;
        key.reserve(text.size());
        for (const char c: text) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                if (key.empty() || key.back() != '#') key += '#';
            } else if (c != ' ') {
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return key;
    }

    bool in_margin(const rect &line, const rect &page) {
        const double zone = 0.1 * page.height();
        return line.top() < page.top() + zone || line.bottom() > page.bottom() - zone;
    }

    // A bare page number's key ("2", "2/9", "- 2 -", "Page 2"); it counts only for a line alone in its
    // block, so a short number in a table or a paragraph near the edge is never taken for one
    bool page_number_key(const std::string &key) { return key == "#" || key == "#/#" || key == "-#-" || key == "page#"; }

    // The key a margin line counts under, or "" when it's too short to count
    std::string counted_key(const text_line &line, const text_block &block) {
        auto key = margin_key(line.text);
        if (page_number_key(key)) return block.lines.size() == 1 ? key : std::string{};
        return key.size() >= 3 ? key : std::string{};
    }

    // The keys of the page's margin lines (each once)
    std::set<std::string> margin_keys(const std::vector<text_block> &blocks, const rect &page) {
        std::set<std::string> keys;
        for (const auto &b: blocks)
            for (const auto &l: b.lines)
                if (in_margin(l.box, page)) {
                    auto key = counted_key(l, b);
                    if (!key.empty()) keys.insert(std::move(key));
                }
        return keys;
    }

    // The blocks without the repeated margin lines already seen (seen gets the page's)
    std::vector<text_block> without_repeats(std::vector<text_block> blocks, const rect &page, const std::set<std::string> &repeated,
                                            std::set<std::string> &seen, int &left_out) {
        if (repeated.empty()) return blocks;
        std::set<std::string> here;
        std::vector<text_block> out;
        for (auto &b: blocks) {
            text_block kept = b;
            kept.lines.clear();
            for (auto &l: b.lines) {
                if (in_margin(l.box, page)) {
                    auto key = counted_key(l, b);
                    if (!key.empty() && repeated.contains(key)) {
                        if (seen.contains(key) || page_number_key(key)) { // a bare page number says nothing even once
                            ++left_out;
                            continue;
                        }
                        here.insert(std::move(key));
                    }
                }
                kept.lines.push_back(std::move(l));
            }
            if (kept.lines.empty()) continue;
            if (kept.lines.size() != b.lines.size()) { // its first line may be gone
                kept.first_size = kept.lines.front().size;
                kept.box = kept.lines.front().box;
                for (const auto &l: kept.lines) kept.box.include(l.box);
            }
            out.push_back(std::move(kept));
        }
        seen.insert(here.begin(), here.end());
        return out;
    }

    size_t markdown_size(const std::vector<piece> &pieces) {
        size_t n = 0;
        for (const auto &p: pieces) n += p.md.size();
        return n;
    }

    // What a document has given so far, for leaving out what it repeats: its pages' and its long
    // blocks' markdown, by page number.
    struct given {
        std::unordered_map<std::string, int> pages, blocks;
        int repeated_blocks = 0, repeated_pages = 0;
    };

    // Adds a page's pieces (pieces_full decides whether it's read at all: what repeats in the margins
    // doesn't send a page to OCR); false when it needs OCR. With so_far, a page the same as an earlier
    // one is a line saying so, and a block of 80 bytes or more given before is left out.
    bool append_pieces(const std::vector<piece> &pieces_full, const std::vector<piece> &pieces, const int page_number, std::string &out,
                       int &block_number, const sc::pdf2txt::options &options, given *so_far = nullptr) {
        const auto full = [&] { return options.max_size && out.size() >= options.max_size; };
        if (full()) return false;
        if (pieces_full.empty()) return false;
        const auto page_size = markdown_size(pieces_full);
        if (page_size == 0 || page_size < options.min_page_text) return false;
        if (options.max_junk_share < 1) {
            std::string whole;
            for (const auto &p: pieces_full) whole += p.md;
            if (sc::pdf2txt::junk_share(whole) > options.max_junk_share) return false;
        }
        if (options.page_marks) out += "<page " + std::to_string(page_number) + ">\n";
        if (so_far) {
            std::string whole;
            for (const auto &p: pieces) whole += p.md;
            if (whole.size() >= 80) {
                const auto [at, fresh] = so_far->pages.try_emplace(whole, page_number);
                if (!fresh) {
                    ++so_far->repeated_pages;
                    out += "(the same text as page " + std::to_string(at->second) + ")\n\n";
                    return true;
                }
            }
        }
        for (const auto &p: pieces) {
            if (full()) break; // at a block
            if (p.md.empty()) continue;
            if (so_far && p.md.size() >= 80 && !so_far->blocks.try_emplace(p.md, page_number).second) {
                ++so_far->repeated_blocks;
                continue;
            }
            if (options.layout.anchors && p.anchored) out += "[b" + std::to_string(++block_number) + "] ";
            out += p.md + "\n";
        }
        return true;
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

    double junk_share(const std::string_view text) {
        std::size_t characters = 0, junk = 0;
        for (std::size_t i = 0; i < text.size();) {
            const auto c = static_cast<unsigned char>(text[i]);
            char32_t code = c;
            std::size_t length = 1;
            if (c >= 0xF0 && i + 3 < text.size()) {
                code = ((c & 0x07u) << 18) | ((text[i + 1] & 0x3Fu) << 12) | ((text[i + 2] & 0x3Fu) << 6) | (text[i + 3] & 0x3Fu);
                length = 4;
            } else if (c >= 0xE0 && i + 2 < text.size()) {
                code = ((c & 0x0Fu) << 12) | ((text[i + 1] & 0x3Fu) << 6) | (text[i + 2] & 0x3Fu);
                length = 3;
            } else if (c >= 0xC0 && i + 1 < text.size()) {
                code = ((c & 0x1Fu) << 6) | (text[i + 1] & 0x3Fu);
                length = 2;
            } else if (c >= 0x80) {
                code = 0xFFFD; // a stray continuation byte
            }
            i += length;
            if (code == ' ' || code == '\t' || code == '\n' || code == '\r') continue;
            ++characters;
            const bool text_like = (code >= 0x21 && code < 0x7F)          // ASCII
                                   || (code >= 0xA0 && code <= 0x24F)     // Latin-1 and Latin Extended: ë, é, ß, €'s neighbours
                                   || (code >= 0x2000 && code <= 0x20CF)  // general punctuation and currency: – ’ “ • €
                                   || (code >= 0x2190 && code <= 0x25FF); // arrows, box drawing, shapes (bullets, ticks)
            if (!text_like) ++junk;
        }
        return characters ? static_cast<double>(junk) / static_cast<double>(characters) : 0.0;
    }

    bool append_page(const mupdf::page &page, std::string &out, int &block_number, const options &options) {
        if (options.max_size && out.size() >= options.max_size) return false;
        const auto pieces = render_page(page, options.layout);
        return append_pieces(pieces, pieces, page.number(), out, block_number, options);
    }

    std::string document_text(const mupdf::document &document, const options &options, const page_reader &ocr,
                              const progress_report &progress, figures *figures) {
        auto page_options = options;
        if (!ocr) page_options.min_page_text = 0; // nothing reads it otherwise: what there is stays

        // The pages' text read once: the first pages' (up to 40) ahead, to find what repeats in the margins
        const int last = options.max_pages ? std::min(options.max_pages, document.page_count()) : document.page_count();
        const int ahead = options.repeated_margins ? std::min(last, 40) : 0;
        std::vector<std::vector<text_block> > read(static_cast<size_t>(ahead));
        std::set<std::string> repeated;
        if (ahead) {
            std::unordered_map<std::string, int> pages_with;
            int with_text = 0;
            for (int i = 0; i < ahead; ++i) {
                const auto page = document.page(i);
                read[static_cast<size_t>(i)] = collect(page, options.layout.tidy_text);
                if (read[static_cast<size_t>(i)].empty()) continue;
                ++with_text;
                for (const auto &key: margin_keys(read[static_cast<size_t>(i)], page.bounds())) ++pages_with[key];
            }
            for (const auto &[key, n]: pages_with)
                if (n >= 3 && n * 10 >= with_text * 4) repeated.insert(key);
        }

        std::string out;
        int block_number = 0;
        std::set<std::string> seen;
        given so_far;
        for (int i = 0; i < last; ++i) {
            if (options.max_size && out.size() >= options.max_size) break;
            const auto page = document.page(i);
            if (figures) ++figures->pages;
            if (options.numbering == anchor_numbering::per_page) block_number = 0;
            auto blocks = i < ahead ? std::move(read[static_cast<size_t>(i)]) : collect(page, options.layout.tidy_text);
            const auto pieces_full = render_blocks(blocks, options.layout);
            int left_out = 0;
            const auto pieces = repeated.empty() ? pieces_full
                                                 : render_blocks(without_repeats(std::move(blocks), page.bounds(), repeated, seen, left_out), options.layout);
            if (append_pieces(pieces_full, pieces, page.number(), out, block_number, page_options, options.repeated_text ? &so_far : nullptr)) {
                if (figures) figures->repeated_lines += left_out;
                continue;
            }
            if (!ocr) continue;
            if (figures) ++figures->ocr_pages;
            const auto ocr_started = std::chrono::steady_clock::now();
            if (progress) progress("Doing OCR on page " + std::to_string(page.number()));
            sc::timer stopwatch;
            const auto text = ocr(page);
            if (options.page_marks) out += "<page " + std::to_string(page.number()) + ">\n";
            out += text;
            if (figures) figures->ocr_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - ocr_started).count();
            if (progress) progress("Done OCR on page " + std::to_string(page.number()) + " -> " + static_cast<std::string>(stopwatch));
        }
        if (figures) {
            figures->repeated_blocks = so_far.repeated_blocks;
            figures->repeated_pages = so_far.repeated_pages;
        }
        return out;
    }

    std::string document_text(const std::filesystem::path &file, const options &options, const page_reader &ocr,
                              const progress_report &progress, figures *figures) {
        const mupdf::document document{file};
        return document_text(document, options, ocr, progress, figures);
    }
}
