/* Standalone differential compute test; no game or OpenXR access. */
#include "nv2a_combiner.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define N 256
struct Case { uint32_t words[56]; float inputs[7][4],output[4]; };
_Static_assert(sizeof(struct Case)==352,"std430 fixture stride");
static uint32_t seed=12345;
static uint32_t random_word(void) { seed=seed*1664525u+1013904223u; return seed; }
int combiner_check(VkPhysicalDevice gpu,VkDevice device,VkQueue queue,VkCommandPool pool) {
    int failed=1; VkResult r;
    VkBuffer buffer=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE;
    VkShaderModule shader=VK_NULL_HANDLE; VkDescriptorSetLayout dsl=VK_NULL_HANDLE;
    VkDescriptorPool dp=VK_NULL_HANDLE; VkPipelineLayout pl=VK_NULL_HANDLE;
    VkPipeline pipeline=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    void *mapped=NULL; uint32_t *code=NULL; FILE *file=NULL;
    struct Case *cases=calloc(N,sizeof(*cases)); if(!cases) return 1;
#define TRY(x) do { r=(x); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#x,r); goto done; } } while(0)
    for(unsigned i=0;i<N;i++) {
        Nv2aCombiner rc={0};
        for(unsigned j=0;j<8;j++) {
            rc.color_icw[j]=random_word(); rc.alpha_icw[j]=random_word();
            rc.color_ocw[j]=random_word() & 0xfffff; rc.alpha_ocw[j]=random_word() & 0x3ffff;
            rc.factor0[j]=random_word(); rc.factor1[j]=random_word();
        }
        rc.control=(i%9)|((i&1)?0x11100:0);
        rc.stage_program=random_word(); rc.final_c0=random_word(); rc.final_c1=random_word();
        if(i%3) { rc.final0=random_word(); rc.final1=random_word(); }
        memcpy(cases[i].words,&rc,sizeof(rc));
        for(unsigned j=0;j<7;j++) for(unsigned k=0;k<4;k++) cases[i].inputs[j][k]=(random_word()&255)/255.f;
    }
    VkBufferCreateInfo bi={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=N*sizeof(*cases),.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    TRY(vkCreateBuffer(device,&bi,NULL,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    uint32_t mt=UINT32_MAX;
    for(uint32_t i=0;i<props.memoryTypeCount;i++) if((req.memoryTypeBits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mt=i; break; }
    if(mt==UINT32_MAX) goto done;
    VkMemoryAllocateInfo ma={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=mt};
    TRY(vkAllocateMemory(device,&ma,NULL,&memory)); TRY(vkBindBufferMemory(device,buffer,memory,0));
    TRY(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped)); memcpy(mapped,cases,N*sizeof(*cases));
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=memory,.size=VK_WHOLE_SIZE};
    TRY(vkFlushMappedMemoryRanges(device,1,&range));
    const char *shader_path=getenv("CHIHIRO_COMBINER_SHADER");
    if(!shader_path) shader_path="/data/local/tmp/vc3-combiner-test.spv";
    file=fopen(shader_path,"rb"); if(!file) {perror(shader_path);goto done;}
    fseek(file,0,SEEK_END); long size=ftell(file); rewind(file);
    if(size<=0 || size%4) goto done;
    code=malloc((size_t)size); if(!code || fread(code,1,(size_t)size,file)!=(size_t)size) goto done;
    VkShaderModuleCreateInfo sm={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=(size_t)size,.pCode=code};
    TRY(vkCreateShaderModule(device,&sm,NULL,&shader));
    VkDescriptorSetLayoutBinding binding={0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    TRY(vkCreateDescriptorSetLayout(device,&dl,NULL,&dsl));
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo di={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    TRY(vkCreateDescriptorPool(device,&di,NULL,&dp));
    VkDescriptorSetAllocateInfo da={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&dsl};
    VkDescriptorSet ds; TRY(vkAllocateDescriptorSets(device,&da,&ds));
    VkDescriptorBufferInfo db={buffer,0,N*sizeof(*cases)};
    VkWriteDescriptorSet dw={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&db};
    vkUpdateDescriptorSets(device,1,&dw,0,NULL);
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&dsl};
    TRY(vkCreatePipelineLayout(device,&li,NULL,&pl));
    VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=shader,.pName="main"},.layout=pl};
    TRY(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,NULL,&pipeline));
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; TRY(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo cb={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; TRY(vkBeginCommandBuffer(cmd,&cb));
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&ds,0,NULL);
    vkCmdDispatch(cmd,N,1,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    TRY(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; TRY(vkCreateFence(device,&fi,NULL,&fence));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    TRY(vkQueueSubmit(queue,1,&si,fence)); TRY(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ULL));
    TRY(vkInvalidateMappedMemoryRanges(device,1,&range));
    failed=0;
    for(unsigned i=0;i<N;i++) {
        Nv2aCombiner rc; memcpy(&rc,cases[i].words,sizeof(rc)); float expected[4];
        nv2a_rc_eval(&rc,cases[i].inputs[0],cases[i].inputs[1],cases[i].inputs[2],(const float (*)[4])&cases[i].inputs[3],expected);
        for(unsigned c=0;c<4;c++) {
            float got=((struct Case *)mapped)[i].output[c];
            if(!isfinite(got) || fabsf(got-expected[c])>0.00002f) { fprintf(stderr,"case%u channel%u got%.8f expected%.8f\n",i,c,got,expected[c]); failed=1; }
        }
    }
    if(!failed) puts("PASS: 256 GPU combiner arithmetic cases match shared CPU evaluator (tolerance 2e-5)");
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
