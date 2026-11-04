/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram tabs: the canvas (drawing on demand, culling, LOD) and its pointer input; PNG rendering in
 * tiles for exports. See dgmview.h. */
#include "dgmview_int.h"
#include "log.h"
#include "pngw.h"
#include "results.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C(h) color_hex(h)
static const uint32_t TAGC[9] = { 0, 0x5B8DEF, 0x5FD39B, 0xF2994A, 0xB98AF0, 0x4FC3D9, 0xF27DB0, 0xE8C266, 0x9AA3B0 };
/* DARKP and LIGHTP: the fixed colours of a dark / light export, whatever theme the app is using.
 * DARKP is also the screen in Lume Dark; any other theme draws with its own colours (screen_pal). */
typedef struct { uint32_t bg, card, head, border, text, muted, line, virt, accent, sel, grid; } Pal;
static const Pal DARKP = { 0x1B1E24, 0x20232A, 0x272B33, 0x343944, 0xD9DDE4, 0x9AA3B0, 0x7C8596, 0xE0A040, 0x5B8DEF, 0x5B8DEF, 0x23262D };
static const Pal LIGHTP = { 0xFAFBFC, 0xFFFFFF, 0xEEF1F6, 0xC9CFD8, 0x1D2330, 0x5D6676, 0x6B7486, 0xB46A00, 0x3B6FD8, 0x3B6FD8, 0xF0F2F5 };
static const Pal *P = &DARKP;
static Pal SCREENP;
static const Pal *screen_pal(void) {
    if (!strcmp(g_theme->id, "lume-dark")) return &DARKP;
    SCREENP = (Pal){ T_EDITOR_BG, T_PANEL, T_PANEL_ALT, T_BORDER, T_TEXT, T_TEXT_MUTED, T_TEXT_DIM, T_YELLOW, T_ACCENT, T_ACCENT, T_LINE_HL };
    return &SCREENP;
}

/* ---------------------------------------------------------------- primitives in world units */
static void dashed(SDL_Renderer *R, float x1, float y1, float x2, float y2, float th, Color c, float on, float off) {
    float dx = x2 - x1, dy = y2 - y1, L = sqrtf(dx * dx + dy * dy); if (L < 0.5f) return;
    float ux = dx / L, uy = dy / L;
    for (float t = 0; t < L; t += on + off) { float e = fminf(L, t + on); draw_line(R, x1 + ux * t, y1 + uy * t, x1 + ux * e, y1 + uy * e, th, c); }
}
static void seg(SDL_Renderer *R, float x1, float y1, float x2, float y2, float th, Color c, int origin, float k) {
    if (origin == DE_VIRTUAL) dashed(R, x1, y1, x2, y2, th, c, 6 * k, 4 * k);
    else if (origin == DE_SUGGESTED) dashed(R, x1, y1, x2, y2, th, c, 2 * k, 4 * k);
    else draw_line(R, x1, y1, x2, y2, th, c);
}
/* crow's foot / simple marks at an edge end (sx, sy) leaving horizontally in direction dx (+1 / -1) */
static void marks(SDL_Renderer *R, float x, float y, float dx, int card, bool optional, Color c, float k, float th, int notation) {
    if (k < 0.25f) return;
    if (notation == DNOT_SIMPLE) { if (card == DC_ONE) draw_line(R, x, y, x + 9 * k * dx, y, th * 2.2f, c); return; }
    if (card == DC_MANY) { draw_line(R, x, y - 6 * k, x + 10 * k * dx, y, th, c); draw_line(R, x, y + 6 * k, x + 10 * k * dx, y, th, c); }
    else if (card == DC_ONE) draw_line(R, x + 8 * k * dx, y - 6 * k, x + 8 * k * dx, y + 6 * k, th, c);
    else draw_rect(R, rect_make(x + 5 * k * dx - 2 * k, y - 2 * k, 4 * k, 4 * k), c);   /* unknown: a neutral dot */
    if (optional) { Rect o = rect_make(x + 16 * k * dx - 3.5f * k, y - 3.5f * k, 7 * k, 7 * k); draw_rrect_outline(R, o, 3.5f * k, th, c); }
    else if (card != DC_UNKNOWN) draw_line(R, x + 15 * k * dx, y - 6 * k, x + 15 * k * dx, y + 6 * k, th, c);
}

