import json,re,statistics,sys
from pathlib import Path
root=Path(sys.argv[1]);names=sys.argv[2:];out={}
for name in names:
 text=(root/name/'game.log').read_text();frames={};pending=0
 for line in text.splitlines():
  m=re.search(r'\[Vulkan pipeline end\] ([\d.]+)ms',line)
  if m:pending+=float(m[1])
  m=re.search(r'present intervals\] frame=(\d+) interval_ms=([\d.]+)',line)
  if m:frames[int(m[1])]={'interval':float(m[2]),'pipeline':pending};pending=0
 out[name]={}
 for phase,a,b in [('combat',1322,1560),('activation',1562,1680),('held',1682,1920)]:
  rows=[frames[n] for n in range(a,b+1) if n in frames]
  if len(rows)!=b-a+1:continue
  values=[r['interval'] for r in rows];clean=[r['interval'] for r in rows if r['pipeline']==0]
  out[name][phase]={'frames':len(rows),'fps':1000/statistics.mean(values),'p95_ms':sorted(values)[int(.95*(len(values)-1))],'max_ms':max(values),'over_33ms':sum(v>1000/30 for v in values),'pipeline_ms':sum(r['pipeline'] for r in rows),'zero_pipeline_frames':len(clean),'zero_pipeline_p95_ms':sorted(clean)[int(.95*(len(clean)-1))] if clean else None}
 (root/name/'intervals.json').write_text(json.dumps(frames,indent=2))
print(json.dumps(out,indent=2))
