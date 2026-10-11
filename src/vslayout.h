// MKBATCH 2026-10-09 (JJ: the distance marker's number moved to the target but "the border and the glow is at standard
// HUD depth and floats around"): AK's batched HUD vertex shaders draw SEVERAL pieces per draw, each piece with its own
// rows in cb0, picked per vertex. Two templates seen (fxc of the dumped shaders):
//   05154232: mad r0.xyzw, v0.zzzz, l(1530.06 = 255 x 6), l(2.1, 3.1, ...) -> position rows cb0[14 + 6k + 2 / + 3]
//   4b432a87: bfi r0.yzw, l(0,30,30,30), l(0,2,2,2), v3.xxxx, l(0,2,2,1) + add 0.1 / 1.1 -> cb0[10 + 4k + 2 / + 3]
// This reads the disassembly and follows the index arithmetic (mad / ishl / bfi / add / utof / ftou / mov) to the two
// position lines, giving: first piece's x row, y row, rows per piece, rows in cb0. Pure C++ (tested offline on the dumps).
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// rowW: position w row for 3D pieces (97657b85: a 4x4 per piece), else -1. prow / pstride: EVERY cb0 row piece 0 reads
// (colour, uv, position) and its step per piece - MKTAG looks for the marker's colour mark in them.
struct VsBatchLayout { int rowX = -1, rowY = -1, stride = 0, rows = 0, base = -1, rowW = -1; int nPiece = 0; int prow[12] = {}, pstride[12] = {}; };

namespace vsl {
struct Off { bool known = false; double off = 0.0; int stride = 0; };
inline int comp(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1; }
// parse "l(a, b, c, d)" starting at p; returns count
inline int lit(const char* p, double v[4])
{
    if (strncmp(p, "l(", 2) != 0) return 0;
    p += 2; int n = 0;
    while (n < 4) { char* e = nullptr; v[n] = strtod(p, &e); if (e == p) break; ++n; p = e; while (*p == ' ' || *p == ',') ++p; if (*p == ')') break; }
    return n;
}
inline double litAt(const double v[4], int n, int i) { return n >= 4 ? v[i] : (n > 0 ? v[0] : 0.0); }
// split the operand list of one instruction line: "op dst, a, b, c"
inline int operands(const std::string& line, std::string out[6])
{
    size_t sp = line.find(' '); if (sp == std::string::npos) return 0;
    int n = 0, depth = 0; std::string cur;
    for (size_t i = sp + 1; i < line.size() && n < 6; ++i)
    {
        const char ch = line[i];
        if (ch == '(' || ch == '[') ++depth; else if (ch == ')' || ch == ']') --depth;
        if (ch == ',' && depth == 0) { out[n++] = cur; cur.clear(); continue; }
        if (ch == ' ' && cur.empty()) continue;
        cur += ch;
    }
    if (!cur.empty() && n < 6) out[n++] = cur;
    for (int i = 0; i < n; ++i) { while (!out[i].empty() && out[i].back() == ' ') out[i].pop_back(); }
    return n;
}
// "r0.xyzw" -> reg 0 + mask; "v3.xxxx" -> input; returns kind 'r' / 'v' / 'o' / 0
inline char reg(const std::string& s, int& idx, std::string& sw)
{
    if (s.size() < 2 || (s[0] != 'r' && s[0] != 'v' && s[0] != 'o')) return 0;
    char* e = nullptr; idx = (int)strtol(s.c_str() + 1, &e, 10); if (e == s.c_str() + 1) return 0;
    sw = (*e == '.') ? std::string(e + 1) : std::string("xyzw");
    return s[0];
}
inline int swz(const std::string& sw, int i) { return comp(sw.size() >= 4 ? sw[i] : sw[sw.size() == 1 ? 0 : (i < (int)sw.size() ? i : 0)]); }
}   // namespace vsl

