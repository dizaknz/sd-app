//
// gui_main.cpp — Dear ImGui front-end for QwenImageGenerator
//
// Threading model:
//   * All generation (and the model load) runs on one worker thread.
//   * The worker never touches ImGui or OpenGL. It pushes log lines and step
//     counts into mutex-guarded state; the UI thread polls that each frame.
//   * Finished pixels are handed over as a plain byte buffer; the UI thread
//     uploads them to a GL texture, because GL calls must stay on the thread
//     that owns the context.
//

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <bindings/imgui_impl_glfw.h>
#include <bindings/imgui_impl_opengl3.h>
#include <imgui.h>

#include "gen/qwen_image_generator.h"

namespace {

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

constexpr size_t kMaxLogLines = 2000;

//
// Shared worker <-> UI state
//
struct SharedState {
    std::mutex mutex;
    std::deque<std::string> log_lines;
    bool log_dirty = false;

    // Set by the worker when an image is ready; consumed by the UI thread.
    QwenImage pending_image;
    bool has_pending_image = false;

    std::string status = "Idle";
    std::string error;

    // Progress is read every frame, so keep it lock-free.
    std::atomic<int> step{0};
    std::atomic<int> total_steps{0};
    std::atomic<float> sec_per_step{0.0f};

    std::atomic<bool> busy{false};
    std::atomic<bool> model_loaded{false};

    void push_log(std::string line) {
        std::lock_guard<std::mutex> lock(mutex);
        // sd.cpp log lines already carry their own newline.
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        log_lines.push_back(std::move(line));
        if (log_lines.size() > kMaxLogLines) {
            log_lines.pop_front();
        }
        log_dirty = true;
    }

    void set_status(std::string s) {
        std::lock_guard<std::mutex> lock(mutex);
        status = std::move(s);
    }
};

//
// GL texture holding the most recent result
//
struct ImageTexture {
    GLuint id = 0;
    int width = 0;
    int height = 0;

    void upload(const QwenImage &img) {
        if (img.data.empty()) {
            return;
        }
        if (id == 0) {
            glGenTextures(1, &id);
        }
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // sd.cpp hands back tightly packed RGB. Default unpack alignment is 4,
        // which silently skews any image whose row length isn't a multiple of 4.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        const GLenum fmt = (img.channel == 4) ? GL_RGBA : GL_RGB;
        glTexImage2D(
            GL_TEXTURE_2D, 0, fmt, static_cast<GLsizei>(img.width), static_cast<GLsizei>(img.height), 0, fmt,
            GL_UNSIGNED_BYTE, img.data.data()
        );

        width = static_cast<int>(img.width);
        height = static_cast<int>(img.height);
    }

    void destroy() {
        if (id) {
            glDeleteTextures(1, &id);
            id = 0;
        }
    }
};

//
// Worker
//
void run_generation(
    QwenImageGenerator *gen, SharedState *shared, QwenImageConfig cfg, std::string prompt, bool save_to_disk
) {
    shared->busy.store(true);
    shared->step.store(0);
    shared->total_steps.store(cfg.steps);

    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->error.clear();
    }

    // Sampling settings can change per run; paths can't, since the context is
    // already built. generate(prompt, overrides) honours exactly that split.
    gen->config() = cfg;

    if (!gen->is_loaded()) {
        shared->set_status("Loading model (this takes a while)...");
        if (!gen->load()) {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->error = gen->last_error();
            shared->status = "Load failed";
            shared->busy.store(false);
            return;
        }
        shared->model_loaded.store(true);
    }

    shared->set_status("Generating...");
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<QwenImage> images = gen->generate(prompt, cfg);

    const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    if (images.empty()) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->error = gen->last_error().empty() ? "generation produced no image" : gen->last_error();
        shared->status = "Failed";
    } else {
        if (save_to_disk) {
            std::error_code ec;
            std::filesystem::create_directories(cfg.output_dir, ec);
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            const std::string path = cfg.output_dir + "/" + cfg.output_stem + "_" + std::to_string(ms) + ".png";
            if (QwenImageGenerator::save_png(images.front(), path)) {
                shared->push_log("[INFO ] wrote " + path);
            } else {
                shared->push_log("[WARN ] could not write " + path);
            }
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->pending_image = std::move(images.front());
        shared->has_pending_image = true;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Done in %.1fs", secs);
        shared->status = buf;
    }

    shared->busy.store(false);
}

