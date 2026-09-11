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
        "* { font-family: \"%s\", Sans; color: #f1f2f3; }"
        "window, dialog, .background { background-color: #090a0b; }"
        "headerbar, .titlebar { background-image: none; background-color: #111315; border-bottom: 1px solid #777d82; color: #f5f6f7; }"
        "headerbar label, .titlebar label { font-weight: bold; }"
        "button { background-image: none; background-color: #1d2023; color: #f2f3f4; border: 1px solid #7e858a; border-radius: 3px; padding: 6px 12px; box-shadow: none; }"
        "button:hover { background-color: #2a2e32; border-color: #c3c7ca; }"
        "button:active, button:checked { background-color: #383d42; }"
        "button:disabled { color: #6d7276; border-color: #3d4144; background-color: #151719; }"
        "entry, combobox button, spinbutton, textview, treeview, viewport, scrolledwindow { background-color: #111315; color: #eef0f1; border-color: #4f5559; }"
        "entry selection, textview text selection, treeview.view:selected { background-color: #60666b; color: #ffffff; }"
        "treeview.view header button { background-color: #1b1e20; border-color: #555b60; font-weight: bold; }"
        "notebook > header { background-color: #0d0f10; border-color: #4f5559; }"
        "notebook tab { background-color: #141719; padding: 7px 12px; }"
        "notebook tab:checked { background-color: #292d30; }"
        "progressbar trough { background-color: #17191b; border: 1px solid #555b60; }"
        "progressbar progress { background-color: #b5b9bc; }"
        "progressbar text { color: #f8f8f8; }"
        "scrollbar slider { background-color: #777d82; border-radius: 3px; }"
        "scrollbar slider:hover { background-color: #a9adb0; }"
        "separator { background-color: #4b5054; }"
        "tooltip { background-color: #202326; color: #f4f4f4; border: 1px solid #777d82; }",
        escaped != NULL ? escaped : "Sans");
    provider = gtk_css_provider_new();
    if (gtk_css_provider_load_from_data(provider, css, -1, &error)) {
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    if (error != NULL) g_error_free(error);
    g_object_unref(provider);
    g_free(css);
    g_free(escaped);
    g_free(family);
}
