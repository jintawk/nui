// test_build.cpp - the layer build path of nui 1.2.0 (parse in place, quad colour fast paths, batch
// merge, reserve) against the reference: the same library compiled with NUI_TEST_REFERENCE, which keeps
// the code of before 1.2.0 (the old parser, every quad colour lerped, no merge). build_build_test.sh
// builds this file twice:
//
//   test_build_ref.exe dump  <install> <fixtures> <ref.bin>   reference library: writes every record
//   test_build.exe     check <install> <fixtures> <ref.bin>   library under test: compares every record,
//                                                             then the checks only the new code has
//   test_build*.exe    bench <install> <fixtures>             us per layer (median), batches
//
// Records (both builds make them in the same order from the same inputs):
//   parse    every real layer text (fixtures/layers: five captured HUD states) and 4,000 fuzzed
//            texts: ok, err, every Cmd field bit for bit
//   quad     random quads straight into DrawList::quad (clip stacks, offsets, one-colour and gradient
//            corners, fractional / huge / inf / NaN coordinates): every vertex and batch byte
//   draw     every real layer through ops_draw (no merge), unclipped and under six clips that cut
//            window bodies, gradients, text and gauges: every vertex and batch byte
// Checks of the new code alone (check mode):
//   merge    on every draw record and on 3,000 random lists: the merged list is a permutation of the
//            quads with their states, every pair of overlapping quads keeps its order, batches tile the
//            list, no two neighbours share a state, never more batches; equal byte for byte to the
//            phase-1 prototype (native agent's merge_list, verbatim below); ops_replay = ops_draw + merge
//   num      the integer fast path against float(atof) for every integer -9,999,999..9,999,999, "-0",
//            leading zeros; the colour fast path against strtoul on 2,000,000 random 8-digit colours
//   1.2.6    the code review's fixes in pure functions, before the records (review_checks): decode sig
//            pic read clamp title recipe assets handles
// A failure prints "FAIL [tag] ..." (tags: parse quad draw merge num and the 1.2.6 ones) and the exit code is 1.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "assets.hpp"
#include "cache.hpp"
#include "client.hpp"
#include "ops.hpp"
#include "textures.hpp"
#include "ui.hpp"

#include "png_write.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace nui;

namespace {

int g_fail = 0;
void fail(char const* tag, std::string const& what) {
    if (++g_fail <= 40) std::printf("FAIL [%s] %s\n", tag, what.c_str());
}

// ---- deterministic random ---------------------------------------------------------------------------
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return std::uint32_t(s >> 11);
    }
    int in(int lo, int hi) { return lo + int(next() % std::uint32_t(hi - lo + 1)); }
    bool p(int pct) { return int(next() % 100) < pct; }
};

// ---- serialisation ----------------------------------------------------------------------------------
void put(std::string& o, void const* p, std::size_t n) { o.append(static_cast<char const*>(p), n); }
template <typename T>
void putv(std::string& o, T const& v) { put(o, &v, sizeof v); }

std::string ser_cmds(bool ok, std::string const& err, std::vector<Cmd> const& cmds) {
    std::string o;
    putv(o, ok);
    putv(o, std::uint32_t(err.size()));
    o += err;
    putv(o, std::uint32_t(cmds.size()));
    for (Cmd const& c : cmds) {
        putv(o, c.k);
        put(o, c.f, sizeof c.f);
        putv(o, c.col);
        putv(o, std::uint32_t(c.s.size()));
        o += c.s;
    }
    return o;
}

std::string ser_dl(DrawList const& dl) {
    std::string o;
    putv(o, std::uint32_t(dl.v.size()));
    if (!dl.v.empty()) put(o, dl.v.data(), dl.v.size() * sizeof(Vtx));
    putv(o, std::uint32_t(dl.batches.size()));
    for (Batch const& b : dl.batches) {
        putv(o, b.tex); putv(o, b.filter); putv(o, b.blend); putv(o, b.first); putv(o, b.count);
    }
    return o;
}

// ---- the record stream: written by the reference, compared by the build under test --------------------
struct Stream {
    bool writing = false;
    std::FILE* f = nullptr;
    long records = 0, compared = 0;
    void rec(char const* tag, std::string const& name, std::string const& bytes) {
        ++records;
        if (writing) {
            std::uint32_t n = std::uint32_t(name.size()), b = std::uint32_t(bytes.size());
            std::fwrite(&n, 4, 1, f); std::fwrite(name.data(), 1, n, f);
            std::fwrite(&b, 4, 1, f); std::fwrite(bytes.data(), 1, b, f);
            return;
        }
        std::uint32_t n = 0, b = 0;
        std::string rn, rb;
        if (std::fread(&n, 4, 1, f) != 1) { fail(tag, name + ": reference stream ended"); return; }
        rn.resize(n);
        if (n && std::fread(&rn[0], 1, n, f) != n) { fail(tag, name + ": reference stream short"); return; }
        if (std::fread(&b, 4, 1, f) != 1) { fail(tag, name + ": reference stream short"); return; }
        rb.resize(b);
        if (b && std::fread(&rb[0], 1, b, f) != b) { fail(tag, name + ": reference stream short"); return; }
        ++compared;
        if (rn != name) { fail(tag, "record order: got " + name + ", reference has " + rn); return; }
        if (rb != bytes) {
            std::size_t at = 0;
            while (at < rb.size() && at < bytes.size() && rb[at] == bytes[at]) ++at;
            char m[160];
            std::snprintf(m, sizeof m, "%s differs from the reference (%u vs %u bytes, first difference at byte %u)",
                          name.c_str(), unsigned(bytes.size()), unsigned(rb.size()), unsigned(at));
            fail(tag, m);
        }
    }
};

// ---- fixtures -----------------------------------------------------------------------------------------
struct FixLayer {
    int id = 0, z = 0;
    float ox = 0, oy = 0;
    bool clip = false;
    Rect c {0, 0, 0, 0};
    std::string text;
};
struct FixState {
    std::string name;
    std::vector<FixLayer> layers;
};

std::vector<FixState> load_fixtures(std::string const& dir) {
    std::vector<FixState> out;
    for (char const* name : {"still", "busy", "flashing", "next", "soak"}) {
        std::ifstream fi(dir + "\\layers\\" + name + ".layers", std::ios::binary);
        if (!fi) { fail("parse", std::string("missing fixture ") + name); continue; }
        std::stringstream ss;
        ss << fi.rdbuf();
        std::string const all = ss.str();
        FixState st;
        st.name = name;
        std::size_t pos = all.find('\n') + 1;              // SCREEN line
        while (pos < all.size()) {
            std::size_t const nl = all.find('\n', pos);
            std::string const hdr = all.substr(pos, nl - pos);
            FixLayer L;
            int clip = 0;
            unsigned n = 0;
            if (std::sscanf(hdr.c_str(), "LAYER %d %d %f %f %d %f %f %f %f %u", &L.id, &L.z, &L.ox, &L.oy, &clip, &L.c.x0, &L.c.y0,
                            &L.c.x1, &L.c.y1, &n) != 10) { fail("parse", "bad fixture header " + hdr); break; }
            L.clip = clip != 0;
            L.text = all.substr(nl + 1, n);
            pos = nl + 1 + n + 1;
            st.layers.push_back(L);
        }
        out.push_back(st);
    }
    return out;
}

