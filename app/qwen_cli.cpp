#include <cstdio>
#include <string>

#include "gen/qwen_image_generator.h"

int main(int argc, char **argv) {
    QwenImageConfig cfg;
    // Defaults already match your CLI invocation. Override anything you like:
    cfg.diffusion_model_path = "models/checkpoints/qwen-image-2512-Q2_K.gguf";
    cfg.vae_path = "models/vae/qwen_image_vae.safetensors";
    cfg.llm_path = "models/text_encoders/Qwen2.5-VL-7B-Instruct-UD-Q4_K_XL.gguf";
    cfg.cfg_scale = 2.5f;
    cfg.sample_method = "euler";
    cfg.steps = 40;
    cfg.width = 1024;
    cfg.height = 1024;
    cfg.flow_shift = 3.0f;
    cfg.diffusion_flash_attn = true;
    cfg.offload_to_cpu = true;
    cfg.max_vram = "6";
    cfg.log_level = SD_LOG_DEBUG; // --log-level verbose
    cfg.output_dir = "output";

    QwenImageGenerator gen(cfg);

    if (!gen.load()) {
        std::fprintf(stderr, "load failed: %s\n", gen.last_error().c_str());
        return 1;
    }

    // Join argv into one prompt, matching "$@" in your shell wrapper.
    std::string prompt;
    for (int i = 1; i < argc; ++i) {
        if (!prompt.empty())
            prompt += " ";
        prompt += argv[i];
    }
    if (prompt.empty()) {
        prompt = "a photo of a tui perched on a flax flower, dawn light";
    }

    gen.config().output_stem = "img_001";
    for (const auto &path : gen.generate_to_files(prompt)) {
        std::printf("wrote %s\n", path.c_str());
    }

    // The model stays resident — a second generation skips the load entirely.
    gen.config().seed = 42;
    gen.config().steps = 20;
    gen.config().output_stem = "img_002";
    for (const auto &path : gen.generate_to_files("the same bird, but at night")) {
        std::printf("wrote %s\n", path.c_str());
    }

    return 0;
}
