#!/usr/bin/env python3
"""Verify actual native-frustum control preserves stereo boundary planes."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/src/gpu/gl_backend.c').read_text()
a = source.index('static void immersive_native_caps(')
function = source[a:source.index('static void emit_eyes(', a)]
harness = r'''
#include <assert.h>
static int caps[4], calls;
static void set_cap(int index, int on) { caps[index] = !!on; calls++; }
'''
checks = r'''
int main(void) {
    for (int raw = 0; raw < 2; raw++)
        for (int one_pass = 0; one_pass < 2; one_pass++) {
            calls = 0;
            for (int i = 0; i < 4; i++) caps[i] = 1;
            immersive_native_caps(raw, one_pass, 0);
            assert(calls == 0);
            for (int i = 0; i < 4; i++) assert(caps[i] == 1);
            immersive_native_caps(raw, one_pass, 1);
            assert(calls == 4);
            assert(caps[0] == (!raw && one_pass));
            assert(caps[1] == 0 && caps[3] == 0);
            assert(caps[2] == (raw && one_pass));
        }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='immersive-caps-') as temp:
    temp = Path(temp)
    (temp / 'test.c').write_text(harness + function + checks)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(temp/'test.c'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test')],check=True)
print('native clipping: default untouched, raw/CPU one-pass eye boundaries preserved: PASS')
