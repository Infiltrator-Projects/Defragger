// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <gtk/gtk.h>
#include <string.h>

#define LDTM_MB_FONT "/usr/share/fonts/truetype/linux-defragger/mb_corpo_s_regular.ttf"

static char *font_family(void) {
    gchar *out = NULL;
    gchar *err = NULL;
    gint status = 0;
    gchar *argv[] = {
        (gchar *)"fc-scan",
        (gchar *)"--format=%{family[0]}",
        (gchar *)LDTM_MB_FONT,
        NULL
    };
    if (g_file_test(LDTM_MB_FONT, G_FILE_TEST_IS_REGULAR) &&
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                     &out, &err, &status, NULL) &&
        status == 0 && out != NULL && *out != '\0') {
        g_strstrip(out);
        g_free(err);
        return out;
    }
    g_free(out);
    g_free(err);
    return g_strdup("Sans");
}

void ldtm_apply_mb_theme(void) {
    GdkScreen *screen = gdk_screen_get_default();
    GtkCssProvider *provider;
    char *family;
    char *escaped;
    char *css;
    GError *error = NULL;
    if (screen == NULL) return;
    family = font_family();
    escaped = g_strescape(family, NULL);
    css = g_strdup_printf(
        "* { font-family: \"%s\", Sans; color: #edf0f2; }"
        "window, dialog, .background { background-color: #080a0b; }"
        "headerbar, .titlebar { background-image: none; background-color: #151719; border-bottom: 1px solid #34383c; color: #f5f6f7; box-shadow: none; }"
        "headerbar label, .titlebar label { color: #c8cdd1; font-weight: normal; }"
        "menubar { background-color: #0d0f10; border-bottom: 1px solid #25292c; padding: 4px 8px; }"
        "menu { background-color: #15181a; border: 1px solid #454b50; }"
        "menuitem { padding: 7px 11px; }"
        "menuitem:hover { background-color: #282c30; }"
        "frame > border { background-color: #0b0d0f; border: 1px solid #2c3135; border-radius: 5px; }"
        "button { background-image: none; background-color: #191c1f; color: #dde1e4; border: 1px solid #5f666c; border-radius: 3px; padding: 7px 13px; min-height: 27px; box-shadow: none; text-shadow: none; }"
        "button:hover { background-color: #24282c; border-color: #9da3a8; color: #ffffff; }"
        "button:active, button:checked { background-color: #30353a; border-color: #b6bbc0; }"
        "button:disabled { color: #555d63; border-color: #2a2e31; background-color: #101214; }"
        "entry, combobox button, spinbutton { background-image: none; background-color: #111416; color: #e8ebed; border: 1px solid #444a4f; border-radius: 3px; box-shadow: none; min-height: 28px; }"
        "entry:focus, spinbutton:focus { border-color: #7f878d; background-color: #15191c; }"
        "textview, textview text, treeview, viewport, scrolledwindow { background-color: #0d1012; color: #dde1e4; border-color: #373d42; }"
        "entry selection, textview text selection, treeview.view:selected { background-color: #555d63; color: #ffffff; }"
        "treeview.view header button { background-color: #171a1d; border-color: #41474c; font-weight: bold; }"
        "notebook > header { background-color: #0b0d0f; border-color: #343a3f; }"
        "notebook tab { background-color: #111416; padding: 7px 12px; }"
        "notebook tab:checked { background-color: #24282c; }"
        "progressbar trough { min-height: 8px; background-color: #101315; border: 1px solid #30363a; border-radius: 2px; }"
        "progressbar progress { background-color: #aeb4b9; border-radius: 1px; }"
        "progressbar text { color: #9ba2a8; font-size: 8pt; }"
        "scrollbar slider { background-color: #555d63; border-radius: 3px; min-width: 7px; min-height: 7px; }"
        "scrollbar slider:hover { background-color: #858c92; }"
        "separator { background-color: #30353a; }"
        "tooltip { background-color: #1a1d20; color: #f1f2f3; border: 1px solid #5d646a; }",
        escaped != NULL ? escaped : "Sans");
    provider = gtk_css_provider_new();
    if (gtk_css_provider_load_from_data(provider, css, -1, &error)) {
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50);
    }
    if (error != NULL) g_error_free(error);
    g_object_unref(provider);
    g_free(css);
    g_free(escaped);
    g_free(family);
}