/* ---------------------------------------------------------------- the scene */
static bool dimmed(const DgDoc *d, int i) { return d->isolate && d->iso && i < d->d.nn && !d->iso[i]; }
static bool is_hit(const DgDoc *d, int node) {
    if (!d->highlight_all || !d->find.len) return false;
    for (int i = 0; i < d->nhits; i++) if (d->hits[i].node == node) return true;
    return false;
}
static void draw_card(App *a, DgDoc *d, int i, Font *fh, Font *fr, float k) {
    SDL_Renderer *R = a->renderer;
    const DgNode *n = &d->d.n[i];
    float sx, sy; dg_world_to_screen(d, a, n->x, n->y, &sx, &sy);
    Rect rc = rect_make(sx, sy, n->w * k, n->h * k);
    if (d->drawn_cards < 200) { char tag[64]; snprintf(tag, sizeof tag, "dgm:card:%.53s", n->kind == DN_ENTITY ? n->key + 7 : n->key); ui_record(tag, rc); }   /* accessible name (DGM-096); the harness keeps 512 targets */
    bool dim = dimmed(d, i);
    uint8_t al = dim ? 70 : 255;
    float rad = fmaxf(1, 6 * k);
    bool flash = d->flash[0] && !strcmp(d->flash, n->key);
    uint32_t border = n->sel ? P->sel : flash ? T_YELLOW : is_hit(d, i) ? T_YELLOW : n->kind == DN_STUB ? P->muted : n->status == DS_CHANGED ? T_YELLOW : n->status == DS_MISSING ? T_RED : P->border;
    float bw = n->sel || flash ? fmaxf(2, 2 * a->scale) : fmaxf(1, a->scale);
    draw_rrect(R, rect_inset(rc, -bw, -bw), rad + bw, color_alpha(C(border), al));
    draw_rrect(R, rc, rad, color_alpha(C(P->card), al));
    if (n->kind == DN_ENTITY && !n->sel) {   /* a logical entity is not a table: dashed outline in the logical colour */
        Color ec = color_alpha(C(T_SYN_KEYWORD), al); float t = fmaxf(1, a->scale);
        dashed(R, rc.x, rc.y - t, rc.x + rc.w, rc.y - t, t, ec, 6 * k, 4 * k); dashed(R, rc.x, rc.y + rc.h, rc.x + rc.w, rc.y + rc.h, t, ec, 6 * k, 4 * k);
        dashed(R, rc.x - t, rc.y, rc.x - t, rc.y + rc.h, t, ec, 6 * k, 4 * k); dashed(R, rc.x + rc.w, rc.y, rc.x + rc.w, rc.y + rc.h, t, ec, 6 * k, 4 * k);
    }
    if (d->lod == 0) {   /* boxes only: the header band shows where a table is; its name when it fits */
        float hh = fminf(rc.h, DG_HEAD_H * k);
        draw_rect(R, rect_make(rc.x, rc.y, rc.w, hh), color_alpha(C(n->color ? TAGC[n->color] : P->head), al));
        if (rc.w >= 20 * a->scale && rc.h > 3 * a->scale) {   /* in points: the same on every display */ Font *ff = dg_font(a, 7); Rect tr = rect_make(rc.x + 2, rc.y + fmaxf(0, (fminf(rc.h, 20 * a->scale / 2) - font_height(ff)) / 2), rc.w - 4, font_height(ff)); draw_rect(R, tr, color_alpha(C(P->card), 200));
            /* cut from the left: table names share prefixes, the end tells them apart */
            const char *nm = dgm_card_label(&d->d, i); char buf[DG_KEY + 8];
            if (font_width(ff, nm, -1) > tr.w) { const char *p = nm; while (*p && font_width(ff, p, -1) + font_width(ff, "\xE2\x80\xA6", -1) > tr.w) p++; snprintf(buf, sizeof buf, "\xE2\x80\xA6%s", p); nm = buf; }
            draw_text_fit(ff, R, nm, -1, tr, color_alpha(C(P->text), al), 0); d->drawn_labels++; }
        d->drawn_cards++;
        return;
    }
    Rect head = rect_make(rc.x, rc.y, rc.w, DG_HEAD_H * k);
    draw_rrect(R, head, rad, color_alpha(C(P->head), al));
    if (n->h > DG_HEAD_H + 1) draw_rect(R, rect_make(head.x, head.y + head.h - rad, head.w, rad), color_alpha(C(P->head), al));
    if (n->color) draw_rect(R, rect_make(rc.x, rc.y + rad, fmaxf(2, 4 * k), head.h - rad), color_alpha(C(TAGC[n->color]), al));
    float ix = rc.x + 14 * k, iy = rc.y + DG_HEAD_H * k / 2;
    if (k > 0.3f) {
        if (n->kind == DN_ENTITY) draw_rrect_outline(R, rect_make(ix - 6 * k, iy - 5 * k, 12 * k, 10 * k), 2 * k, fmaxf(1, k), color_alpha(C(T_SYN_KEYWORD), al));
        else if (n->kind == DN_STUB) draw_icon_table(R, ix, iy, 12 * k, color_alpha(C(P->muted), al), color_alpha(C(P->muted), al), C(P->head));
        else if (d->d.x && n->ti >= 0 && d->d.x->t[n->ti].is_view) draw_icon_view(R, ix, iy, 12 * k, color_alpha(C(T_SYN_FUNC), al), C(P->head));
        else draw_icon_table(R, ix, iy, 12 * k, color_alpha(C(P->text), al), color_alpha(C(P->accent), al), C(P->head));
    }
    const char *label = dgm_card_label(&d->d, i);
    char lab[DG_KEY + 40]; snprintf(lab, sizeof lab, "%s%s", label, n->kind == DN_STUB ? (n->ti < 0 ? "  (external)" : "") : "");
    float right = rc.x + rc.w - 8 * k;
    char badge[24] = "";
    if (n->more > 0 && n->kind == DN_TABLE) snprintf(badge, sizeof badge, "+%d", n->more);
    if (badge[0] && k > 0.3f) {
        float bwid = font_width(fr, badge, -1) + 10 * k; Rect br = rect_make(right - bwid, iy - 8 * k, bwid, 16 * k);
        bool hov = d->hover == i && d->hover_more;
        draw_rrect(R, br, 8 * k, color_alpha(C(hov ? P->accent : P->border), al));
        draw_text_fit(fr, R, badge, -1, br, color_alpha(C(hov ? T_ON_ACCENT : P->muted), al), 1);
        right = br.x - 6 * k;
    }
    if (n->status && k > 0.3f) { const char *st = n->status == DS_CHANGED ? "changed" : n->status == DS_MISSING ? "missing" : "new"; float w = font_width(fr, st, -1) + 8 * k; draw_text_fit(fr, R, st, -1, rect_make(right - w, rc.y, w, head.h), color_alpha(C(n->status == DS_MISSING ? T_RED : T_YELLOW), al), 1); right -= w + 4 * k; }
    if (n->pinned && k > 0.3f) { draw_text_fit(fr, R, "pin", -1, rect_make(right - 24 * k, rc.y, 24 * k, head.h), color_alpha(C(P->muted), al), 1); right -= 28 * k; }
    draw_text_fit(fh, R, lab, -1, rect_make(rc.x + 26 * k, rc.y, right - rc.x - 26 * k, head.h), color_alpha(C(n->kind == DN_STUB ? P->muted : P->text), al), 0);
    if (d->d.conceptual && n->alias[0] && n->kind == DN_TABLE) {   /* the physical name, muted, after the business name */
        float lw = font_width(fh, lab, -1) + 8 * k;
        if (rc.x + 26 * k + lw < right - 20 * k) draw_text_fit(fr, R, n->key, -1, rect_make(rc.x + 26 * k + lw, rc.y, right - rc.x - 26 * k - lw, head.h), color_alpha(C(P->muted), al), 0);
    }
    d->drawn_labels++;
    if (n->kind == DN_STUB && n->ti < 0 && k > 0.35f && n->h > DG_HEAD_H) { /* nothing more */ }
    if (d->lod == 1) { d->drawn_cards++; return; }
    int rows[512]; int nr = dgm_card_rows(&d->d, i, rows, 512);
    clip_push(R, rc);
    for (int r = n->scroll; r < nr && r - n->scroll < DG_MAX_ROWS; r++) {
        float ry = rc.y + (DG_HEAD_H + 4 + (float)(r - n->scroll) * DG_ROW_H) * k;
        if (ry > d->canvas.y + d->canvas.h || ry + DG_ROW_H * k < d->canvas.y) continue;
        Rect row = rect_make(rc.x, ry, rc.w, DG_ROW_H * k);
        if (d->focus_node == i && d->focus_row == r) draw_rect(R, row, C(T_SELECTED));
        if (n->kind == DN_TABLE && d->d.x && n->ti >= 0) {
            const SxTable *t = &d->d.x->t[n->ti]; const SxCol *c = &t->cols[rows[r]];
            bool fk = false; for (int q = 0; q < t->nfks; q++) if (dgm_col_in(t->fks[q].cols, c->name)) fk = true;
            if (c->pk) draw_icon_key(R, rc.x + 14 * k, row.y + row.h / 2, 10 * k, color_alpha(C(T_YELLOW), al));
            else if (fk) draw_text_fit(fr, R, "FK", -1, rect_make(rc.x + 5 * k, row.y, 20 * k, row.h), color_alpha(C(P->accent), al), 1);
            char nm[300]; snprintf(nm, sizeof nm, "%s%s", c->name, d->d.show_nullability && !c->notnull ? "?" : "");
            float tw = d->d.show_types && c->type ? font_width(fr, c->type, -1) : 0;
            draw_text_fit(fr, R, nm, -1, rect_make(rc.x + 26 * k, row.y, rc.w - 34 * k - tw - 8 * k, row.h), color_alpha(C(c->notnull ? P->text : P->muted), al), 0);
            if (tw > 0) draw_text_fit(fr, R, c->type, -1, rect_make(rc.x + rc.w - 8 * k - tw, row.y, tw, row.h), color_alpha(C(P->muted), al), 2);
        } else if (n->kind == DN_ENTITY) {
            int line = 0; const char *p = n->attrs; for (; *p && line < rows[r]; p++) if (*p == '\n') line++;
            const char *e2 = strchr(p, '\n'); char s[300]; snprintf(s, sizeof s, "%.*s", e2 ? (int)(e2 - p) : (int)strlen(p), p);
            bool pk = !strncmp(s, "PK ", 3);
            if (pk) draw_icon_key(R, rc.x + 14 * k, row.y + row.h / 2, 10 * k, color_alpha(C(T_YELLOW), al));
            draw_text_fit(fr, R, pk ? s + 3 : s, -1, rect_make(rc.x + 26 * k, row.y, rc.w - 34 * k, row.h), color_alpha(C(P->text), al), 0);
        }
    }
    if (nr - n->scroll > DG_MAX_ROWS && k > 0.4f) {
        char more[40]; snprintf(more, sizeof more, "%d more (scroll)", nr - n->scroll - DG_MAX_ROWS);
        draw_rect(R, rect_make(rc.x, rc.y + rc.h - 14 * k, rc.w, 14 * k), color_alpha(C(P->head), 230));
        draw_text_fit(fr, R, more, -1, rect_make(rc.x, rc.y + rc.h - 14 * k, rc.w, 14 * k), C(P->muted), 1);
    }
    clip_pop(R);
    d->drawn_cards++;
}
static void draw_edge(App *a, DgDoc *d, int e, float k) {
    SDL_Renderer *R = a->renderer;
    const DgEdge *g = &d->d.e[e];
    float p[8]; int np = dgm_route(&d->d, e, p);
    float s[8]; for (int i = 0; i < np; i++) dg_world_to_screen(d, a, p[2 * i], p[2 * i + 1], &s[2 * i], &s[2 * i + 1]);
    /* culling: the polyline's box against the canvas */
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < np; i++) { x0 = fminf(x0, s[2 * i]); x1 = fmaxf(x1, s[2 * i]); y0 = fminf(y0, s[2 * i + 1]); y1 = fmaxf(y1, s[2 * i + 1]); }
    if (x1 < d->canvas.x - 20 || x0 > d->canvas.x + d->canvas.w + 20 || y1 < d->canvas.y - 20 || y0 > d->canvas.y + d->canvas.h + 20) return;
    bool hov = d->hover_edge == e, selected = d->d.n[g->from].sel || d->d.n[g->to].sel;
    bool dim = dimmed(d, g->from) || dimmed(d, g->to);
    uint32_t col = g->origin == DE_FK ? (hov || selected ? P->accent : P->line) : g->origin == DE_LOGICAL ? T_SYN_KEYWORD : P->virt;
    Color c = color_alpha(C(col), dim ? 50 : 255);
    float th = fmaxf(1, (hov ? 2.2f : 1.4f) * a->scale * fminf(1.0f, fmaxf(0.6f, d->d.zoom)));
    for (int i = 0; i + 1 < np; i++) seg(R, s[2 * i], s[2 * i + 1], s[2 * i + 2], s[2 * i + 3], th, c, g->origin, k);
    float dxa = s[2] >= s[0] ? 1 : -1, dxb = s[2 * np - 4] >= s[2 * np - 2] ? 1 : -1;
    marks(R, s[0], s[1], dxa, g->card_from, false, c, k, th, d->d.notation);
    marks(R, s[2 * np - 2], s[2 * np - 1], dxb, g->card_to, g->optional, c, k, th, d->d.notation);
    if (g->origin != DE_FK && k > 0.45f && d->lod == 2) {   /* the label of a user relation, mid-way */
        Font *f = dg_font(a, 10 * d->d.zoom);
        float mx = (s[2] + s[4 < 2 * np ? 4 : 2]) / 2, my = (s[3] + s[5 < 2 * np ? 5 : 3]) / 2;
        char lab[120]; snprintf(lab, sizeof lab, "%s%s", g->origin == DE_SUGGESTED ? "? " : "", g->name);
        float w = font_width(f, lab, -1) + 8; Rect lr = rect_make(mx - w / 2, my - font_height(f) / 2 - 1, w, font_height(f) + 2);
        draw_rrect(R, lr, 3, color_alpha(C(P->bg), 230)); draw_text_fit(f, R, lab, -1, lr, c, 1);
    }
    d->drawn_edges++;
}
static void draw_notes(App *a, DgDoc *d, bool areas, float k) {
    SDL_Renderer *R = a->renderer;
    for (int i = 0; i < d->d.nnotes; i++) {
        const DgNote *n = &d->d.notes[i]; if (n->area != areas) continue;
        float sx, sy; dg_world_to_screen(d, a, n->x, n->y, &sx, &sy);
        Rect rc = rect_make(sx, sy, n->w * k, n->h * k);
        if (rc.x > d->canvas.x + d->canvas.w || rc.y > d->canvas.y + d->canvas.h || rc.x + rc.w < d->canvas.x || rc.y + rc.h < d->canvas.y) continue;
        bool fl = d->flash_note == i;
        if (areas) {
            uint32_t tc = TAGC[n->color ? n->color : 1];
            draw_rrect(R, rc, 10 * k, color_alpha(C(tc), 22));
            draw_rrect_outline(R, rc, 10 * k, fmaxf(1, a->scale) * (fl ? 2 : 1), color_alpha(C(fl ? T_YELLOW : tc), 140));
            (void)tc;   /* the title is drawn after the cards (draw_area_titles) */
        } else {
            draw_rrect(R, rc, 4 * k, C(fl ? 0xFFF0A0 : 0xF6EBB0));
            if (d->lod) {
                Font *f = dg_font(a, 11.5f * d->d.zoom);
                draw_text_fit(dg_font(a, 12.5f * d->d.zoom), R, n->title, -1, rect_make(rc.x + 8 * k, rc.y + 4 * k, rc.w - 16 * k, 20 * k), C(0x3A3420), 0);
                clip_push(R, rc);
                results_draw_wrapped(f, R, n->text, rect_make(rc.x + 8 * k, rc.y + 26 * k, rc.w - 16 * k, rc.h - 30 * k), C(0x3A3420), 0, true);
                clip_pop(R);
            }
        }
    }
}
static void draw_area_titles(App *a, DgDoc *d, float k) {
    SDL_Renderer *R = a->renderer;
    if (!d->lod) return;
    Font *f = dg_font(a, 13 * d->d.zoom);
    for (int i = 0; i < d->d.nnotes; i++) {
        const DgNote *n = &d->d.notes[i]; if (!n->area || !n->title[0]) continue;
        float sx, sy; dg_world_to_screen(d, a, n->x, n->y, &sx, &sy);
        float tw = fminf(font_width(f, n->title, -1) + 16 * k, n->w * k - 8 * k);
        Rect tr = rect_make(sx + 8 * k, sy - 22 * k - 2, tw, 22 * k);   /* a tab above the area's corner: never over a card */
        uint32_t tc = TAGC[n->color ? n->color : 1];
        draw_rrect(R, tr, 4 * k, color_alpha(C(P->bg), 220));
        draw_text_fit(f, R, n->title, -1, rect_inset(tr, 6 * k, 0), C(tc), 0);
    }
}
void dg_draw_scene(App *a, DgDoc *d, Rect r, bool export_mode) {
    SDL_Renderer *R = a->renderer;
    float k = dg_k(d, a);
    d->drawn_cards = d->drawn_edges = d->drawn_labels = 0;
    /* LOD with hysteresis (DGM-022): names below 45 %, boxes below 18 % */
    float z = d->d.zoom;
    if (d->lod == 2 && z < 0.40f) d->lod = 1;
    else if (d->lod == 1 && z >= 0.50f) d->lod = 2;
    if (d->lod >= 1 && z < 0.16f) d->lod = 0;
    else if (d->lod == 0 && z >= 0.22f) d->lod = 1;
    if (export_mode) d->lod = z < 0.4f ? 1 : 2;
    draw_rect(R, r, C(P->bg));
    if (!export_mode && k > 0.35f) {   /* a light dot grid (every 40 units) */
        float step = 40 * k; float wx, wy; dg_screen_to_world(d, a, r.x, r.y, &wx, &wy);
        float gx = r.x + (ceilf(wx / 40) * 40 - wx) * k, gy = r.y + (ceilf(wy / 40) * 40 - wy) * k;
        if (step > 8) for (float x = gx; x < r.x + r.w; x += step) for (float y = gy; y < r.y + r.h; y += step) draw_rect(R, rect_make(x, y, a->scale, a->scale), C(P->grid));
    }
    if (!d->d.grid_ok) dgm_grid_build(&d->d);
    draw_notes(a, d, true, k);
    /* edges first, culled by their box; then the cards the grid says are visible */
    for (int e = 0; e < d->d.ne; e++) if (!d->d.e[e].hidden) draw_edge(a, d, e, k);
    float wx0, wy0, wx1, wy1; dg_screen_to_world(d, a, r.x, r.y, &wx0, &wy0); dg_screen_to_world(d, a, r.x + r.w, r.y + r.h, &wx1, &wy1);
    float m = 40;
    int cap = d->d.nn ? d->d.nn : 1; int *vis = malloc(sizeof(int) * (size_t)cap);
    int nv = dgm_grid_query(&d->d, wx0 - m, wy0 - m, wx1 + m, wy1 + m, vis, cap);
    Font *fh = dg_font(a, 12.5f * z), *fr = dg_font(a, 11.5f * z);
    for (int q = 0; q < nv; q++) if (!d->d.n[vis[q]].sel) draw_card(a, d, vis[q], fh, fr, k);
    for (int q = 0; q < nv; q++) if (d->d.n[vis[q]].sel) draw_card(a, d, vis[q], fh, fr, k);   /* selected on top */
    free(vis);
    draw_notes(a, d, false, k);
    draw_area_titles(a, d, k);
}

