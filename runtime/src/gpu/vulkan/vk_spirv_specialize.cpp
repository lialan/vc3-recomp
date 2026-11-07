/* Specialization on the CPU, for drivers that do not fold specialization
 * constants themselves. Qualcomm's (the Quest's) keeps the generic shader
 * body: the combiner ran ~78 ALU a fragment and the fixed-function and
 * vertex-program shaders 2500-4200 a vertex, re-run per bin. Here a
 * pipeline's constants become plain constants and SPIRV-Tools folds them,
 * unrolls the loops over stages and attributes and drops what is dead, so
 * the driver gets a shader the size of the work. */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spirv-tools/optimizer.hpp>

#include "vk_spirv_specialize.h"

/* Specialized modules, by a hash of the module and the constants: loaded
 * from the file beside the pipeline cache, each new one appended. The
 * specializer takes ~0.5 s a shader on the Quest's CPU, so it runs only
 * where the caller allows it (the boot-time recipe precompile); a draw that
 * misses keeps the driver-specialized module and is specialized next boot. */
static std::mutex s_mutex;
static std::unordered_map<uint64_t,std::vector<uint32_t>> s_cache;
static std::string s_path;
static uint64_t mix(uint64_t h,uint64_t v) {h^=v;h*=0x100000001b3ull;h^=h>>29;return h;}
static uint64_t cache_key(const uint32_t *code,size_t words,const VkSpecializationInfo *spec) {
    uint64_t h=0xcbf29ce484222325ull^words;
    for(size_t i=0;i<words;i++) h=mix(h,code[i]);
    if(spec) for(uint32_t i=0;i<spec->mapEntryCount;i++) {
        const VkSpecializationMapEntry &e=spec->pMapEntries[i];
        uint32_t v=0;if(e.size==4 && e.offset+4<=spec->dataSize) std::memcpy(&v,(const char *)spec->pData+e.offset,4);
        h=mix(h,((uint64_t)e.constantID<<32)|v);
    }
    return h;
}
static const uint32_t kMagic=0x43535031u;   /* "1PSC": key, word count, words */
/* Packaged with the build (vk_recipe_assets.S; empty without). */
extern "C" const unsigned char ch_vk_packaged_specialized[],ch_vk_packaged_specialized_end[];
extern "C" void ch_vk_spirv_specialize_cache_open(const char *path)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_path=path?path:"";s_cache.clear();
    FILE *f=s_path.empty()?NULL:fopen(s_path.c_str(),"rb");
    if(f) {
        uint32_t magic=0;size_t loaded=0;
        if(fread(&magic,4,1,f)==1 && magic==kMagic) for(;;) {
            uint64_t key;uint32_t words;
            if(fread(&key,8,1,f)!=1 || fread(&words,4,1,f)!=1 || !words || words>(1u<<20)) break;
            std::vector<uint32_t> v(words);
            if(fread(v.data(),4,words,f)!=words) break;
            s_cache[key]=std::move(v);loaded++;
        }
        fclose(f);
        fprintf(stderr,"[Vulkan] %zu CPU-specialized shaders from %s\n",loaded,s_path.c_str());
    }
    /* Those packaged with the build (vk_recipe_assets.S; the same records):
     * a first launch's precompile specialized every shader here, ~0.5 s each
     * on the Quest's CPU -- 42 s for HOTD3. Not written to the file. */
    size_t packaged=0;
    const unsigned char *p=ch_vk_packaged_specialized,*end=ch_vk_packaged_specialized_end;
    uint32_t pmagic=0;
    if(end-p>=4) {std::memcpy(&pmagic,p,4);p+=4;}
    if(pmagic==kMagic) while(end-p>=12) {
        uint64_t key;uint32_t words;std::memcpy(&key,p,8);std::memcpy(&words,p+8,4);p+=12;
        if(!words || words>(1u<<20) || (size_t)(end-p)<(size_t)words*4) break;
        std::vector<uint32_t> v(words);std::memcpy(v.data(),p,(size_t)words*4);p+=(size_t)words*4;
        if(s_cache.emplace(key,std::move(v)).second) packaged++;
    }
    if(packaged) fprintf(stderr,"[Vulkan] %zu packaged CPU-specialized shaders added\n",packaged);
}
static void store(uint64_t key,const std::vector<uint32_t> &v)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if(!s_cache.emplace(key,v).second || s_path.empty()) return;
    FILE *f=fopen(s_path.c_str(),"ab");
    if(!f) return;
    if(ftell(f)==0) fwrite(&kMagic,4,1,f);
    uint32_t words=(uint32_t)v.size();
    fwrite(&key,8,1,f);fwrite(&words,4,1,f);fwrite(v.data(),4,words,f);
    fclose(f);
}
extern "C" int ch_vk_spirv_specialize(const uint32_t *code,size_t bytes,const VkSpecializationInfo *spec,
    int compute,uint32_t **out_code,size_t *out_bytes)
{
    if(!code || !bytes || bytes%4 || !out_code || !out_bytes) return -1;
    const uint64_t key=cache_key(code,bytes/4,spec);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto hit=s_cache.find(key);
        if(hit!=s_cache.end()) {
            uint32_t *copy=(uint32_t *)std::malloc(hit->second.size()*4);
            if(!copy) return -1;
            std::memcpy(copy,hit->second.data(),hit->second.size()*4);
            *out_code=copy;*out_bytes=hit->second.size()*4;
            return 0;
        }
    }
    if(!compute) return 1;
    std::unordered_map<uint32_t,std::string> values;
    if(spec) for(uint32_t i=0;i<spec->mapEntryCount;i++) {
        const VkSpecializationMapEntry &e=spec->pMapEntries[i];
        if(e.size!=4 || e.offset+4>spec->dataSize) return -1;   /* ours are all 32-bit integers */
        uint32_t v;std::memcpy(&v,(const char *)spec->pData+e.offset,4);
        values[e.constantID]=std::to_string(v);
    }
    if(const char *dir=getenv("CHIHIRO_VULKAN_SPECIALIZE_DUMP")) {   /* inputs, to tune the passes offline */
        static std::atomic<unsigned> n{0};
        unsigned i=n++;
        if(i<64) {
            std::string base=std::string(dir)+"/spec-"+std::to_string(i);
            if(FILE *f=fopen((base+".spv").c_str(),"wb")) {fwrite(code,1,bytes,f);fclose(f);}
            if(FILE *f=fopen((base+".txt").c_str(),"w")) {
                for(auto &kv:values) fprintf(f,"%u:%s ",kv.first,kv.second.c_str());
                fclose(f);
            }
        }
    }
    spvtools::Optimizer opt(SPV_ENV_VULKAN_1_0);
    opt.SetMessageConsumer([](spv_message_level_t,const char *,const spv_position_t &,const char *) {});
    opt.RegisterPass(spvtools::CreateSetSpecConstantDefaultValuePass(values))
       .RegisterPass(spvtools::CreateFreezeSpecConstantValuePass())
       .RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass())
       .RegisterPass(spvtools::CreateInlineExhaustivePass())
       .RegisterPass(spvtools::CreateEliminateDeadFunctionsPass())
       .RegisterPass(spvtools::CreateCCPPass())
       .RegisterPass(spvtools::CreateDeadBranchElimPass())
       .RegisterPass(spvtools::CreateLoopUnrollPass(true))
       .RegisterPerformancePasses()
       .RegisterPass(spvtools::CreateLoopUnrollPass(true))
       .RegisterPerformancePasses();
    spvtools::OptimizerOptions options;
    options.set_run_validator(false);
    std::vector<uint32_t> out;
    if(!opt.Run(code,bytes/4,&out,options) || out.empty()) return -1;
    store(key,out);
    uint32_t *copy=(uint32_t *)std::malloc(out.size()*4);
    if(!copy) return -1;
    std::memcpy(copy,out.data(),out.size()*4);
    *out_code=copy;*out_bytes=out.size()*4;
    return 0;
}
