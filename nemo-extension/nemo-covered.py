#!/usr/bin/env python3
"""
nemo-covered.py – Nemo extension that colors files and folders
based on the user.covered xattr exposed by cover_fuse.

Folder coloring uses the same mechanism as nemo-folder-color-switcher:
it reads /usr/share/folder-color-switcher/colors.d/*.json to find the
color-variant icon theme for the current GTK theme, then sets
metadata::custom-icon to the folder icon from that variant theme.

Color mapping:
  covered   → Green  folder + emblem-default (green check)
  uncovered → Red    folder + emblem-important (red exclamation)
  partial   → Orange folder + emblem-new (orange star)
  empty     → Aqua   folder (no emblem – dir contains no files at all)
  error     → Grey   folder + emblem-unreadable (crossed circle – access error)

Delta (user.delta xattr) affects visual appearance:
  deleted   → file: additional emblem-unreadable; dir: dimmed color
  new       → file: additional emblem-new; dir: brightened color

Columns:
  Covered   → Coverage state
  Delta     → Update tracking state (new/deleted/unchanged)
  Backup    → Compact backup path for covered files
  Covered at → Full path of the file in the backup

Install:
  sudo cp nemo-covered.py /usr/share/nemo-python/extensions/
  nemo -q && nemo
"""

import json
import os
import urllib.parse

import gi
gi.require_version('Gtk', '3.0')

from gi.repository import GObject, GLib, Gio, Gtk, Nemo
import subprocess

# ── Constants ─────────────────────────────────────────────────────────────────

XATTR_NAME         = b"user.covered"
XATTR_DELTA        = b"user.delta"
XATTR_BACKUP       = b"user.covered_backup"
XATTR_COVERED_AT   = b"user.covered_at"
XATTR_COVERED_SRC  = b"user.covered_source"
COLORS_D_DIR       = "/usr/share/folder-color-switcher/colors.d"

# Map xattr value → emblem icon name (None = no emblem)
EMBLEM_MAP = {
    b"covered":   "emblem-default",     # green check
    b"uncovered": "emblem-important",   # red exclamation
    b"partial":   "emblem-new",         # orange/yellow star
    b"empty":     None,                 # no emblem for empty dirs
    b"error":     "emblem-unreadable",  # crossed circle for access errors
}

# Additional emblems for deltas
DELTA_FILE_EMBLEM = {
    b"deleted": "emblem-unreadable",    # crossed circle
    b"new":     "emblem-new",           # orange star
}

# Map xattr value → human-readable color name as it appears in colors.d JSON.
STATE_COLOR_MAP = {
    b"covered":   "Green",
    b"uncovered": "Red",
    b"partial":   "Orange",
    b"empty":     "Aqua",
    b"error":     "Grey",
}

# Dimmed color variants: we use darker/desaturated alternatives.
# For "dim" we map to "Grey" for all states except empty.
DIMMED_COLOR_MAP = {
    b"covered":   "Grey",
    b"uncovered": "Grey",
    b"partial":   "Grey",
    b"empty":     "Aqua",
    b"error":     "Grey",
}

# Brightened color variants: use lighter alternatives.
BRIGHTENED_COLOR_MAP = {
    b"covered":   "Aqua",
    b"uncovered": "Orange",
    b"partial":   "Yellow",
    b"empty":     "Aqua",
    b"error":     "Orange",
}

# Per-session cache: file URI → last icon URI written to GIO metadata.
# Prevents the metadata-change → update_file_info → metadata-change loop.
_icon_cache: dict = {}


# ── Helpers ──────────────────────────────────────────────────────────────────

def _get_covered_xattr(path: str):
    """Return the raw bytes value of user.covered, or None."""
    try:
        return os.getxattr(path, XATTR_NAME)
    except (OSError, AttributeError):
        return None


def _get_xattr(path: str, name: bytes):
    """Return the raw bytes value of the given xattr, or None."""
    try:
        return os.getxattr(path, name)
    except (OSError, AttributeError):
        return None