/* ---------------------------------------------------------------- minimap (DGM-025) */
static Rect mm_rect(App *a, DgDoc *d) { return rect_make(d->canvas.x + d->canvas.w - px(a, 196), d->canvas.y + d->canvas.h - px(a, 136), px(a, 184), px(a, 124)); }
static void mm_xform(App *a, DgDoc *d, Rect mr, float *s, float *ox, float *oy, float *bx0, float *by0) {
    float x0, y0, x1, y1; dgm_bounds(&d->d, false, &x0, &y0, &x1, &y1);
    float vx0, vy0, vx1, vy1; dg_screen_to_world(d, a, d->canvas.x, d->canvas.y, &vx0, &vy0); dg_screen_to_world(d, a, d->canvas.x + d->canvas.w, d->canvas.y + d->canvas.h, &vx1, &vy1);
    x0 = fminf(x0, vx0); y0 = fminf(y0, vy0); x1 = fmaxf(x1, vx1); y1 = fmaxf(y1, vy1);
    *s = fminf((mr.w - 8) / fmaxf(1, x1 - x0), (mr.h - 8) / fmaxf(1, y1 - y0));
    *ox = mr.x + 4 + ((mr.w - 8) - (x1 - x0) * *s) / 2; *oy = mr.y + 4 + ((mr.h - 8) - (y1 - y0) * *s) / 2; *bx0 = x0; *by0 = y0;
}
static void minimap(App *a, DgDoc *d) {
    SDL_Renderer *R = a->renderer; Rect mr = mm_rect(a, d);
    draw_rrect(R, rect_inset(mr, -1, -1), px(a, 6), C(T_BORDER));
    bool hv = rect_contains(mr, a->in.mouse_x, a->in.mouse_y);
    draw_rrect(R, mr, px(a, 5), color_alpha(C(T_PANEL), hv ? 245 : 170));
    float s, ox, oy, bx, by; mm_xform(a, d, mr, &s, &ox, &oy, &bx, &by);
    for (int i = 0; i < d->d.nn; i++) { DgNode *n = &d->d.n[i]; draw_rect(R, rect_make(ox + (n->x - bx) * s, oy + (n->y - by) * s, fmaxf(1, n->w * s), fmaxf(1, n->h * s)), C(n->sel ? T_ACCENT : n->color ? TAGC[n->color] : T_TEXT_DIM)); }
    float vx0, vy0, vx1, vy1; dg_screen_to_world(d, a, d->canvas.x, d->canvas.y, &vx0, &vy0); dg_screen_to_world(d, a, d->canvas.x + d->canvas.w, d->canvas.y + d->canvas.h, &vx1, &vy1);
    Rect vr = rect_make(ox + (vx0 - bx) * s, oy + (vy0 - by) * s, (vx1 - vx0) * s, (vy1 - vy0) * s);
    draw_rect_outline(R, vr, fmaxf(1, a->scale), C(T_ACCENT));
    ui_record("dgm:minimap", mr);
}
static void minimap_move(App *a, DgDoc *d, float sx, float sy) {
    Rect mr = mm_rect(a, d); float s, ox, oy, bx, by; mm_xform(a, d, mr, &s, &ox, &oy, &bx, &by);
    float wx = bx + (sx - ox) / s, wy = by + (sy - oy) / s, k = dg_k(d, a);
    d->d.panx = wx - d->canvas.w / k / 2; d->d.pany = wy - d->canvas.h / k / 2; d->anim = false;
}

