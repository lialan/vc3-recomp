/* Standalone device setup for combiner differential compute test. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(call) do { VkResult r=(call); if(r!=VK_SUCCESS) { fprintf(stderr,"%s: %d\n",#call,r); goto done; } } while(0)
int combiner_check(VkPhysicalDevice,VkDevice,VkQueue,VkCommandPool);
int main(void) {
    int result=1;
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkPhysicalDevice *physical=NULL;
    VkQueueFamilyProperties *families=NULL;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="Chihiro Vulkan probe",.apiVersion=VK_API_VERSION_1_0};
    VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    CHECK(vkCreateInstance(&ici,NULL,&instance));
    uint32_t count=0;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,NULL));
    if(!count) goto done;
    physical=calloc(count,sizeof(*physical)); if(!physical) goto done;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,physical));
    VkPhysicalDevice gpu=VK_NULL_HANDLE;
    uint32_t family=0;
    for(uint32_t i=0;i<count && !gpu;i++) {
        uint32_t n=0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i],&n,NULL);
        families=calloc(n,sizeof(*families)); if(!families) goto done;
        vkGetPhysicalDeviceQueueFamilyProperties(physical[i],&n,families);
        for(uint32_t j=0;j<n;j++) if(families[j].queueCount && (families[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)) { gpu=physical[i];family=j;break; }
        free(families); families=NULL;
    }
    if(!gpu) goto done;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu,&props);
    printf("device=%s vendor=%04x deviceID=%04x api=%u.%u.%u\n",props.deviceName,props.vendorID,props.deviceID,VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),VK_VERSION_PATCH(props.apiVersion));
    float priority=1;
    VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
    CHECK(vkCreateDevice(gpu,&dci,NULL,&device));
    VkQueue queue;
    vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    CHECK(vkCreateCommandPool(device,&pci,NULL,&pool));
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device,&ai,&command));
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(command,&bi));
    CHECK(vkEndCommandBuffer(command));
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK(vkCreateFence(device,&fi,NULL,&fence));
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    CHECK(vkQueueSubmit(queue,1,&si,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ULL));
    result=combiner_check(gpu,device,queue,pool);
done:
    if(device) { vkDeviceWaitIdle(device); if(fence) vkDestroyFence(device,fence,NULL); if(pool) vkDestroyCommandPool(device,pool,NULL); vkDestroyDevice(device,NULL); }
    if(instance) vkDestroyInstance(instance,NULL);
    free(families);free(physical);
    return result;
}
