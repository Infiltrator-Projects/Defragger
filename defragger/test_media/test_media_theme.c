// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_media.h"

#include <infiltratr/design.h>

#include <gtk/gtk.h>

void ldtm_apply_mb_theme(void) {
    GdkScreen *screen = gdk_screen_get_default();
    GtkCssProvider *provider;
    char *css;
    GError *error = NULL;
    const InfiltratrTypography *typography = infiltratr_typography();
    const InfiltratrDesignMetrics *metrics = infiltratr_design_metrics();
    const InfiltratrThemePalette *palette =
        infiltratr_theme_resolve(INFILTRATR_THEME_NIGHT, true);
    if (screen == NULL || typography == NULL || metrics == NULL || palette == NULL) return;

    /*
     * Common owns family identity and neutral structural metrics. Test Media
     * packages the verified faces and deliberately omits platform fallbacks.
     */
    css = g_strdup_printf(
        "* { font-family: \"%s\"; color: #%06x; }"
        "headerbar .title, .titlebar .title { font-family: \"%s\"; font-weight: normal; color: #%06x; }"
        "window, dialog, .background { background-color: #%06x; }"
        "headerbar, .titlebar { background-image: none; background-color: #%06x; border-bottom: 1px solid #%06x; color: #%06x; box-shadow: none; }"
        "headerbar label, .titlebar label { color: #%06x; font-weight: normal; }"
        "menubar { background-color: #%06x; border-bottom: 1px solid #%06x; padding: 4px 8px; }"
        "menu { background-color: #%06x; border: 1px solid #%06x; }"
        "menuitem { padding: 7px 11px; }"
        "menuitem:hover { background-color: #%06x; }"
        "frame > border { background-color: #%06x; border: 1px solid #%06x; border-radius: %upx; }"
        "button { background-image: none; background-color: #%06x; color: #%06x; border: 1px solid #%06x; border-radius: %upx; padding: 7px 13px; min-height: 27px; box-shadow: none; text-shadow: none; }"
        "button:hover { background-color: #%06x; border-color: #%06x; }"
        "button:active, button:checked { background-color: #%06x; border-color: #%06x; }"
        "button:disabled { color: #%06x; border-color: #%06x; background-color: #%06x; }"
        "entry, combobox button, spinbutton { background-image: none; background-color: #%06x; color: #%06x; border: 1px solid #%06x; border-radius: %upx; box-shadow: none; min-height: 28px; }"
        "entry:focus, spinbutton:focus { border-color: #%06x; background-color: #%06x; }"
        "textview, textview text, treeview, viewport, scrolledwindow { background-color: #%06x; color: #%06x; border-color: #%06x; }"
        "entry selection, textview text selection, treeview.view:selected { background-color: #%06x; color: #%06x; }"
        "treeview.view header button { background-color: #%06x; border-color: #%06x; font-weight: bold; }"
        "notebook > header { background-color: #%06x; border-color: #%06x; }"
        "notebook tab { background-color: #%06x; padding: 7px 12px; }"
        "notebook tab:checked { background-color: #%06x; }"
        "progressbar trough { min-height: 8px; background-color: #%06x; border: 1px solid #%06x; border-radius: %upx; }"
        "progressbar progress { background-color: #%06x; border-radius: %upx; }"
        "progressbar text { color: #%06x; font-size: 8pt; }"
        "scrollbar slider { background-color: #%06x; border-radius: %upx; min-width: 7px; min-height: 7px; }"
        "scrollbar slider:hover { background-color: #%06x; }"
        "separator { background-color: #%06x; }"
        "tooltip { background-color: #%06x; color: #%06x; border: 1px solid #%06x; }",
        typography->ui_family, (unsigned)palette->text_rgb,
        typography->brand_family, (unsigned)palette->title_rgb,
        (unsigned)palette->background_rgb,
        (unsigned)palette->panel_rgb, (unsigned)palette->border_rgb, (unsigned)palette->text_rgb,
        (unsigned)palette->muted_rgb,
        (unsigned)palette->surface_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->card_hover_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->border_rgb, metrics->small_radius,
        (unsigned)palette->button_background_rgb, (unsigned)palette->button_foreground_rgb,
        (unsigned)palette->border_rgb, metrics->control_radius,
        (unsigned)palette->surface_hover_rgb, (unsigned)palette->subtle_rgb,
        (unsigned)palette->operation_hover_rgb, (unsigned)palette->subtle_rgb,
        (unsigned)palette->subtle_rgb, (unsigned)palette->border_rgb, (unsigned)palette->background_rgb,
        (unsigned)palette->input_rgb, (unsigned)palette->text_rgb, (unsigned)palette->border_rgb,
        metrics->small_radius,
        (unsigned)palette->subtle_rgb, (unsigned)palette->surface_hover_rgb,
        (unsigned)palette->surface_rgb, (unsigned)palette->text_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->selection_background_rgb, (unsigned)palette->selection_foreground_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->border_rgb,
        (unsigned)palette->surface_rgb, (unsigned)palette->operation_hover_rgb,
        (unsigned)palette->background_rgb, (unsigned)palette->border_rgb, metrics->small_radius,
        (unsigned)palette->neutral_accent_rgb, metrics->small_radius,
        (unsigned)palette->subtle_rgb,
        (unsigned)palette->muted_rgb, metrics->small_radius,
        (unsigned)palette->text_rgb,
        (unsigned)palette->border_rgb,
        (unsigned)palette->card_rgb, (unsigned)palette->text_rgb, (unsigned)palette->border_rgb);

    provider = gtk_css_provider_new();
    if (gtk_css_provider_load_from_data(provider, css, -1, &error)) {
        gtk_style_context_add_provider_for_screen(
            screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 50);
    }
    if (error != NULL) g_error_free(error);
    g_object_unref(provider);
    g_free(css);
}