/* ---------------------------------------------------------------- hit tests */
static float dist_seg(float px_, float py_, float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1, L = dx * dx + dy * dy, t = L > 0 ? ((px_ - x1) * dx + (py_ - y1) * dy) / L : 0;
    t = fmaxf(0, fminf(1, t)); float qx = x1 + t * dx - px_, qy = y1 + t * dy - py_;
    return sqrtf(qx * qx + qy * qy);
}
static int edge_at(App *a, DgDoc *d, float sx, float sy) {
    float best = px(a, 5); int be = -1;
    for (int e = 0; e < d->d.ne; e++) {
        if (d->d.e[e].hidden) continue;
        float p[8]; int np = dgm_route(&d->d, e, p);
        for (int i = 0; i + 1 < np; i++) {
            float x1, y1, x2, y2; dg_world_to_screen(d, a, p[2 * i], p[2 * i + 1], &x1, &y1); dg_world_to_screen(d, a, p[2 * i + 2], p[2 * i + 3], &x2, &y2);
            float dd = dist_seg(sx, sy, x1, y1, x2, y2);
            if (dd < best) { best = dd; be = e; }
        }
    }
    return be;
}
static int note_at(DgDoc *d, float wx, float wy, bool areas) {
    for (int i = d->d.nnotes - 1; i >= 0; i--) { DgNote *n = &d->d.notes[i]; if (n->area == areas && wx >= n->x && wx <= n->x + n->w && wy >= n->y && wy <= n->y + n->h) return i; }
    return -1;
}

