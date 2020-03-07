#include <iostream>
#include <array>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <string>
#include "real_dlsym.h"
#include "loaders/loader_gl.h"
#include "GL/gl3w.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "font_default.h"
#include "overlay.h"
#include "../gpu.h"
#include "../cpu.h"
#include "../mesa/util/os_time.h"
#include "../memory.h"
#include "../iostats.h"

#include <chrono>
#include <iomanip>

#define EXPORT_C_(type) extern "C" __attribute__((__visibility__("default"))) type

EXPORT_C_(void *) glXGetProcAddress(const unsigned char* procName);
EXPORT_C_(void *) glXGetProcAddressARB(const unsigned char* procName);

gl_loader gl;
uint64_t last_fps_update;

struct state {
    ImGuiContext *imgui_ctx = nullptr;
    ImFont* font = nullptr;
    ImFont* font1 = nullptr;
};

static ImVec2 window_size;
static overlay_params params {};
static swapchain_stats sw_stats {};
static state *current_state;
static bool inited = false;
std::unordered_map<void*, state> g_imgui_states;

void imgui_init()
{
    if (inited)
        return;
    inited = true;
    parse_overlay_config(&params, getenv("MANGOHUD_CONFIG"));
    window_size = ImVec2(params.width, params.height);
}

void imgui_create(void *ctx)
{
    if (!ctx)
        return;
    imgui_init();

    gl3wInit();
    std::cerr << "GL version: " << glGetString(GL_VERSION) << std::endl;

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    auto& state = g_imgui_states[ctx];
    state.imgui_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_FrameBg]   = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
    style.Colors[ImGuiCol_PlotLines] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]  = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    GLint vp [4]; glGetIntegerv (GL_VIEWPORT, vp);
    printf("viewport %d %d %d %d\n", vp[0], vp[1], vp[2], vp[3]);
    ImGui::GetIO().IniFilename = NULL;
    ImGui::GetIO().DisplaySize = ImVec2(vp[2], vp[3]);

    ImGui_ImplOpenGL3_Init();
    // Make a dummy GL call (we don't actually need the result)
    // IF YOU GET A CRASH HERE: it probably means that you haven't initialized the OpenGL function loader used by this code.
    // Desktop OpenGL 3/4 need a function loader. See the IMGUI_IMPL_OPENGL_LOADER_xxx explanation above.
    GLint current_texture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_texture);

    float font_size = 24.f;
    ImFontConfig font_cfg = ImFontConfig();
    const char* ttf_compressed_base85 = GetDefaultCompressedFontDataTTFBase85();
    const ImWchar* glyph_ranges = io.Fonts->GetGlyphRangesDefault();

    state.font = io.Fonts->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size, &font_cfg, glyph_ranges);
    state.font1 = io.Fonts->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size * 0.55, &font_cfg, glyph_ranges);
    current_state = &state;
    engineName = "OpenGL";
}

void imgui_destroy(void *ctx)
{
    if (!ctx)
        return;

    auto it = g_imgui_states.find(ctx);
    if (it != g_imgui_states.end()) {
        ImGui::DestroyContext(it->second.imgui_ctx);
        g_imgui_states.erase(it);
    }
}

void imgui_shutdown()
{
    std::cerr << __func__ << std::endl;

    ImGui_ImplOpenGL3_Shutdown();

    for(auto &p : g_imgui_states)
        ImGui::DestroyContext(p.second.imgui_ctx);
    g_imgui_states.clear();
}

void imgui_set_context(void *ctx)
{
    if (!ctx) {
        imgui_shutdown();
        current_state = nullptr;
        return;
    }
    std::cerr << __func__ << std::endl;

    auto it = g_imgui_states.find(ctx);
    if (it != g_imgui_states.end()) {
        ImGui::SetCurrentContext(it->second.imgui_ctx);
        current_state = &it->second;
    } else {
        imgui_create(ctx);
    }
    sw_stats.font1 = current_state->font1;
}

