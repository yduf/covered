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

# ── Constants ─────────────────────────────────────────────────────────────────

XATTR_NAME   = b"user.covered"
COLORS_D_DIR = "/usr/share/folder-color-switcher/colors.d"

# Map xattr value → emblem icon name (None = no emblem)
EMBLEM_MAP = {
    b"covered":   "emblem-default",    # green check
    b"uncovered": "emblem-important",  # red exclamation
    b"partial":   "emblem-new",        # orange/yellow star
    b"empty":     None,                # no emblem for empty dirs
}

# Map xattr value → human-readable color name as it appears in colors.d JSON.
# "Aqua" is the closest available Mint-X color to cyan.
STATE_COLOR_MAP = {
    b"covered":   "Green",
    b"uncovered": "Red",
    b"partial":   "Orange",
    b"empty":     "Aqua",
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
    Nemo.NameAndDescProvider,
):
    """
    Reads user.covered xattr and:
    - Adds a coloured emblem to files and folders.
    - Sets a coloured folder icon for directories via metadata::custom-icon,
      using the color-variant icon themes from nemo-folder-color-switcher.
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

    # ── Nemo.InfoProvider ───────────────────────────────────────────────────

    def update_file_info(self, file):
        if file.get_uri_scheme() != "file":
            return Nemo.OperationResult.COMPLETE

        uri  = file.get_uri()
        path = urllib.parse.unquote(uri[7:])   # strip "file://"

        val = _get_covered_xattr(path)
        if val is None:
            return Nemo.OperationResult.COMPLETE

        # Emblem (files and dirs)
        emblem = EMBLEM_MAP.get(val)
        if emblem:
            file.add_emblem(emblem)

        # Coloured folder icon (dirs only)
        if file.is_directory():
            color_name = STATE_COLOR_MAP.get(val)
            icon_uri   = self._get_folder_icon_uri(color_name) if color_name else None

            # Only write GIO metadata when value changed (breaks update loop)
            if _icon_cache.get(uri) != icon_uri:
                _set_custom_icon(file.get_location(), icon_uri)
                _icon_cache[uri] = icon_uri

        return Nemo.OperationResult.COMPLETE

    def get_name_and_desc(self):
        return ["nemo-covered:::Color files/folders by backup coverage (user.covered xattr)"]