/* ---------------------------------------------------------------- input + draw */
void dg_canvas(App *a, DgDoc *d, Rect r) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    Uint64 t0 = SDL_GetPerformanceCounter();
    d->canvas = r;
    P = screen_pal();
    if (d->need_fit && d->d.nn && !d->lay_th) dg_fit(a, d, false);
    bool over = rect_contains(r, in->mouse_x, in->mouse_y) && !d->dlg && !d->cm.open;
    /* the find list is drawn above the canvas later in this frame: its rect from the previous frame */
    if (over && d->find_focus && d->find_list && d->nframe - d->findlist_frame <= 2 && rect_contains(d->findlist_rect, in->mouse_x, in->mouse_y)) over = false;
    bool mm_on = d->minimap && d->d.nn;
    Rect mr = mm_rect(a, d);
    float wx, wy; dg_screen_to_world(d, a, in->mouse_x, in->mouse_y, &wx, &wy);
    if (!d->d.grid_ok) dgm_grid_build(&d->d);
    /* hover */
    int hov = over && !(mm_on && rect_contains(mr, in->mouse_x, in->mouse_y)) ? dgm_hit(&d->d, wx, wy) : -1;
    if (hov != d->hover) app_wake_in(a, 16);
    d->hover = hov; d->hover_more = false; d->hover_edge = -1; d->hover_note = -1;
    if (hov >= 0) {
        DgNode *n = &d->d.n[hov]; float k = dg_k(d, a);
        float sx, sy; dg_world_to_screen(d, a, n->x + n->w, n->y, &sx, &sy);
        if (n->more > 0 && in->mouse_x > sx - 46 * k && in->mouse_y < sy + DG_HEAD_H * k) d->hover_more = true;
    } else if (over) { d->hover_edge = edge_at(a, d, in->mouse_x, in->mouse_y); if (d->hover_edge < 0) d->hover_note = note_at(d, wx, wy, false); if (d->hover_note < 0 && d->hover_edge < 0) { int ar = note_at(d, wx, wy, true); if (ar >= 0) { DgNote *n = &d->d.notes[ar]; if (wy < n->y + 26) d->hover_note = ar; } } }
    if (d->hover >= 0 || d->hover_edge >= 0) a->want_cursor = d->hover_more || d->linking ? CURSOR_HAND : CURSOR_ARROW;
    /* pinch on the trackpad: zoom at the pointer */
    if (over && in->pinch != 0 && in->pinch != 1) { dg_zoom_at(a, d, in->pinch, in->mouse_x, in->mouse_y); in->pinch = 1; }
    /* wheel: zoom with the primary modifier (or pinch), pan otherwise; over a long card, its rows scroll (Shift) */
    if (over && (in->wheel_x != 0 || in->wheel_y != 0)) {
        SDL_Keymod mods = SDL_GetModState();
        bool zoom = (mods & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
        if (zoom) dg_zoom_at(a, d, in->wheel_y > 0 ? 1.12f : 1 / 1.12f, in->mouse_x, in->mouse_y);
        else if ((mods & SDL_KMOD_SHIFT) && hov >= 0) {
            int rows[512]; int nr = dgm_card_rows(&d->d, hov, rows, 512);
            DgNode *n = &d->d.n[hov];
            if (nr > DG_MAX_ROWS) { n->scroll += in->wheel_y > 0 ? -2 : 2; if (n->scroll < 0) n->scroll = 0; if (n->scroll > nr - DG_MAX_ROWS) n->scroll = nr - DG_MAX_ROWS; }
        } else { float k = dg_k(d, a); d->d.panx -= in->wheel_x / k; d->d.pany -= in->wheel_y / k; d->anim = false; }
        in->wheel_x = in->wheel_y = 0;
        app_wake_in(a, 16);
    }
    /* press */
    if (over && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        SDL_Keymod mods = SDL_GetModState();
        bool add = (mods & (SDL_KMOD_SHIFT | SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
        d->mx0 = in->mouse_x; d->my0 = in->mouse_y; d->wx0 = wx; d->wy0 = wy; d->pan0x = d->d.panx; d->pan0y = d->d.pany;
        d->find_focus = false; d->find_list = false;
        if (mm_on && rect_contains(mr, in->mouse_x, in->mouse_y)) { d->drag = 4; minimap_move(a, d, in->mouse_x, in->mouse_y); }
        else if (d->linking) {   /* relation drawing: the target card */
            if (hov >= 0 && hov != d->link_from) {
                int rr = dgm_add_rel(&d->d, d->link_kind, d->d.n[d->link_from].key, d->d.n[hov].key, NULL, NULL, NULL);
                if (rr >= 0) { snprintf(d->d.rels[rr].author, sizeof d->d.rels[rr].author, "user"); dgm_edges(&d->d); dg_touch(d); dg_prompt(d, PR_REL_LABEL, rr, "Relation label", d->d.rels[rr].label); }
                d->linking = false;
            } else if (hov < 0) { d->linking = false; dg_msg(d, "Relation cancelled"); }
        }
        else if (hov >= 0) {
            DgNode *n = &d->d.n[hov];
            if (d->hover_more) { DgCat *k = dg_cat(d); if (k && n->ti >= 0) dg_expand(a, d, n->ti, 1); }
            else if (in->clicks >= 2) { dg_select_only(d, hov); dg_action(a, d, n->kind == DN_ENTITY ? DA_ATTRS : n->kind == DN_STUB ? DA_FOLLOW : DA_OPEN_DEFINITION); }
            else {
                if (add) n->sel = !n->sel;
                else if (!n->sel) dg_select_only(d, hov);
                d->focus_node = hov; d->focus_row = -1;
                /* row click: the keyboard focus goes to that column */
                float sx, sy; dg_world_to_screen(d, a, n->x, n->y, &sx, &sy); float k = dg_k(d, a);
                float ry = (in->mouse_y - sy) / k - DG_HEAD_H - 4;
                if (ry >= 0) d->focus_row = n->scroll + (int)(ry / DG_ROW_H);
                d->drag = n->sel ? 2 : 0;
                if (d->drag == 2) {
                    free(d->drag_xy); d->ndrag = d->d.nn; d->drag_xy = malloc(sizeof(float) * 2 * (size_t)(d->ndrag ? d->ndrag : 1));
                    for (int i = 0; i < d->d.nn; i++) { d->drag_xy[2 * i] = d->d.n[i].x; d->drag_xy[2 * i + 1] = d->d.n[i].y; }
                    dgm_snapshot(&d->d);
                }
            }
        } else if (d->hover_note >= 0) {
            d->drag_note = d->hover_note; DgNote *nt = &d->d.notes[d->drag_note];
            d->drag = (wx > nt->x + nt->w - 14 && wy > nt->y + nt->h - 14) ? 5 : 6;
            d->drag_xy = realloc(d->drag_xy, sizeof(float) * 4); d->drag_xy[0] = nt->x; d->drag_xy[1] = nt->y; d->drag_xy[2] = nt->w; d->drag_xy[3] = nt->h;
            if (in->clicks >= 2) { d->drag = 0; d->cm_note = d->drag_note; dg_action(a, d, DA_EDIT_NOTE); }
        } else if (d->hover_edge >= 0) {
            DgEdge *g = &d->d.e[d->hover_edge];
            for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = i == g->from || i == g->to;   /* the edge's endpoints (DGM-023) */
            dg_msg(d, "%s: %s(%s) -> %s(%s)", g->name, d->d.n[g->from].key, g->cols, d->d.n[g->to].key, g->ref_cols[0] ? g->ref_cols : "key");
        } else {
            bool space = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_SPACE];
            if (in->clicks >= 2) { dg_zoom_at(a, d, (mods & (SDL_KMOD_SHIFT | SDL_KMOD_ALT)) ? 1 / 1.6f : 1.6f, in->mouse_x, in->mouse_y); d->drag = 0; }   /* double click on the background: zoom in (Shift / Option: out) */
            else if ((mods & SDL_KMOD_SHIFT) && !space) { d->drag = 3; d->mq_x0 = d->mq_x1 = wx; d->mq_y0 = d->mq_y1 = wy; }
            else { d->drag = 1; if (!add) { for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = false; d->focus_node = -1; } }
        }
        app_wake_in(a, 16);
    }
    if (over && in->mouse_rpressed) {
        int kind = hov >= 0 ? 1 : d->hover_edge >= 0 ? 2 : d->hover_note >= 0 ? 3 : 0;
        if (hov >= 0 && !d->d.n[hov].sel) dg_select_only(d, hov);
        dg_open_menu(a, d, kind, hov, d->hover_edge, d->hover_note, in->mouse_x, in->mouse_y);
        d->cm_wx = wx; d->cm_wy = wy;
    }
    /* drag */
    if (d->drag && in->mouse_down) {
        float k = dg_k(d, a);
        if (d->drag == 1) { d->d.panx = d->pan0x - (in->mouse_x - d->mx0) / k; d->d.pany = d->pan0y - (in->mouse_y - d->my0) / k; d->anim = false; }
        else if (d->drag == 2 && d->drag_xy && d->ndrag == d->d.nn) {
            float dx = (in->mouse_x - d->mx0) / k, dy = (in->mouse_y - d->my0) / k;
            for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel && !d->d.n[i].pinned) {
                float x = d->drag_xy[2 * i] + dx, y = d->drag_xy[2 * i + 1] + dy;
                if (d->d.snap) { x = roundf(x / 20) * 20; y = roundf(y / 20) * 20; }
                d->d.n[i].x = x; d->d.n[i].y = y;
            }
            d->d.grid_ok = false;
        } else if (d->drag == 3) { d->mq_x1 = wx; d->mq_y1 = wy; }
        else if (d->drag == 4) minimap_move(a, d, in->mouse_x, in->mouse_y);
        else if ((d->drag == 5 || d->drag == 6) && d->drag_note >= 0 && d->drag_note < d->d.nnotes) {
            DgNote *nt = &d->d.notes[d->drag_note]; float dx = (in->mouse_x - d->mx0) / k, dy = (in->mouse_y - d->my0) / k;
            if (d->drag == 6) { nt->x = d->drag_xy[0] + dx; nt->y = d->drag_xy[1] + dy; }
            else { nt->w = fmaxf(80, d->drag_xy[2] + dx); nt->h = fmaxf(40, d->drag_xy[3] + dy); }
        }
        app_wake_in(a, 16);
    }
    if (d->drag && !in->mouse_down) {
        float moved = fabsf(in->mouse_x - d->mx0) + fabsf(in->mouse_y - d->my0);
        if (d->drag == 2 && moved > 2) { dg_touch(d); dg_iso_update(d); }
        else if (d->drag == 2 && d->d.iundo == d->d.nundo && d->d.nundo) { /* a click, not a move: drop the snapshot */ }
        if ((d->drag == 5 || d->drag == 6) && moved > 2) dg_touch(d);
        if (d->drag == 3) {
            float x0 = fminf(d->mq_x0, d->mq_x1), x1 = fmaxf(d->mq_x0, d->mq_x1), y0 = fminf(d->mq_y0, d->mq_y1), y1 = fmaxf(d->mq_y0, d->mq_y1);
            for (int i = 0; i < d->d.nn; i++) { DgNode *n = &d->d.n[i]; if (n->x < x1 && n->x + n->w > x0 && n->y < y1 && n->y + n->h > y0) n->sel = true; }
            int ns = dg_nsel(d); if (ns) dg_msg(d, "%d selected", ns);
        }
        if (d->drag == 1 && moved < 3 && !d->flash[0]) { /* a click on empty space */ }
        d->drag = 0; app_wake_in(a, 16);
    }
    /* draw */
    clip_push(R, r);
    dg_draw_scene(a, d, r, false);
    if (d->drag == 3) {
        float sx0, sy0, sx1, sy1; dg_world_to_screen(d, a, d->mq_x0, d->mq_y0, &sx0, &sy0); dg_world_to_screen(d, a, d->mq_x1, d->mq_y1, &sx1, &sy1);
        Rect mq = rect_make(fminf(sx0, sx1), fminf(sy0, sy1), fabsf(sx1 - sx0), fabsf(sy1 - sy0));
        draw_rect(R, mq, color_alpha(C(T_ACCENT), 30)); draw_rect_outline(R, mq, fmaxf(1, a->scale), C(T_ACCENT));
    }
    if (d->linking && d->link_from >= 0 && d->link_from < d->d.nn) {
        DgNode *n = &d->d.n[d->link_from]; float sx, sy; dg_world_to_screen(d, a, n->x + n->w, n->y + DG_HEAD_H / 2, &sx, &sy);
        dashed(R, sx, sy, in->mouse_x, in->mouse_y, fmaxf(1, 1.5f * a->scale), C(P->virt), 6 * a->scale, 4 * a->scale);
        app_wake_in(a, 33);
    }
    if (mm_on) minimap(a, d);
    /* hover tooltip of an edge (DGM-042) */
    if (d->hover_edge >= 0 && !d->drag) {
        DgEdge *g = &d->d.e[d->hover_edge];
        char fb[300]; snprintf(fb, sizeof fb, "%s(%s)", d->d.x && g->fk_t >= 0 ? d->d.x->t[g->fk_t].name : "", g->cols);
        bool named = strcmp(g->name, fb) != 0;
        char tip[600]; snprintf(tip, sizeof tip, "%s%s%s%s(%s) \xE2\x86\x92 %s(%s)  %s", g->origin == DE_FK ? "" : g->origin == DE_VIRTUAL ? "virtual: " : g->origin == DE_SUGGESTED ? "suggested: " : "logical: ", named ? g->name : "", named ? "  " : "",
                 d->d.n[g->from].key, g->cols, d->d.n[g->to].key, g->ref_cols[0] ? g->ref_cols : "key",
                 g->card_from == DC_ONE ? "one-to-one" : g->card_from == DC_MANY ? "many-to-one" : "cardinality unknown");
        float w = fminf(font_width(a->font_small, tip, -1) + px(a, 16), r.w - px(a, 20));
        Rect tr = rect_make(fminf(in->mouse_x + px(a, 12), r.x + r.w - w - px(a, 6)), in->mouse_y + px(a, 16), w, px(a, 24));
        draw_rrect(R, tr, px(a, 4), C(T_BORDER)); draw_rrect(R, rect_inset(tr, 1, 1), px(a, 4), C(T_PANEL_ALT));
        draw_text_fit(a->font_small, R, tip, -1, rect_inset(tr, px(a, 8), 0), C(T_TEXT), 0);
    } else if (d->hover >= 0 && !d->drag && d->lod < 2) {   /* far zoom: the card's name under the pointer */
        const char *nm = dgm_card_label(&d->d, d->hover);
        float w = font_width(a->font_small, nm, -1) + px(a, 16);
        Rect tr = rect_make(fminf(in->mouse_x + px(a, 12), r.x + r.w - w - px(a, 6)), in->mouse_y + px(a, 16), w, px(a, 24));
        draw_rrect(R, tr, px(a, 4), C(T_BORDER)); draw_rrect(R, rect_inset(tr, 1, 1), px(a, 4), C(T_PANEL_ALT));
        draw_text_fit(a->font_small, R, nm, -1, rect_inset(tr, px(a, 8), 0), C(T_TEXT), 0);
    } else if (d->hover >= 0 && !d->drag && d->d.n[d->hover].kind == DN_TABLE && d->d.x && d->d.n[d->hover].ti >= 0) {
        const SxTable *t = &d->d.x->t[d->d.n[d->hover].ti];
        if (t->comment && t->comment[0] && d->d.show_comments) {
            float w = fminf(font_width(a->font_small, t->comment, -1) + px(a, 16), px(a, 420));
            Rect tr = rect_make(fminf(in->mouse_x + px(a, 12), r.x + r.w - w - px(a, 6)), in->mouse_y + px(a, 16), w, px(a, 24));
            draw_rrect(R, tr, px(a, 4), C(T_PANEL_ALT)); draw_text_fit(a->font_small, R, t->comment, -1, rect_inset(tr, px(a, 8), 0), C(T_TEXT_MUTED), 0);
        }
    }
    /* empty states */
    DgCat *ck = dg_cat(d);
    if (!d->d.nn) {
        const char *m = !ck ? "This diagram has no connection: open it from a connection's table menu (Show ER Diagram)."
                      : ck->state == CS_ERROR ? ck->err
                      : ck->state != CS_READY ? "Reading the structure (names, keys and relations only, no rows)..."
                      : "An empty diagram. Find a table (Cmd+F) and press Enter to add it, or right-click for more.";
        Rect er = rect_make(r.x + r.w / 2 - px(a, 260), r.y + r.h / 2 - px(a, 30), px(a, 520), px(a, 60));
        results_draw_wrapped(a->font_ui, R, m, er, C(ck && ck->state == CS_ERROR ? T_RED : T_TEXT_MUTED), 1, true);
        if (ck && ck->state == CS_ERROR && ui_button(a, rect_make(r.x + r.w / 2 - px(a, 50), er.y + er.h + px(a, 8), px(a, 100), px(a, 28)), "Retry", BTN_NORMAL, true)) dg_refresh(a, d, true);
        if (ck && (ck->state == CS_WAIT || ck->state == CS_RUN)) {
            if (ui_button(a, rect_make(r.x + r.w / 2 - px(a, 50), er.y + er.h + px(a, 8), px(a, 100), px(a, 28)), "Cancel", BTN_QUIET, true)) { if (ck->job) exec_cancel(ck->job); else { ck->state = CS_ERROR; snprintf(ck->err, sizeof ck->err, "Cancelled"); } }
            app_wake_in(a, 250);
        }
    }
    clip_pop(R);
    ui_record("dgm:canvas", r);
    double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    d->last_ms = ms; d->frame_ms[d->nframe % 240] = ms; d->nframe++;
}

