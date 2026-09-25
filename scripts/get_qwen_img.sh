#!/bin/bash

SCRIPT_DIR=$(dirname "$(realpath "${BASH_SOURCE[0]}")")

outdir=$SCRIPT_DIR/../
[ -d $outdir/models ] || mkdir $outdir/models

for f in checkpoints controlnet embeddings loras photomaker taesd text_encoders unet upscale_models vae; do
    [ -d $outdir/models/$f ] || mkdir $output/models/$f
done

## Diffusion Models
curl -L -C - -o $outdir/models/checkpoints/qwen-image-2512-Q2_K.gguf \
  https://huggingface.co/unsloth/Qwen-Image-2512-GGUF/resolve/main/qwen-image-2512-Q2_K.gguf
curl -L -C - -o $outdir/models/checkpoints/qwen-image-edit-2511-Q2_K.gguf \
  https://huggingface.co/unsloth/Qwen-Image-Edit-2511-GGUF/resolve/main/qwen-image-edit-2511-Q2_K.gguf
 
## Text Encoder + VAE   
curl -L -C - -o $outdir/models/text_encoders/Qwen2.5-VL-7B-Instruct-UD-Q4_K_XL.gguf \
  https://huggingface.co/unsloth/Qwen2.5-VL-7B-Instruct-GGUF/resolve/main/Qwen2.5-VL-7B-Instruct-UD-Q4_K_XL.gguf
curl -L -C - -o $outdir/models/vae/qwen_image_vae.safetensors \
  https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/vae/qwen_image_vae.safetensors

