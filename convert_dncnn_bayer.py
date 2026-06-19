import torch
import struct
import numpy as np
import os

state = torch.load('dncnn.pth', map_location='cpu')

layers = []
blobs = []

# Input layer
layers.append("Input in0 0 1 in0")

blob_in = "in0"
blob_idx = 0

bin_file = open('app/src/main/assets/dncnn.bin', 'wb')

for i in range(17):
    # Convolution
    w_key = f"model.{i*2}.weight"
    b_key = f"model.{i*2}.bias"
    
    w = state[w_key].numpy()
    b = state[b_key].numpy()
    
    out_ch, in_ch, k_h, k_w = w.shape
    weight_size = out_ch * in_ch * k_h * k_w
    
    blob_out = f"out{blob_idx}"
    blob_idx += 1
    
    # EXACT OFFICIAL WEIGHTS, but mapped mathematically to Bayer by using Dilation=2 and Padding=2!
    layers.append(f"Convolution conv{i} 1 1 {blob_in} {blob_out} 0={out_ch} 1={k_h} 2=2 3=1 4=2 5=1 6={weight_size}")
    
    # Write bias then weights
    bin_file.write(b.tobytes())
    bin_file.write(w.tobytes())
    
    blob_in = blob_out
    
    # ReLU except for last layer
    if i < 16:
        blob_out = f"out{blob_idx}"
        blob_idx += 1
        layers.append(f"ReLU relu{i} 1 1 {blob_in} {blob_out}")
        blob_in = blob_out

# Finally, BinaryOp SUB: in0 - noise_prediction
layers.append(f"BinaryOp sub0 2 1 in0 {blob_in} out_final 0=1")

bin_file.close()

param_str = "7767517\n"
param_str += f"{len(layers)} {blob_idx + 2}\n"
param_str += "\n".join(layers)

with open('app/src/main/assets/dncnn.param', 'w') as f:
    f.write(param_str)

print("Conversion complete: Official DnCNN mapped to Bayer Dilation!")