// ---- inputs -------------------------------------------------------------------------------------------
std::string rnum(Rng& r) {
    static char const* const odd[] = {"", " ", "  7", "7 ", "-0", "+5", "-", "1e3", "-2.5e-1", "0x10", "inf", "-inf", "nan",
                                      "1.5", "-12.75", "007", "-0000012", "99999999", "12345678", "3.4e38", "1e39", "\r",
                                      " \r", ".5", "5.", "1,5", "abc", "12abc", "\x01", "  -0", "2", "2.0", "1"};
    int const k = r.in(0, 9);
    char b[64];
    if (k < 4) { std::snprintf(b, sizeof b, "%d", r.in(-4000, 4000)); return b; }
    if (k < 6) { std::snprintf(b, sizeof b, "%.*f", r.in(1, 4), (int(r.next() % 800000) - 400000) / 97.0); return b; }
    if (k < 7) { std::snprintf(b, sizeof b, "%d", int(r.next()) >> r.in(0, 31)); return b; }
    if (k < 8) { std::string s(std::size_t(r.in(60, 90)), '1'); return s; }    // longer than the stack buffer
    return odd[r.next() % (sizeof odd / sizeof *odd)];
}
std::string rcol(Rng& r) {
    static char const* const odd[] = {"", "FFF", "ffffffff", "FFFFFFFFF", "0x12345678", "-1", " 12345678", "1234567g", "AbCdEf01",
                                      "80808080 ", "00000000", "7fffffff", "+0000FFFF"};
    if (r.p(70)) {
        char b[16];
        std::snprintf(b, sizeof b, r.p(50) ? "%08X" : "%08x", unsigned(r.next() ^ (r.next() << 16)));
        return b;
    }
    return odd[r.next() % (sizeof odd / sizeof *odd)];
}
std::string rtext(Rng& r) {
    std::string s;
    int const n = r.in(0, 14);
    for (int i = 0; i < n; ++i) {
        int const k = r.in(0, 9);
        s += k < 7 ? char(r.in(0x20, 0x7E)) : k == 7 ? ' ' : k == 8 ? char(r.in(0x80, 0xFF)) : char(r.in(1, 31) == 9 ? 1 : r.in(1, 31));
    }
    if (r.p(5)) s += std::string(1, '\0') + "after-nul";
    return s;
}
std::string rline(Rng& r) {
    static char const kinds[] = "WTHRQIGCc";
    char const k = r.p(97) ? kinds[r.next() % 9] : char(r.in(0x21, 0x7E));
    std::vector<std::string> f;
    f.push_back(std::string(1, k) + (r.p(3) ? "x" : ""));
    auto nums = [&](int n) { for (int i = 0; i < n; ++i) f.push_back(rnum(r)); };
    switch (k) {
    case 'W': nums(4); f.push_back(rtext(r)); if (r.p(50)) f.push_back(r.p(50) ? "1" : rnum(r)); break;
    case 'T': nums(2); f.push_back(rcol(r)); f.push_back(rtext(r)); break;
    case 'H': nums(3); f.push_back(rcol(r)); f.push_back(rtext(r)); break;
    case 'R': nums(4); f.push_back(rcol(r)); break;
    case 'Q': f.push_back(r.p(80) ? "0" : rnum(r)); nums(8); f.push_back(rcol(r)); f.push_back(r.p(70) ? std::to_string(r.in(0, 7)) : rnum(r)); break;
    case 'I': nums(3); f.push_back(r.p(70) ? std::to_string(r.in(4096, 4200)) : rnum(r)); f.push_back(rcol(r)); if (r.p(50)) f.push_back(r.p(60) ? "2" : rnum(r)); break;
    case 'G': nums(4); f.push_back(rnum(r)); f.push_back(rcol(r)); break;
    case 'C': nums(4); break;
    default: break;
    }
    if (r.p(4) && f.size() > 1) f.pop_back();                 // too few fields
    if (r.p(4)) f.push_back(rnum(r));                          // an extra field
    if (r.p(2)) f[0].clear();                                  // a line starting with a tab
    std::string s;
    for (std::size_t i = 0; i < f.size(); ++i) s += (i ? "\t" : "") + f[i];
    if (r.p(3)) s = "#" + s;
    if (r.p(5)) s += "\r";
    return s;
}
std::string rdoc(Rng& r) {
    std::string s;
    int const n = r.in(0, 30);
    for (int i = 0; i < n; ++i) {
        if (r.p(5)) s += r.p(50) ? "" : "\r";                  // empty / CR-only line
        else s += rline(r);
        if (i + 1 < n || r.p(50)) s += "\n";
    }
    return s;
}
std::string mutate(Rng& r, std::string s) {
    int const n = r.in(1, 8);
    for (int i = 0; i < n && !s.empty(); ++i) {
        std::size_t const at = r.next() % s.size();
        switch (r.in(0, 4)) {
        case 0: s[at] = char(r.in(0, 255)); break;
        case 1: s.insert(at, 1, '\t'); break;
        case 2: s.erase(at, 1); break;
        case 3: s.insert(at, "\r\n"); break;
        default: s[at] = ' '; break;
        }
    }
    return s;
}
std::vector<std::string> fuzz_docs() {
    Rng r(12345);
    std::vector<std::string> d;
    d.push_back("");
    d.push_back("\n");
    d.push_back("\r\n");
    d.push_back("c");
    d.push_back("#only a comment");
    d.push_back("R\t1\t2\t3\t4\tFFFFFFFF");
    d.push_back("R\t1\t2\t3\t4\tFFFFFFFF\n");
    d.push_back("R\t1\t2\t3\t4\tFFFFFFFF\r\n\r\nR\t\t\t \t4\t");
    d.push_back(std::string("T\t1\t2\tFFFFFFFF\tab\0cd\n", 22));
    for (int i = 0; i < 4000; ++i) d.push_back(rdoc(r));
    return d;
}

// ---- reference merge: the phase-1 prototype (p1_native, native/src/d3d8_backend.cpp merge_list), verbatim
void proto_merge(std::vector<Vtx>& v, std::vector<Batch> const& in, std::vector<Batch>& out) {
    struct Q { float x0, y0, x1, y1; uint32_t at; };
    struct B { Batch b; std::vector<Q> q; float x0, y0, x1, y1; };
    std::vector<B> bs;
    for (Batch const& b : in) {
        for (uint32_t i = b.first; i < b.first + b.count; i += 6) {
            Q q {v[i].x, v[i].y, v[i].x, v[i].y, i};
            for (int k = 1; k < 6; ++k) {
                q.x0 = std::min(q.x0, v[i + k].x); q.x1 = std::max(q.x1, v[i + k].x);
                q.y0 = std::min(q.y0, v[i + k].y); q.y1 = std::max(q.y1, v[i + k].y);
            }
            int last_ov = -1;
            for (int j = int(bs.size()) - 1; j >= 0 && last_ov < 0; --j) {
                B const& c = bs[j];
                if (!(q.x0 < c.x1 && c.x0 < q.x1 && q.y0 < c.y1 && c.y0 < q.y1)) continue;
                for (Q const& o : c.q)
                    if (q.x0 < o.x1 && o.x0 < q.x1 && q.y0 < o.y1 && o.y0 < q.y1) { last_ov = j; break; }
            }
            int dest = -1;
            for (int j = int(bs.size()) - 1; j >= std::max(last_ov, 0); --j)
                if (bs[j].b.tex == b.tex && bs[j].b.filter == b.filter && bs[j].b.blend == b.blend) { dest = j; break; }
            if (dest < 0) {
                bs.push_back(B {Batch {b.tex, b.filter, b.blend, 0, 0}, {}, q.x0, q.y0, q.x1, q.y1});
                dest = int(bs.size()) - 1;
            }
            B& d = bs[dest];
            d.q.push_back(q);
            d.x0 = std::min(d.x0, q.x0); d.y0 = std::min(d.y0, q.y0); d.x1 = std::max(d.x1, q.x1); d.y1 = std::max(d.y1, q.y1);
        }
    }
    std::vector<Vtx> nv;
    nv.reserve(v.size());
    out.clear();
    for (B const& b : bs) {
        Batch nb = b.b;
        nb.first = uint32_t(nv.size());
        for (Q const& q : b.q) for (int k = 0; k < 6; ++k) nv.push_back(v[q.at + k]);
        nb.count = uint32_t(nv.size()) - nb.first;
        out.push_back(nb);
    }
    v.swap(nv);
}

