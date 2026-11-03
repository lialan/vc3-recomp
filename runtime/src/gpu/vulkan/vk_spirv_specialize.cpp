/* Specialization on the CPU, for drivers that do not fold specialization
 * constants themselves. Qualcomm's (the Quest's) keeps the generic shader
 * body: the combiner ran ~78 ALU a fragment and the fixed-function and
 * vertex-program shaders 2500-4200 a vertex, re-run per bin. Here a
 * pipeline's constants become plain constants and SPIRV-Tools folds them,
 * unrolls the loops over stages and attributes and drops what is dead, so
 * the driver gets a shader the size of the work. */
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <spirv-tools/optimizer.hpp>

#include "vk_spirv_specialize.h"

extern "C" int ch_vk_spirv_specialize(const uint32_t *code,size_t bytes,const VkSpecializationInfo *spec,
    uint32_t **out_code,size_t *out_bytes)
{
    if(!code || !bytes || bytes%4 || !out_code || !out_bytes) return -1;
    std::unordered_map<uint32_t,std::string> values;
    if(spec) for(uint32_t i=0;i<spec->mapEntryCount;i++) {
        const VkSpecializationMapEntry &e=spec->pMapEntries[i];
        if(e.size!=4 || e.offset+4>spec->dataSize) return -1;   /* ours are all 32-bit integers */
        uint32_t v;std::memcpy(&v,(const char *)spec->pData+e.offset,4);
        values[e.constantID]=std::to_string(v);
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
    uint32_t *copy=(uint32_t *)std::malloc(out.size()*4);
    if(!copy) return -1;
    std::memcpy(copy,out.data(),out.size()*4);
    *out_code=copy;*out_bytes=out.size()*4;
    return 0;
}
