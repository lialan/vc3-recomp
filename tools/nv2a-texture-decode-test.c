/* CPU-only bulk decoder parity against the coordinate sampler. */
#include "../external/xboxrecomp/src/kernel/nv2a_pb_exec.c"
static unsigned char memory[16 * 1024 * 1024];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)memory; }
int main(void) {
    uint32_t seed=12345;
    for(size_t i=0;i<sizeof memory;i++) {seed=seed*1664525u+1013904223u;memory[i]=seed>>24;}
    uint32_t *out=malloc(1024*1024*4);
    unsigned cases=0;
    const unsigned dims[][2]={{1,1},{1,64},{64,1},{2,8},{32,4},{4,32},{128,128},{640,480},{7,5}};
    for(unsigned fmt=0;fmt<128;fmt++) for(unsigned d=0;d<sizeof dims/sizeof dims[0];d++)
    for(unsigned palette=0;palette<2;palette++) {
        unsigned w=dims[d][0],h=dims[d][1];
        Nv2aTexture tx={.offset=4096,.width=w,.height=h,.pitch=w*4+16,.color=fmt,.palette=palette?1024:0};
        Texture t={.offset=tx.offset,.width=w,.height=h,.pitch=tx.pitch,.color=fmt,.palette=tx.palette,.addr_u=3,.addr_v=3,.valid=1,.levels=1};
        int ok=nv2a_backend_decode_texture(&tx,out),ref=1;
        for(unsigned y=0;y<h && ref;y++) for(unsigned x=0;x<w;x++) {
            uint32_t v;ref=sample_tex(&t,0,x,y,&v);
            if(!ref) break;
            if(!ok || out[(size_t)y*w+x]!=v) {fprintf(stderr,"mismatch fmt=%x size=%ux%u xy=%u,%u\n",fmt,w,h,x,y);return 1;}
        }
        if(ok!=ref) {fprintf(stderr,"status mismatch fmt=%x\n",fmt);return 1;}
        cases++;
    }
    for(unsigned fmt=5;fmt<=6;fmt++) {
        Nv2aTexture tx={.offset=4096,.width=512,.height=512,.pitch=2048,.color=fmt};
        Texture t={.offset=4096,.width=512,.height=512,.pitch=2048,.color=fmt,.addr_u=3,.addr_v=3,.valid=1,.levels=1};
        clock_t start=clock();
        for(unsigned n=0;n<40;n++) nv2a_backend_decode_texture(&tx,out);
        double bulk=(double)(clock()-start)/CLOCKS_PER_SEC;
        start=clock();
        for(unsigned n=0;n<40;n++) for(unsigned y=0;y<512;y++) for(unsigned x=0;x<512;x++) sample_tex(&t,0,x,y,&out[y*512+x]);
        double reference=(double)(clock()-start)/CLOCKS_PER_SEC;
        printf("format %x: bulk %.3fms reference %.3fms per 512x512 decode\n",fmt,bulk*25,reference*25);
    }
    free(out);printf("PASS %u texture format/layout/palette cases\n",cases);return 0;
}
