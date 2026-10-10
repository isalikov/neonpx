#include "theme.h"

#include <stddef.h>

#include "util.h"

static const theme themes[] = {
    {
        .name = "neon",
        .bg = 0x0a0514,
        .bg_alt = 0x0f0920,
        .panel = 0x140b2a,
        .head_a = 0x3a0d5c,
        .head_b = 0x0a0514,
        .border = 0x3d1f6b,
        .text = 0xdcd6f7,
        .dim = 0x8a7fb3,
        .faint = 0x463a6b,
        .primary = 0x00f0ff,
        .secondary = 0xff2a6d,
        .warn = 0xf5d300,
        .ok = 0x39ff14,
        .danger = 0xff1744,
        .sel_bg = 0x2c0f52,
        .rt =
            {
                [RT_NONE] = 0x8a7fb3,
                [RT_NODE] = 0x39ff14,
                [RT_BUN] = 0xffb3e6,
                [RT_DENO] = 0x00f0ff,
                [RT_PYTHON] = 0xf5d300,
                [RT_RUBY] = 0xff2a6d,
                [RT_JAVA] = 0xff9e00,
                [RT_GO] = 0x29beb0,
                [RT_PHP] = 0x9d8cff,
                [RT_DOTNET] = 0xb388ff,
                [RT_BEAM] = 0xd65cff,
                [RT_DOCKER] = 0x2ea8ff,
                [RT_PROXY] = 0x5d6fa8,
                [RT_UNKNOWN] = 0xff1744,
            },
    },
    {
        .name = "synthwave",
        .bg = 0x1a0b2e,
        .bg_alt = 0x211038,
        .panel = 0x2a1245,
        .head_a = 0xff3c8e,
        .head_b = 0x1a0b2e,
        .border = 0x5b2a86,
        .text = 0xfbe7ff,
        .dim = 0xb48ac9,
        .faint = 0x5b3a78,
        .primary = 0x36f9f6,
        .secondary = 0xff7edb,
        .warn = 0xfede5d,
        .ok = 0x72f1b8,
        .danger = 0xfe4450,
        .sel_bg = 0x44206b,
        .rt =
            {
                [RT_NONE] = 0xb48ac9,
                [RT_NODE] = 0x72f1b8,
                [RT_BUN] = 0xffcce9,
                [RT_DENO] = 0x36f9f6,
                [RT_PYTHON] = 0xfede5d,
                [RT_RUBY] = 0xfe4450,
                [RT_JAVA] = 0xf97e72,
                [RT_GO] = 0x36f9f6,
                [RT_PHP] = 0xc792ea,
                [RT_DOTNET] = 0xc792ea,
                [RT_BEAM] = 0xff7edb,
                [RT_DOCKER] = 0x03edf9,
                [RT_PROXY] = 0x848bbd,
                [RT_UNKNOWN] = 0xfe4450,
            },
    },
    {
        .name = "matrix",
        .bg = 0x000a02,
        .bg_alt = 0x021206,
        .panel = 0x03170a,
        .head_a = 0x0b4d1c,
        .head_b = 0x000a02,
        .border = 0x0f4d22,
        .text = 0xb6ffc8,
        .dim = 0x3fa45f,
        .faint = 0x165c2c,
        .primary = 0x00ff41,
        .secondary = 0xa8ff60,
        .warn = 0xe6ff00,
        .ok = 0x00ff41,
        .danger = 0xff3b3b,
        .sel_bg = 0x0b3b18,
        .rt =
            {
                [RT_NONE] = 0x3fa45f,
                [RT_NODE] = 0x00ff41,
                [RT_BUN] = 0xa8ff60,
                [RT_DENO] = 0x7dffaf,
                [RT_PYTHON] = 0xe6ff00,
                [RT_RUBY] = 0xff6b6b,
                [RT_JAVA] = 0xd4ff4f,
                [RT_GO] = 0x7dffaf,
                [RT_PHP] = 0x9fffb0,
                [RT_DOTNET] = 0x9fffb0,
                [RT_BEAM] = 0xa8ff60,
                [RT_DOCKER] = 0x5cffd6,
                [RT_PROXY] = 0x2f7a46,
                [RT_UNKNOWN] = 0xff3b3b,
            },
    },
};

const theme *theme_find(const char *name) {
    if (name)
        for (size_t i = 0; i < ARRAY_LEN(themes); i++)
            if (str_iequal(name, themes[i].name)) return &themes[i];
    return NULL;
}

const theme *theme_get(const char *name) {
    const theme *t = theme_find(name);
    return t ? t : &themes[0];
}

const char *theme_names(void) { return "neon, synthwave, matrix"; }
