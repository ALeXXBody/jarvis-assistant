Import("env")  # noqa  (PlatformIO pre-script)

"""Patch pioarduino's espidf builder for the esp_hosted component.

The component registers a forced include with CMake's SHELL: prefix:

    target_compile_options(eh_host_config INTERFACE
        "SHELL:-include eh_host_port_master_config.h")

CMake/Ninja keep that option intact (the compile_commands.json entry is
correct), but PlatformIO's builder re-parses each fragment with SCons's
ParseFlags, which converts "-include header_file" into a CCFLAGS tuple
('-include', File(...)) whose File node is treated as an extra *source
file*. Every translation unit then fails with:

    riscv32-esp-elf-gcc: fatal error: cannot specify '-o' with '-c' ...
    with multiple files

Fix: bypass SCons's flag parser for fragments that carry "-include" and
pass the tokens through as plain CCFLAGS strings instead. Plain string
tokens are emitted verbatim into the compile command line, keeping the
pair adjacent — exactly what gcc needs.

The patch touches only InstalledPlatforms pioarduino's local copy of
builder/frameworks/espidf.py and is idempotent, so it is safe to run on
every build.
"""
import os
import re

espidf_path = os.path.join(
    os.path.expanduser("~"), ".platformio", "platforms", "espressif32",
    "builder", "frameworks", "espidf.py")

MARK = "# [fix_esp_hosted] ParseFlags turns '-include file' into a"
patch_snippet = (
    "    " + MARK + "\n"
    "    if build_flags.lstrip().startswith(\"-include\"):\n"
    "        import shlex as _shlex\n"
    "        toks = _shlex.split(build_flags)\n"
    "        parsed = {\"CCFLAGS\": toks, \"CFLAGS\": [], \"CXXFLAGS\": []}\n"
    "        return parsed\n"
)

if not os.path.exists(espidf_path):
    print("(fix_esp_hosted: espidf.py not found at %s - skipping)" % espidf_path)
else:
    with open(espidf_path) as f:
        c = f.read()
    if MARK in c:
        print("fix_esp_hosted: builder already patched")
    else:
        anchor = "def parse_flag_extended(env, build_flags):\n    parsed = env.ParseFlags(build_flags)"
        if anchor not in c:
            print("fix_esp_hosted: WARNING: anchor missing in espidf.py;"
                  " builder layout changed, no patch applied")
        else:
            c = c.replace(anchor,
                          anchor.replace(
                              "    parsed = env.ParseFlags(build_flags)",
                              patch_snippet +
                              "    parsed = env.ParseFlags(build_flags)"))
            with open(espidf_path, "w") as f:
                f.write(c)
            print("fix_esp_hosted: patched builder parse_flag_extended")

# The e_hosted component's SHELL:-include stays as-is: correct for IDF's own
# ninja compilation of component sources; only the PIO-parsed TUs needed the
# workaround above.

