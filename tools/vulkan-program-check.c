/* Standalone differential compute test; no game or OpenXR access. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vk_fixed.h"
#include "nv2a_vsh_interp.h"
#define N 256
struct Case {float input[16][4];Nv2aVshOutput output;};
_Static_assert(sizeof(struct Case)==384,"std430 case stride");
static uint32_t seed=12345;
static uint32_t random_word(void) {seed=seed*1664525u+1013904223u;return seed;}
/* Identity-swizzled input operands by default. */
static void instruction(uint32_t *p,unsigned mac,unsigned ilu,unsigned input,unsigned out) {
    p[0]=0;p[1]=(mac<<21)|(ilu<<25)|(input<<9)|0x1b;
    p[2]=(2u<<26)|(0x1bu<<17)|(2u<<11)|(0x1bu<<2);
    p[3]=(2u<<28)|(15u<<12)|(1u<<11)|(out<<3);
}
static void decode_attribute(const float encoded[4],unsigned type,unsigned size,float out[4]) {
    const unsigned char *p=(const unsigned char *)encoded;
    out[0]=out[1]=out[2]=0;out[3]=1;
    if(type==0) {out[0]=p[2]/255.f;out[1]=p[1]/255.f;out[2]=p[0]/255.f;out[3]=p[3]/255.f;}
    else if(type==2) memcpy(out,p,size*4);
    else if(type==4) for(unsigned i=0;i<size;i++) out[i]=p[i]/255.f;
    else if(type==1 || type==5) for(unsigned i=0;i<size;i++) {int16_t v;memcpy(&v,p+i*2,2);out[i]=type==1?v/32767.f:v;}
    else if(type==6) {uint32_t v;memcpy(&v,p,4);out[0]=((int32_t)(v<<21)>>21)/1023.f;out[1]=((int32_t)(v<<10)>>21)/1023.f;out[2]=((int32_t)v>>22)/511.f;}
}
int combiner_check(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,VkCommandPool pool) {
    int failed=1; VkResult r;
    VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
    VkShaderModule shader=VK_NULL_HANDLE; VkDescriptorSetLayout dsl=VK_NULL_HANDLE;
    VkDescriptorPool dp=VK_NULL_HANDLE; VkPipelineLayout pl=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    void *mapped=NULL; uint32_t *code=NULL; FILE *file=NULL;
    struct Case *cases=calloc(N,sizeof(*cases)); if(!cases) return 1;
    ChVkFixedParams params={0};
    const size_t header=(sizeof params+255)/256*256,total=header+N*sizeof(*cases);
    for(unsigned j=0;j<192;j++) for(unsigned k=0;k<4;k++) params.constants[j][k]=(float)(j+1)*(k+1)*.03125f;
    for(unsigned j=0;j<192;j++) nv2a_vsh_set_constant(j,params.constants[j]);
    nv2a_vsh_set_start_slot(0);nv2a_vsh_set_cxt_write(0);
#define TRY(x) do { r=(x); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#x,r); goto done; } } while(0)
    for(unsigned i=0;i<N;i++) for(unsigned j=0;j<16;j++) for(unsigned k=0;k<4;k++)
        cases[i].input[j][k]=((int)(random_word()%1025)-512)/128.f;
    // Edge operands, including both signed zeros, infinities and NaNs.
    uint32_t edges[]={0,0x80000000,0x7f800000,0xff800000,0x7fc12345,0x3f800000,0xbf800000,0x00800000};
    for(unsigned i=0;i<8;i++) for(unsigned k=0;k<4;k++) memcpy(&cases[i].input[0][k],&edges[i],4);
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=total,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    TRY(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    uint32_t mt=UINT32_MAX;
    for(uint32_t i=0;i<props.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt=i; break; }
    if(mt==UINT32_MAX) goto done;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
    TRY(vkAllocateMemory(device,&ma,NULL,&memory)); TRY(vkBindBufferMemory(device,buffer,memory,0));
    TRY(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped)); memcpy(mapped,&params,sizeof params);memcpy((char *)mapped+header,cases,N*sizeof(*cases));
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};
    TRY(vkFlushMappedMemoryRanges(device,1,&range));
    const char *shader_path=getenv("CHIHIRO_PROGRAM_SHADER");
    if(!shader_path) shader_path="program_test.spv";
    file=fopen(shader_path,"rb"); if(!file) {perror(shader_path);goto done;}
    fseek(file,0,SEEK_END); long size=ftell(file); rewind(file);
    if(size<=0 || size%4) goto done;
    code=malloc((size_t)size); if(!code || fread(code,1,(size_t)size,file)!=(size_t)size) goto done;
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=code};
    TRY(vkCreateShaderModule(device,&sm,NULL,&shader));
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=2,.pBindings=bindings};
    TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&dsl));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    TRY(vkCreateDescriptorPool(device,&di,NULL,&dp));
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; TRY(vkAllocateDescriptorSets(device,&da,&ds));
    VkDescriptorBufferInfo db={buffer,header,N*sizeof(*cases)};
    VkWriteDescriptorSet dw={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
    vkUpdateDescriptorSets(device,1,&dw,0,NULL);
    db=(VkDescriptorBufferInfo){buffer,0,sizeof params};dw.dstBinding=3;
    vkUpdateDescriptorSets(device,1,&dw,0,NULL);
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    TRY(vkCreatePipelineLayout(device,&li,NULL,&pl));
    VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=pl};
    VkSpecializationMapEntry entries[545];uint32_t spec[545]={0};
    for(unsigned i=0;i<545;i++) entries[i]=(VkSpecializationMapEntry){i,i*4,4};
    VkSpecializationInfo specialization={545,entries,sizeof spec,spec};pi.stage.pSpecializationInfo=&specialization;
    for(unsigned test=0;test<168;test++) {
        unsigned mac=(test/8)%16,ilu=test%8;uint32_t words[136][4]={{0}};
        // Paired outputs to R2/R1, then export each result independently.
        instruction(words[0],mac,ilu,0,9);
        words[0][3]|=(15u<<24)|(2u<<20)|(15u<<16);
        instruction(words[1],1,0,0,10);words[1][2]=(1u<<26)|(2u<<28);
        instruction(words[2],1,0,0,11);words[2][2]=(1u<<26)|(1u<<28);
        instruction(words[3],1,0,0,0);words[3][2]=(1u<<26)|(12u<<28);words[3][3]|=1;
        unsigned length=4;
        if(test>=128 && test<144) {
            unsigned variant=test-128;
            // ARL from finite constant, followed by relative constant reads;
            // partial destinations, every temp index, negation and swizzles.
            instruction(words[0],13,0,0,5);words[0][2]=3u<<26;
            words[0][1]|=(variant==15?191u:variant)<<13;
            instruction(words[1],1,0,0,9);
            words[1][1]=(1u<<21)|((variant==15?255u:96u)<<13)|(variant*17u)|(1u<<8);
            words[1][2]=3u<<26;words[1][3]=(words[1][3]&~(15u<<12))|(variant<<12)|2u;
            instruction(words[2],1,0,0,10);words[2][2]=(1u<<26)|(variant<<28);
            // Ignored constant output with CXT_WRITE disabled.
            instruction(words[3],1,0,0,96);words[3][3]&=~(1u<<11);
            instruction(words[4],1,0,1,0);words[4][3]|=1u;length=5;
        }
        unsigned attribute_type=0,attribute_size=0;
        params.attribute_format[0]=0;
        if(test>=144) {
            const unsigned types[]={0,1,2,4,5,6};
            attribute_type=types[(test-144)/4];attribute_size=(test-144)%4+1;
            params.attribute_format[0]=(attribute_size<<8)|(attribute_type+1);
            instruction(words[0],1,0,0,9);
            instruction(words[1],1,0,0,0);words[1][3]|=1;length=2;
            for(unsigned i=0;i<N;i++) for(unsigned k=0;k<4;k++) {
                uint32_t bits=i<8?edges[i]:random_word();
                memcpy(&cases[i].input[0][k],&bits,4);
            }
        }
        memcpy(mapped,&params,sizeof params);
        // Do not compare undefined float-to-int conversions in the CPU ARL.
        if(mac==13 && test<128) for(unsigned i=0;i<8;i++) cases[i].input[0][0]=.25f;
        memcpy((char *)mapped+header,cases,N*sizeof(*cases));TRY(vkFlushMappedMemoryRanges(device,1,&range));
        spec[0]=length;memcpy(spec+1,words,sizeof words);
        for(unsigned i=0;i<length;i++) nv2a_vsh_set_instruction(i,words[i]);
        TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; TRY(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo cb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; TRY(vkBeginCommandBuffer(cmd,&cb));
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,NULL);
    vkCmdDispatch(cmd,N/64,1,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    TRY(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    TRY(vkQueueSubmit(queue,1,&si,fence)); TRY(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ULL));
    TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
    for(unsigned i=0;i<N;i++) {
        struct Case *got=(struct Case *)((char *)mapped+header)+i;Nv2aVshOutput expected;
        float decoded[16][4];memcpy(decoded,cases[i].input,sizeof decoded);
        if(test>=144) decode_attribute(cases[i].input[0],attribute_type,attribute_size,decoded[0]);
        if(!nv2a_vsh_run(decoded,&expected)) goto done;
        const float *x=(const float *)&got->output,*y=(const float *)&expected;
        for(unsigned k=0;k<32;k++) {
            if((isnan(x[k]) && isnan(y[k])) || x[k]==y[k]) continue;
            if(isfinite(x[k]) && isfinite(y[k]) && fabsf(x[k]-y[k])<=2e-5f*fmaxf(1,fabsf(y[k]))) continue;
            fprintf(stderr,"program=%u case=%u component=%u GPU=%g CPU=%g\n",test,i,k,x[k],y[k]);goto done;
        }
    }
    vkDestroyFence(device,fence,NULL);fence=VK_NULL_HANDLE;
    vkFreeCommandBuffers(device,pool,1,&cmd);vkDestroyPipeline(device,pipeline,NULL);pipeline=VK_NULL_HANDLE;
    }
    failed=0;puts("PASS: 144 MAC/ILU, relative-address, mask/swizzle programs and 24 attribute-format cases x 256 vertices against CPU oracle (2e-5 relative/absolute tolerance)");
done:
    vkDeviceWaitIdle(device);
    if(mapped) vkUnmapMemory(device,memory);
    if(fence) vkDestroyFence(device,fence,NULL);
    if(pipeline) vkDestroyPipeline(device,pipeline,NULL);
    if(pl) vkDestroyPipelineLayout(device,pl,NULL);
    if(dp) vkDestroyDescriptorPool(device,dp,NULL);
    if(dsl) vkDestroyDescriptorSetLayout(device,dsl,NULL);
    if(shader) vkDestroyShaderModule(device,shader,NULL);
    if(buffer) vkDestroyBuffer(device,buffer,NULL);
    if(memory) vkFreeMemory(device,memory,NULL);
    if(file) fclose(file); free(code); free(cases); return failed;
}
