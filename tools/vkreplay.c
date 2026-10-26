// Standalone Vulkan replay of a post-processing chain on the host GPU (found the AMD GCN1 barrier bug, see
// docs/STATUS.md): input S (raw RGBA8) is copied by a copy shader into X, then a blur shader runs X->Y, Y->Z, Z->Y with
// uniform buffers 4 / 5 (cb5 offsets x1, x2, x4), as the game's light-shaft passes 023-026. Shaders: run/build/glsl.
//   vkreplay <vs> <fs> <in.rgba> <iw> <ih> <ow> <oh> <cb4.f32> <cb5.f32> <out> [mode]   (outputs <out>.p0..p3)
//   env: COPY_VS / COPY_FS copy shaders, FRAMES=n, LOCKSTEP=1, EXPLICIT=1 (explicit barrier flags), CLR / NO_CLEAR,
//        NO_RESTART, VB_SIZE, ONLY_COPY, PASS0.  Build: clang -O1 -o vkreplay.exe tools/vkreplay.c -lvulkan-1 -lshaderc_shared
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <shaderc/shaderc.h>

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)
static VkDevice dev;
static VkPhysicalDevice pd;

static void *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(*n + 1); fread(b, 1, *n, f); b[*n] = 0; fclose(f);
    return b;
}

static VkShaderModule module(const char *path, shaderc_shader_kind kind) {
    size_t n; char *src = slurp(path, &n);
    shaderc_compiler_t c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, src, n, kind, path, "main", o);
    if (shaderc_result_get_compilation_status(r)) { fprintf(stderr, "%s\n", shaderc_result_get_error_message(r)); exit(1); }
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shaderc_result_get_length(r),
                                   .pCode = (const uint32_t *)shaderc_result_get_bytes(r)};
    VkShaderModule m;
    CK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "no memory type\n"); exit(1);
}

typedef struct { VkBuffer b; VkDeviceMemory m; void *p; } Buf;
static Buf buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf r;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    CK(vkCreateBuffer(dev, &bi, NULL, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, r.b, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CK(vkAllocateMemory(dev, &ai, NULL, &r.m));
    CK(vkBindBufferMemory(dev, r.b, r.m, 0));
    CK(vkMapMemory(dev, r.m, 0, size, 0, &r.p));
    return r;
}

static int like_port = 1;
static VkImage image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkDeviceMemory *mem) {
    if (like_port) usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = like_port ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VkImage im; CK(vkCreateImage(dev, &ci, NULL, &im));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                               .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CK(vkAllocateMemory(dev, &ai, NULL, mem)); CK(vkBindImageMemory(dev, im, *mem, 0));
    return im;
}

