#include "gen/qwen_image_generator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

scheduler_t str_to_scheduler(const std::string &str) {
    static const std::map<std::string, scheduler_t> str_scheduler_lookup = {
        {"discrete", DISCRETE_SCHEDULER},
        {"karras", KARRAS_SCHEDULER},
        {"exponential", EXPONENTIAL_SCHEDULER},
        {"ays", AYS_SCHEDULER},
        {"gits", GITS_SCHEDULER},
        {"sgm_uniform", SGM_UNIFORM_SCHEDULER},
        {"simple", SIMPLE_SCHEDULER},
        {"smoothstep", SMOOTHSTEP_SCHEDULER},
        {"kl_optimal", KL_OPTIMAL_SCHEDULER},
        {"lcm", LCM_SCHEDULER}
    };
    auto it = str_scheduler_lookup.find(str);
    if (it != str_scheduler_lookup.end()) {
        return it->second;
    }
    // invalid
    return SCHEDULER_COUNT;
}

//
// Impl: owns the sd_ctx_t and the std::strings whose c_str() the C API borrows.
// sd_ctx_params_t stores raw const char*, so these must outlive new_sd_ctx().
//
struct QwenImageGenerator::Impl {
    sd_ctx_t *ctx = nullptr;

    // backing storage for pointers handed to the C API
    std::string diffusion_model_path;
    std::string vae_path;
    std::string llm_path;
    std::string taesd_path;
    std::string control_net_path;
    std::string lora_model_dir;
    std::string max_vram;
    std::string backend;
    std::string params_backend;

    sd_log_level_t log_level = SD_LOG_DEBUG;
    bool show_progress = true;

    QwenImageGenerator::LogSink log_sink;
    QwenImageGenerator::ProgressSink progress_sink;

    ~Impl() {
        if (ctx) {
            free_sd_ctx(ctx);
            ctx = nullptr;
        }
    }
};

