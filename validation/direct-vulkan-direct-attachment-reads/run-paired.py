import os,sys,subprocess,json,time,hashlib,shutil
from pathlib import Path
root=Path('/tmp/vc3-warm-p95');names=sys.argv[1:]
base=json.loads(Path('/tmp/vc3-mac-gl-vk-compare/vulkan-warm/report.json').read_text())['environment']
for name in names:
 out=root/name;out.mkdir();[(out/d).mkdir() for d in ('save','config')]
 backend='paired'
 binary=root/(backend+'-vc3');cache=out/'pipelines.cache';seed=root/('image-on/pipelines.cache' if name=='paired-train' else 'paired-train/pipelines.cache');shutil.copy2(seed,cache)
 env={k:v for k,v in os.environ.items() if not k.startswith(('CHIHIRO_','VC3_','RECOMP_','VK_'))};env.update(base)
 env.update(VC3_SAVE_DIR=str(out/'save'),VC3_CONFIG_DIR=str(out/'config'),CHIHIRO_VULKAN_PIPELINE_CACHE=str(cache),CHIHIRO_VULKAN_PIPELINE_LOG='1')
 env['CHIHIRO_VULKAN_DIRECT_DEPTH_READ']='0' if name.endswith('off') else '1'
 env['CHIHIRO_VULKAN_DIRECT_COLOR_READ']=env['CHIHIRO_VULKAN_DIRECT_DEPTH_READ']
 if name=='paired-train':env.update(CHIHIRO_VULKAN_PROFILE='1',CHIHIRO_VULKAN_FRAME_COSTS='1',CHIHIRO_VULKAN_GPU_TIMESTAMPS='1')
 start=time.monotonic()
 with (out/'game.log').open('w') as f:
  try:r=subprocess.run([str(binary),'/Users/lialan/Downloads/VR/VC3/extracted'],env=env,stdout=f,stderr=subprocess.STDOUT,timeout=180);code=r.returncode
  except subprocess.TimeoutExpired:code='timeout'
 report={'binary':str(binary),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'seed_sha256':hashlib.sha256(seed.read_bytes()).hexdigest(),'environment':{k:v for k,v in env.items() if k.startswith(('CHIHIRO_','VC3_','RECOMP_','VK_'))},'exit_code':code,'elapsed':time.monotonic()-start}
 (out/'report.json').write_text(json.dumps(report,indent=2));print(name,code,flush=True)
