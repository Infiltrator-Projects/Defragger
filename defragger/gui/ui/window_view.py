# SPDX-License-Identifier: GPL-3.0-or-later
"""GTK widget construction and presentation for one Defragger window."""

from __future__ import annotations

from typing import Any, Protocol

from gi.repository import Gdk, Gtk

from .map_presenter import MapPresentation
from .operation_planner import ControlState
from .widgets import DiskMap, SummaryCard


APP_NAME = "Linux Defragger"
APP_ICON_NAME = "io.github.linuxdefragger"
PROJECT_URL = "https://github.com/Infiltrator-Projects/Defragger"
COPYRIGHT = "Copyright © 2026 Shannon Smith"
ABOUT_COMMENTS = (
    "A C-first Linux filesystem allocation analyser and offline defragmenter "
    "authored by Shannon Smith."
)
ABOUT_LICENSE = (
    "Linux Defragger is free software licensed under the GNU General Public "
    "License version 3 or, at your option, any later version "
    "(GPL-3.0-or-later).\n\n"
    "See LICENSE in the source package or COPYING.GPL-3.0 in the installed "
    "documentation for the complete licence text."
)


class WindowActions(Protocol):
    """User-intent interface implemented by the window composition root."""

    def refresh_devices(
        self,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> None: ...

    def on_device_changed(self, combo: Gtk.ComboBoxText) -> None: ...

    def open_image(self, button: Gtk.Widget) -> None: ...

    def unmount_selected(self, button: Gtk.Button) -> None: ...

    def on_map_size_allocate(
        self,
        widget: Gtk.Widget,
        allocation: Gdk.Rectangle,
    ) -> None: ...

    def analyze(
        self,
        clear_log: bool = True,
        target_cells: int | None = None,
        quiet: bool = False,
    ) -> None: ...

    def start_mutation(self, operation: str) -> None: ...

    def request_stop(self, button: Gtk.Button) -> None: ...



class WindowView:
    """Own every GTK widget; delegate user intent to the window controller."""

    def __init__(
        self,
        window: Gtk.ApplicationWindow,
        controller: WindowActions,
        *,
        gui_version: str,
        engine_version: str,
        build_label: str,
    ) -> None:
        self.window = window
        self.controller = controller
        self.gui_version = gui_version
        self.engine_version = engine_version
        self.build_label = build_label
        self._build()
        self._load_css()

    def _build(self) -> None:
        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.window.add(outer)
        outer.pack_start(self._build_menu_bar(), False, False, 0)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        root.set_border_width(12)
        outer.pack_start(root, True, True, 0)

        title_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        title = Gtk.Label()
        title.set_markup(
            "<span size='x-large' weight='bold'>Linux Defragger</span>"
        )
        title.set_xalign(0)
        subtitle = Gtk.Label(
            label=(
                "Analyse allocation, fully pack and defragment supported "
                "filesystems, or create 10% growth-space layouts"
            )
        )
        subtitle.set_xalign(0)
        subtitle.set_line_wrap(True)
        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=1)
        title_box.pack_start(title, False, False, 0)
        title_box.pack_start(subtitle, False, False, 0)
        title_row.pack_start(title_box, True, True, 0)
        version = Gtk.Label(
            label=f"Engine {self.engine_version} · GUI {self.gui_version}"
        )
        version.get_style_context().add_class("dim-label")
        title_row.pack_end(version, False, False, 0)
        root.pack_start(title_row, False, False, 0)

        device_frame = Gtk.Frame(label="Volume")
        device_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        device_box.set_border_width(8)
        self.device_combo = Gtk.ComboBoxText()
        self.device_combo.set_hexpand(True)
        self.device_combo.connect("changed", self.controller.on_device_changed)
        device_box.pack_start(self.device_combo, True, True, 0)
        self.refresh_button = Gtk.Button.new_with_label("Refresh")
        self.refresh_button.connect(
            "clicked",
            lambda _button: self.controller.refresh_devices(clear_cache=True),
        )
        device_box.pack_start(self.refresh_button, False, False, 0)
        self.image_button = Gtk.Button.new_with_label("Open image…")
        self.image_button.connect("clicked", self.controller.open_image)
        device_box.pack_start(self.image_button, False, False, 0)
        self.unmount_button = Gtk.Button.new_with_label("Unmount")
        self.unmount_button.connect("clicked", self.controller.unmount_selected)
        device_box.pack_start(self.unmount_button, False, False, 0)
        device_frame.add(device_box)
        root.pack_start(device_frame, False, False, 0)

        cards = Gtk.Grid(column_spacing=8, row_spacing=8)
        cards.set_column_homogeneous(True)
        self.capacity_card = SummaryCard("Capacity")
        self.free_card = SummaryCard("Free space")
        self.files_card = SummaryCard("Files")
        self.fragmented_card = SummaryCard("Fragmentation")
        cards.attach(self.capacity_card, 0, 0, 1, 1)
        cards.attach(self.free_card, 1, 0, 1, 1)
        cards.attach(self.files_card, 2, 0, 1, 1)
        cards.attach(self.fragmented_card, 3, 0, 1, 1)
        root.pack_start(cards, False, False, 0)

        map_frame = Gtk.Frame(label="Allocation map")
        map_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=7)
        map_box.set_border_width(8)
        self.disk_map = DiskMap()
        self.disk_map.connect(
            "size-allocate", self.controller.on_map_size_allocate
        )
        map_box.pack_start(self.disk_map, True, True, 0)
        legend = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        for label, colour in (
            ("Free", DiskMap.COLORS["free"]),
            ("Outside active filesystem", DiskMap.COLORS["outside"]),
            ("Used", DiskMap.COLORS["used"]),
            ("Fragmented", DiskMap.COLORS["fragmented"]),
            ("Directory", DiskMap.COLORS["directory"]),
            ("Unknown", DiskMap.COLORS["unknown"]),
            ("Filesystem metadata/reserved", DiskMap.COLORS["bad"]),
        ):
            item = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=5)
            swatch = Gtk.DrawingArea()
            swatch.set_size_request(15, 15)
            swatch.connect("draw", self._draw_swatch, colour)
            item.pack_start(swatch, False, False, 0)
            item.pack_start(Gtk.Label(label=label), False, False, 0)
            legend.pack_start(item, False, False, 0)
        self.map_caption = Gtk.Label(
            label="Each square represents a range of filesystem allocation units."
        )
        self.map_caption.set_xalign(1)
        self.map_caption.get_style_context().add_class("dim-label")
        legend.pack_end(self.map_caption, True, True, 0)
        map_box.pack_start(legend, False, False, 0)
        map_frame.add(map_box)
        root.pack_start(map_frame, True, True, 0)

        action_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.analyze_button = Gtk.Button.new_with_label("Analyse")
        self.analyze_button.connect(
            "clicked", lambda _button: self.controller.analyze()
        )
        action_row.pack_start(self.analyze_button, False, False, 0)
        self.defrag_button = Gtk.Button.new_with_label("Defragment")
        self.defrag_button.connect(
            "clicked",
            lambda _button: self.controller.start_mutation("defrag"),
        )
        action_row.pack_start(self.defrag_button, False, False, 0)
        self.growth_button = Gtk.Button.new_with_label("Growth Defrag")
        self.growth_button.connect(
            "clicked",
            lambda _button: self.controller.start_mutation("growth-defrag"),
        )
        action_row.pack_start(self.growth_button, False, False, 0)
        self.recover_button = Gtk.Button.new_with_label("Recover")
        self.recover_button.connect(
            "clicked",
            lambda _button: self.controller.start_mutation("recover"),
        )
        action_row.pack_start(self.recover_button, False, False, 0)
        self.stop_button = Gtk.Button.new_with_label("Stop safely")
        self.stop_button.connect("clicked", self.controller.request_stop)
        self.stop_button.set_sensitive(False)
        action_row.pack_start(self.stop_button, False, False, 0)
        self.progress = Gtk.ProgressBar()
        self.progress.set_hexpand(True)
        self.progress.set_show_text(True)
        self.progress.set_text("Ready")
        action_row.pack_start(self.progress, True, True, 8)
        root.pack_start(action_row, False, False, 0)

        expander = Gtk.Expander(label="Operation log")
        expander.set_expanded(True)
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.set_min_content_height(150)
        self.log_view = Gtk.TextView()
        self.log_view.set_editable(False)
        self.log_view.set_cursor_visible(False)
        self.log_view.set_monospace(True)
        self.log_buffer = self.log_view.get_buffer()
        scroll.add(self.log_view)
        expander.add(scroll)
        root.pack_start(expander, False, True, 0)

        self.status_label = Gtk.Label(label="Ready")
        self.status_label.set_xalign(0)
        self.status_label.get_style_context().add_class("dim-label")
        root.pack_start(self.status_label, False, False, 0)

    def _build_menu_bar(self) -> Gtk.MenuBar:
        menu_bar = Gtk.MenuBar()
        file_item = Gtk.MenuItem.new_with_mnemonic("_File")
        file_menu = Gtk.Menu()
        file_item.set_submenu(file_menu)

        new_window_item = Gtk.MenuItem.new_with_mnemonic("_New window")
        new_window_item.connect(
            "activate",
            lambda _item: self.window.get_application().new_window(),
        )
        file_menu.append(new_window_item)
        open_item = Gtk.MenuItem.new_with_mnemonic("_Open image…")
        open_item.connect("activate", self.controller.open_image)
        file_menu.append(open_item)
        refresh_item = Gtk.MenuItem.new_with_mnemonic("_Refresh volumes")
        refresh_item.connect(
            "activate",
            lambda _item: self.controller.refresh_devices(clear_cache=True),
        )
        file_menu.append(refresh_item)
        file_menu.append(Gtk.SeparatorMenuItem())
        quit_item = Gtk.MenuItem.new_with_mnemonic("_Quit")
        quit_item.connect("activate", lambda _item: self.window.close())
        file_menu.append(quit_item)

        about_item = Gtk.MenuItem.new_with_mnemonic("_About")
        about_menu = Gtk.Menu()
        about_item.set_submenu(about_menu)
        about_dialog_item = Gtk.MenuItem.new_with_label("About Linux Defragger")
        about_dialog_item.connect("activate", lambda _item: self.show_about())
        about_menu.append(about_dialog_item)
        menu_bar.append(file_item)
        menu_bar.append(about_item)
        return menu_bar

    @staticmethod
    def _draw_swatch(
        _widget: Gtk.Widget,
        cr: Any,
        colour: tuple[float, float, float],
    ) -> bool:
        cr.set_source_rgb(*colour)
        cr.rectangle(0, 0, 15, 15)
        cr.fill()
        return False

    def _load_css(self) -> None:
        css = b"""
        .summary-title { color: #68717d; font-size: 10pt; }
        .summary-value { font-size: 15pt; font-weight: bold; }
        .dim-label { color: #68717d; }
        button.suggested-action { font-weight: bold; }
        """
        provider = Gtk.CssProvider()
        provider.load_from_data(css)
        screen = Gdk.Screen.get_default()
        if screen is not None:
            Gtk.StyleContext.add_provider_for_screen(
                screen,
                provider,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
            )
        self.defrag_button.get_style_context().add_class("suggested-action")

    def populate_volumes(self, names: list[str], active_index: int) -> None:
        self.device_combo.remove_all()
        for name in names:
            self.device_combo.append_text(name)
        self.device_combo.set_active(active_index)

    def append_log(self, text: str) -> None:
        if not text:
            return
        end = self.log_buffer.get_end_iter()
        self.log_buffer.insert(end, text if text.endswith("\n") else text + "\n")
        mark = self.log_buffer.create_mark(
            None, self.log_buffer.get_end_iter(), False
        )
        self.log_view.scroll_mark_onscreen(mark)
        self.log_buffer.delete_mark(mark)

    def clear_log(self) -> None:
        self.log_buffer.set_text("")

    def show_error(self, title: str, message: str) -> None:
        dialog = Gtk.MessageDialog(
            transient_for=self.window,
            modal=True,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.CLOSE,
            text=title,
        )
        dialog.format_secondary_text(message)
        dialog.run()
        dialog.destroy()

    def confirm(self, title: str, message: str) -> bool:
        dialog = Gtk.MessageDialog(
            transient_for=self.window,
            modal=True,
            message_type=Gtk.MessageType.WARNING,
            buttons=Gtk.ButtonsType.CANCEL,
            text=title,
        )
        dialog.format_secondary_text(message)
        dialog.add_button("Proceed", Gtk.ResponseType.OK)
        response = dialog.run()
        dialog.destroy()
        return response == Gtk.ResponseType.OK

    def choose_image(self) -> str | None:
        chooser = Gtk.FileChooserDialog(
            title="Open filesystem image",
            transient_for=self.window,
            action=Gtk.FileChooserAction.OPEN,
        )
        chooser.add_buttons(
            Gtk.STOCK_CANCEL,
            Gtk.ResponseType.CANCEL,
            Gtk.STOCK_OPEN,
            Gtk.ResponseType.OK,
        )
        response = chooser.run()
        filename = (
            chooser.get_filename() if response == Gtk.ResponseType.OK else None
        )
        chooser.destroy()
        return filename


    def show_about(self) -> None:
        dialog = Gtk.AboutDialog(transient_for=self.window, modal=True)
        dialog.set_program_name(APP_NAME)
        dialog.set_version(self.gui_version)
        dialog.set_logo_icon_name(APP_ICON_NAME)
        dialog.set_comments(f"{ABOUT_COMMENTS}\n\nBuild: {self.build_label}")
        dialog.set_authors(["Shannon Smith — Author and project maintainer"])
        dialog.set_website(PROJECT_URL)
        dialog.set_website_label("Website")
        dialog.set_copyright(COPYRIGHT)
        dialog.set_license(ABOUT_LICENSE)
        dialog.set_wrap_license(True)
        dialog.run()
        dialog.destroy()

    def reset_summary(self) -> None:
        self.capacity_card.set_title("Capacity")
        self.free_card.set_title("Free space")
        self.files_card.set_title("Files")
        self.fragmented_card.set_title("Fragmentation")
        for card in (
            self.capacity_card,
            self.free_card,
            self.files_card,
            self.fragmented_card,
        ):
            card.set_value("—")
        self.map_caption.set_text(
            "Allocation grid · detail increases with the available drawing area."
        )

    def apply_map_presentation(self, presentation: MapPresentation) -> None:
        self.disk_map.set_cells(presentation.cells)
        self.capacity_card.set_title(presentation.capacity_title)
        self.capacity_card.set_value(presentation.capacity_value)
        self.free_card.set_title(presentation.free_title)
        self.free_card.set_value(presentation.free_value)
        self.files_card.set_title(presentation.files_title)
        self.files_card.set_value(presentation.files_value)
        self.fragmented_card.set_title(presentation.fragmentation_title)
        self.fragmented_card.set_value(presentation.fragmentation_value)
        self.disk_map.set_unit_label(presentation.unit_label)
        self.map_caption.set_text(presentation.caption)
        self.status_label.set_text(presentation.status)

    def set_control_state(self, state: ControlState) -> None:
        self.refresh_button.set_sensitive(state.refresh)
        self.image_button.set_sensitive(state.refresh)
        self.device_combo.set_sensitive(state.select_device)
        self.analyze_button.set_sensitive(state.analyse)
        self.unmount_button.set_sensitive(state.unmount)
        self.defrag_button.set_sensitive(state.defrag)
        self.growth_button.set_sensitive(state.growth_defrag)
        self.recover_button.set_sensitive(state.recover)
        self.stop_button.set_sensitive(state.stop)

    def set_operation_tooltips(self, tooltips: dict[str, str]) -> None:
        self.defrag_button.set_tooltip_text(tooltips["defrag"])
        self.growth_button.set_tooltip_text(tooltips["growth-defrag"])
        self.recover_button.set_tooltip_text(tooltips["recover"])