//
// UI helpers
//
void combo_string(const char *label, std::string &value, const char *const *options, int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if (value == options[i]) {
            current = i;
            break;
        }
    }
    if (ImGui::Combo(label, &current, options, count)) {
        value = options[current];
    }
}

void glfw_error_callback(int error, const char *description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void ApplyDarkTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Window & Layout Padding
    style.WindowPadding     = ImVec2(15.0f, 15.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.IndentSpacing     = 25.0f;
    style.ScrollbarSize     = 15.0f;
    style.GrabMinSize       = 12.0f;

    // Border & Corner Rounding
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 6.0f;

    // Backgrounds
    colors[ImGuiCol_WindowBg]             = ImVec4(0.09f, 0.09f, 0.10f, 1.00f); // Deep dark gray
    colors[ImGuiCol_ChildBg]              = ImVec4(0.12f, 0.12f, 0.14f, 1.00f); 
    colors[ImGuiCol_PopupBg]              = ImVec4(0.12f, 0.12f, 0.14f, 0.98f);
    
    // Borders
    colors[ImGuiCol_Border]               = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Text
    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.92f, 0.95f, 1.00f); // Soft white
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);

    // Headers & Fields
    colors[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.22f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.26f, 0.26f, 0.30f, 1.00f);
    
    colors[ImGuiCol_TitleBg]              = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);

    // Buttons
    colors[ImGuiCol_Button]               = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.38f, 0.38f, 0.43f, 1.00f);

    // Headers (Collapsing headers, tree nodes)
    colors[ImGuiCol_Header]               = ImVec4(0.20f, 0.20f, 0.22f, 1.00f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);

    // Accents (Tabs, Sliders, Scrollbars)
    colors[ImGuiCol_Tab]                  = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.28f, 0.28f, 0.31f, 1.00f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]  = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);

    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.24f, 0.24f, 0.27f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.38f, 0.38f, 0.43f, 1.00f);

    colors[ImGuiCol_SliderGrab]           = ImVec4(0.44f, 0.44f, 0.48f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);

    colors[ImGuiCol_CheckMark]            = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
}

} // namespace

