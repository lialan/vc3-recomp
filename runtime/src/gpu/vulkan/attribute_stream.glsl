// Byte-exact gathering from immutable draw-owned encoded snapshots.
#ifndef CH_ATTRIBUTE_FORMAT
#define CH_ATTRIBUTE_FORMAT(slot) params.p.attribute_format[slot]
#endif
layout(std430,set=0,binding=4) readonly buffer AttributeStreams {uint words[];} streams;
uvec4 ch_stream_attribute(uint slot,uint legacy_stride) {
    if(params.p.stream_bytes==0u) {
        uint at=uint(gl_VertexIndex)*legacy_stride+slot*4u;
        return uvec4(streams.words[at],streams.words[at+1u],streams.words[at+2u],streams.words[at+3u]);
    }
    uint format=CH_ATTRIBUTE_FORMAT(slot),kind=format&255u,size=(format>>8)&255u;
    uint bytes=kind==0u?16u:(kind==1u || kind==7u)?4u:(kind==2u || kind==6u)?size*2u:kind==5u?size:size*4u;
    // Merged draws (vk_target.c): instance k -- k/2 with a pair of eyes --
    // reads the k-th copy of the streams, stream_bytes apart.
    uint copy=uint(gl_InstanceIndex)>>(params.p.imm_mode==1u || params.p.imm_mode==2u?1u:0u);
    uint offset=params.p.attribute_offset[slot]+uint(gl_VertexIndex)*params.p.attribute_stride[slot]+copy*params.p.stream_bytes;
    uint at=offset>>2u,shift=(offset&3u)*8u;
    // Every word the attribute can touch is read first, unconditionally (the
    // index kept inside the buffer), and then stitched without branches:
    // read inside the branches, each word was a wait of its own -- the
    // Quest's eye passes stalled on these chains (24-36 reads, 19-28 waits a
    // vertex). The same bytes as before.
    uint last=uint(streams.words.length())-1u,w[5];
    for(uint i=0u;i<5u;i++) w[i]=streams.words[min(at+i,last)];
    uvec4 value=uvec4(0u,0u,0u,0x3f800000u);
    for(uint i=0u;i<4u;i++) if(i*4u<bytes) {
        uint word=shift==0u?w[i]:(w[i]>>shift)|(w[i+1u]<<(32u-shift));
        uint remaining=min(4u,bytes-i*4u);
        uint mask=remaining==4u?0xffffffffu:(1u<<(remaining*8u))-1u;
        value[i]=(value[i]&~mask)|(word&mask);
    }
    return value;
}
