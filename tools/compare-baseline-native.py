#!/usr/bin/env python3
"""Compare a separately stripped candidate libmain.so to the preserved APK.
Mismatch does not disprove gameplay parity; section equality is narrower than
binary reproduction. Never modifies or installs either artifact.
"""
import argparse
import hashlib
import json
import pathlib
import struct
import zipfile

def sha(data):
    return hashlib.sha256(data).hexdigest()

def sections(data):
    if data[:6]!=b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64')
    if struct.unpack_from('<H',data,18)[0]!=183:
        raise ValueError('Expected AArch64')
    offset=struct.unpack_from('<Q',data,40)[0]
    size,count,names_index=struct.unpack_from('<HHH',data,58)
    if size!=64 or not count or names_index>=count:
        raise ValueError('Unsupported section table')
    entries=[struct.unpack_from('<IIQQQQIIQQ',data,offset+i*size) for i in range(count)]
    names=entries[names_index]
    names_data=data[names[4]:names[4]+names[5]]
    result={}
    for entry in entries:
        name=names_data[entry[0]:].split(b'\0',1)[0].decode()
        if name in ('.text','.rodata','.data','.note.gnu.build-id'):
            content=data[entry[4]:entry[4]+entry[5]]
            if len(content)!=entry[5]:
                raise ValueError('Truncated section')
            result[name]={'bytes':len(content),'sha256':sha(content)}
    return result

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('candidate',type=pathlib.Path)
parser.add_argument('--output',type=pathlib.Path)
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
with zipfile.ZipFile(root/'validation/baseline/VirtuaCop3VR-working.apk') as archive:
    baseline=archive.read('lib/arm64-v8a/libmain.so')
candidate=args.candidate.read_bytes()
a,b=sections(baseline),sections(candidate)
report={'candidate':str(args.candidate.resolve()),'baseline_sha256':sha(baseline),
    'candidate_sha256':sha(candidate),'exact_native_match':baseline==candidate,
    'sections':{name:{'baseline':a.get(name),'candidate':b.get(name),
        'equal':a.get(name)==b.get(name)} for name in a.keys()|b.keys()},
    'scope':'Native artifact comparison only; no APK reproduction or gameplay parity claim'}
text=json.dumps(report,indent=2,sort_keys=True)+'\n'
if args.output:
    args.output.write_text(text)
print(text,end='')
