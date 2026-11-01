#!/usr/bin/env python3
"""Embed validated, driver-specific CHVK cache seeds; never execute a game during build."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--manifest', type=Path, required=True)
p.add_argument('--seed', type=Path, action='append', default=[])
p.add_argument('--vendor-id', type=lambda s: int(s, 0), help='Require this target GPU vendor (Quest: 0x5143)')
a = p.parse_args()
entries = []
seen = set()
for path in a.seed:
    if path.stat().st_size > 128 * 1024 * 1024 + 24:
        p.error(f'oversized cache: {path}')
    data = path.read_bytes()
    if len(data) < 56 or data[:8] != b'CHVKPC\x00\x01':
        p.error(f'invalid cache envelope: {path}')
    size, expected = struct.unpack_from('<QQ', data, 8)
    if size != len(data) - 24:
        p.error(f'truncated cache: {path}')
    checksum = 14695981039346656037
    for v in data[24:]:
        checksum = ((checksum ^ v) * 1099511628211) & 0xffffffffffffffff
    if checksum != expected:
        p.error(f'cache checksum mismatch: {path}')
    hs, version, vendor, device = struct.unpack_from('<IIII', data, 24)
    uuid = data[40:56]
    if hs != 32 or version != 1:
        p.error(f'unsupported Vulkan cache header: {path}')
    if a.vendor_id is not None and vendor != a.vendor_id:
        p.error(f'cache GPU vendor {vendor:#x} does not match target {a.vendor_id:#x}: {path}')
    key = (vendor, device, uuid)
    if key in seen:
        p.error('duplicate GPU/driver seed; merge training caches first')
    seen.add(key)
    compressed = zlib.compress(data, 9)
    entries.append((vendor, device, uuid, data, compressed))
manifest = [dict(vendor_id=v, device_id=d, pipeline_cache_uuid=u.hex(),
                 bytes=len(raw), compressed_bytes=len(z), sha256=hashlib.sha256(raw).hexdigest())
            for v, d, u, raw, z in entries]
with a.output.open('w') as out:
    out.write('#include "vk_pipeline_cache_file.h"\n#include <stdlib.h>\n#include <string.h>\n')
    if entries:
        out.write('#include <zlib.h>\n')
    for i, (_, _, _, _, z) in enumerate(entries):
        out.write(f'static const unsigned char seed_{i}[] = {{\n')
        for offset in range(0, len(z), 24):
            out.write(','.join(str(v) for v in z[offset:offset+24]) + ',\n')
        out.write('};\n')
    out.write('VkResult ch_vk_pipeline_cache_packaged_load(VkPhysicalDevice gpu,VkDevice device,VkPipelineCache *cache,size_t *loaded) {\n')
    out.write('if(loaded)*loaded=0; if(!gpu || !device || !cache)return VK_ERROR_INITIALIZATION_FAILED; *cache=VK_NULL_HANDLE;\n')
    if entries:
        out.write('VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(gpu,&props);\n')
    for i, (vendor, dev, uuid, raw, _) in enumerate(entries):
        out.write('{static const unsigned char uuid[16]={' + ','.join(str(v) for v in uuid) + '};\n')
        out.write(f'if(props.vendorID=={vendor}u && props.deviceID=={dev}u && !memcmp(props.pipelineCacheUUID,uuid,16)) {{\n')
        out.write(f'uLongf bytes={len(raw)};unsigned char *raw=malloc(bytes);if(!raw)return VK_ERROR_OUT_OF_HOST_MEMORY;\n')
        out.write(f'int z=uncompress(raw,&bytes,seed_{i},sizeof seed_{i});\n')
        out.write(f'if(z!=Z_OK || bytes!={len(raw)}) {{free(raw);return ch_vk_pipeline_cache_memory_load(gpu,device,NULL,0,cache,loaded);}}\n')
        out.write('VkResult r=ch_vk_pipeline_cache_memory_load(gpu,device,raw,bytes,cache,loaded);free(raw);return r;}}\n')
    out.write('return ch_vk_pipeline_cache_memory_load(gpu,device,NULL,0,cache,loaded); }\n')
a.manifest.write_text(json.dumps(dict(schema=1, seeds=manifest), indent=2)+'\n')
