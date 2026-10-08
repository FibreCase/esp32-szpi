"""Recompile the actual IDF callbacks and check GCC's static stack budgets.
Usage: python3 tests/check_wifi_callback_stack.py build/compile_commands.json
Outputs are confined to /tmp; no configuration or SDK files are edited.
"""
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

entries = json.loads(pathlib.Path(sys.argv[1]).read_text())
files = {"szpi_wifi.c": {"wifi_event_handler"}, "wifi_service.c": {"szpi_wifi_post_event"}}
with tempfile.TemporaryDirectory(prefix="szpi-stack-") as temporary:
    for filename, functions in files.items():
        entry = next(e for e in entries if pathlib.Path(e["file"]).name == filename)
        arguments = entry.get("arguments") or shlex.split(entry["command"])
        if pathlib.Path(arguments[0]).name == "ccache":
            arguments = arguments[1:]
        clean = []
        skip = False
        for argument in arguments:
            if skip:
                skip = False
                continue
            if argument in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
            elif argument not in ("-MD", "-MMD", "-MP"):
                clean.append(argument)
        output = pathlib.Path(temporary) / (filename + ".o")
        subprocess.run(clean + ["-fstack-usage", "-o", str(output)], cwd=entry["directory"], check=True)
        observed = {}
        for line in output.with_suffix(".su").read_text().splitlines():
            label, size, kind = line.split("\t")
            function = label.rsplit(":", 1)[-1]
            if function in functions:
                observed[function] = int(size)
                assert kind == "static", (function, kind)
                assert int(size) <= 160, (function, size)
        assert functions <= observed.keys(), (filename, observed)
        for function, size in observed.items():
            print(f"{function}: {size} bytes static frame (budget <=160): PASS")