static VkImageView view(VkImage im) {
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v; CK(vkCreateImageView(dev, &ci, NULL, &v)); return v;
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    VkPipelineStageFlags ss = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, ds = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (getenv("EXPLICIT")) {
        mb.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT;
        mb.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        ss = ds = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    vkCmdPipelineBarrier(cb, ss, ds, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void to_general(VkCommandBuffer cb, VkImage im) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = im, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

int main(int argc, char **argv) {
    if (argc < 11) { fprintf(stderr, "usage: replay vs fs in.rgba iw ih ow oh cb4 cb5 out.rgba [mode]\n"); return 1; }
    uint32_t iw = atoi(argv[4]), ih = atoi(argv[5]), ow = atoi(argv[6]), oh = atoi(argv[7]);
    int mode = argc > 11 ? atoi(argv[11]) : 0;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    fprintf(stderr, "gpu: %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    // resources
    size_t sz; void *in = slurp(argv[3], &sz);
    Buf up = buffer((VkDeviceSize)iw * ih * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT); memcpy(up.p, in, (size_t)iw * ih * 4);
    Buf rb = buffer((VkDeviceSize)ow * oh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buf ub[3];
    for (int i = 0; i < 3; i++) { ub[i] = buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); memset(ub[i].p, 0, 256); }
    { size_t k; void *d = slurp(argv[8], &k); memcpy(ub[0].p, d, k > 256 ? 256 : k); d = slurp(argv[9], &k); memcpy(ub[1].p, d, k > 256 ? 256 : k); }
    // images: S uploaded input, X = rendered copy of S (like pass 023's target), Y / Z ping-pong (024 -> Y, 025 -> Z, 026 -> Y)
    VkDeviceMemory mm[4];
    VkImage S = image(iw, ih, 0, &mm[0]), X = image(ow, oh, 0, &mm[1]), Y = image(ow, oh, 0, &mm[2]), Z = image(ow, oh, 0, &mm[3]);
    VkImageView Sv = view(S), Xv = view(X), Yv = view(Y), Zv = view(Z);
    VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
                               .maxLod = VK_LOD_CLAMP_NONE, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSampler smp; CK(vkCreateSampler(dev, &sci, NULL, &smp));
    VkDescriptorSetLayoutBinding b[4] = {{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {19, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
                                         {32, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    VkDescriptorSetLayout sl[2];
    VkDescriptorSetLayoutCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    CK(vkCreateDescriptorSetLayout(dev, &empty, NULL, &sl[0]));
    CK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &sl[1]));
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sl};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkAttachmentDescription ad = {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = 1, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad,
                                   .subpassCount = 1, .pSubpasses = &sp};
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkImageView tv[3] = {Xv, Yv, Zv};
    VkFramebuffer tf[3];
    for (int i = 0; i < 3; i++) {
        VkFramebufferCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
                                       .pAttachments = &tv[i], .width = ow, .height = oh, .layers = 1};
        CK(vkCreateFramebuffer(dev, &fci, NULL, &tf[i]));
    }
    VkPipeline pipes[2];
    const char *fsp[2] = {argv[2], getenv("COPY_FS")}, *vsp[2] = {argv[1], getenv("COPY_VS")};
    for (int pi = getenv("ONLY_COPY") ? 1 : 0; pi < 2; pi++) {
        VkPipelineShaderStageCreateInfo st[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = module(vsp[pi], shaderc_vertex_shader), .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = module(fsp[pi], shaderc_fragment_shader), .pName = "main"}};
        VkVertexInputBindingDescription vbd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription vad = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
                                                   .pVertexBindingDescriptions = &vbd, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &vad};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, .primitiveRestartEnable = getenv("NO_RESTART") ? VK_FALSE : VK_TRUE};
        VkViewport vp = {0, (float)oh, (float)ow, -(float)oh, 0, 1};
        VkRect2D sc = {{0, 0}, {ow, oh}};
        VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp,
                                                .scissorCount = 1, .pScissors = &sc};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1,
                                                     .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
        VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba};
        VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
                                             .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs, .pRasterizationState = &rs,
                                             .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs, .layout = pl, .renderPass = rp};
        CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipes[pi]));
    }
    float quad[4][4] = {{-1, 1, 0, 0}, {-1, -1, 0, 1}, {1, 1, 1, 0}, {1, -1, 1, 1}};
    Buf vb = buffer(getenv("VB_SIZE") ? atoi(getenv("VB_SIZE")) : sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); memcpy(vb.p, quad, sizeof quad);
    Buf c5[3];  // cb5 per blur pass: offsets x1, x2, x4 (passes 024, 025, 026)
    for (int i = 0; i < 3; i++) {
        c5[i] = buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); memcpy(c5[i].p, ub[1].p, 256);
        float *f = c5[i].p; for (int j = 0; j < 16; j += 2) f[j] *= (float)(1 << i);
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 16, .poolSizeCount = 2, .pPoolSizes = ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    // passes: 0 copy S->X, 1 blur X->Y, 2 blur Y->Z, 3 blur Z->Y
    VkImageView pin[4] = {Sv, Xv, Yv, Zv};
    VkFramebuffer pout[4] = {tf[0], tf[1], tf[2], tf[1]};
    VkImage pimg[4] = {X, Y, Z, Y};
    int pp[4] = {1, 0, 0, 0};
    VkDescriptorSet sets[4][2];
    for (int i = 0; i < 4; i++) {
        VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 2, .pSetLayouts = sl};
        CK(vkAllocateDescriptorSets(dev, &dsai, sets[i]));
        VkDescriptorBufferInfo bi[3] = {{ub[0].b, 0, 256}, {c5[i ? i - 1 : 0].b, 0, 256}, {ub[2].b, 0, 32}};
        VkDescriptorImageInfo ii = {smp, pin[i], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[4];
        uint32_t bind[3] = {4, 5, 19};
        for (int k = 0; k < 3; k++)
            w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i][1], .dstBinding = bind[k], .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[k]};
        w[3] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[i][1], .dstBinding = 32, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
        vkUpdateDescriptorSets(dev, 4, w, 0, NULL);
    }
    Buf rbs[4];
    for (int i = 0; i < 4; i++) rbs[i] = buffer((VkDeviceSize)ow * oh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 4, lockstep = getenv("LOCKSTEP") != NULL;
    for (int fr = 0; fr < frames; fr++) {
        VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = 0, .commandBufferCount = 1};
        VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &cbai, &cb));
        VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CK(vkBeginCommandBuffer(cb, &cbbi));
        if (fr == 0) {
            VkImage all4[4] = {S, X, Y, Z};
            for (int i = 0; i < 4; i++) {
                to_general(cb, all4[i]);
                VkClearColorValue zero = {{getenv("CLR") ? (float)atof(getenv("CLR")) : 0, 0, getenv("CLR") ? (float)atof(getenv("CLR")) : 0, getenv("CLR_A") ? (float)atof(getenv("CLR_A")) : 0}};
                VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                if (!getenv("NO_CLEAR")) vkCmdClearColorImage(cb, all4[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
            }
            barrier(cb);
            VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {iw, ih, 1}};
            vkCmdCopyBufferToImage(cb, up.b, S, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
        for (int i = 0; i < (getenv("ONLY_COPY") || getenv("PASS0") ? 1 : 4); i++) {
            barrier(cb);
            VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = pout[i], .renderArea = {{0, 0}, {ow, oh}}};
            vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[pp[i]]);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets[i], 0, NULL);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cb, 0, 1, &vb.b, &off);
            vkCmdDraw(cb, 4, 1, 0, 0);
            vkCmdEndRenderPass(cb);
            if (fr == frames - 1) {  // last frame: every pass's output
                barrier(cb);
                VkBufferImageCopy c2 = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {ow, oh, 1}};
                vkCmdCopyImageToBuffer(cb, pimg[i], VK_IMAGE_LAYOUT_GENERAL, rbs[i].b, 1, &c2);
            }
        }
        CK(vkEndCommandBuffer(cb));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
        if (lockstep) CK(vkQueueWaitIdle(q));
    }
    CK(vkQueueWaitIdle(q));
    for (int i = 0; i < 4; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s.p%d", argv[10], i);
        FILE *f = fopen(p, "wb"); fwrite(rbs[i].p, 4, (size_t)ow * oh, f); fclose(f);
    }
    fprintf(stderr, "ok\n");
    return 0;
}
