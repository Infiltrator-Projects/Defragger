#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Linux Defragger
# Author: Shannon Smith
# Purpose: Compose the GTK view, volume model, command runner and worker events.

"""Top-level GTK application composition.

``MainWindow`` coordinates typed services.  Widget construction, volume
collection policy, operation validation, subprocess ownership, privilege IPC,
and live-map interpretation are implemented by dedicated modules.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    import gi

    gi.require_version("Gtk", "3.0")
    gi.require_version("Gdk", "3.0")
    from gi.repository import Gdk, GLib, Gtk
except (ImportError, ValueError) as exc:
    print(
        "Linux Defragger requires GTK 3 Python bindings.\n"
        "Install them on Linux Mint with:\n"
        "  sudo apt install python3-gi python3-cairo gir1.2-gtk-3.0",
        file=sys.stderr,
    )
    raise SystemExit(1) from exc

from version import BUILD_LABEL, VERSION
from backends.base import CAP_DEFRAG, CAP_GROWTH_DEFRAG, CAP_RECOVER

from .command_runner import CommandRunner
from .devices import Volume
from .engine_client import (
    load_backend_catalog,
    query_engine_version,
)
from .live_controller import LiveEventController
from .operation_coordinator import OperationCoordinator
from .operation_planner import (
    control_state,
    operation_tooltips,
)
from .operation_presenter import OperationPresenter
from .support import (
    find_mapper,
    find_operation_engine,
    find_privileged_helper,
    state_dir,
)
from .volume_coordinator import VolumeCoordinator
from .widgets import MAX_MAP_CELLS, MIN_MAP_CELLS
from .window_view import APP_NAME, WindowView


class MainWindow(Gtk.ApplicationWindow):
    """Compose independent GUI, storage, runner, and protocol components."""

    def __init__(self, application: Gtk.Application) -> None:
        super().__init__(application=application, title=f"{APP_NAME} {VERSION}")
        self.set_default_size(1050, 760)
        self.set_position(Gtk.WindowPosition.CENTER)

        self.mapper = find_mapper()
        self.operation_engine = find_operation_engine()
        self.privileged_helper = find_privileged_helper()
        self.backend_catalog = load_backend_catalog(self.mapper)
        self.engine_version = query_engine_version(self.operation_engine)

        self.volumes = VolumeCoordinator(self.backend_catalog)

        self.view = WindowView(
            self,
            self,
            gui_version=VERSION,
            engine_version=self.engine_version,
            build_label=BUILD_LABEL,
        )
        self.live_events = LiveEventController()
        self.runner = CommandRunner(
            mapper=self.mapper,
            operation_engine=self.operation_engine,
            privileged_helper=self.privileged_helper,
            on_event=lambda event: self.operations.on_runner_event(event),
            scheduler=GLib.idle_add,
        )
        self.operations = OperationPresenter(
            view=self.view,
            runner=self.runner,
            live_events=self.live_events,
            map_provider=lambda: self.coordinator.map_data,
            scheduler=GLib.timeout_add,
            cancel_scheduled=GLib.source_remove,
            controls_changed=self.update_controls,
        )
        self.coordinator = OperationCoordinator(
            view=self.view,
            volumes=self.volumes,
            catalog=self.backend_catalog,
            runner=self.runner,
            presenter=self.operations,
            mapper=self.mapper,
            operation_engine=self.operation_engine,
            state_directory=state_dir,
            minimum_cells=MIN_MAP_CELLS,
            maximum_cells=MAX_MAP_CELLS,
        )

        self.connect(
            "delete-event",
            lambda *_args: self.operations.defer_close_while_busy(),
        )
        self.connect("destroy", lambda *_args: self.runner.shutdown())
        self.refresh_devices()
        GLib.timeout_add(150, self._authenticate_on_launch)

    @property
    def current_volume(self) -> Volume | None:
        return self.volumes.current

    @property
    def busy(self) -> bool:
        return self.runner.busy

    def append_log(self, text: str) -> None:
        self.view.append_log(text)

    def clear_log(self) -> None:
        self.view.clear_log()

    def show_error(self, title: str, message: str) -> None:
        self.view.show_error(title, message)

    def confirm(self, title: str, message: str) -> bool:
        return self.view.confirm(title, message)

    def _authenticate_on_launch(self) -> bool:
        self.runner.authenticate()
        return False

    def create_fragmented_test_data(self, _item: Gtk.MenuItem) -> None:
        folder = self.view.choose_test_folder()
        if not folder:
            return
        if not self.confirm(
            "Create deliberately fragmented test data?",
            f"Linux Defragger will create and delete test files inside:\n{folder}\n\n"
            "Use an empty folder on a disposable test volume. Existing files "
            "outside that folder are not touched.",
        ):
            return
        self.clear_log()
        self.append_log(f"Creating fragmented test data in {folder}…")
        self.coordinator.run_command(
            ["/usr/bin/linux-defragger-testdata", folder],
            privileged=False,
            purpose="test-data",
        )

    def refresh_devices(
        self,
        preserve_path: str | None = None,
        clear_cache: bool = False,
    ) -> None:
        if self.busy:
            return
        try:
            active = self.volumes.refresh(
                preserve_path=preserve_path,
                clear_cache=clear_cache,
            )
        except Exception as exc:
            self.show_error("Unable to enumerate storage devices", str(exc))
            return
        self.view.populate_volumes(
            [volume.display_name for volume in self.volumes.volumes],
            active,
        )
        if not self.volumes.volumes:
            self.view.status_label.set_text(
                "No supported filesystems detected. Open an image or attach a "
                "supported volume."
            )
        self.update_controls()

    def on_device_changed(self, combo: Gtk.ComboBoxText) -> None:
        selection = self.volumes.select(combo.get_active())
        self.view.reset_summary()
        volume = selection.volume
        if volume is None:
            self.coordinator.reset_map()
            self.update_controls()
            return
        cached = selection.cached_map
        if cached is not None:
            self.coordinator.use_cached_map(cached)
            self.view.status_label.set_text(
                self.view.status_label.get_text() + " · cached analysis"
            )
        else:
            self.coordinator.reset_map()
            self.view.status_label.set_text(
                volume.display_name + " · analysing…"
            )
            GLib.idle_add(self._auto_analyse_selected, volume.path)
        self.update_controls()

    def _auto_analyse_selected(self, selected_path: str) -> bool:
        if (
            self.current_volume is not None
            and self.current_volume.path == selected_path
            and self.volumes.cached_map() is None
            and not self.busy
        ):
            self.analyze(clear_log=True)
        return False

    def open_image(self, _button: Gtk.Widget) -> None:
        filename = self.view.choose_image()
        if not filename:
            return
        try:
            volume = self.volumes.open_image(filename)
        except Exception as exc:
            self.show_error("Unable to open filesystem image", str(exc))
            return
        self.refresh_devices(volume.path)

    def unmount_selected(self, _button: Gtk.Button) -> None:
        volume = self.current_volume
        if not volume or volume.image or not volume.mounted:
            return
        self.clear_log()
        self.append_log(f"Unmounting {volume.path} through udisksctl…")
        self.coordinator.run_command(
            ["udisksctl", "unmount", "-b", volume.path],
            privileged=True,
            purpose="unmount",
            on_success=lambda _output: self.refresh_devices(volume.path),
        )

    def journal_path(self) -> str:
        return self.coordinator.journal_path

    def on_map_size_allocate(
        self,
        _widget: Gtk.Widget,
        _allocation: Gdk.Rectangle,
    ) -> None:
        if self.coordinator.map_data:
            self.view.disk_map.queue_draw()

    def analyze(
        self,
        clear_log: bool = True,
        target_cells: int | None = None,
        quiet: bool = False,
    ) -> None:
        self.coordinator.analyze(
            clear_log=clear_log,
            target_cells=target_cells,
            quiet=quiet,
        )

    def start_mutation(self, operation: str) -> None:
        self.coordinator.start_mutation(operation)

    def request_stop(self, _button: Gtk.Button) -> None:
        self.operations.request_stop()

    def update_controls(self) -> None:
        volume = self.current_volume
        mutation_backend = bool(
            volume
            and volume.capabilities
            & (CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER)
        )
        journal_exists = bool(
            mutation_backend
            and volume
            and Path(self.coordinator.journal_path).exists()
        )
        state = control_state(
            volume,
            busy=self.busy,
            stop_requested=self.runner.stop_requested,
            journal_exists=journal_exists,
        )
        self.view.set_control_state(state)
        self.view.set_operation_tooltips(
            dict(operation_tooltips(volume, self.backend_catalog))
        )