inline bool vs_batch_layout(const char* text, VsBatchLayout& L)
{
    using namespace vsl;
    const char* cbd = strstr(text, "dcl_constantbuffer CB0[");
    if (!cbd || !strstr(cbd, "dynamicIndexed")) return false;
    L.rows = atoi(cbd + 23);
    const char* d = strstr(text, ", position"); if (!d) return false;
    const char* o = d; while (o > text && *o != 'o') --o;
    const int posReg = atoi(o + 1);
    Off r[16][4];
    const char* p = strstr(text, "dcl_temps"); if (!p) return false;
    int foundX = -1, foundY = -1, strideX = 0, strideY = 0;
    while (p && *p)
    {
        const char* e = strchr(p, '\n'); std::string line(p, e ? e - p : strlen(p)); p = e ? e + 1 : nullptr;
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string op = line.substr(0, line.find(' '));
        std::string a[6]; const int na = operands(line, a);
        if (na < 1) continue;
        int di = 0; std::string dm; const char dk = reg(a[0], di, dm);
        // every "cb0[rI.c + B]" source: one of the piece's rows (MKTAG)
        auto cbref = [&](const std::string& cb, int& row, int& st) -> bool
        {
            if (cb.compare(0, 5, "cb0[r") != 0) return false;
            int ri = 0; std::string rs; const size_t plus = cb.find('+');
            if (plus == std::string::npos || reg(cb.substr(4, cb.find(' ', 4) - 4), ri, rs) != 'r' || ri < 0 || ri > 15) return false;
            const int c = comp(rs.empty() ? 'x' : rs[0]);
            if (c < 0 || !r[ri][c].known) return false;
            row = atoi(cb.c_str() + plus + 1) + (int)std::floor(r[ri][c].off); st = r[ri][c].stride; return true;
        };
        for (int k = 1; k < na; ++k)
        {
            int row = 0, st = 0;
            if (!cbref(a[k], row, st)) continue;
            bool have = false; for (int q = 0; q < L.nPiece; ++q) have = have || L.prow[q] == row;
            if (!have && L.nPiece < 12) { L.prow[L.nPiece] = row; L.pstride[L.nPiece] = st; ++L.nPiece; }
        }
        if (op == "dp4" && dk == 'o' && di == posReg && na >= 3 && dm == "w") { int row = 0, st = 0; if (cbref(a[2], row, st)) L.rowW = row; continue; }
        if (op == "dp4" && dk == 'o' && di == posReg && na >= 3 && (dm == "x" || dm == "y"))
        {   // "cb0[r0.x + 14].xyzw"
            const std::string& cb = a[2];
            if (cb.compare(0, 5, "cb0[r") != 0) continue;
            int ri = 0; std::string rs; const size_t plus = cb.find('+');
            if (plus == std::string::npos || reg(cb.substr(4, cb.find(' ', 4) - 4), ri, rs) != 'r' || ri < 0 || ri > 15) continue;
            const int base = atoi(cb.c_str() + plus + 1), c = comp(rs.empty() ? 'x' : rs[0]);
            if (c < 0 || !r[ri][c].known) continue;
            const int row = base + (int)std::floor(r[ri][c].off);
            if (dm == "x") { foundX = row; strideX = r[ri][c].stride; L.base = base; } else { foundY = row; strideY = r[ri][c].stride; }
            continue;
        }
        if (dk != 'r' || di < 0 || di > 15) continue;
        Off nv[4]; bool w[4] = {};
        for (char ch : dm) { const int c = comp(ch); if (c >= 0) w[c] = true; }
        double l1[4], l2[4], l3[4];
        if ((op == "mad" || op == "imad") && na >= 4)
        {   // mad dst, vN.c, l(k), l(o): index = input x k + o, input = UNORM byte (k = 255 x rows per piece) or integer
            int si = 0; std::string ss; const int n1 = lit(a[2].c_str(), l1), n2 = lit(a[3].c_str(), l2);
            if (reg(a[1], si, ss) == 'v' && n1 && n2)
                for (int c = 0; c < 4; ++c) if (w[c]) { const double k = litAt(l1, n1, c); nv[c].known = true; nv[c].off = litAt(l2, n2, c); nv[c].stride = (int)std::lround(k > 100.0 ? k / 255.0 : k); }
        }
        else if (op == "ishl" && na >= 3)
        {
            int si = 0; std::string ss; const int n1 = lit(a[2].c_str(), l1); const char sk = reg(a[1], si, ss);
            if (sk == 'v' && n1)
            {
                for (int c = 0; c < 4; ++c) if (w[c]) { nv[c].known = true; nv[c].off = 0.0; nv[c].stride = 1 << (int)litAt(l1, n1, c); }
            }
            else if (sk == 'r' && si >= 0 && si < 16 && n1)   // a piece index already in a temp, shifted (97657b85: x 4)
            {
                for (int c = 0; c < 4; ++c) if (w[c]) { const int sc = swz(ss, c); if (sc >= 0 && r[si][sc].known) { const int sh = (int)litAt(l1, n1, c); nv[c] = r[si][sc]; nv[c].off = std::floor(nv[c].off) * (1 << sh); nv[c].stride <<= sh; } }
            }
        }
        else if (op == "bfi" && na >= 5)
        {   // bfi dst, l(width), l(shift), vN.c, l(base)
            int si = 0; std::string ss; const int n2 = lit(a[2].c_str(), l2), n3 = lit(a[4].c_str(), l3);
            if (reg(a[3], si, ss) == 'v' && n2 && n3)
                for (int c = 0; c < 4; ++c) if (w[c]) { nv[c].known = true; nv[c].off = litAt(l3, n3, c); nv[c].stride = 1 << (int)litAt(l2, n2, c); }
        }
        else if ((op == "add" || op == "iadd") && na >= 3)
        {
            int si = 0; std::string ss; const int n1 = lit(a[2].c_str(), l1);
            if (reg(a[1], si, ss) == 'r' && si >= 0 && si < 16 && n1)
                for (int c = 0; c < 4; ++c) if (w[c]) { const int sc = swz(ss, c); if (sc >= 0 && r[si][sc].known) { nv[c] = r[si][sc]; nv[c].off += litAt(l1, n1, c); } }
        }
        else if ((op == "utof" || op == "ftou" || op == "mov" || op == "ftoi" || op == "itof") && na >= 2)
        {
            int si = 0; std::string ss;
            if (reg(a[1], si, ss) == 'r' && si >= 0 && si < 16)
                for (int c = 0; c < 4; ++c) if (w[c]) { const int sc = swz(ss, c); if (sc >= 0 && r[si][sc].known) { nv[c] = r[si][sc]; if (op == "ftou" || op == "ftoi") nv[c].off = std::floor(nv[c].off); } }
        }
        for (int c = 0; c < 4; ++c) if (w[c]) r[di][c] = nv[c];   // anything else: unknown
    }
    if (foundX < 0 || foundY < 0 || strideX <= 0 || strideX != strideY) return false;
    L.rowX = foundX; L.rowY = foundY; L.stride = strideX;
    return L.rowX < L.rows && L.rowY < L.rows;
}
