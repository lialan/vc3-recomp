import pathlib, struct, sys
src=pathlib.Path(sys.argv[1]).read_bytes()
if len(src)%4 or src[:4]!=b"\x03\x02\x23\x07": raise SystemExit("Invalid SPIR-V")
words=struct.unpack("<"+"I"*(len(src)//4),src)
name=sys.argv[3] if len(sys.argv)>3 else "depth_roundtrip_spv"
if not name.isidentifier(): raise SystemExit("Invalid C identifier")
pathlib.Path(sys.argv[2]).write_text("static const uint32_t "+name+"[]={"+",".join(hex(w) for w in words)+"};\n")