/* ---------------------------------------------------------------- PNG in tiles (DGM-061) */
bool dg_render_png(App *a, DgDoc *d, int scope, float scale, bool dark, bool transparent, const char *path, char *why, int wn, uint8_t **rgba_out, int *w_out, int *h_out) {
    float x0, y0, x1, y1;
    if (scope == EXS_VISIBLE) { dg_screen_to_world(d, a, d->canvas.x, d->canvas.y, &x0, &y0); dg_screen_to_world(d, a, d->canvas.x + d->canvas.w, d->canvas.y + d->canvas.h, &x1, &y1); }
    else if (!dgm_bounds(&d->d, scope == EXS_SELECTION, &x0, &y0, &x1, &y1)) { snprintf(why, (size_t)wn, scope == EXS_SELECTION ? "Nothing is selected" : "The diagram is empty"); return false; }
    float m = 30; x0 -= m; y0 -= m; x1 += m; y1 += m;
    float sc = scale > 0 ? scale : 2;
    float W = (x1 - x0) * sc, H = (y1 - y0) * sc;
    const float MAXS = 16384;
    if (W > MAXS || H > MAXS) { float f = fminf(MAXS / W, MAXS / H); sc *= f; W *= f; H *= f; }   /* very large diagrams are scaled down, never a giant buffer */
    int iw = (int)ceilf(W), ih = (int)ceilf(H);
    uint8_t *img = calloc((size_t)iw * (size_t)ih, 4);
    if (!img) { snprintf(why, (size_t)wn, "Not enough memory for a %d x %d image: export the selection or the visible part", iw, ih); return false; }
    SDL_Renderer *R = a->renderer;
    /* the view state is borrowed: zoom so that one world unit = sc pixels, and restored afterwards */
    float oz = d->d.zoom, opx = d->d.panx, opy = d->d.pany; Rect oc = d->canvas; int olod = d->lod;
    bool sel_only = scope == EXS_SELECTION;
    bool *hide = NULL;
    if (sel_only) { hide = calloc((size_t)d->d.nn + 1, 1); }
    const int T = 2048;
    SDL_Texture *tex = SDL_CreateTexture(R, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, T, T);
    if (!tex) { free(img); free(hide); snprintf(why, (size_t)wn, "The image could not be rendered: %s", SDL_GetError()); return false; }
    SDL_Texture *old = SDL_GetRenderTarget(R);
    const Pal *oldp = P; P = dark ? &DARKP : &LIGHTP;
    /* selection-only: the others become invisible by moving them far away for the render */
    float *saved = NULL;
    if (sel_only) { saved = malloc(sizeof(float) * 2 * (size_t)(d->d.nn ? d->d.nn : 1)); for (int i = 0; i < d->d.nn; i++) { saved[2 * i] = d->d.n[i].x; if (!d->d.n[i].sel) d->d.n[i].x = 9e5f; } d->d.grid_ok = false; }
    bool ok = true;
    for (int ty = 0; ty < ih && ok; ty += T) for (int tx = 0; tx < iw && ok; tx += T) {
        SDL_SetRenderTarget(R, tex);
        SDL_SetRenderDrawColor(R, 0, 0, 0, 0); SDL_RenderClear(R);
        d->d.zoom = sc / a->scale; d->d.panx = x0 + (float)tx / sc; d->d.pany = y0 + (float)ty / sc;
        Rect tr = rect_make(0, 0, (float)T, (float)T);
        d->canvas = tr;
        uint32_t bgc = P->bg;
        if (!transparent) draw_rect(R, tr, C(bgc));
        const Pal *pp = P; uint32_t bg0 = pp->bg;
        (void)bg0;
        dg_draw_scene(a, d, tr, true);
        if (transparent) { /* dg_draw_scene paints the background: redo without it is not possible; transparent keeps the theme background as alpha 0 below */ }
        SDL_Surface *s = SDL_RenderReadPixels(R, NULL);
        if (!s) { ok = false; snprintf(why, (size_t)wn, "The image could not be read back: %s", SDL_GetError()); break; }
        SDL_Surface *c32 = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32); SDL_DestroySurface(s);
        if (!c32) { ok = false; break; }
        int cw = iw - tx < T ? iw - tx : T, chh = ih - ty < T ? ih - ty : T;
        for (int yy = 0; yy < chh; yy++) {
            uint8_t *dst = img + ((size_t)(ty + yy) * (size_t)iw + (size_t)tx) * 4;
            memcpy(dst, (uint8_t *)c32->pixels + (size_t)yy * (size_t)c32->pitch, (size_t)cw * 4);
            if (transparent) for (int xx = 0; xx < cw; xx++) { uint8_t *q = dst + xx * 4; Color b = C(P->bg); if (q[0] == b.r && q[1] == b.g && q[2] == b.b) q[3] = 0; }
        }
        SDL_DestroySurface(c32);
    }
    SDL_SetRenderTarget(R, old);
    SDL_DestroyTexture(tex);
    P = oldp;
    if (sel_only) { for (int i = 0; i < d->d.nn; i++) d->d.n[i].x = saved[2 * i]; free(saved); d->d.grid_ok = false; }
    free(hide);
    d->d.zoom = oz; d->d.panx = opx; d->d.pany = opy; d->canvas = oc; d->lod = olod;
    if (ok && path) ok = png_write_rgba(path, img, iw, ih, iw * 4, why, wn);
    if (ok && rgba_out) { *rgba_out = img; *w_out = iw; *h_out = ih; img = NULL; }
    free(img);
    if (ok) LOG_I("diagram: PNG %d x %d rendered in tiles of %d", iw, ih, T);
    return ok;
}
