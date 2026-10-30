#!/usr/bin/env python3
"""Generate Legacy module_strings.h without the obsolete Python 2 pipeline."""
import re
from pathlib import Path

module_dir = Path.cwd()
readme = (module_dir / 'README.rst').read_bytes().decode('utf-8')
lines = readme.replace('\r\n', '\n').splitlines()
title = lines[0].strip()
body = '\n'.join(lines[2:]).strip()
body = re.sub(r'^\.\..*$', '', body, flags=re.MULTILINE)
body = re.sub(r'^:([^:]+):.*$', '', body, flags=re.MULTILINE)
body = re.sub(r'\n{3,}', '\n\n', body).strip()

def c_string(value):
    return value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')

entries = [
    ('Name', title),
    ('Description', body),
]
output = []
for index, (name, value) in enumerate(entries):
    suffix = chr(ord('a') + index)
    output.append('static char __module_string_%s_name [] MODULE_STRINGS_SECTION = "%s";' % (suffix, c_string(name)))
    output.append('static char __module_string_%s_value[] MODULE_STRINGS_SECTION = "%s";' % (suffix, c_string(value)))
output.append('')
output.append('#define MODULE_STRINGS() \\')
output.append('  MODULE_STRINGS_START() \\')
for index, _ in enumerate(entries):
    suffix = chr(ord('a') + index)
    output.append('    MODULE_STRING(__module_string_%s_name, __module_string_%s_value) \\' % (suffix, suffix))
output.append('  MODULE_STRINGS_END()')
print('\n'.join(output))
