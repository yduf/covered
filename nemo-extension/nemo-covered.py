#!/usr/bin/env python3
"""
nemo-covered.py – Nemo extension that colors files and folders
based on the user.covered xattr exposed by cover_fuse.

Color scheme:
  covered   → green  (emblem-ok / emblem-checked)
  uncovered → red    (emblem-important)
  partial   → orange (emblem-new / emblem-generic)

Install:
  sudo cp nemo-covered.py /usr/share/nemo-python/extensions/
  Then restart Nemo: nemo -q && nemo
"""

import os
import urllib.parse

from gi.repository import GObject, Nemo

XATTR_NAME = b"user.covered"

# Map xattr value → emblem icon name (None = no emblem added)
EMBLEM_MAP = {
    b"covered":   "emblem-default",   # green check – theme provides this in green
    b"uncovered": "emblem-important",  # red/orange exclamation
    b"partial":   "emblem-new",        # yellow/orange star
    b"empty":     None,                # empty dir – no emblem (visually neutral)
}

# Map xattr value → color string (for NemoInfoProvider tag color hint)
# Nemo doesn't directly support foreground color via Python API, but we can
# use the emblem approach which is the standard supported mechanism.


def _get_covered_xattr(path: str):
    """Return the raw bytes value of user.covered, or None if not available."""
    try:
        val = os.getxattr(path, XATTR_NAME)
        return val
    except (OSError, AttributeError):
        return None


class CoveredExtension(
    GObject.GObject,
    Nemo.InfoProvider,
    Nemo.NameAndDescProvider,
):
    """
    Nemo InfoProvider: reads user.covered xattr from FUSE-mounted covered
    filesystem and adds a color emblem to files and folders.
    """

    def update_file_info(self, file):
        """Called by Nemo for each visible file/dir to let us annotate it."""
        # Only handle local file:// URIs
        if file.get_uri_scheme() != "file":
            return Nemo.OperationResult.COMPLETE

        uri = file.get_uri()
        path = urllib.parse.unquote(uri[7:])  # strip "file://"

        val = _get_covered_xattr(path)
        if val is None:
            return Nemo.OperationResult.COMPLETE

        emblem = EMBLEM_MAP.get(val)
        if emblem:
            file.add_emblem(emblem)

        return Nemo.OperationResult.COMPLETE

    def get_name_and_desc(self):
        return ["nemo-covered:::Color files based on backup coverage (user.covered xattr)"]