def _set_custom_icon(location: Gio.File, icon_uri) -> None:
    """Write (or clear) metadata::custom-icon on a Gio.File location."""
    try:
        if icon_uri:
            location.set_attribute_string("metadata::custom-icon", icon_uri, 0, None)
        else:
            location.set_attribute(
                "metadata::custom-icon",
                Gio.FileAttributeType.INVALID,
                0, 0, None,
            )
    except Exception:
        pass


# ── Extension ─────────────────────────────────────────────────────────────────

class CoveredExtension(
    GObject.GObject,
    Nemo.InfoProvider,
    Nemo.ColumnProvider,
    Nemo.MenuProvider,
    Nemo.NameAndDescProvider,
):
    """
    Reads user.covered and user.delta xattrs and:
    - Adds a coloured emblem to files and folders.
    - Adds delta emblems for new/deleted files.
    - Sets a coloured folder icon for directories via metadata::custom-icon,
      using the color-variant icon themes from nemo-folder-color-switcher.
    - Dims or brightens folder colors based on delta.
    """

    def __init__(self):
        # Load color styles from folder-color-switcher's JSON catalogue.
        # Keys are GTK icon theme names (e.g. "Mint-X"); values are style dicts.
        self._styles: dict = {}
        if os.path.exists(COLORS_D_DIR):
            for fname in sorted(os.listdir(COLORS_D_DIR)):
                if not fname.endswith(".json"):
                    continue
                try:
                    with open(os.path.join(COLORS_D_DIR, fname)) as fh:
                        data = json.load(fh)
                        for style in data.get("styles", []):
                            self._styles[style["name"]] = style
                except Exception:
                    pass

        # Secondary cache: (gtk_theme, color_name, icon_size) → icon_uri | None
        self._resolved: dict = {}

    # ── folder-color lookup ─────────────────────────────────────────────────

    def _get_folder_icon_uri(self, color_name: str, size: int = 48) -> str | None:
        """
        Return a file:// URI for a color-variant folder icon matching
        the current GTK icon theme, or None if unsupported.
        """
        gtk_theme = Gtk.Settings.get_default().get_property("gtk-icon-theme-name")
        cache_key = (gtk_theme, color_name, size)
        if cache_key in self._resolved:
            return self._resolved[cache_key]

        uri = None
        style = self._styles.get(gtk_theme)
        if style:
            # Find the theme variant for the requested color name
            for entry in style.get("icon-themes", []):
                if entry.get("name") == color_name:
                    variant_theme = entry["theme"]
                    t = Gtk.IconTheme.new()
                    t.set_custom_theme(variant_theme)
                    info = t.choose_icon_for_scale(["folder", None], size, 1, 0)
                    if info:
                        filename = info.get_filename()
                        if filename:
                            uri = GLib.filename_to_uri(filename, None)
                    break

        self._resolved[cache_key] = uri
        return uri

    # ── Nemo.ColumnProvider ─────────────────────────────────────────────────

    def get_columns(self):
        col_covered = Nemo.Column(
            name="NemoCovered::covered_state",
            attribute="covered_state",
            label="Covered",
            description="Backup coverage state (covered/uncovered/partial/empty/error)",
        )
        col_delta = Nemo.Column(
            name="NemoCovered::delta_state",
            attribute="delta_state",
            label="Delta",
            description="Update tracking state (new/deleted/unchanged)",
        )
        col_backup = Nemo.Column(
            name="NemoCovered::backup",
            attribute="covered_backup",
            label="Backup",
            description="Compact backup path where this file was found",
        )
        col_covered_at = Nemo.Column(
            name="NemoCovered::covered_at",
            attribute="covered_at",
            label="Covered at",
            description="Full path to the matched file in the backup",
        )
        return [col_covered, col_delta, col_backup, col_covered_at]

    # ── Nemo.InfoProvider ───────────────────────────────────────────────────

    def update_file_info(self, file):
        if file.get_uri_scheme() != "file":
            return Nemo.OperationResult.COMPLETE

        uri  = file.get_uri()
        path = urllib.parse.unquote(uri[7:])   # strip "file://"

        val = _get_covered_xattr(path)
        if val is None:
            return Nemo.OperationResult.COMPLETE

        delta_val = _get_xattr(path, XATTR_DELTA)

        # ── Emblems ───────────────────────────────────────────────────────

        # Primary emblem (coverage)
        emblem = EMBLEM_MAP.get(val)
        if emblem:
            file.add_emblem(emblem)

        # Delta emblem for files
        if not file.is_directory() and delta_val:
            delta_emblem = DELTA_FILE_EMBLEM.get(delta_val)
            if delta_emblem:
                file.add_emblem(delta_emblem)

        # ── Coloured folder icon (dirs only) ─────────────────────────────

        if file.is_directory():
            # Determine effective color name based on coverage + delta
            if delta_val == b"deleted":
                color_name = DIMMED_COLOR_MAP.get(val, "Grey")
            elif delta_val == b"new":
                color_name = BRIGHTENED_COLOR_MAP.get(val)
            else:
                color_name = STATE_COLOR_MAP.get(val)

            icon_uri = self._get_folder_icon_uri(color_name) if color_name else None

            # Only write GIO metadata when value changed (breaks update loop)
            if _icon_cache.get(uri) != icon_uri:
                _set_custom_icon(file.get_location(), icon_uri)
                _icon_cache[uri] = icon_uri

        # ── Custom columns ────────────────────────────────────────────────

        covered_label = val.decode("utf-8")
        file.add_string_attribute("covered_state", covered_label)

        if delta_val:
            delta_label = delta_val.decode("utf-8")
        else:
            delta_label = "unchanged"
        file.add_string_attribute("delta_state", delta_label)

        # Backup columns: only for covered files (not dirs)
        if not file.is_directory() and val == b"covered":
            backup = _get_xattr(path, XATTR_BACKUP)
            if backup:
                file.add_string_attribute("covered_backup", backup.decode("utf-8"))

            covered_at = _get_xattr(path, XATTR_COVERED_AT)
            if covered_at:
                file.add_string_attribute("covered_at", covered_at.decode("utf-8"))

        return Nemo.OperationResult.COMPLETE

    # ── Nemo.MenuProvider ───────────────────────────────────────────────────

    def get_file_items(self, window, files):
        """Add context menu items for any item under the FUSE mount."""
        if len(files) != 1:
            return
        f = files[0]
        if f.get_uri_scheme() != "file":
            return

        path = urllib.parse.unquote(f.get_uri()[7:])
        val = _get_covered_xattr(path)
        if val is None:
            return

        items = []

        # "Open original containing folder" — works for files and directories
        source_path = _get_xattr(path, XATTR_COVERED_SRC)
        if source_path:
            src_path = source_path.decode("utf-8")
            if f.is_directory():
                # For a directory, open the directory itself
                target = src_path
            else:
                # For a file, open its containing folder
                target = os.path.dirname(src_path)
            item_source = Nemo.MenuItem(
                name="NemoCovered::open_source_folder",
                label="Open original containing folder",
                tip="Open the source folder holding this item on the original filesystem",
            )
            item_source.connect("activate", self._open_folder, target)
            items.append(item_source)

        # "Open containing backup folder" — only for covered files (not dirs)
        if not f.is_directory() and val == b"covered":
            covered_at = _get_xattr(path, XATTR_COVERED_AT)
            if covered_at:
                backup_file_path = covered_at.decode("utf-8")
                item_backup = Nemo.MenuItem(
                    name="NemoCovered::open_backup_folder",
                    label="Open containing backup folder",
                    tip="Open the backup folder containing this file and select it",
                )
                item_backup.connect("activate", self._open_folder, backup_file_path)
                items.append(item_backup)

        if not items:
            return None

        # Group all items under a "Covered ☂️" submenu
        submenu = Nemo.Menu()
        for item in items:
            submenu.append_item(item)

        parent_item = Nemo.MenuItem(
            name="NemoCovered::submenu",
            label="Covered ☂️",
            tip="Covered backup coverage actions",
        )
        parent_item.set_submenu(submenu)

        return [parent_item]

    def _open_folder(self, menu_item, folder_path):
        """Open Nemo showing the given folder."""
        subprocess.Popen(
            ["nemo", "--no-desktop", folder_path],
            start_new_session=True,
        )

    def get_name_and_desc(self):
        return ["nemo-covered:::Color files/folders by backup coverage (user.covered xattr)"]