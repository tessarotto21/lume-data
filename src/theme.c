/* SPDX-License-Identifier: AGPL-3.0-only */
/* Colour themes: see theme.h. */
#include "theme.h"
#include <SDL3/SDL.h>
#include <string.h>

/* id, name, light,
 * bg, panel, panel_alt, editor_bg, line_hl, border, hover, selected,
 * accent, accent_dim, accent_hover, on_accent, text, text_muted, text_dim,
 * green, red, yellow, scrollbar, warn_bg,
 * syn: keyword, string, number, comment, func, op, punct */
static const LumeTheme THEMES[] = {
    { "lume-dark", "Lume Dark", false,
      0x121316, 0x16181D, 0x1B1D23, 0x1B1E24, 0x22262E, 0x2A2E36, 0x22262E, 0x2A3A5C,
      0x5B8DEF, 0x3A4F80, 0x6E9CF2, 0xFFFFFF, 0xD9DDE4, 0x9AA3B0, 0x7A8290,
      0x5FD39B, 0xF27070, 0xE8C266, 0x3A3D48, 0x3A3220,
      0xC792EA, 0xA5D6A7, 0xF78C6C, 0x7A8290, 0x82AAFF, 0x89DDFF, 0x9AA3B0 },
    { "lume-light", "Lume Light", true,
      0xE9EBEF, 0xF3F4F6, 0xE4E7EC, 0xFFFFFF, 0xF1F4F9, 0xD3D7DE, 0xE3E7EE, 0xCCDCF7,
      0x2F6FDB, 0xA9C2EE, 0x4A82E3, 0xFFFFFF, 0x1F2328, 0x4B5563, 0x5F6672,
      0x1A7F4B, 0xC62F2F, 0x8A5D00, 0xC2C7D0, 0xFFF1CC,
      0xA626A4, 0x237A3B, 0xB5501A, 0x6B7280, 0x1F5FBF, 0x0E6E8C, 0x4B5563 },
    { "dracula", "Dracula", false,
      0x191A21, 0x1E1F29, 0x21222C, 0x282A36, 0x30323F, 0x343746, 0x343746, 0x44475A,
      0xBD93F9, 0x5B4A85, 0xCBA8FB, 0x191A21, 0xF8F8F2, 0xBAC0D6, 0x8C94B8,
      0x50FA7B, 0xFF5555, 0xF1FA8C, 0x44475A, 0x3D3622,
      0xFF79C6, 0xF1FA8C, 0xBD93F9, 0x7D88B8, 0x50FA7B, 0xFF79C6, 0xF8F8F2 },
    { "one-dark", "One Dark", false,
      0x1B1F25, 0x21252B, 0x252931, 0x282C34, 0x2C313C, 0x333842, 0x2C313A, 0x3E4451,
      0x61AFEF, 0x2F5577, 0x7DBDF2, 0x1B1F25, 0xD7DAE0, 0xABB2BF, 0x868D9A,
      0x98C379, 0xE06C75, 0xE5C07B, 0x3E4451, 0x3A3424,
      0xC678DD, 0x98C379, 0xD19A66, 0x7F848E, 0x61AFEF, 0x56B6C2, 0xABB2BF },
    { "nord", "Nord", false,
      0x242933, 0x2A303C, 0x2E3440, 0x2E3440, 0x3B4252, 0x3B4252, 0x3B4252, 0x434C5E,
      0x88C0D0, 0x4C6A78, 0xA0CEDA, 0x242933, 0xECEFF4, 0xD8DEE9, 0x9AA5B8,
      0xA3BE8C, 0xBF616A, 0xEBCB8B, 0x4C566A, 0x3E3A2E,
      0x81A1C1, 0xA3BE8C, 0xB48EAD, 0x8691A8, 0x88C0D0, 0x81A1C1, 0xD8DEE9 },
    { "solarized-dark", "Solarized Dark", false,
      0x00212B, 0x002B36, 0x073642, 0x002B36, 0x073642, 0x0D3D4A, 0x0A3946, 0x124B5A,
      0x268BD2, 0x1A4F6E, 0x3D9BDD, 0xFFFFFF, 0xEEE8D5, 0x93A1A1, 0x839496,
      0x859900, 0xDC322F, 0xB58900, 0x0F4A59, 0x3A3416,
      0x859900, 0x2AA198, 0xD33682, 0x7A8F94, 0x268BD2, 0xCB4B16, 0x93A1A1 },
    { "solarized-light", "Solarized Light", true,
      0xEEE8D5, 0xF5EFDC, 0xEAE3CE, 0xFDF6E3, 0xF3ECD8, 0xD9D2BD, 0xE8E1CC, 0xCFE0EA,
      0x1F78B8, 0xA7CDE9, 0x268BD2, 0xFFFFFF, 0x073642, 0x4A5F66, 0x5C7076,
      0x5F7A00, 0xC62B28, 0x8A6A00, 0xCFC8B2, 0xF8E7B5,
      0x6F8000, 0x1E8078, 0xC22F78, 0x6F8285, 0x1F78B8, 0xB5440F, 0x4A5F66 },
};
#define NTHEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

const LumeTheme *g_theme = &THEMES[0];
static char g_choice[32] = "lume-dark";

int theme_count(void) { return NTHEMES; }
const LumeTheme *theme_at(int i) { return i >= 0 && i < NTHEMES ? &THEMES[i] : NULL; }
const char *theme_choice(void) { return g_choice; }

static const LumeTheme *by_id(const char *id) {
    for (int i = 0; id && i < NTHEMES; i++) if (!strcmp(THEMES[i].id, id)) return &THEMES[i];
    return NULL;
}
static const LumeTheme *system_theme(void) { return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT ? &THEMES[1] : &THEMES[0]; }

bool theme_select(const char *id) {
    if (!id) return false;
    const LumeTheme *t = !strcmp(id, "system") ? system_theme() : by_id(id);
    if (!t) return false;
    SDL_strlcpy(g_choice, id, sizeof g_choice);
    g_theme = t;
    return true;
}

bool theme_follow_system(void) {
    if (strcmp(g_choice, "system")) return false;
    const LumeTheme *t = system_theme();
    if (t == g_theme) return false;
    g_theme = t;
    return true;
}
