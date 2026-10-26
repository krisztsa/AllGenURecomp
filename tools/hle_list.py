"""Writes runtime/hle/hle_symbols.txt: every HLE_SDK symbol in runtime/hle/*.c that the SDK module defines.

Usage: hle_list.py <sdk nso>
"""
import glob
import os
import re
import sys

from nso import Nso

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

have = {s[0] for s in Nso(sys.argv[1]).syms if s[2]}
want = []
for p in sorted(glob.glob(os.path.join(ROOT, 'runtime', '**', '*.c'), recursive=True)):
    want += re.findall(r'HLE_SDK\("([^"]+)"', open(p).read())
missing = [w for w in want if w not in have]
if missing:
    print('not in the SDK (imports only):', ' '.join(missing))
with open(os.path.join(ROOT, 'runtime', 'hle', 'hle_symbols.txt'), 'w', newline='\n') as f:
    f.write('# SDK functions replaced by host implementations (HLE_SDK in runtime/**/*.c; tools/hle_list.py)\n')
    f.write(''.join(w + '\n' for w in want if w in have))
print(f'{len(want) - len(missing)} SDK functions replaced')
