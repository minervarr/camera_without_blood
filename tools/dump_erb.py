import torch
import numpy as np
from df.enhance import init_df

def main():
    print("Initializing DeepFilterNet...")
    model, state, _ = init_df()
    
    print("Extracting ERB filterbank weights...")
    # The ERB filterbank is usually inside model.erb.erb_fb
    # In DeepFilterNet3, the erb bank is a tensor used to multiply STFT
    if hasattr(model, 'erb_fb'):
        fb = model.erb_fb.detach().cpu().numpy()
    elif hasattr(model.erb, 'erb_fb'):
        fb = model.erb.erb_fb.detach().cpu().numpy()
    elif hasattr(model.erb, 'fb'):
        fb = model.erb.fb.detach().cpu().numpy()
    else:
        print("Could not find ERB filterbank in model!")
        return

    print(f"ERB Matrix Shape: {fb.shape}")
    
    with open("app/src/main/cpp/audio/erb_weights.hh", "w") as f:
        f.write("#pragma once\n\n")
        f.write("namespace aud {\n\n")
        
        # Flatten and dump
        flat = fb.flatten()
        f.write(f"const float ERB_WEIGHTS[{len(flat)}] = {{\n")
        for i, val in enumerate(flat):
            f.write(f"    {val}f,\n")
        f.write("};\n\n")
        
        f.write(f"const int ERB_SHAPE[] = {{")
        for dim in fb.shape:
            f.write(f"{dim}, ")
        f.write("};\n\n")
        
        f.write("} // namespace aud\n")
    
    print("Successfully generated erb_weights.hh")

if __name__ == "__main__":
    main()
