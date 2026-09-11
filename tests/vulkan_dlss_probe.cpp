// Isolated Vulkan/NGX smoke test using the unmodified official NVIDIA DLSS SDK.
// Not an NVIDIA sample. Synthetic input verifies execution and nonblack readback.
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#define VOLK_IMPLEMENTATION
#include <volk.h>
#include <d3d11.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <algorithm>
#include "IpcProtocol.h"

void vkcheck(VkResult r,const char* operation) { if(r!=VK_SUCCESS){printf("FAIL %s VkResult=%d\n",operation,r);exit(2);} }
void ngxcheck(NVSDK_NGX_Result r,const char* operation) {printf("%s result=0x%08x\n",operation,unsigned(r));if(NVSDK_NGX_FAILED(r))exit(3);}
#define VKC(x) vkcheck((x),#x)
#define NGXC(x) ngxcheck((x),#x)
void NVSDK_CONV ngxlog(const char* msg,NVSDK_NGX_Logging_Level l,NVSDK_NGX_Feature f){printf("NGX[%u/%u] %s\n",unsigned(l),unsigned(f),msg);}
VkDevice dev; VkPhysicalDevice pd; VkQueue queue; VkCommandBuffer cmd; VkFence fence;
VkInstance vkInstance; VkSurfaceKHR surface; VkSwapchainKHR swapchain; HWND windowHandle;
VkExtent2D swapExtent; VkFormat swapFormat; std::vector<VkImage> swapImages; VkSemaphore acquired;
DWORD replacementDelayMs=0; bool reinitializeDlss=false;
bool dxlLoaded=false;
bool auxDxgi=false; HWND helperWindow=nullptr;
ID3D11Device* helperDevice=nullptr;ID3D11DeviceContext* helperContext=nullptr;IDXGISwapChain* helperChain=nullptr;
void createDxgiHelper(){
 helperWindow=CreateWindowW(L"STATIC",L"DXL hidden DXGI auxiliary",WS_OVERLAPPEDWINDOW,0,0,16,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 DXGI_SWAP_CHAIN_DESC d{};d.BufferDesc.Width=d.BufferDesc.Height=8;d.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=1;d.OutputWindow=helperWindow;d.Windowed=TRUE;
 HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&helperChain,&helperDevice,nullptr,&helperContext);printf("AUX hidden D3D11 helper create=0x%08x hwnd=%p\n",unsigned(hr),helperWindow);if(FAILED(hr))exit(12);helperChain->Present(0,0);
}
struct DxlCounts {uint64_t presents=0,nr=0;bool valid=false;};
DxlCounts dxlCounts(){
 DxlCounts result;wchar_t name[128];swprintf_s(name,L"Local\\DXL.Status.%lu",GetCurrentProcessId());HANDLE h=OpenFileMappingW(FILE_MAP_READ,FALSE,name);if(!h)return result;auto*s=static_cast<const DXL::Ipc::Status*>(MapViewOfFile(h,FILE_MAP_READ,0,0,sizeof(DXL::Ipc::Status)));if(s){for(int n=0;n<20;n++){uint32_t seq=s->sequence;if(seq&1){Sleep(1);continue;}MemoryBarrier();auto p=s->presentCount,nr=s->nrEvaluateCount;MemoryBarrier();if(s->sequence==seq && s->magic==DXL::Ipc::MAGIC){result={p,nr,true};break;}}UnmapViewOfFile(s);}CloseHandle(h);return result;
}
void pump(){MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void createWindowSurface(uint32_t width,uint32_t height){
 WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DXLVulkanProbe";RegisterClassW(&wc);
 RECT r{0,0,LONG(width),LONG(height)};AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);windowHandle=CreateWindowW(wc.lpszClassName,L"DXL Vulkan DLSS isolated probe",WS_OVERLAPPEDWINDOW,80,80,r.right-r.left,r.bottom-r.top,nullptr,nullptr,wc.hInstance,nullptr);ShowWindow(windowHandle,SW_SHOWNOACTIVATE);pump();
 VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};ci.hinstance=wc.hInstance;ci.hwnd=windowHandle;VKC(vkCreateWin32SurfaceKHR(vkInstance,&ci,nullptr,&surface));printf("Vulkan surface created hwnd=%p\n",windowHandle);
}
void createSwapchain(){
 VkSurfaceCapabilitiesKHR cap;VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd,surface,&cap));uint32_t n=0;VKC(vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&n,nullptr));std::vector<VkSurfaceFormatKHR> formats(n);VKC(vkGetPhysicalDeviceSurfaceFormatsKHR(pd,surface,&n,formats.data()));auto fmt=formats[0];for(auto f:formats)if(f.format==VK_FORMAT_B8G8R8A8_UNORM){fmt=f;break;}
 swapExtent=cap.currentExtent;swapFormat=fmt.format;VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;ci.minImageCount=std::max(2u,cap.minImageCount);if(cap.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,cap.maxImageCount);ci.imageFormat=fmt.format;ci.imageColorSpace=fmt.colorSpace;ci.imageExtent=swapExtent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=cap.currentTransform;ci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;ci.oldSwapchain=swapchain;
 VkSwapchainKHR next;VKC(vkCreateSwapchainKHR(dev,&ci,nullptr,&next));if(swapchain)vkDestroySwapchainKHR(dev,swapchain,nullptr);swapchain=next;VKC(vkGetSwapchainImagesKHR(dev,swapchain,&n,nullptr));swapImages.resize(n);VKC(vkGetSwapchainImagesKHR(dev,swapchain,&n,swapImages.data()));printf("Vulkan swapchain created=%p size=%ux%u buffers=%u\n",swapchain,swapExtent.width,swapExtent.height,n);
}
uint32_t memtype(uint32_t bits,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(pd,&p);for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;puts("FAIL memory type");exit(4);}
struct Image {VkImage image;VkImageView view;VkDeviceMemory mem;NVSDK_NGX_Resource_VK ngx;};
VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
Image createImage(uint32_t w,uint32_t h,VkFormat format){
 Image a{};VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={w,h,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
 VKC(vkCreateImage(dev,&ci,nullptr,&a.image));VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,a.image,&mr);VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=mr.size;ma.memoryTypeIndex=memtype(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VKC(vkAllocateMemory(dev,&ma,nullptr,&a.mem));VKC(vkBindImageMemory(dev,a.image,a.mem,0));
 VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=a.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange=range;VKC(vkCreateImageView(dev,&vi,nullptr,&a.view));a.ngx=NVSDK_NGX_Create_ImageView_Resource_VK(a.view,a.image,range,format,w,h,true);return a;
}
void destroy(Image&a){vkDestroyImageView(dev,a.view,nullptr);vkDestroyImage(dev,a.image,nullptr);vkFreeMemory(dev,a.mem,nullptr);}
void barrier(Image&a,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=from;b.newLayout=to;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=a.image;b.subresourceRange=range;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}
void begin(){VKC(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;VKC(vkBeginCommandBuffer(cmd,&b));}
void submit(){VKC(vkEndCommandBuffer(cmd));VKC(vkResetFences(dev,1,&fence));VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cmd;VKC(vkQueueSubmit(queue,1,&s,fence));VKC(vkWaitForFences(dev,1,&fence,VK_TRUE,15000000000ull));}
void present(Image& source){
 pump();uint32_t index=0;auto a=vkAcquireNextImageKHR(dev,swapchain,15000000000ull,acquired,VK_NULL_HANDLE,&index);if(a!=VK_SUBOPTIMAL_KHR)VKC(a);begin();Image dest{};dest.image=swapImages[index];barrier(source,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);barrier(dest,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
 VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};b.dstSubresource=b.srcSubresource;b.srcOffsets[1]={int(source.ngx.Resource.ImageViewInfo.Width),int(source.ngx.Resource.ImageViewInfo.Height),1};b.dstOffsets[1]={int(swapExtent.width),int(swapExtent.height),1};vkCmdBlitImage(cmd,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dest.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,VK_FILTER_LINEAR);barrier(dest,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0);barrier(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
 VKC(vkEndCommandBuffer(cmd));VKC(vkResetFences(dev,1,&fence));VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.waitSemaphoreCount=1;s.pWaitSemaphores=&acquired;s.pWaitDstStageMask=&waitStage;s.commandBufferCount=1;s.pCommandBuffers=&cmd;VKC(vkQueueSubmit(queue,1,&s,fence));VKC(vkWaitForFences(dev,1,&fence,VK_TRUE,15000000000ull));VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;auto r=vkQueuePresentKHR(queue,&pi);if(r!=VK_SUBOPTIMAL_KHR)VKC(r);VKC(vkQueueWaitIdle(queue));
}
int wmain(int argc,wchar_t** argv){
 setvbuf(stdout,nullptr,_IONBF,0);printf("Vulkan DLSS probe pid=%lu\n",GetCurrentProcessId());
 bool windowed=false;for(int i=1;i<argc;i++){if(wcscmp(argv[i],L"--present")==0)windowed=true;if(wcscmp(argv[i],L"--aux-dxgi")==0)auxDxgi=true;if(wcscmp(argv[i],L"--reinit-dlss")==0)reinitializeDlss=true;if(wcscmp(argv[i],L"--replacement-delay-ms")==0 && i+1<argc)replacementDelayMs=wcstoul(argv[++i],nullptr,10);if(wcscmp(argv[i],L"--dxl")==0 && i+1<argc){HMODULE m=LoadLibraryW(argv[++i]);printf("DXL LoadLibrary=%p error=%lu\n",m,GetLastError());if(!m)return 5;dxlLoaded=true;Sleep(3000);}}
 VKC(volkInitialize());uint32_t ni=0,nd=0;const char** ie=nullptr;const char** de=nullptr;
 NGXC(NVSDK_NGX_VULKAN_RequiredExtensions(&ni,&ie,&nd,&de));
 for(uint32_t i=0;i<ni;i++)printf("Instance extension %s\n",ie[i]);for(uint32_t i=0;i<nd;i++)printf("Device extension %s\n",de[i]);
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="DXL isolated Vulkan DLSS probe";app.apiVersion=VK_API_VERSION_1_2;
 std::vector<const char*> instanceExt(ie,ie+ni),deviceExt(de,de+nd);if(windowed){instanceExt.push_back(VK_KHR_SURFACE_EXTENSION_NAME);instanceExt.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);deviceExt.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);}
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;ici.enabledExtensionCount=uint32_t(instanceExt.size());ici.ppEnabledExtensionNames=instanceExt.data();VkInstance instance;VKC(vkCreateInstance(&ici,nullptr,&instance));volkLoadInstance(instance);vkInstance=instance;
 uint32_t n=0;VKC(vkEnumeratePhysicalDevices(instance,&n,nullptr));std::vector<VkPhysicalDevice> devices(n);VKC(vkEnumeratePhysicalDevices(instance,&n,devices.data()));
 for(auto d:devices){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(d,&p);printf("GPU %s vendor=%x api=%u.%u.%u\n",p.deviceName,p.vendorID,VK_VERSION_MAJOR(p.apiVersion),VK_VERSION_MINOR(p.apiVersion),VK_VERSION_PATCH(p.apiVersion));if(p.vendorID==0x10de)pd=d;}
 if(!pd){puts("FAIL no NVIDIA Vulkan device");return 6;}uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,nullptr);std::vector<VkQueueFamilyProperties> qs(qn);vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qs.data());uint32_t qi=0;while(qi<qn&&(qs[qi].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))qi++;if(qi==qn)return 7;
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=qi;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(pd,&features);VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=uint32_t(deviceExt.size());dci.ppEnabledExtensionNames=deviceExt.data();dci.pEnabledFeatures=&features;VKC(vkCreateDevice(pd,&dci,nullptr,&dev));volkLoadDevice(dev);vkGetDeviceQueue(dev,qi,0,&queue);
 if(windowed){createWindowSurface(960,540);if(auxDxgi)createDxgiHelper();VkBool32 supported;VKC(vkGetPhysicalDeviceSurfaceSupportKHR(pd,qi,surface,&supported));if(!supported)return 10;createSwapchain();VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VKC(vkCreateSemaphore(dev,&si,nullptr,&acquired));}
 VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=qi;pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VkCommandPool pool;VKC(vkCreateCommandPool(dev,&pci,nullptr,&pool));VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VKC(vkAllocateCommandBuffers(dev,&cai,&cmd));VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VKC(vkCreateFence(dev,&fi,nullptr,&fence));
 wchar_t cwd[MAX_PATH];GetCurrentDirectoryW(MAX_PATH,cwd);const wchar_t* paths[]={cwd};NVSDK_NGX_FeatureCommonInfo common{};common.PathListInfo.Path=paths;common.PathListInfo.Length=1;common.LoggingInfo.LoggingCallback=&ngxlog;common.LoggingInfo.MinimumLoggingLevel=NVSDK_NGX_LOGGING_LEVEL_ON;common.LoggingInfo.DisableOtherLoggingSinks=false;
 NGXC(NVSDK_NGX_VULKAN_Init_with_ProjectID("8cd1888b-fbd1-48b5-af01-2311b9a3bcee",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"1.0",cwd,instance,pd,dev,vkGetInstanceProcAddr,vkGetDeviceProcAddr,&common));
 NVSDK_NGX_Parameter* params=nullptr;NGXC(NVSDK_NGX_VULKAN_GetCapabilityParameters(&params));int available=0;params->Get(NVSDK_NGX_Parameter_SuperSampling_Available,&available);printf("DLSS available=%d\n",available);if(!available)return 8;
 constexpr uint32_t iw=640,ih=360,ow=960,oh=540;Image color=createImage(iw,ih,VK_FORMAT_R32G32B32A32_SFLOAT),depth=createImage(iw,ih,VK_FORMAT_R32_SFLOAT),mv=createImage(iw,ih,VK_FORMAT_R32G32_SFLOAT),out=createImage(ow,oh,VK_FORMAT_R32G32B32A32_SFLOAT);
 VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=ow*oh*16;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer staging;VKC(vkCreateBuffer(dev,&bci,nullptr,&staging));VkMemoryRequirements br;vkGetBufferMemoryRequirements(dev,staging,&br);VkMemoryAllocateInfo bma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};bma.allocationSize=br.size;bma.memoryTypeIndex=memtype(br.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);VkDeviceMemory bm;VKC(vkAllocateMemory(dev,&bma,nullptr,&bm));VKC(vkBindBufferMemory(dev,staging,bm,0));float* pixels;VKC(vkMapMemory(dev,bm,0,VK_WHOLE_SIZE,0,reinterpret_cast<void**>(&pixels)));
 for(uint32_t y=0;y<ih;y++)for(uint32_t x=0;x<iw;x++){size_t i=(y*iw+x)*4;pixels[i]=float(x)/iw;pixels[i+1]=float(y)/ih;pixels[i+2]=((x/24+y/24)&1)?0.2f:0.8f;pixels[i+3]=1;}
 begin();for(Image*a:{&color,&depth,&mv,&out})barrier(*a,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
 VkBufferImageCopy bic{};bic.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};bic.imageExtent={iw,ih,1};vkCmdCopyBufferToImage(cmd,staging,color.image,VK_IMAGE_LAYOUT_GENERAL,1,&bic);VkClearColorValue cd{{0.5f,0,0,0}},cz{{0,0,0,0}};vkCmdClearColorImage(cmd,depth.image,VK_IMAGE_LAYOUT_GENERAL,&cd,1,&range);vkCmdClearColorImage(cmd,mv.image,VK_IMAGE_LAYOUT_GENERAL,&cz,1,&range);vkCmdClearColorImage(cmd,out.image,VK_IMAGE_LAYOUT_GENERAL,&cz,1,&range);
 for(Image*a:{&color,&depth,&mv,&out})barrier(*a,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
 submit();if(windowed){puts("PHASE DLSS OFF: Vulkan presents");for(int i=0;i<12;i++)present(color);}
 begin();NVSDK_NGX_DLSS_Create_Params cp{};cp.Feature.InWidth=iw;cp.Feature.InHeight=ih;cp.Feature.InTargetWidth=ow;cp.Feature.InTargetHeight=oh;cp.Feature.InPerfQualityValue=NVSDK_NGX_PerfQuality_Value_MaxQuality;cp.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;NVSDK_NGX_Handle* handle=nullptr;NGXC(NGX_VULKAN_CREATE_DLSS_EXT1(dev,cmd,1,1,&handle,params,&cp));submit();
 NVSDK_NGX_VK_DLSS_Eval_Params ep{};ep.Feature.pInColor=&color.ngx;ep.Feature.pInOutput=&out.ngx;ep.pInDepth=&depth.ngx;ep.pInMotionVectors=&mv.ngx;ep.InRenderSubrectDimensions={iw,ih};ep.InMVScaleX=ep.InMVScaleY=1;ep.InPreExposure=ep.InExposureScale=1;ep.InFrameTimeDeltaInMsec=16.667f;
 const int phaseFrames=windowed?180:8;
 DxlCounts beforeReplacement;
 for(int f=0;f<(windowed?3*phaseFrames:8);f++){
  if(windowed && f==phaseFrames){puts("PHASE resize same HWND");VKC(vkDeviceWaitIdle(dev));RECT r{0,0,800,450};AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(windowHandle,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);pump();createSwapchain();}
  if(windowed && f==2*phaseFrames){beforeReplacement=dxlCounts();printf("DXL before replacement: valid=%d presents=%llu NR=%llu\n",beforeReplacement.valid,beforeReplacement.presents,beforeReplacement.nr);puts("PHASE replacement HWND/swapchain");VKC(vkDeviceWaitIdle(dev));vkDestroySwapchainKHR(dev,swapchain,nullptr);swapchain=VK_NULL_HANDLE;vkDestroySurfaceKHR(instance,surface,nullptr);DestroyWindow(windowHandle);printf("Replacement window gap=%lu ms\n",replacementDelayMs);Sleep(replacementDelayMs);createWindowSurface(960,540);createSwapchain();if(reinitializeDlss){NGXC(NVSDK_NGX_VULKAN_ReleaseFeature(handle));handle=nullptr;puts("PHASE DLSS OFF then recreate feature");for(int j=0;j<12;j++)present(color);begin();NGXC(NGX_VULKAN_CREATE_DLSS_EXT1(dev,cmd,1,1,&handle,params,&cp));submit();}}
  begin();ep.InReset=f==0 || f==2*phaseFrames;auto eval=NGX_VULKAN_EVALUATE_DLSS_EXT(cmd,handle,params,&ep);if(NVSDK_NGX_FAILED(eval)||f<3||f%60==0)ngxcheck(eval,"NGX_VULKAN_EVALUATE_DLSS_EXT");barrier(out,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);submit();if(f<3||f%60==0)printf("GPU completed frame=%d\n",f);if(windowed)present(out);
  if(helperChain && f%30==0)helperChain->Present(0,0);
 }
 bool dxlProgress=true;if(dxlLoaded&&windowed){auto after=dxlCounts();dxlProgress=beforeReplacement.valid&&after.valid&&after.presents>beforeReplacement.presents+30&&after.nr>beforeReplacement.nr;printf("DXL after replacement: valid=%d presents=%llu NR=%llu; progress=%s\n",after.valid,after.presents,after.nr,dxlProgress?"PASS":"FAIL");}
 if(helperChain){helperChain->Release();helperContext->Release();helperDevice->Release();DestroyWindow(helperWindow);}
 begin();barrier(out,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);bic.imageExtent={ow,oh,1};vkCmdCopyImageToBuffer(cmd,out.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging,1,&bic);submit();
 double sum=0;uint64_t nonzero=0;float maxvalue=0;FILE* ppm=nullptr;fopen_s(&ppm,"vulkan-dlss-output.ppm","wb");if(ppm)fprintf(ppm,"P6\n%u %u\n255\n",ow,oh);
 for(size_t i=0;i<ow*oh;i++)for(int c=0;c<3;c++){float v=pixels[i*4+c];if(v>0.001f)nonzero++;sum+=v;maxvalue=std::max(maxvalue,v);unsigned char b=static_cast<unsigned char>(std::clamp(v,0.0f,1.0f)*255);if(ppm)fwrite(&b,1,1,ppm);}if(ppm)fclose(ppm);printf("READBACK mean=%f max=%f nonzero=%llu of %u\n",sum/(ow*oh*3),maxvalue,nonzero,ow*oh*3);
 NGXC(NVSDK_NGX_VULKAN_ReleaseFeature(handle));NGXC(NVSDK_NGX_VULKAN_DestroyParameters(params));NGXC(NVSDK_NGX_VULKAN_Shutdown1(dev));vkUnmapMemory(dev,bm);vkDestroyBuffer(dev,staging,nullptr);vkFreeMemory(dev,bm,nullptr);for(Image*a:{&color,&depth,&mv,&out})destroy(*a);vkDestroyFence(dev,fence,nullptr);vkDestroyCommandPool(dev,pool,nullptr);if(windowed){vkDestroySemaphore(dev,acquired,nullptr);vkDestroySwapchainKHR(dev,swapchain,nullptr);vkDestroySurfaceKHR(instance,surface,nullptr);DestroyWindow(windowHandle);}vkDestroyDevice(dev,nullptr);vkDestroyInstance(instance,nullptr);printf("%s Vulkan DLSS: evaluations + GPU fence + nonblack image; presents=%u\n",nonzero>ow*oh?"PASS":"FAIL",windowed?552+(reinitializeDlss?12:0):0);return !dxlProgress?11:nonzero>ow*oh?0:9;
}
