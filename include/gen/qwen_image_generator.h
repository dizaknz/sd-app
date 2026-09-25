#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stable-diffusion.h"

//
// Configuration with every field mapping to one sd-cli flag
//
struct QwenImageConfig {
    // --- model paths (context) ---
    std::string diffusion_model_path = "models/checkpoints/qwen-image-2512-Q2_K.gguf";
    std::string vae_path = "models/vae/qwen_image_vae.safetensors";
    std::string llm_path = "models/text_encoders/Qwen2.5-VL-7B-Instruct-UD-Q4_K_XL.gguf";

    // optional extras, left empty by default
    std::string taesd_path;
    std::string control_net_path;
    std::string lora_model_dir;

    // --- backend / memory (context) ---
    bool offload_to_cpu = true; // --offload-to-cpu
    std::string max_vram = "6"; // --max-vram 6   (string: also accepts "cuda0=6,vulkan0=2", "-1", "0")
    std::string backend;        // --backend, e.g. "cuda0" or "diffusion=cuda0,vae=cpu". empty = auto
    std::string params_backend; // --params-backend, e.g. "diffusion=disk". offload_to_cpu prepends "*=cpu"
    bool enable_mmap = false;   // --mmap

    bool diffusion_flash_attn = true;   // --diffusion-fa
    bool diffusion_conv_direct = false; // --diffusion-conv-direct
    bool vae_conv_direct = false;       // --vae-conv-direct
    int n_threads = -1;                 // --threads (-1 = physical core count)

    // --- sampling (per generation) ---
    float cfg_scale = 2.5f; // --cfg-scale

    // Sampler and scheduler are held as the CLI spellings and converted with the
    // library's own str_to_sample_method() / str_to_schedule(). The enum
    // identifiers get renamed between releases; these strings do not.
    // "euler", "euler_a", "heun", "dpm2", "dpm++2s_a", "dpm++2m", "dpm++2mv2",
    // "ipndm", "ipndm_v", "lcm", "ddim_trailing", "tcd"
    std::string sample_method = "euler"; // --sampling-method; "" = model default
    // "discrete", "karras", "exponential", "ays", "gits", "sgm_uniform",
    // "simple", "smoothstep", "kl_optimal", "lcm"
    std::string scheduler; // --scheduler;       "" = model default

    int steps = 40;          // --steps
    float flow_shift = 3.0f; // --flow-shift
    float eta = 0.0f;        // --eta (DDIM/TCD only)

    int width = 1024;    // -W
    int height = 1024;   // -H
    int64_t seed = -1;   // --seed (<0 = random)
    int batch_count = 1; // --batch-count
    int clip_skip = -1;  // --clip-skip (<=0 = unspecified)

    std::string negative_prompt; // --negative-prompt

    // --- output / logging ---
    std::string output_dir = "output";       // dirname of --output
    std::string output_stem = "qwen";        // basename; _0, _1... appended per batch item
    sd_log_level_t log_level = SD_LOG_DEBUG; // --log-level verbose
    bool show_progress = true;
};

// Result of one generation: raw RGB(A) pixels plus where it was written.
struct QwenImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channel = 0;
    std::vector<uint8_t> data; // width * height * channel, row-major
    std::string path;          // empty unless save_png() / generate_to_files() wrote it
};

// 
// QwenImageGenerator
//
// Loads the model once in load() and keeps it resident, so repeated
// generate() calls skip the multi-second model load the CLI pays every run.
// Not thread-safe: one sd_ctx_t cannot serve concurrent generations.
//
class QwenImageGenerator {
  public:
    explicit QwenImageGenerator(QwenImageConfig config = {});
    ~QwenImageGenerator();

    QwenImageGenerator(const QwenImageGenerator &) = delete;
    QwenImageGenerator &operator=(const QwenImageGenerator &) = delete;
    QwenImageGenerator(QwenImageGenerator &&) noexcept;
    QwenImageGenerator &operator=(QwenImageGenerator &&) noexcept;

    // Builds the sd_ctx_t. Returns false and sets last_error() on failure.
    // Called automatically by generate() if you haven't called it yourself.
    bool load();
    bool is_loaded() const;
    void unload();

    // Generate from a prompt using the current config.
    std::vector<QwenImage> generate(const std::string &prompt);

    // Same, but with a per-call config override (paths are ignored — the
    // context is already built; only sampling/size/seed fields take effect).
    std::vector<QwenImage> generate(const std::string &prompt, const QwenImageConfig &overrides);

    // Generate and write PNGs into config.output_dir. Returns the file paths.
    std::vector<std::string> generate_to_files(const std::string &prompt);

    // Write a single image out. Returns false on I/O failure.
    static bool save_png(const QwenImage &image, const std::string &path);

    // Live config — mutate between calls to change steps, seed, size, etc.
    QwenImageConfig &config() { return cfg_; }
    const QwenImageConfig &config() const { return cfg_; }

    const std::string &last_error() const { return last_error_; }

    // Sinks for log lines and sampler progress. Both are invoked from whatever
    // thread calls generate(), so a GUI must marshal them onto its own queue
    // rather than touching widgets directly. Set them before load().
    using LogSink = std::function<void(sd_log_level_t level, const char *text)>;
    using ProgressSink = std::function<void(int step, int steps, float seconds_per_step)>;
    void set_log_sink(LogSink sink);
    void set_progress_sink(ProgressSink sink);

    // Maps "verbose"/"info"/"warn"/"error" onto sd_log_level_t.
    // Returns false if the name isn't recognised.
    static bool parse_log_level(const std::string &name, sd_log_level_t &out);

    // Opaque; public only so the C-API callback trampolines can cast to it.
    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
    QwenImageConfig cfg_;
    std::string last_error_;
};