// qwen app entry point
int main(int, char **) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow *window = glfwCreateWindow(1500, 1000, "Qwen Image", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    ApplyDarkTheme();

    // --- app state ---
    SharedState shared;
    QwenImageConfig cfg; // defaults already match your CLI line
    QwenImageGenerator gen(cfg);
    ImageTexture texture;
    std::thread worker;

    gen.set_log_sink([&shared](sd_log_level_t level, const char *text) {
        const char *tag = (level == SD_LOG_ERROR)  ? "[ERROR] "
                          : (level == SD_LOG_WARN) ? "[WARN ] "
                          : (level == SD_LOG_INFO) ? "[INFO ] "
                                                   : "[DEBUG] ";
        shared.push_log(std::string(tag) + text);
    });

    gen.set_progress_sink([&shared](int step, int steps, float sec) {
        shared.step.store(step);
        shared.total_steps.store(steps);
        shared.sec_per_step.store(sec);
    });

    char prompt_buf[4096] = "a photo of a tui perched on a flax flower, dawn light";
    char negative_buf[1024] = "";
    bool save_to_disk = true;
    bool auto_scroll = true;

    static const char *kSamplers[] = {"euler",     "euler_a", "heun",    "dpm2", "dpm++2s_a",     "dpm++2m",
                                      "dpm++2mv2", "ipndm",   "ipndm_v", "lcm",  "ddim_trailing", "tcd"};
    static const char *kSchedulers[] = {"",           "discrete",   "karras",      "exponential",
                                        "ays",        "gits",       "sgm_uniform", "simple",
                                        "smoothstep", "kl_optimal", "lcm"};

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Pick up a finished image on the UI thread, where GL is legal.
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            if (shared.has_pending_image) {
                texture.upload(shared.pending_image);
                shared.has_pending_image = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin(
            "Qwen Image", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus
        );

        const bool busy = shared.busy.load();

        // ---------------- left: controls ----------------
        ImGui::BeginChild("controls", ImVec2(440, 0), true);
        {
            ImGui::BeginDisabled(busy);

            ImGui::SeparatorText("Prompt");
            ImGui::InputTextMultiline("##prompt", prompt_buf, sizeof(prompt_buf), ImVec2(-FLT_MIN, 110));
            ImGui::InputText("negative", negative_buf, sizeof(negative_buf));

            ImGui::SeparatorText("Sampling");
            ImGui::SliderInt("steps", &cfg.steps, 1, 100);
            ImGui::SliderFloat("cfg scale", &cfg.cfg_scale, 1.0f, 12.0f, "%.2f");
            ImGui::SliderFloat("flow shift", &cfg.flow_shift, 0.0f, 12.0f, "%.2f");
            combo_string("sampler", cfg.sample_method, kSamplers, IM_ARRAYSIZE(kSamplers));
            combo_string("scheduler", cfg.scheduler, kSchedulers, IM_ARRAYSIZE(kSchedulers));

            int seed = static_cast<int>(cfg.seed);
            if (ImGui::InputInt("seed (-1 = random)", &seed)) {
                cfg.seed = seed;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("rand")) {
                cfg.seed = -1;
            }

            ImGui::SeparatorText("Size");
            ImGui::InputInt("width", &cfg.width, 64, 256);
            ImGui::InputInt("height", &cfg.height, 64, 256);
            // Qwen-Image wants multiples of 16; snap rather than fail at sample time.
            cfg.width = (cfg.width / 16) * 16;
            cfg.height = (cfg.height / 16) * 16;
            if (cfg.width < 256)
                cfg.width = 256;
            if (cfg.height < 256)
                cfg.height = 256;

            // ---- context options: locked once the model is resident ----
            const bool loaded = shared.model_loaded.load();
            ImGui::SeparatorText(loaded ? "Memory (locked — reload to change)" : "Memory");
            ImGui::BeginDisabled(loaded);
            ImGui::Checkbox("offload params to CPU", &cfg.offload_to_cpu);
            char vram_buf[64];
            std::snprintf(vram_buf, sizeof(vram_buf), "%s", cfg.max_vram.c_str());
            if (ImGui::InputText("max VRAM (GiB)", vram_buf, sizeof(vram_buf))) {
                cfg.max_vram = vram_buf;
            }
            ImGui::Checkbox("diffusion flash attn", &cfg.diffusion_flash_attn);
            ImGui::EndDisabled();

            ImGui::SeparatorText("Output");
            ImGui::Checkbox("save PNG to output dir", &save_to_disk);

            ImGui::EndDisabled();

            ImGui::Separator();
            if (busy) {
                ImGui::BeginDisabled();
                ImGui::Button("Generating...", ImVec2(-FLT_MIN, 36));
                ImGui::EndDisabled();
            } else {
                if (ImGui::Button("Generate", ImVec2(-FLT_MIN, 36))) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                    cfg.negative_prompt = negative_buf;
                    worker = std::thread(run_generation, &gen, &shared, cfg, std::string(prompt_buf), save_to_disk);
                }
            }

            // Progress
            const int step = shared.step.load();
            const int total = shared.total_steps.load();
            const float frac = total > 0 ? static_cast<float>(step) / total : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%d / %d  (%.2f s/it)", step, total, shared.sec_per_step.load());
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), overlay);

            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                ImGui::TextWrapped("%s", shared.status.c_str());
                if (!shared.error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                    ImGui::TextWrapped("%s", shared.error.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // ---------------- right: image + log ----------------
        ImGui::BeginChild("right", ImVec2(0, 0));
        {
            const float log_height = 240.0f;

            ImGui::BeginChild("image", ImVec2(0, -log_height), true, ImGuiWindowFlags_HorizontalScrollbar);
            if (texture.id) {
                // Fit to the pane while preserving aspect ratio.
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float scale = (std::min)(avail.x / texture.width, avail.y / texture.height);
                const ImVec2 size(texture.width * scale, texture.height * scale);
                // Centre it.
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f);
                ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(texture.id)), size);
            } else {
                ImGui::TextDisabled("No image yet.");
            }
            ImGui::EndChild();

            ImGui::BeginChild("log", ImVec2(0, 0), true);
            ImGui::Checkbox("auto-scroll", &auto_scroll);
            ImGui::SameLine();
            if (ImGui::SmallButton("clear")) {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.log_lines.clear();
            }
            ImGui::Separator();
            ImGui::BeginChild("logscroll");
            {
                std::lock_guard<std::mutex> lock(shared.mutex);
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(shared.log_lines.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        ImGui::TextUnformatted(shared.log_lines[i].c_str());
                    }
                }
                if (auto_scroll && shared.log_dirty) {
                    ImGui::SetScrollHereY(1.0f);
                    shared.log_dirty = false;
                }
            }
            ImGui::EndChild();
            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        int fb_w, fb_h;
        glfwGetFramebufferSize(window, &fb_w, &fb_h);
        glViewport(0, 0, fb_w, fb_h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (worker.joinable()) {
        worker.join();
    }
    texture.destroy();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