// ---- the merge's invariants ---------------------------------------------------------------------------
struct QBox { float x0, y0, x1, y1; };
QBox qbox(std::vector<Vtx> const& v, std::size_t q) {
    QBox b {v[q * 6].x, v[q * 6].y, v[q * 6].x, v[q * 6].y};
    for (std::size_t k = q * 6 + 1; k < q * 6 + 6; ++k) {
        b.x0 = std::min(b.x0, v[k].x); b.x1 = std::max(b.x1, v[k].x);
        b.y0 = std::min(b.y0, v[k].y); b.y1 = std::max(b.y1, v[k].y);
    }
    return b;
}
bool tiles(DrawList const& d) {
    uint32_t at = 0;
    for (Batch const& b : d.batches) {
        if (b.first != at || b.count % 6) return false;
        at += b.count;
    }
    return at == d.v.size();
}
std::vector<std::string> blocks(DrawList const& d) {      // per quad: its state and its six vertices
    std::vector<std::string> o(d.v.size() / 6);
    for (Batch const& b : d.batches)
        for (uint32_t i = b.first; i < b.first + b.count; i += 6) {
            std::string& s = o[i / 6];
            putv(s, b.tex); putv(s, b.filter); putv(s, b.blend);
            put(s, &d.v[i], 6 * sizeof(Vtx));
        }
    return o;
}
// returns false (and says why) unless m is a valid merge of raw
bool check_merge(DrawList const& raw, DrawList const& m, std::string const& name) {
    if (!tiles(raw)) return true;                          // merge_batches leaves these alone (checked separately)
    if (!tiles(m) || m.v.size() != raw.v.size()) { fail("merge", name + ": merged batches do not tile the list"); return false; }
    if (m.batches.size() > raw.batches.size()) { fail("merge", name + ": more batches after the merge"); return false; }
    for (std::size_t i = 1; i < m.batches.size(); ++i) {
        Batch const& a = m.batches[i - 1];
        Batch const& b = m.batches[i];
        if (a.tex == b.tex && a.filter == b.filter && a.blend == b.blend) {
            fail("merge", name + ": two neighbouring batches with one state"); return false;
        }
    }
    std::vector<std::string> const rb = blocks(raw), mb = blocks(m);
    std::map<std::string, std::deque<int>> where;
    for (std::size_t i = 0; i < rb.size(); ++i) where[rb[i]].push_back(int(i));
    std::vector<int> pos(rb.size(), -1);
    for (std::size_t j = 0; j < mb.size(); ++j) {
        auto it = where.find(mb[j]);
        if (it == where.end() || it->second.empty()) { fail("merge", name + ": a merged quad (with its state) is not in the list"); return false; }
        pos[std::size_t(it->second.front())] = int(j);
        it->second.pop_front();
    }
    std::vector<QBox> bx(rb.size());
    for (std::size_t i = 0; i < rb.size(); ++i) bx[i] = qbox(raw.v, i);
    for (std::size_t i = 0; i < rb.size(); ++i)
        for (std::size_t j = i + 1; j < rb.size(); ++j) {
            QBox const& a = bx[i];
            QBox const& b = bx[j];
            if (a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1 && pos[i] > pos[j] && rb[i] != rb[j]) {
                char w[200];
                std::snprintf(w, sizeof w, "%s: quads %u and %u overlap ([%g,%g]x[%g,%g] / [%g,%g]x[%g,%g]) and swapped order",
                              name.c_str(), unsigned(i), unsigned(j), a.x0, a.x1, a.y0, a.y1, b.x0, b.x1, b.y0, b.y1);
                fail("merge", w);
                return false;
            }
        }
    for (std::size_t i = 0; i < rb.size(); ++i)
        if (!(bx[i].x0 <= bx[i].x1 && bx[i].y0 <= bx[i].y1)) {          // a NaN coordinate: left in order
            if (ser_dl(m) != ser_dl(raw)) { fail("merge", name + ": a list with a NaN coordinate was reordered"); return false; }
            return true;
        }
    DrawList p = raw;
    proto_merge(p.v, raw.batches, p.batches);
    if (ser_dl(p) != ser_dl(m)) { fail("merge", name + ": differs from the phase-1 prototype merge"); return false; }
    return true;
}

// ---- builds -------------------------------------------------------------------------------------------
Assets* g_assets = nullptr;
TextureStore g_store;

void build(FixLayer const& L, Rect const* clip, DrawList& out, bool merge) {
    Rect const c = clip ? *clip : L.c;
    bool const has = clip || L.clip;
    out.clear(has ? Rect {c.x0 + L.ox, c.y0 + L.oy, c.x1 + L.ox, c.y1 + L.oy} : Rect {-1e9f, -1e9f, 1e9f, 1e9f});
    out.ox = L.ox;
    out.oy = L.oy;
    std::vector<Cmd> cmds;
    std::string err;
    ops_parse(L.text.data(), L.text.size(), cmds, err);
    Ui ui {*g_assets, out};
    if (merge) ops_replay(ui, cmds, &g_store, nullptr);
    else ops_draw(ui, cmds, &g_store, nullptr);
}

Rect const kClips[] = {{3, 5, 150.5f, 60.25f}, {40, 17.5f, 333, 200}, {0, 100, 2000, 101.5f},
                       {57.3f, -10, 58.9f, 400}, {-20, 9.75f, 5000, 30.125f}, {200.5f, 0.5f, 260.25f, 390.75f}};

void random_quads(Rng& r, DrawList& dl, int n) {
    dl.clear(r.p(70) ? Rect {-1e9f, -1e9f, 1e9f, 1e9f} : Rect {float(r.in(-50, 50)), float(r.in(-50, 50)), float(r.in(100, 600)), float(r.in(100, 600))});
    auto coord = [&]() -> float {
        int const k = r.in(0, 99);
        if (k < 50) return float(r.in(-40, 700));
        if (k < 80) return float(r.in(-400, 7000)) / 8.0f;
        if (k < 95) return (int(r.next() % 2000000) - 1000000) / 1777.0f;
        if (k < 97) return r.p(50) ? 3e38f : -3e38f;
        if (k < 99) return r.p(50) ? INFINITY : -INFINITY;
        return NAN;
    };
    for (int i = 0; i < n; ++i) {
        int const k = r.in(0, 99);
        if (k < 6) { dl.push_clip({coord(), coord(), coord(), coord()}); continue; }
        if (k < 10) { dl.pop_clip(); continue; }
        if (k < 13) { dl.ox = float(r.in(-30, 30)) / (r.p(50) ? 1 : 4); dl.oy = float(r.in(-30, 30)) / (r.p(50) ? 1 : 3); continue; }
        if (k < 18) { dl.blend = r.in(0, 1); continue; }
        uint32_t c[4];
        int const kind = r.in(0, 3);
        c[0] = r.next() ^ (r.next() << 7);
        for (int j = 1; j < 4; ++j) c[j] = kind == 0 ? c[0] : kind == 1 && j == 1 ? c[0] : kind == 2 && j == 2 ? c[0] : r.next() ^ (r.next() << 9);
        float const x0 = coord(), y0 = coord();
        float const x1 = r.p(85) ? x0 + float(r.in(1, 300)) / (r.p(60) ? 1 : 7) : coord();
        float const y1 = r.p(85) ? y0 + float(r.in(1, 300)) / (r.p(60) ? 1 : 7) : coord();
        dl.quad(r.in(0, 3), r.in(0, 3), x0, y0, x1, y1, coord(), coord(), coord(), coord(), c);
    }
}

// a random list for the merge alone: few states, heavy overlap, edges that touch
void random_merge_list(Rng& r, DrawList& dl) {
    dl.clear();
    int const n = r.in(1, 400);
    int const span = r.in(20, 600);
    int const states = r.in(1, 6);
    for (int i = 0; i < n; ++i) {
        int const s = r.in(0, states - 1);
        dl.blend = s & 1;
        float const x0 = float(r.in(0, span)) / (r.p(80) ? 1 : 2), y0 = float(r.in(0, span / 3 + 1)) / (r.p(80) ? 1 : 2);
        float const w = float(r.in(1, 64)), h = float(r.in(1, 24));
        dl.quad(s >> 1, r.p(90) ? 0 : 2, x0, y0, x0 + w, y0 + h, 0, 0, w, h, 0xFFFFFFFFu);
    }
}

