#!/usr/bin/env python3
"""Mirror the canonical scalar-only C fixed state into GLSL std430.
Emit compile-time assertions so C ABI padding cannot silently drift."""
import pathlib, re, sys
backend,params,glsl,checks=map(pathlib.Path,sys.argv[1:])

def fields(path,name):
    text=re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)
    body=re.search(r"typedef\s+struct\s*\{([^{}]*)\}\s*"+name+r"\s*;",text)
    if not body: raise SystemExit("Missing canonical structure: "+name)
    result=[]
    for decl in body[1].split(";"):
        if not decl.strip(): continue
        match=re.fullmatch(r"\s*(float|uint32_t|Nv2aFixedState)\s+(.+)\s*",decl,re.S)
        if not match: raise SystemExit("Unsupported fixed-state layout: "+decl)
        for field in match[2].split(","):
            item=re.fullmatch(r"\s*(\w+)((?:\s*\[\d+\])*)\s*",field)
            if not item: raise SystemExit("Unsupported fixed-state field: "+field)
            dims=[int(n) for n in re.findall(r"\[(\d+)\]",item[2])]
            result.append((match[1],item[1],dims))
    return result

sources=[(backend,"Nv2aFixedState","ChNvFixedState"),(params,"ChVkFixedParams","ChVkFixedParams")]
code=[];assertions=["/* Generated from the canonical C state. */", "#include <stddef.h>"];sizes={}
for path,cname,gname in sources:
    code.append("struct "+gname+" {"); offset=0
    for kind,name,dims in fields(path,cname):
        gtype={"float":"float","uint32_t":"uint","Nv2aFixedState":"ChNvFixedState"}[kind]
        code.append("    "+gtype+" "+name+"".join("["+str(n)+"]" for n in dims)+";")
        assertions.append('_Static_assert(offsetof('+cname+','+name+')=='+str(offset)+',"'+cname+'.'+name+' layout");')
        size=sizes[kind] if kind in sizes else 4
        for n in dims: size*=n
        offset+=size
    code.append("};");sizes[cname]=offset
    assertions.append('_Static_assert(sizeof('+cname+')=='+str(offset)+',"'+cname+' size");')
glsl.write_text("\n".join(code)+"\n");checks.write_text("\n".join(assertions)+"\n")
