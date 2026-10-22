#!/usr/bin/env python3
"""Regenerate owl_energy_display/web_console_page.h from esp_socket's html/index.html.

This project removes the Update and Provision actions from the esp_socket web console's
dropdown (neither is implemented here -- see handleSocketMessage() in the sketch), while
keeping Runtime and Reboot. Run this again if esp_socket's own page is ever updated
upstream and you want to pick up the rest of the change.

Usage:
    python3 scripts/gen_web_console_page.py [path to esp_socket's html/index.html]

With no argument, it looks for esp_socket's html/index.html next to where ESPSOCKET is
installed in your Arduino libraries folder (Windows default shown; edit SOURCE_GUESSES to
match your setup if that's not where yours is).
"""
import os
import sys

OUT = os.path.join(os.path.dirname(__file__), "..", "owl_energy_display", "web_console_page.h")

SOURCE_GUESSES = [
    os.path.expanduser("~/Documents/Arduino/libraries/ESPSOCKET/html/index.html"),
    os.path.expanduser("~/OneDrive/Documents/Arduino/libraries/ESPSOCKET/html/index.html"),
]

REMOVE_LINES = (
    '            <option value="update" class="pointer">Update</option>\n',
    '            <option value="provision" class="pointer">Provision</option>\n',
)
KEEP_MARKERS = ('value="runtime"', 'value="reboot"')


def find_source():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for guess in SOURCE_GUESSES:
        if os.path.isfile(guess):
            return guess
    sys.exit(
        "Couldn't find esp_socket's html/index.html. Pass its path as an argument, e.g.\n"
        '  python3 scripts/gen_web_console_page.py "C:\\path\\to\\ESPSOCKET\\html\\index.html"'
    )


def main():
    src = find_source()
    with open(src, "r", encoding="utf-8") as f:
        html = f.read()

    removed = 0
    for line in REMOVE_LINES:
        if line in html:
            html = html.replace(line, "")
            removed += 1
    if removed != len(REMOVE_LINES):
        sys.exit(
            f"Expected to remove {len(REMOVE_LINES)} <option> lines but removed {removed} -- "
            "esp_socket's page.html has likely changed; check REMOVE_LINES above still match."
        )
    for marker in KEEP_MARKERS:
        assert marker in html, f"expected to still find {marker!r} in the page"
    assert 'value="update"' not in html and 'value="provision"' not in html

    data = html.encode("utf-8")

    header = [
        "// Generated from esp_socket's html/index.html (https://github.com/RobertByrnes/esp_socket),",
        "// with the Update and Provision actions removed from the dropdown for this project --",
        "// neither is implemented here, and leaving them visible invited trying them. Runtime and",
        "// Reboot remain, and are implemented in handleSocketMessage() in owl_energy_display.ino.",
        "// Regenerate with scripts/gen_web_console_page.py if esp_socket's page is ever updated.",
        "#ifndef OWL_WEB_CONSOLE_PAGE_H",
        "#define OWL_WEB_CONSOLE_PAGE_H",
        "",
        "#ifndef Arduino_h",
        "  #include <Arduino.h>",
        "#endif",
        "",
    ]

    hex_bytes = [f"0x{b:02X}" for b in data]
    array_lines = ["const char owl_web_console_html[] PROGMEM = {"]
    for i in range(0, len(hex_bytes), 16):
        chunk = hex_bytes[i : i + 16]
        sep = "," if i + 16 < len(hex_bytes) else ""
        array_lines.append("  " + ", ".join(chunk) + sep)
    array_lines.append("};")
    array_lines.append("")
    array_lines.append(f"const size_t owl_web_console_html_len = {len(data)};")
    array_lines.append("")
    array_lines.append("#endif  // OWL_WEB_CONSOLE_PAGE_H")

    out_path = os.path.normpath(OUT)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(header + array_lines) + "\n")

    print(f"wrote {out_path}: {len(data)} bytes of HTML, {removed} <option> lines removed")


if __name__ == "__main__":
    main()