//
// Logging / progress plumbing
//
namespace {

const char *level_name(sd_log_level_t level) {
    switch (level) {
    case SD_LOG_DEBUG:
        return "DEBUG";
    case SD_LOG_INFO:
        return "INFO ";
    case SD_LOG_WARN:
        return "WARN ";
    case SD_LOG_ERROR:
        return "ERROR";
    default:
        return "N/A";
    }
}

void log_trampoline(enum sd_log_level_t level, const char *text, void *data) {
    auto *impl = static_cast<QwenImageGenerator::Impl *>(data);
    if (!impl || !text) {
        return;
    }
    if (level < impl->log_level) {
        return;
    }
    if (impl->log_sink) {
        impl->log_sink(level, text);
        return;
    }
    std::fprintf(level >= SD_LOG_WARN ? stderr : stdout, "[%s] %s", level_name(level), text);
    std::fflush(level >= SD_LOG_WARN ? stderr : stdout);
}

void progress_trampoline(int step, int steps, float time, void *data) {
    auto *impl = static_cast<QwenImageGenerator::Impl *>(data);
    if (!impl || !impl->show_progress || steps <= 0) {
        return;
    }
    if (impl->progress_sink) {
        impl->progress_sink(step, steps, time);
        return;
    }
    std::fprintf(stderr, "\rstep %d/%d  %.2fs/it", step, steps, time);
    if (step == steps) {
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Mirrors sd-cli's --offload-to-cpu: prepend "*=cpu" to the params-backend
// spec so explicit per-module assignments can still override it.
std::string build_params_backend(bool offload_to_cpu, const std::string &user_spec) {
    if (!offload_to_cpu) {
        return user_spec;
    }
    if (user_spec.empty()) {
        return "*=cpu";
    }
    return "*=cpu," + user_spec;
}

const char *or_null(const std::string &s) { return s.empty() ? nullptr : s.c_str(); }

} // namespace

//
// Construction / lifetime
//
QwenImageGenerator::QwenImageGenerator(QwenImageConfig config)
    : impl_(std::make_unique<Impl>()), cfg_(std::move(config)) {}

QwenImageGenerator::~QwenImageGenerator() = default;
QwenImageGenerator::QwenImageGenerator(QwenImageGenerator &&) noexcept = default;
QwenImageGenerator &QwenImageGenerator::operator=(QwenImageGenerator &&) noexcept = default;

void QwenImageGenerator::set_log_sink(LogSink sink) {
    impl_->log_sink = std::move(sink);
}

void QwenImageGenerator::set_progress_sink(ProgressSink sink) {
    impl_->progress_sink = std::move(sink);
}

bool QwenImageGenerator::is_loaded() const {
    return impl_ && impl_->ctx != nullptr;
}

void QwenImageGenerator::unload() {
    if (impl_ && impl_->ctx) {
        free_sd_ctx(impl_->ctx);
        impl_->ctx = nullptr;
    }
}

bool QwenImageGenerator::load() {
    if (is_loaded()) {
        return true;
    }
    last_error_.clear();

    // Stash strings first — sd_ctx_params_t only holds borrowed pointers.
    impl_->diffusion_model_path = cfg_.diffusion_model_path;
    impl_->vae_path = cfg_.vae_path;
    impl_->llm_path = cfg_.llm_path;
    impl_->taesd_path = cfg_.taesd_path;
    impl_->control_net_path = cfg_.control_net_path;
    impl_->lora_model_dir = cfg_.lora_model_dir;
    impl_->max_vram = cfg_.max_vram;
    impl_->backend = cfg_.backend;
    impl_->params_backend = build_params_backend(cfg_.offload_to_cpu, cfg_.params_backend);
    impl_->log_level = cfg_.log_level;
    impl_->show_progress = cfg_.show_progress;

    sd_set_log_callback(log_trampoline, impl_.get());
    sd_set_progress_callback(progress_trampoline, impl_.get());

    // Always init first: the library fills in defaults for every field,
    // including ones added in later releases. Then override only what we want.
    sd_ctx_params_t params;
    sd_ctx_params_init(&params);

    params.diffusion_model_path = or_null(impl_->diffusion_model_path);
    params.vae_path = or_null(impl_->vae_path);
    params.llm_path = or_null(impl_->llm_path);
    params.taesd_path = or_null(impl_->taesd_path);
    params.control_net_path = or_null(impl_->control_net_path);

    params.max_vram = or_null(impl_->max_vram);
    params.backend = or_null(impl_->backend);
    params.params_backend = or_null(impl_->params_backend);
    params.enable_mmap = cfg_.enable_mmap;

    params.diffusion_flash_attn = cfg_.diffusion_flash_attn;
    params.diffusion_conv_direct = cfg_.diffusion_conv_direct;
    params.vae_conv_direct = cfg_.vae_conv_direct;
    params.n_threads = cfg_.n_threads;

    impl_->ctx = new_sd_ctx(&params);
    if (!impl_->ctx) {
        last_error_ = "new_sd_ctx failed — check model paths and available memory";
        return false;
    }
    return true;
}

//
// Generation
//
std::vector<QwenImage> QwenImageGenerator::generate(const std::string &prompt) {
    return generate(prompt, cfg_);
}

std::vector<QwenImage> QwenImageGenerator::generate(const std::string &prompt, const QwenImageConfig &cfg) {
    std::vector<QwenImage> out;
    last_error_.clear();

    if (!load()) {
        return out;
    }
    if (prompt.empty()) {
        last_error_ = "empty prompt";
        return out;
    }

    // Same pattern as the context: init to defaults, then override.
    sd_img_gen_params_t gen;
    sd_img_gen_params_init(&gen);

    gen.prompt = prompt.c_str();
    gen.negative_prompt = or_null(cfg.negative_prompt);
    gen.width = cfg.width;
    gen.height = cfg.height;
    gen.seed = cfg.seed;
    gen.batch_count = cfg.batch_count;
    gen.clip_skip = cfg.clip_skip;

    // sd_img_gen_params_init() already set these to the "let the model decide"
    // sentinel, so only override when the caller named one.
    if (!cfg.sample_method.empty()) {
        gen.sample_params.sample_method = str_to_sample_method(cfg.sample_method.c_str());
    }
    if (!cfg.scheduler.empty()) {
        gen.sample_params.scheduler = str_to_scheduler(cfg.scheduler);
    }

    gen.sample_params.sample_steps = cfg.steps;
    gen.sample_params.eta = cfg.eta;
    gen.sample_params.flow_shift = cfg.flow_shift;
    gen.sample_params.guidance.txt_cfg = cfg.cfg_scale;

    int num_images = 0;
    sd_image_t *results = nullptr;
    if (!generate_image(impl_->ctx, &gen, &results, &num_images)) {
        last_error_ = "generate_image returned null";
        return out;
    }
    num_images = cfg.batch_count > 0 ? cfg.batch_count : 1;

    out.reserve(num_images);
    for (int i = 0; i < num_images; ++i) {
        const sd_image_t &src = results[i];
        if (!src.data) {
            continue;
        }
        QwenImage img;
        img.width = src.width;
        img.height = src.height;
        img.channel = src.channel;
        const size_t n = static_cast<size_t>(src.width) * src.height * src.channel;
        img.data.assign(src.data, src.data + n);
        out.push_back(std::move(img));
    }

    free_sd_images(results, num_images);
    return out;
}

std::vector<std::string> QwenImageGenerator::generate_to_files(const std::string &prompt) {
    std::vector<std::string> paths;
    auto images = generate(prompt);
    if (images.empty()) {
        return paths;
    }

    std::error_code ec;
    if (!cfg_.output_dir.empty()) {
        fs::create_directories(cfg_.output_dir, ec);
    }

    for (size_t i = 0; i < images.size(); ++i) {
        fs::path p = cfg_.output_dir.empty() ? fs::path(".") : fs::path(cfg_.output_dir);
        std::string name = cfg_.output_stem;
        if (images.size() > 1) {
            name += "_" + std::to_string(i);
        }
        p /= (name + ".png");

        if (save_png(images[i], p.string())) {
            images[i].path = p.string();
            paths.push_back(p.string());
        } else {
            last_error_ = "failed to write " + p.string();
        }
    }
    return paths;
}

bool QwenImageGenerator::save_png(const QwenImage &image, const std::string &path) {
    if (image.data.empty() || image.width == 0 || image.height == 0) {
        return false;
    }
    const int stride = static_cast<int>(image.width) * static_cast<int>(image.channel);
    bool status = stbi_write_png(
        path.c_str(), static_cast<int>(image.width), static_cast<int>(image.height), static_cast<int>(image.channel),
        image.data.data(), stride
    );
    return status != 0;
}

//
// CLI-name parsers
//
bool QwenImageGenerator::parse_log_level(const std::string &name, sd_log_level_t &out) {
    const std::string n = to_lower(name);
    if (n == "verbose" || n == "debug") {
        out = SD_LOG_DEBUG;
        return true;
    }
    if (n == "info") {
        out = SD_LOG_INFO;
        return true;
    }
    if (n == "warn") {
        out = SD_LOG_WARN;
        return true;
    }
    if (n == "error") {
        out = SD_LOG_ERROR;
        return true;
    }
    return false;
}
