"""ADBC driver for OpenLink Virtuoso.

Provides the ``connect`` entrypoint required by the ADBC driver manager
(``adbc_driver_manager`` / ``pyarrow``).  The entrypoint is registered
in ``pyproject.toml`` so that::

    import adbc_driver_manager
    with adbc_driver_manager.AdbcDatabase(driver="virtuoso") as db:
        db.set_options(uri="virtuoso://user:pass@host:port")
        ...

loads this driver from the installed wheel.
"""

import ctypes
import os
import pathlib
import sys
from typing import Any, Dict


def _driver_path() -> pathlib.Path:
    """Return the absolute path to the bundled libvirtadbc shared library."""
    here = pathlib.Path(__file__).resolve().parent

    # Platform-specific extension.
    if sys.platform == "darwin":
        ext = ".dylib"
    elif sys.platform == "win32":
        ext = ".dll"
    else:
        ext = ".so"

    candidate = here / f"libvirtadbc{ext}"
    if candidate.exists():
        return candidate

    # Fall back to a system-installed libvirtadbc (e.g. from make install).
    system_names = [f"libvirtadbc{ext}"]
    for name in system_names:
        try:
            return pathlib.Path(ctypes.util.find_library(name))
        except (OSError, TypeError):
            pass

    raise FileNotFoundError(
        "Cannot find libvirtadbc.  "
        "Install the driver (make install) or place the .so next to "
        f"__init__.py in {here}."
    )


def connect() -> Dict[str, Any]:
    """Return a driver-entry dictionary for the ADBC driver manager.

    This is the function registered as the ``virtuoso`` entry point in
    ``pyproject.toml``.  The driver manager calls it to discover the
    driver's ``AdbcDriverInit`` symbol and shared library path.
    """
    lib_path = _driver_path()

    return {
        "driver_library": str(lib_path),
        "entrypoint": "AdbcDriverInit",
        "driver_name": "ADBC Driver for Virtuoso",
        "driver_version": "0.0.1-dev",
    }