void imgui_render()
{
    if (!ImGui::GetCurrentContext())
        return;
    GLint vp [4]; glGetIntegerv (GL_VIEWPORT, vp);
    ImGui::GetIO().DisplaySize = ImVec2(vp[2], vp[3]);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    position_layer(params, window_size, vp[2], vp[3]);
    render_imgui(sw_stats, params, window_size, vp[2], vp[3]);
    ImGui::PopStyleVar(2);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void* get_proc_address(const char* name) {
    void (*func)() = (void (*)())real_dlsym( RTLD_NEXT, name );

    if (!func) {
        std::cerr << "MangoHud: Failed to get function '" << name << "'" << std::endl;
        exit( 1 );
    }

    return (void*)func;
}

void* get_glx_proc_address(const char* name) {
    gl.Load();

    void *func = gl.glXGetProcAddress( (const unsigned char*) name );

    if (!func)
        func = gl.glXGetProcAddressARB( (const unsigned char*) name );

    if (!func)
        func = get_proc_address( name );

    return func;
}

EXPORT_C_(void *) glXCreateContext(void *dpy, void *vis, void *shareList, int direct)
{
    gl.Load();
    void *ctx = gl.glXCreateContext(dpy, vis, shareList, direct);
    std::cerr << __func__ << ":" << ctx << std::endl;
    return ctx;
}

EXPORT_C_(bool) glXMakeCurrent(void* dpy, void* drawable, void* ctx) {
    gl.Load();
    bool ret = gl.glXMakeCurrent(dpy, drawable, ctx);
    if (ret)
        imgui_set_context(ctx);
    std::cerr << __func__ << std::endl;
    return ret;
}

EXPORT_C_(void) glXSwapBuffers(void* dpy, void* drawable) {
    gl.Load();
    uint64_t now = os_time_get(); /* us */
    double elapsed = (double)(now - last_fps_update); /* us */
    if (elapsed > 500000){
        uint32_t f_idx = sw_stats.n_frames % (sizeof(sw_stats.frames_stats) / sizeof(sw_stats.frames_stats[0]));
        sw_stats.fps = 1000000.0f * sw_stats.n_frames / elapsed;
        memset(&sw_stats.frames_stats[f_idx], 0, sizeof(sw_stats.frames_stats[f_idx]));
        for (int s = 0; s < OVERLAY_PARAM_ENABLED_MAX; s++) {
            sw_stats.frames_stats[f_idx].stats[s] = 50;
        }
        if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats]) {
            std::string gpu = (char*)glGetString(GL_RENDERER);
            if (gpu.find("Radeon") != std::string::npos
                || gpu.find("AMD") != std::string::npos){
                pthread_create(&gpuThread, NULL, &getAmdGpuUsage, NULL);
            } else {
                pthread_create(&gpuThread, NULL, &getNvidiaGpuInfo, NULL);
            }

        }
        cpuStats.UpdateCPUData();
        sw_stats.total_cpu = cpuStats.GetCPUDataTotal().percent;
        pthread_create(&memoryThread, NULL, &update_meminfo, NULL);
        pthread_create(&ioThread, NULL, &getIoStats, &sw_stats.io);
        sw_stats.n_frames = 0;
        last_fps_update = now;
    }
    sw_stats.n_frames++;
    imgui_render();
    gl.glXSwapBuffers(dpy, drawable);
}

struct func_ptr {
   const char *name;
   void *ptr;
};

static std::array<const func_ptr, 5> name_to_funcptr_map = {{
#define ADD_HOOK(fn) { #fn, (void *) fn }
   ADD_HOOK(glXGetProcAddress),
   ADD_HOOK(glXGetProcAddressARB),
   ADD_HOOK(glXCreateContext),
   ADD_HOOK(glXMakeCurrent),
   ADD_HOOK(glXSwapBuffers),
#undef ADD_HOOK
}};

static void *find_ptr(const char *name)
{
   for (auto& func : name_to_funcptr_map) {
      if (strcmp(name, func.name) == 0)
         return func.ptr;
   }

   return nullptr;
}

EXPORT_C_(void *) glXGetProcAddress(const unsigned char* procName) {
    gl.Load();

    //std::cerr << __func__ << ":" << procName << std::endl;

    void* func = find_ptr( (const char*)procName );
    if (func)
        return func;

    return gl.glXGetProcAddress(procName);
}

EXPORT_C_(void *) glXGetProcAddressARB(const unsigned char* procName) {
    gl.Load();

    //std::cerr << __func__ << ":" << procName << std::endl;

    void* func = find_ptr( (const char*)procName );
    if (func)
        return func;

    return gl.glXGetProcAddressARB(procName);
}

#ifdef HOOK_DLSYM
EXPORT_C_(void*) dlsym(void * handle, const char * name)
{
    void* func = find_ptr(name);
    if (func) {
        //std::cerr << __func__ << ":" << name << std::endl;
        return func;
    }

    //std::cerr << __func__ << ": foreign: " << name << std::endl;
    return real_dlsym(handle, name);
}
#endif