"""Entry point: python -m rf_setup [--cli] [--firmware DIR]

Opens the setup window, or the guided command-line setup with --cli (also
used automatically when no display is available).
"""

import argparse
import os
import sys


def main() -> int:
    parser = argparse.ArgumentParser(prog="rf-setup", description="Set up an ESP32-S3 RF filter controller.")
    parser.add_argument("--cli", action="store_true", help="use the guided command-line setup")
    parser.add_argument("--firmware", help="folder holding the firmware, or an ESP-IDF build folder (default: next to the tool)")
    args = parser.parse_args()

    use_cli = args.cli or (os.name != "nt" and not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"))
    if not use_cli:
        try:
            from . import gui
            return gui.run(args.firmware)
        except ImportError:
            print("The window needs Tkinter, which is not installed; using the command-line setup.\n"
                  "On Debian or Ubuntu: sudo apt install python3-tk")
    from . import cli
    return cli.run(args.firmware)


if __name__ == "__main__":
    sys.exit(main())