double qpc_us() {
    static LARGE_INTEGER f {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1e6 / double(f.QuadPart);
}

// ---- the record run (both builds) ----------------------------------------------------------------------
void records(Stream& s, std::vector<FixState> const& fx) {
    // parse
    for (FixState const& st : fx)
        for (FixLayer const& L : st.layers) {
            std::vector<Cmd> cmds;
            std::string err;
            bool const ok = ops_parse(L.text.data(), L.text.size(), cmds, err);
            s.rec("parse", "parse:" + st.name + "/" + std::to_string(L.id), ser_cmds(ok, err, cmds));
        }
    std::vector<std::string> const docs = fuzz_docs();
    Rng mr(777);
    for (std::size_t i = 0; i < docs.size(); ++i) {
        std::vector<Cmd> cmds;
        std::string err;
        bool const ok = ops_parse(docs[i].data(), docs[i].size(), cmds, err);
        s.rec("parse", "parse:fuzz" + std::to_string(i), ser_cmds(ok, err, cmds));
        std::vector<Cmd> c2;
        std::string e2;
        bool const ok2 = ops_parse(docs[i], c2, e2);          // the std::string form agrees with the pointer form
        if (ser_cmds(ok, err, cmds) != ser_cmds(ok2, e2, c2)) fail("parse", "fuzz" + std::to_string(i) + ": the two ops_parse forms disagree");
    }
    for (FixState const& st : fx)
        for (FixLayer const& L : st.layers)
            for (int k = 0; k < 6; ++k) {
                std::string const m = mutate(mr, L.text);
                std::vector<Cmd> cmds;
                std::string err;
                bool const ok = ops_parse(m.data(), m.size(), cmds, err);
                s.rec("parse", "parse:mut:" + st.name + "/" + std::to_string(L.id) + "/" + std::to_string(k), ser_cmds(ok, err, cmds));
            }
    // quad
    Rng qr(4242);
    for (int i = 0; i < 3000; ++i) {
        DrawList dl;
        random_quads(qr, dl, qr.in(1, 80));
        s.rec("quad", "quad:" + std::to_string(i), ser_dl(dl));
    }
    // draw (no merge), real layers, unclipped and clipped; then the fuzz texts that parse
    for (FixState const& st : fx)
        for (FixLayer const& L : st.layers)
            for (int k = -1; k < int(sizeof kClips / sizeof *kClips); ++k) {
                DrawList dl;
                build(L, k < 0 ? nullptr : &kClips[k], dl, false);
                s.rec("draw", "draw:" + st.name + "/" + std::to_string(L.id) + "/" + std::to_string(k), ser_dl(dl));
            }
    for (std::size_t i = 0; i < docs.size(); ++i) {
        std::vector<Cmd> cmds;
        std::string err;
        if (!ops_parse(docs[i], cmds, err)) continue;
        bool huge = false;                                     // a window of 1e8 x 1e8 is billions of quads, before and after
        for (Cmd const& c : cmds)
            if (c.k == 'W' && !(std::fabs(c.f[2]) <= 5000 && std::fabs(c.f[3]) <= 5000)) huge = true;
        if (huge) continue;
        DrawList dl;
        if (i % 3 == 0) dl.clear({10.25f, -5, 300.5f, 77.75f});
        Ui ui {*g_assets, dl};
        ops_draw(ui, cmds, &g_store, nullptr);
        s.rec("draw", "draw:fuzz" + std::to_string(i), ser_dl(dl));
    }
}

// ---- checks of the new code alone ----------------------------------------------------------------------
#ifndef NUI_TEST_REFERENCE
void merge_checks(std::vector<FixState> const& fx) {
    int n = 0;
    std::size_t before = 0, after = 0;
    for (FixState const& st : fx)
        for (FixLayer const& L : st.layers)
            for (int k = -1; k < int(sizeof kClips / sizeof *kClips); ++k) {
                DrawList raw, rep;
                build(L, k < 0 ? nullptr : &kClips[k], raw, false);
                build(L, k < 0 ? nullptr : &kClips[k], rep, true);
                DrawList m = raw;
                merge_batches(m);
                std::string const name = st.name + "/" + std::to_string(L.id) + "/" + std::to_string(k);
                check_merge(raw, m, name);
                if (ser_dl(m) != ser_dl(rep)) fail("merge", name + ": ops_replay is not ops_draw + merge_batches");
                if (k < 0) { before += raw.batches.size(); after += m.batches.size(); }
                ++n;
            }
    std::printf("  merge: %d real layer builds checked; unclipped %u -> %u batches over %u layers\n", n, unsigned(before),
                unsigned(after), unsigned([&] { std::size_t c = 0; for (auto const& s : fx) c += s.layers.size(); return c; }()));
    Rng r(99);
    for (int i = 0; i < 3000; ++i) {
        DrawList raw;
        if (i % 2) random_merge_list(r, raw); else random_quads(r, raw, r.in(1, 200));
        if (i % 4 == 1)                                       // blocks not of DrawList::quad's shape: the general box
            for (Vtx& x : raw.v)
                if (r.p(10)) { x.x += float(r.in(-20, 20)) / 2; if (r.p(30)) x.y -= float(r.in(0, 9)); }
        DrawList m = raw;
        merge_batches(m);
        check_merge(raw, m, "random" + std::to_string(i));
    }
    // lists not made of whole quads in order are left as they are
    DrawList odd;
    odd.v.resize(9);
    odd.batches = {{0, 0, 0, 0, 6}, {1, 0, 0, 6, 3}};
    std::string const o0 = ser_dl(odd);
    merge_batches(odd);
    if (ser_dl(odd) != o0) fail("merge", "a list of partial quads was reordered");
    std::printf("  merge: 3,000 random lists checked\n");
}

void num_checks() {
    // integers: every value of up to 7 digits, through the parser, against float(atof)
    long bad = 0, n = 0;
    std::string text;
    char b[64];
    std::vector<Cmd> cmds;
    std::string err;
    auto flush = [&](std::vector<std::string> const& strs) {
        text.clear();
        // 1.2.6: in Q's texture field, the one number not clamped to +-65536 (review REND-7)
        for (std::string const& s : strs) { text += "Q\t"; text += s; text += "\t0\t0\t0\t0\t0\t0\t0\t0\tFFFFFFFF\t0\n"; }
        cmds.clear();
        if (!ops_parse(text, cmds, err) || cmds.size() != strs.size()) { fail("num", "integer batch did not parse"); return; }
        for (std::size_t i = 0; i < strs.size(); ++i) {
            float const want = float(std::atof(strs[i].c_str()));
            if (std::memcmp(&want, &cmds[i].f[0], 4) != 0) {
                if (++bad <= 5) fail("num", "\"" + strs[i] + "\" parses to a different float than atof");
            }
            ++n;
        }
    };
    std::vector<std::string> strs;
    strs.reserve(100000);
    for (long v = -9999999; v <= 9999999; ++v) {
        std::snprintf(b, sizeof b, "%ld", v);
        strs.push_back(b);
        if (strs.size() == 100000) { flush(strs); strs.clear(); }
    }
    for (char const* s : {"-0", "0", "-00", "0000000", "-0000000", "00000001", "-0000001", "007", "-0000012", "1234567", "-1234567",
                          "12345678", "-12345678", "", "-", "--1", "1-", "+1", " 1", "1 "})
        strs.push_back(s);
    flush(strs);
    std::printf("  num: %ld integer fields equal to float(atof)\n", n - bad);
    // colours: random 8-digit hex, both cases, against strtoul
    Rng r(31337);
    long cbad = 0, cn = 0;
    strs.clear();
    auto cflush = [&]() {
        text.clear();
        for (std::string const& s : strs) { text += "R\t0\t0\t1\t1\t"; text += s; text += "\n"; }
        cmds.clear();
        if (!ops_parse(text, cmds, err) || cmds.size() != strs.size()) { fail("num", "colour batch did not parse"); return; }
        for (std::size_t i = 0; i < strs.size(); ++i) {
            unsigned long const v = std::strtoul(strs[i].c_str(), nullptr, 16);
            uint32_t const want = argb(int((v >> 24) & 0xFF), int((v >> 16) & 0xFF), int((v >> 8) & 0xFF), int(v & 0xFF));
            if (want != cmds[i].col && ++cbad <= 5) fail("num", "colour \"" + strs[i] + "\" differs from strtoul");
            ++cn;
        }
        strs.clear();
    };
    static char const hx[] = "0123456789abcdefABCDEF";
    for (int i = 0; i < 2000000; ++i) {
        char s[9];
        for (int k = 0; k < 8; ++k) s[k] = hx[r.next() % 22];
        s[8] = 0;
        strs.push_back(s);
        if (strs.size() == 100000) cflush();
    }
    for (char const* s : {"00000000", "FFFFFFFF", "ffffffff", "7FFFFFFF", "80000000", "0000000g", "-0000001", "+FFFFFFF", " FFFFFFF",
                          "0x000000", "FFFFFFFFF", "FFFFFFF", ""})
        strs.push_back(s);
    cflush();
    std::printf("  num: %ld colour fields equal to strtoul\n", cn - cbad);
}

#endif

#ifndef NUI_TEST_REFERENCE
#define NUI_HAVE_REVIEW_CHECKS
// ---- nui 1.2.6: the review's fixes in the library's pure functions (check mode, before the records) --
// Tags: decode (REND-4), sig (REND-2), pic (REND-13), read (HOST-10), clamp (REND-7), title (REND-9),
// recipe (REND-8), assets (HOST-5), handles (HOST-8). Each has a NUI_FAULT_* in build_build_test.sh.

std::string temp_dir() {
    char b[MAX_PATH];
    DWORD const n = GetTempPathA(MAX_PATH, b);
    char d[MAX_PATH + 64];
    std::snprintf(d, sizeof d, "%.*snui_build_check_%lu\\", int(n), b, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(d, nullptr);
    return d;
}

std::vector<unsigned char> file_bytes(std::string const& p) {
    std::vector<unsigned char> v;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        unsigned char b[65536];
        std::size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
        std::fclose(f);
    }
    return v;
}

void decode_checks(std::string const& dir) {
    // REND-4: an error message without a ':' (the fit's "larger than 4096") must not throw
    std::string const wide = dir + "wide.png";
    pngw::save(wide, pngw::encode(5000, 16, 3, [](int, int) { return 0xFF336699u; }));
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    std::string err;
    bool ok = true, threw = false;
    try {
        ok = decode_image(wide, 0, 0, px, w, h, err);
    } catch (...) {
        threw = true;
    }
    if (threw || ok || err.find("larger than 4096") == std::string::npos)
        fail("decode", "decode_image of a 5000 px wide picture: " + std::string(threw ? "threw" : ok ? "succeeded" : err.c_str()));
    // REND-4: a re-read file of another size than the picture's is refused
    std::string const pic = dir + "pic.png";
    pngw::save(pic, pngw::encode(64, 48, 4, [](int x, int y) { return 0xFF000000u | uint32_t(x * 4) << 16 | uint32_t(y * 5); }));
    TextureStore ts;
    Picture p;
    p.path = pic;
    p.w = 64;
    p.h = 48;
    ts.pics[2] = p;
    if (!ts.picture_pixels(2, px, w, h) || w != 64 || h != 48) fail("decode", "picture_pixels of an unchanged file failed");
    pngw::save(pic, pngw::encode(32, 32, 4, [](int, int) { return 0xFFFFFFFFu; }));
    px.clear();
    if (ts.picture_pixels(2, px, w, h))
        fail("decode", "picture_pixels used a file that now decodes to " + std::to_string(w) + " x " + std::to_string(h) + ", not 64 x 48");
}

void sig_checks(std::string const& dir) {
    // REND-2: only PNG, JPEG and BMP signatures reach stb_image. A JPEG header behind a fill byte
    // (FF FF D8) passes stb's own test, but not the signature check.
    unsigned char const sof[] = {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00};
    std::vector<unsigned char> good {0xFF, 0xD8};
    good.insert(good.end(), sof, sof + sizeof sof);
    std::vector<unsigned char> fill {0xFF, 0xFF, 0xD8};
    fill.insert(fill.end(), sof, sof + sizeof sof);
    pngw::save(dir + "good.jpg", good);
    pngw::save(dir + "fill.jpg", fill);
    Picture out;
    std::string err;
    std::vector<unsigned char> bytes;
    if (!TextureStore::prepare(dir + "good.jpg", 64, 64, out, err, &bytes) || out.w != 16)
        fail("sig", "a JPEG header (FF D8 FF, SOF 16 x 16) was refused: " + err);
    err.clear();
    if (TextureStore::prepare(dir + "fill.jpg", 64, 64, out, err, &bytes))
        fail("sig", "a file starting FF FF D8 (not a JPEG signature) was accepted by tex_load");
    unsigned char const gif[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0};
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    if (decode_image_mem(gif, sizeof gif, 0, 0, px, w, h, err) || err.find("not a PNG") == std::string::npos)
        fail("sig", "decode_image_mem of a GIF header: " + err);
    unsigned char bmp[18] = {'B', 'M'};
    bmp[14] = 40;
    if (!image_signature_ok(bmp, sizeof bmp)) fail("sig", "a BMP header (BITMAPINFOHEADER) refused");
    bmp[14] = 41;
    if (image_signature_ok(bmp, sizeof bmp)) fail("sig", "a BMP with an unknown DIB header size accepted");
}

void pic_checks() {
    // REND-13: 262,144 opaque white pixels averaged into one: 64-bit sums, still white
    std::vector<unsigned char> const png = pngw::encode(512, 512, 4, [](int, int) { return 0xFFFFFFFFu; });
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    std::string err;
    if (!decode_image_mem(png.data(), png.size(), 1, 1, px, w, h, err) || w != 1 || h != 1 || px.size() != 1) {
        fail("pic", "512 x 512 to 1 x 1 did not decode: " + err);
        return;
    }
    if (px[0] != 0xFFFFFFFFu) {
        char b[96];
        std::snprintf(b, sizeof b, "512 x 512 white fitted to 1 x 1 is %08lx, not ffffffff (a sum wrapped)", (unsigned long)px[0]);
        fail("pic", b);
    }
}

void read_checks(std::string const& dir) {
    // HOST-10: a file over 1 MiB is sized from its first MiB; the decode thread reads and hashes it
    std::string const big = dir + "big.png";
    pngw::save(big, pngw::encode(800, 500, 4, [](int x, int y) { return 0xFF000000u | uint32_t((x * 7) ^ (y * 13)); }));
    std::vector<unsigned char> const all = file_bytes(big);
    Picture out;
    std::string err;
    std::vector<unsigned char> bytes;
    if (!TextureStore::prepare(big, 64, 64, out, err, &bytes)) { fail("read", "prepare of a 1.6 MB picture: " + err); return; }
    if (!out.deferred || out.read > kReadAllBytes || out.hash != 0 || !bytes.empty() || out.w != 64 || out.h != 40) {
        char b[160];
        std::snprintf(b, sizeof b, "a %u-byte picture: deferred %d, read %u bytes here (limit %u), hash %s, %u bytes handed on, fitted %d x %d",
                      unsigned(all.size()), int(out.deferred), unsigned(out.read), unsigned(kReadAllBytes), out.hash ? "set" : "0",
                      unsigned(bytes.size()), out.w, out.h);
        fail("read", b);
    }
    std::string const small = dir + "small.png";
    pngw::save(small, pngw::encode(40, 30, 3, [](int, int) { return 0xFF808080u; }));
    std::vector<unsigned char> const sb = file_bytes(small);
    if (!TextureStore::prepare(small, 64, 64, out, err, &bytes) || out.deferred || out.hash != nc_fnv1a64(sb.data(), sb.size()) ||
        bytes.size() != sb.size())
        fail("read", "a small picture is not read and hashed whole here: " + err);
}

void clamp_checks() {
    // REND-7: sizes and coordinates from Lua kept in range, inf / NaN dropped
    std::string const text = "W\t0\t0\t1e10\t1e10\tx\nR\tinf\tnan\t-1e9\t5\tFFFFFFFF\nI\t0\t0\t1e9\t70000\tFFFFFFFF\n"
                             "G\t0\t0\t-5\t1e30\t0.5\tFFFFFFFF\nQ\t3\t0\t0\t10\t10\t0\t0\t1\t1\tFFFFFFFF\t1";
    std::vector<Cmd> c;
    std::string err;
    if (!ops_parse(text, c, err) || c.size() != 5) { fail("clamp", "parse: " + err); return; }
    bool const ok = c[0].f[2] == 16384.0f && c[0].f[3] == 16384.0f && c[1].f[0] == 0.0f && c[1].f[1] == 0.0f &&
                    c[1].f[2] == -65536.0f && c[1].f[3] == 5.0f && c[2].f[2] == 16384.0f && c[2].f[3] == 65536.0f &&
                    c[3].f[2] == 0.0f && c[3].f[3] == 16384.0f && c[4].f[0] == 3.0f && c[4].f[3] == 10.0f;
    if (!ok) {
        char b[200];
        std::snprintf(b, sizeof b, "W %g x %g, R %g %g %g %g, I size %g id %g, G %g x %g, Q tex %g", c[0].f[2], c[0].f[3], c[1].f[0],
                      c[1].f[1], c[1].f[2], c[1].f[3], c[2].f[2], c[2].f[3], c[3].f[2], c[3].f[3], c[4].f[0]);
        fail("clamp", std::string("numbers not clamped: ") + b);
        return;
    }
    DrawList dl;
    Ui ui {*g_assets, dl};
    ops_replay(ui, c, &g_store, nullptr);
    if (dl.v.size() > 6u * 40000u) fail("clamp", "a clamped 16384 x 16384 window made " + std::to_string(dl.v.size() / 6) + " quads");
}

void title_checks() {
    // REND-9: no title font (or an 'M' of no height): nothing drawn, width 0, never px / 0
    if (!g_assets->title_ok()) { fail("title", "the game's title font did not load"); return; }
    Assets none = *g_assets;
    none.have_title = false;
    Assets flat = *g_assets;
    flat.title['M' - 0x20].lh = 0;
    for (Assets const* a : {&none, &flat}) {
        DrawList dl;
        Ui ui {*a, dl};
        float const w = ui.title_width("Three Paths", 9) + ui.title_text(10, 10, "Three Paths", 9, 0xFFFFFFFFu);
        bool finite = w - w == 0.0f;
        for (Vtx const& v : dl.v) finite = finite && v.x - v.x == 0.0f && v.y - v.y == 0.0f;
        if (!finite || w != 0.0f || !dl.v.empty())
            fail("title", std::string(a == &none ? "no title font" : "an 'M' of no height") + ": width " + std::to_string(w) + ", " +
                              std::to_string(dl.v.size() / 6) + " quads" + (finite ? "" : ", inf or NaN"));
    }
    DrawList dl;
    Ui ui {*g_assets, dl};
    if (!(ui.title_width("Three Paths", 9) > 0)) fail("title", "the real title font measures nothing");
}

// the DAT a file id names (VTABLE / FTABLE), as assets.cpp reads it
std::string dat_of(std::string const& ffxi, unsigned fid) {
    std::vector<unsigned char> const vt = file_bytes(ffxi + "VTABLE.DAT"), ft = file_bytes(ffxi + "FTABLE.DAT");
    if (fid >= vt.size() || fid * 2 + 1 >= ft.size() || vt[fid] != 1) return {};
    unsigned const e = ft[fid * 2] | (ft[fid * 2 + 1] << 8);
    char p[64];
    std::snprintf(p, sizeof p, "ROM\\%u\\%u.DAT", e >> 7, e & 0x7F);
    return ffxi + p;
}

void recipe_checks(std::string const& install, std::string const& dir) {
    // REND-8: frame_recipe_ok wants sprites 0-8 (and 9 when present) each with a quad
    std::vector<std::vector<SpriteQuad>> f = g_assets->frame;
    if (!frame_recipe_ok(f)) fail("recipe", "the game's own win00 recipe is refused");
    f[3].clear();
    if (frame_recipe_ok(f)) fail("recipe", "a recipe whose sprite 3 has no quad is accepted (Ui::window reads F[3][0])");
    f = g_assets->frame;
    f.resize(8);
    if (frame_recipe_ok(f)) fail("recipe", "a recipe of 8 sprites is accepted");
    // REND-8: the recipe is the sprite sheet named 'menu    win00', not the DAT's last one. A copy of
    // window type 6's DAT with another sheet appended (ten sprites, no quads) must still load win00.
    std::string const src = dat_of(install, 19);
    std::vector<unsigned char> dat = file_bytes(src);
    if (src.empty() || dat.empty()) { fail("recipe", "cannot read window type 6's DAT"); return; }
    std::size_t end = 0;
    while (end + 16 <= dat.size()) {
        uint32_t info;
        std::memcpy(&info, &dat[end + 4], 4);
        std::size_t const len = ((info >> 7) & 0x7FFFF) * 16;
        if (len < 16 || end + len > dat.size()) break;
        end += len;
    }
    dat.resize(end);
    std::vector<unsigned char> sheet(48, 0);
    std::memcpy(&sheet[0], "junk", 4);
    uint32_t const info = 0x31u | (uint32_t(sheet.size() / 16) << 7);
    std::memcpy(&sheet[4], &info, 4);
    std::memcpy(&sheet[16], "menu    junk    ", 16);
    sheet[32] = 0;                        // no textures
    sheet[33] = 10;                       // ten sprites
    dat.insert(dat.end(), sheet.begin(), sheet.end());   // each of the ten has no quad (zero bytes)
    std::string const inst = dir + "install\\";
    CreateDirectoryA(inst.c_str(), nullptr);
    CreateDirectoryA((inst + "ROM").c_str(), nullptr);
    CreateDirectoryA((inst + "ROM\\0").c_str(), nullptr);
    std::vector<unsigned char> vt(20, 0), ft(40, 0);
    vt[19] = 1;
    ft[38] = 19;                          // ROM\0\19.DAT
    pngw::save(inst + "VTABLE.DAT", vt);
    pngw::save(inst + "FTABLE.DAT", ft);
    pngw::save(inst + "ROM\\0\\19.DAT", dat);
    Assets a;
    a.load(inst, 6, 39542, false);       // no menu DAT or FFXiMain.dll here: only the window part matters
    bool const same = a.frame.size() == g_assets->frame.size() && !a.frame.empty() && a.frame[0].size() == g_assets->frame[0].size();
    if (!a.have_window || !same)
        fail("recipe", "with another sprite sheet after win00: have_window " + std::to_string(int(a.have_window)) + ", " +
                           std::to_string(a.frame.size()) + " sprites, the first with " +
                           std::to_string(a.frame.empty() ? 0 : a.frame[0].size()) + " quads (win00's: " +
                           std::to_string(g_assets->frame[0].size()) + ")");
}

void assets_checks(std::string const& install) {
    // HOST-5: FFXiMain.dll is read once per install for the moji widths
    long const n0 = assets_dll_reads();
    Assets a;
    a.load(install, 3, 39542, false);
    long const reads = assets_dll_reads() - n0;
    if (reads != 0) fail("assets", "a second load read FFXiMain.dll again (" + std::to_string(reads) + " reads)");
    if (std::memcmp(a.widths, g_assets->widths, 96) != 0 || a.widths_from_game != g_assets->widths_from_game)
        fail("assets", "the cached widths differ from the first load's");
    if (!g_assets->widths_from_game) fail("assets", "the widths did not come from FFXiMain.dll");
}

void handles_checks() {
    // HOST-8: stop() (TextureStore::clear) never hands out a handle again
    TextureStore ts;
    ts.next = 7;
    ts.clear();
    if (ts.next != 7) fail("handles", "clear() restarted texture handles at " + std::to_string(ts.next) + " (old handles alias new pictures)");
}

// ---- 1.2.10: every texture format the game's own loader takes (ffxi-chronicle issue 10) --------
// This install stores the window art and the gauge as DXT1 and the fonts as DXT3, and before 1.2.10
// those two were all nui decoded. A client that keeps them in any of the other formats the game
// itself takes (Render_CreateTextureFromDat, FFXiMain 0x10039e50: DXT1 to DXT5, or uncompressed at
// 4, 8, 16, 24 or 32 bits) loaded none of them, so the module refused to start and drew nothing at
// all. Here window type 6's art and a stand-in menu DAT are written in each of those formats and
// read back: every piece must load and every pixel must come out as it went in.
namespace fmt_fix {

// 4 x 4 blocks of one of 16 colours, so one DXT block holds a block of the pattern exactly. The
// channels are kept as their own 5 and 6 bit codes, so a colour survives DXT's 5-6-5 and
// A1R5G5B5's 5-5-5 untouched and the expected image is the pattern itself: nothing is decoded twice.
struct PatCol { int r5, g5, g6, b5; };
PatCol pat_codes(int i) {
    int const j = (i * 11 + 3) % 16, k = (i * 5 + 9) % 16;
    return {i * 2, j * 2, j * 4, k * 2};
}
int pat_index(int x, int y) { return ((x / 4) * 7 + (y / 4) * 5) % 16; }
std::uint32_t pat_color(int i, bool dxt) {
    PatCol const c = pat_codes(i);
    std::uint32_t const r = std::uint32_t(c.r5 * 255 / 31), b = std::uint32_t(c.b5 * 255 / 31);
    std::uint32_t const g = dxt ? std::uint32_t(c.g6 * 255 / 63) : std::uint32_t(c.g5 * 255 / 31);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
std::uint32_t pat(int x, int y, bool dxt) { return pat_color(pat_index(x, y), dxt); }

// The bytes after the 44 byte texture header for a w x h image of the pattern. With a fourcc: the
// 8 bytes nui skips, then one block per 4 x 4 cell, colour 0 holding that cell's colour and every
// index 0 (DXT3 every alpha nibble opaque, DXT5 alpha 0 opaque and every index 0). Otherwise `bits`
// is the depth, the palette comes first for 4 and 8, and the rows run bottom to top.
std::vector<unsigned char> encode(int w, int h, unsigned bits, std::uint32_t fourcc) {
    std::vector<unsigned char> d;
    if (fourcc) {
        d.assign(8, 0);
        bool const a4 = fourcc == 0x44585432 || fourcc == 0x44585433;
        bool const a8 = fourcc == 0x44585434 || fourcc == 0x44585435;
        for (int by = 0; by < h / 4; ++by)
            for (int bx = 0; bx < w / 4; ++bx) {
                PatCol const c = pat_codes(pat_index(bx * 4, by * 4));
                unsigned const v = unsigned(c.r5 << 11 | c.g6 << 5 | c.b5);
                unsigned char blk[16] = {};
                int n = 0;
                if (a4) { for (int i = 0; i < 8; ++i) blk[i] = 0xFF; n = 8; }
                else if (a8) { blk[0] = 0xFF; n = 8; }
                blk[n] = static_cast<unsigned char>(v & 0xFF);
                blk[n + 1] = static_cast<unsigned char>(v >> 8);
                d.insert(d.end(), blk, blk + n + 8);
            }
        return d;
    }
    std::size_t const pal = bits == 4 ? 64 : bits == 8 ? 1024 : 0;
    d.assign(pal + std::size_t(w) * std::size_t(h) * bits / 8, 0);
    for (int i = 0; i < int(pal / 4); ++i) {
        std::uint32_t const c = pat_color(i % 16, false);
        d[std::size_t(i) * 4 + 0] = static_cast<unsigned char>(c & 0xFF);            // B
        d[std::size_t(i) * 4 + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);     // G
        d[std::size_t(i) * 4 + 2] = static_cast<unsigned char>((c >> 16) & 0xFF);    // R
        d[std::size_t(i) * 4 + 3] = 0x80;                                            // the client's opaque
    }
    unsigned char* p = d.data() + pal;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            std::size_t const q = std::size_t(h - 1 - y) * std::size_t(w) + std::size_t(x);
            int const i = pat_index(x, y);
            PatCol const c = pat_codes(i);
            std::uint32_t const col = pat_color(i, false);
            if (bits == 4) p[q / 2] = static_cast<unsigned char>(p[q / 2] | (i << ((q & 1) ? 0 : 4)));
            else if (bits == 8) p[q] = static_cast<unsigned char>(i);
            else if (bits == 16) {
                unsigned const v = 0x8000u | unsigned(c.r5 << 10 | c.g5 << 5 | c.b5);
                p[q * 2] = static_cast<unsigned char>(v & 0xFF);
                p[q * 2 + 1] = static_cast<unsigned char>(v >> 8);
            } else {
                std::size_t const n = bits / 8;
                p[q * n + 0] = static_cast<unsigned char>(col & 0xFF);
                p[q * n + 1] = static_cast<unsigned char>((col >> 8) & 0xFF);
                p[q * n + 2] = static_cast<unsigned char>((col >> 16) & 0xFF);
                if (n == 4) p[q * n + 3] = 0x80;
            }
        }
    return d;
}

void put32(std::vector<unsigned char>& v, std::size_t at, std::uint32_t x) { std::memcpy(&v[at], &x, 4); }

// one 0x20 texture chunk: the 16 byte chunk header, a flag byte, the 16 byte name, the 44 byte
// texture header (width at +4, height at +8, depth at +14, fourcc at +40), then the payload
std::vector<unsigned char> tex_chunk(char const* name16, int w, int h, unsigned bits, std::uint32_t fourcc) {
    std::vector<unsigned char> const payload = encode(w, h, bits, fourcc);
    std::vector<unsigned char> c((77 + payload.size() + 15) / 16 * 16, 0);
    put32(c, 4, 0x20u | (std::uint32_t(c.size() / 16) << 7));
    c[16] = 0xA1;
    std::memcpy(&c[17], name16, 16);
    put32(c, 33 + 4, std::uint32_t(w));
    put32(c, 33 + 8, std::uint32_t(h));
    c[33 + 14] = static_cast<unsigned char>(bits & 0xFF);
    c[33 + 15] = static_cast<unsigned char>(bits >> 8);
    put32(c, 33 + 40, fourcc);
    std::memcpy(&c[77], payload.data(), payload.size());
    return c;
}

bool wmkdir(std::string const& p) {
    return CreateDirectoryW(to_wide(p).c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool wwrite(std::string const& p, std::vector<unsigned char> const& b) {
    HANDLE const f = CreateFileW(to_wide(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool const ok = b.empty() || (WriteFile(f, b.data(), DWORD(b.size()), &n, nullptr) != 0 && n == b.size());
    CloseHandle(f);
    return ok;
}

// A fixture install under `inst` (ends with a backslash, may be UTF-8): VTABLE / FTABLE, window
// type 6's DAT (the install's own win00 recipe behind the four re-encoded pieces) and a stand-in
// menu DAT holding moji and the gauge. No FFXiMain.dll: the widths fall back, which is not what is
// under test here.
bool make_install(std::string const& inst, std::vector<unsigned char> const& win00, unsigned bits,
                  std::uint32_t fourcc, bool with_window) {
    if (!wmkdir(inst) || !wmkdir(inst + "ROM") || !wmkdir(inst + "ROM\\0") || !wmkdir(inst + "ROM\\119")) return false;
    std::vector<unsigned char> vt(39543, 0), ft(79086, 0);
    vt[19] = 1;
    ft[38] = 19;                                        // ROM\0\19.DAT
    vt[39542] = 1;
    unsigned const e = (119u << 7) | 51u;               // ROM\119\51.DAT
    ft[39542 * 2] = static_cast<unsigned char>(e & 0xFF);
    ft[39542 * 2 + 1] = static_cast<unsigned char>(e >> 8);
    if (!wwrite(inst + "VTABLE.DAT", vt) || !wwrite(inst + "FTABLE.DAT", ft)) return false;
    if (with_window) {
        std::vector<unsigned char> win;
        for (char const* nm : {"menu    newtex  ", "menu    hfr1    ", "menu    vfr1    ", "menu    corner  "}) {
            int const s = std::strstr(nm, "newtex") ? 128 : 32;
            std::vector<unsigned char> const c = tex_chunk(nm, s, s, bits, fourcc);
            win.insert(win.end(), c.begin(), c.end());
        }
        win.insert(win.end(), win00.begin(), win00.end());
        if (!wwrite(inst + "ROM\\0\\19.DAT", win)) return false;
    }
    std::vector<unsigned char> menu = tex_chunk("font    moji    ", 1024, 32, bits, fourcc);
    std::vector<unsigned char> const g = tex_chunk("menu    gauge   ", 64, 64, bits, fourcc);
    menu.insert(menu.end(), g.begin(), g.end());
    return wwrite(inst + "ROM\\119\\51.DAT", menu);
}

// the install's own 'menu    win00' chunk, so the recipe behind the fixture art is the real one
std::vector<unsigned char> win00_chunk(std::string const& install) {
    std::vector<unsigned char> const dat = file_bytes(dat_of(install, 19));
    std::size_t off = 0;
    while (off + 32 <= dat.size()) {
        std::uint32_t info;
        std::memcpy(&info, &dat[off + 4], 4);
        std::size_t const len = ((info >> 7) & 0x7FFFF) * 16;
        if (len < 16 || off + len > dat.size()) break;
        if ((info & 0x7F) == 0x31 && std::memcmp(&dat[off + 16], "menu    win00   ", 16) == 0)
            return std::vector<unsigned char>(dat.begin() + off, dat.begin() + off + len);
        off += len;
    }
    return {};
}

int wrong(std::vector<std::uint32_t> const& atlas, int ax, int ay, int w, int h, bool dxt) {
    if (atlas.size() < std::size_t(kAtlasW) * kAtlasH) return w * h;
    int bad = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (atlas[std::size_t(ay + y) * kAtlasW + ax + x] != pat(x, y, dxt)) ++bad;
    return bad;
}

}  // namespace fmt_fix

void format_checks(std::string const& install, std::string const& dir) {
    using namespace fmt_fix;
    std::vector<unsigned char> const win00 = win00_chunk(install);
    if (win00.empty()) { fail("fmt", "cannot find the win00 recipe in window type 6's DAT"); return; }
    struct Case { char const* name; unsigned bits; std::uint32_t fourcc; };
    static Case const kCases[] = {
        {"DXT1", 0, 0x44585431}, {"DXT2", 0, 0x44585432}, {"DXT3", 0, 0x44585433},
        {"DXT4", 0, 0x44585434}, {"DXT5", 0, 0x44585435},
        {"4 bit", 4, 0}, {"8 bit", 8, 0}, {"16 bit", 16, 0}, {"24 bit", 24, 0}, {"32 bit", 32, 0},
    };
    int n = 0;
    for (Case const& c : kCases) {
        std::string const inst = dir + "fmt" + std::to_string(n++) + "\\";
        if (!make_install(inst, win00, c.bits, c.fourcc, true)) {
            fail("fmt", std::string(c.name) + ": cannot write the fixture install");
            continue;
        }
        Assets a;
        bool const ok = a.load(inst, 6, 39542, true);
        if (!ok || !a.have_window || !a.have_moji || !a.have_gauge) {
            fail("fmt", std::string(c.name) + ": " + a.log);
            continue;
        }
        bool const dxt = c.fourcc != 0;
        int const bad = wrong(a.atlas, kBodyX, kBodyY, 128, 128, dxt) + wrong(a.atlas, kHfrX, kHfrY, 32, 32, dxt) +
                        wrong(a.atlas, kVfrX, kVfrY, 32, 32, dxt) + wrong(a.atlas, kCornerX, kCornerY, 32, 32, dxt) +
                        wrong(a.atlas, kGaugeX, kGaugeY, 64, 64, dxt) + wrong(a.atlas, kMojiX, kMojiY, 1024, 32, dxt);
        if (bad) fail("fmt", std::string(c.name) + ": " + std::to_string(bad) + " pixels differ from the pattern written");
    }
    // Without the window art the module still loads, and the log says which file it could not read:
    // a plain frame is drawn instead (ui.cpp), where before 1.2.10 nothing at all was.
    std::string const bare = dir + "fmt_nowin\\";
    if (!make_install(bare, win00, 32, 0, false)) {
        fail("fmt", "cannot write the window-less fixture install");
    } else {
        Assets a;
        bool const ok = a.load(bare, 6, 39542, true);
        if (!ok || a.have_window)
            fail("fmt", "without the window DAT: load " + std::to_string(int(ok)) + ", have_window " +
                            std::to_string(int(a.have_window)));
        else if (a.log.find("19.DAT") == std::string::npos || a.log.find(bare) == std::string::npos)
            fail("fmt", "a missing window DAT is not named in the log: " + a.log);
    }
    // A header claiming a size the file cannot hold is refused, not decoded: 128 x 0x2000000 at
    // 32 bits wraps a 32-bit byte count to 0, so without the size bound the check passes and the
    // decode runs off the end of the DAT. make_install writes newtex first, so its texture header
    // is at 0x21 and the height at 0x21 + 8.
    std::string const bad = dir + "fmt_badsize\\";
    if (!make_install(bad, win00, 32, 0, true)) {
        fail("fmt", "cannot write the bad-size fixture install");
    } else {
        std::vector<unsigned char> dat = file_bytes(bad + "ROM\\0\\19.DAT");
        if (dat.size() < 0x30) {
            fail("fmt", "the bad-size fixture's window DAT is empty");
        } else {
            put32(dat, 0x21 + 8, 0x2000000u);
            wwrite(bad + "ROM\\0\\19.DAT", dat);
            Assets a;
            a.load(bad, 6, 39542, true);
            if (a.have_window || a.log.find("size 128x33554432") == std::string::npos)
                fail("fmt", "a header claiming 128x33554432 was not refused by its size: " + a.log);
        }
    }
    // A folder the system ANSI codepage cannot spell: every file nui opens is opened wide, so the
    // install still reads. Before 1.2.10 each went through the ANSI CRT and read nothing at all.
    std::string const wide = dir + "fmt-\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88-\xce\xa9\\";
    if (!make_install(wide, win00, 32, 0, true)) {
        fail("fmt", "cannot write a fixture install under a non-ANSI folder");
        return;
    }
    Assets a;
    if (!a.load(wide, 6, 39542, true) || !a.have_window || !a.have_moji || !a.have_gauge)
        fail("fmt", "an install under a non-ANSI folder: " + a.log);
    else if (wrong(a.atlas, kBodyX, kBodyY, 128, 128, false))
        fail("fmt", "an install under a non-ANSI folder decoded differently");
}

void review_checks(std::string const& install) {
    std::string const dir = temp_dir();
    int const f0 = g_fail;
    assets_checks(install);               // first: the loads below read other install folders
    decode_checks(dir);
    sig_checks(dir);
    pic_checks();
    read_checks(dir);
    clamp_checks();
    title_checks();
    recipe_checks(install, dir);
    format_checks(install, dir);
    handles_checks();
    std::printf("  review checks (decode sig pic read clamp title recipe assets fmt handles): %d failed\n", g_fail - f0);
}

#endif

// ---- bench ---------------------------------------------------------------------------------------------
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
void bench(std::vector<FixState> const& fx) {
#ifdef NUI_TEST_REFERENCE
    char const* const which = "reference (before 1.2.0)";
#else
    char const* const which = "1.2.0";
#endif
    std::printf("bench, %s: us per call, median of 300 (parse = text -> cmds incl. the host's copy before 1.2.0; build = a fresh\n"
                "list as the host makes one: ops_replay, which merges in 1.2.0; draw = ops_draw only; merge = merge_batches only)\n", which);
    std::printf("  %-14s %6s %5s %8s %8s %8s %8s %8s %6s %6s\n", "layer", "bytes", "quads", "parse", "build", "draw", "merge", "total",
                "batch0", "batch1");
    char const* names[17] = {};
    names[1] = "info"; names[2] = "bar"; names[3] = "ws"; names[4] = "recasts"; names[5] = "party"; names[6] = "target";
    names[7] = "gear"; names[16] = "target.over"; names[14] = "party.under"; names[15] = "party.over";
    double tot[4] = {0, 0, 0, 0};
    std::size_t tb0 = 0, tb1 = 0;
    for (FixState const& st : fx) {
        double sum[4] = {0, 0, 0, 0};
        std::size_t b0 = 0, b1 = 0;
        for (FixLayer const& L : st.layers) {
            std::vector<double> tp, tbuild, tdraw, tmerge;
            DrawList raw, rep;
            for (int i = 0; i < 300; ++i) {
                std::vector<Cmd> cmds;
                std::string err;
                double const t0 = qpc_us();
#ifdef NUI_TEST_REFERENCE
                ops_parse(std::string(L.text.data(), L.text.size()), cmds, err);   // the host copied the text before 1.2.0
#else
                ops_parse(L.text.data(), L.text.size(), cmds, err);
#endif
                double const t1 = qpc_us();
                {
                    DrawList dl;
                    dl.clear();
                    dl.ox = L.ox; dl.oy = L.oy;
                    Ui ui {*g_assets, dl};
                    double const a = qpc_us();
                    ops_replay(ui, cmds, &g_store, nullptr);
                    tbuild.push_back(qpc_us() - a);
                    if (i == 0) rep = dl;
                }
                {
                    DrawList dl;
                    dl.clear();
                    dl.ox = L.ox; dl.oy = L.oy;
                    Ui ui {*g_assets, dl};
                    double const a = qpc_us();
                    ops_draw(ui, cmds, &g_store, nullptr);
                    double const b = qpc_us();
                    tdraw.push_back(b - a);
                    merge_batches(dl);
                    tmerge.push_back(qpc_us() - b);
                    if (i == 0) raw = dl;
                }
                tp.push_back(t1 - t0);
            }
            // batches without the merge: the reference's ops_replay never merges; 1.2.0's draw before merge_batches
            DrawList nm;
            {
                std::vector<Cmd> cmds;
                std::string err;
                ops_parse(L.text, cmds, err);
                nm.ox = L.ox; nm.oy = L.oy;
                Ui ui {*g_assets, nm};
                ops_draw(ui, cmds, &g_store, nullptr);
            }
            double const p = median(tp), bu = median(tbuild), dr = median(tdraw), me = median(tmerge);
            std::printf("  %-8s %-5s %6u %5u %8.1f %8.1f %8.1f %8.1f %8.1f %6u %6u\n", st.name.c_str(),
                        names[L.id] ? names[L.id] : std::to_string(L.id).c_str(), unsigned(L.text.size()), unsigned(rep.v.size() / 6),
                        p, bu, dr, me, p + bu, unsigned(nm.batches.size()), unsigned(rep.batches.size()));
            sum[0] += p; sum[1] += bu; sum[2] += dr; sum[3] += me;
            b0 += nm.batches.size();
            b1 += rep.batches.size();
        }
        std::printf("  %-14s %6s %5s %8.1f %8.1f %8.1f %8.1f %8.1f %6u %6u\n", (st.name + " all").c_str(), "", "", sum[0], sum[1], sum[2],
                    sum[3], sum[0] + sum[1], unsigned(b0), unsigned(b1));
        for (int k = 0; k < 4; ++k) tot[k] += sum[k];
        tb0 += b0;
        tb1 += b1;
    }
    std::printf("  %-14s %6s %5s %8.1f %8.1f %8.1f %8.1f %8.1f %6u %6u\n", "five states", "", "", tot[0], tot[1], tot[2], tot[3],
                tot[0] + tot[1], unsigned(tb0), unsigned(tb1));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: test_build dump|check|bench <install dir> <fixtures dir> [ref.bin]\n");
        return 2;
    }
    std::string const mode = argv[1], install = argv[2], fix = argv[3];
    static Assets a;
    if (!a.load(install, 6, 39542, false) || !a.have_window) {
        std::printf("test_build: cannot load the window art from %s: %s\n", install.c_str(), a.log.c_str());
        return 2;
    }
    g_assets = &a;
    g_store.ffxi = install;
    std::vector<FixState> const fx = load_fixtures(fix);
    if (mode == "bench") {
        bench(fx);
        return g_fail ? 1 : 0;
    }
    if (argc < 5) return 2;
    Stream s;
    s.writing = mode == "dump";
    s.f = std::fopen(argv[4], s.writing ? "wb" : "rb");
    if (!s.f) { std::printf("test_build: cannot open %s\n", argv[4]); return 2; }
    double const t0 = qpc_us();
#ifdef NUI_HAVE_REVIEW_CHECKS
    if (!s.writing) review_checks(install);
#endif
    records(s, fx);
    if (!s.writing) {
        char extra = 0;
        if (std::fread(&extra, 1, 1, s.f) == 1) fail("parse", "the reference stream has more records");
    }
    std::fclose(s.f);
    std::printf("  %s: %ld records (%s) in %.0f ms\n", s.writing ? "reference written" : "compared with the reference",
                s.records, s.writing ? argv[4] : "parse, quad, draw", (qpc_us() - t0) / 1000);
    if (mode == "check") {
#ifdef NUI_TEST_REFERENCE
        std::printf("test_build: check needs the library under test, not the reference build\n");
        return 2;
#else
        merge_checks(fx);
        num_checks();
#endif
    }
    if (g_fail) {
        std::printf("test_build: %d FAIL\n", g_fail);
        return 1;
    }
    std::printf("test_build: %s ok\n", mode.c_str());
    return 0;
}
