"""
Resize GigaLearn checkpoint from 256-hidden to 512-hidden.

Loads the existing SHARED_HEAD.lt, POLICY.lt, CRITIC.lt,
creates new 512-wide models, copies old weights into the top-left
corner of each tensor, initializes the rest with Kaiming (weights)
or zeros (biases), and saves back as TorchScript.

Optimizer states are deleted (ADAM restarts fresh).

Usage:
    python resize_checkpoint.py <checkpoint_dir>

Example:
    python resize_checkpoint.py ../checkpoints/3368052736
"""

import sys
import os
import shutil
import torch
import torch.nn as nn
import math


OLD_HIDDEN = 256
NEW_HIDDEN = 512
OBS_SIZE = 109
NUM_ACTIONS = 90


def build_sequential(num_inputs, layer_sizes, add_output_layer, num_outputs):
    """Build a Sequential matching GigaLearn's Model constructor."""
    layers = []
    last_size = num_inputs
    for size in layer_sizes:
        layers.append(nn.Linear(last_size, size))
        layers.append(nn.LayerNorm(size))
        layers.append(nn.ReLU())
        last_size = size
    if add_output_layer:
        layers.append(nn.Linear(last_size, num_outputs))
    return nn.Sequential(*layers)


def resize_linear_weight(old_w, new_out, new_in):
    """Resize a Linear weight tensor, preserving old values in top-left corner."""
    new_w = torch.zeros(new_out, new_in)
    # Kaiming uniform init for the new weights
    fan_in = new_in
    bound = math.sqrt(6.0 / fan_in)  # Kaiming uniform for ReLU
    nn.init.uniform_(new_w, -bound, bound)
    # Copy old weights into top-left corner
    old_out, old_in = old_w.shape
    new_w[:old_out, :old_in] = old_w
    return new_w


def resize_linear_bias(old_b, new_size):
    """Resize a Linear bias tensor, zeros for new entries."""
    new_b = torch.zeros(new_size)
    old_size = old_b.shape[0]
    new_b[:old_size] = old_b
    return new_b


def resize_layernorm_weight(old_w, new_size):
    """Resize LayerNorm weight (gamma). New entries initialized to 1.0."""
    new_w = torch.ones(new_size)
    old_size = old_w.shape[0]
    new_w[:old_size] = old_w
    return new_w


def resize_layernorm_bias(old_b, new_size):
    """Resize LayerNorm bias (beta). New entries initialized to 0.0."""
    new_b = torch.zeros(new_size)
    old_size = old_b.shape[0]
    new_b[:old_size] = old_b
    return new_b


def resize_output_linear_weight(old_w, new_in):
    """Resize output layer weight: output dim stays same, input dim changes."""
    old_out, old_in = old_w.shape
    new_w = torch.zeros(old_out, new_in)
    fan_in = new_in
    bound = math.sqrt(6.0 / fan_in)
    nn.init.uniform_(new_w, -bound, bound)
    new_w[:, :old_in] = old_w
    return new_w


def resize_shared_head(old_sd):
    """
    Shared head: 109 -> [256,256] becomes 109 -> [512,512]
    Keys: 0(Linear) 1(LN) 2(ReLU) 3(Linear) 4(LN) 5(ReLU)
    """
    new_model = build_sequential(OBS_SIZE, [NEW_HIDDEN, NEW_HIDDEN],
                                  add_output_layer=False, num_outputs=0)
    new_sd = new_model.state_dict()

    # Layer 0: Linear(109, 256) -> Linear(109, 512)
    new_sd['0.weight'] = resize_linear_weight(old_sd['0.weight'], NEW_HIDDEN, OBS_SIZE)
    new_sd['0.bias'] = resize_linear_bias(old_sd['0.bias'], NEW_HIDDEN)

    # Layer 1: LayerNorm(256) -> LayerNorm(512)
    new_sd['1.weight'] = resize_layernorm_weight(old_sd['1.weight'], NEW_HIDDEN)
    new_sd['1.bias'] = resize_layernorm_bias(old_sd['1.bias'], NEW_HIDDEN)

    # Layer 3: Linear(256, 256) -> Linear(512, 512)
    new_sd['3.weight'] = resize_linear_weight(old_sd['3.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['3.bias'] = resize_linear_bias(old_sd['3.bias'], NEW_HIDDEN)

    # Layer 4: LayerNorm(256) -> LayerNorm(512)
    new_sd['4.weight'] = resize_layernorm_weight(old_sd['4.weight'], NEW_HIDDEN)
    new_sd['4.bias'] = resize_layernorm_bias(old_sd['4.bias'], NEW_HIDDEN)

    new_model.load_state_dict(new_sd)
    return new_model


def resize_policy(old_sd):
    """
    Policy: 256 -> [256,256,256] -> 90 becomes 512 -> [512,512,512] -> 90
    Keys: 0(Linear) 1(LN) 2(ReLU) 3(Linear) 4(LN) 5(ReLU) 6(Linear) 7(LN) 8(ReLU) 9(Linear output)
    """
    new_model = build_sequential(NEW_HIDDEN, [NEW_HIDDEN, NEW_HIDDEN, NEW_HIDDEN],
                                  add_output_layer=True, num_outputs=NUM_ACTIONS)
    new_sd = new_model.state_dict()

    # Layer 0: Linear(256, 256) -> Linear(512, 512)
    new_sd['0.weight'] = resize_linear_weight(old_sd['0.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['0.bias'] = resize_linear_bias(old_sd['0.bias'], NEW_HIDDEN)

    # Layer 1: LayerNorm
    new_sd['1.weight'] = resize_layernorm_weight(old_sd['1.weight'], NEW_HIDDEN)
    new_sd['1.bias'] = resize_layernorm_bias(old_sd['1.bias'], NEW_HIDDEN)

    # Layer 3: Linear(256, 256) -> Linear(512, 512)
    new_sd['3.weight'] = resize_linear_weight(old_sd['3.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['3.bias'] = resize_linear_bias(old_sd['3.bias'], NEW_HIDDEN)

    # Layer 4: LayerNorm
    new_sd['4.weight'] = resize_layernorm_weight(old_sd['4.weight'], NEW_HIDDEN)
    new_sd['4.bias'] = resize_layernorm_bias(old_sd['4.bias'], NEW_HIDDEN)

    # Layer 6: Linear(256, 256) -> Linear(512, 512)
    new_sd['6.weight'] = resize_linear_weight(old_sd['6.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['6.bias'] = resize_linear_bias(old_sd['6.bias'], NEW_HIDDEN)

    # Layer 7: LayerNorm
    new_sd['7.weight'] = resize_layernorm_weight(old_sd['7.weight'], NEW_HIDDEN)
    new_sd['7.bias'] = resize_layernorm_bias(old_sd['7.bias'], NEW_HIDDEN)

    # Layer 9: Linear(256, 90) -> Linear(512, 90) — output dim stays 90
    new_sd['9.weight'] = resize_output_linear_weight(old_sd['9.weight'], NEW_HIDDEN)
    new_sd['9.bias'] = old_sd['9.bias'].clone()  # Output bias unchanged

    new_model.load_state_dict(new_sd)
    return new_model


def resize_critic(old_sd):
    """
    Critic: 256 -> [256,256,256] -> 1 becomes 512 -> [512,512,512] -> 1
    Same structure as policy but output is 1.
    """
    new_model = build_sequential(NEW_HIDDEN, [NEW_HIDDEN, NEW_HIDDEN, NEW_HIDDEN],
                                  add_output_layer=True, num_outputs=1)
    new_sd = new_model.state_dict()

    # Layer 0
    new_sd['0.weight'] = resize_linear_weight(old_sd['0.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['0.bias'] = resize_linear_bias(old_sd['0.bias'], NEW_HIDDEN)
    new_sd['1.weight'] = resize_layernorm_weight(old_sd['1.weight'], NEW_HIDDEN)
    new_sd['1.bias'] = resize_layernorm_bias(old_sd['1.bias'], NEW_HIDDEN)

    # Layer 3
    new_sd['3.weight'] = resize_linear_weight(old_sd['3.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['3.bias'] = resize_linear_bias(old_sd['3.bias'], NEW_HIDDEN)
    new_sd['4.weight'] = resize_layernorm_weight(old_sd['4.weight'], NEW_HIDDEN)
    new_sd['4.bias'] = resize_layernorm_bias(old_sd['4.bias'], NEW_HIDDEN)

    # Layer 6
    new_sd['6.weight'] = resize_linear_weight(old_sd['6.weight'], NEW_HIDDEN, NEW_HIDDEN)
    new_sd['6.bias'] = resize_linear_bias(old_sd['6.bias'], NEW_HIDDEN)
    new_sd['7.weight'] = resize_layernorm_weight(old_sd['7.weight'], NEW_HIDDEN)
    new_sd['7.bias'] = resize_layernorm_bias(old_sd['7.bias'], NEW_HIDDEN)

    # Layer 9: Linear(256, 1) -> Linear(512, 1)
    new_sd['9.weight'] = resize_output_linear_weight(old_sd['9.weight'], NEW_HIDDEN)
    new_sd['9.bias'] = old_sd['9.bias'].clone()

    new_model.load_state_dict(new_sd)
    return new_model


def main():
    if len(sys.argv) != 2:
        print(f"Usage: python {sys.argv[0]} <checkpoint_dir>")
        sys.exit(1)

    checkpoint_dir = sys.argv[1]

    if not os.path.isdir(checkpoint_dir):
        print(f"Error: {checkpoint_dir} is not a directory")
        sys.exit(1)

    # Verify files exist
    for name in ['SHARED_HEAD.lt', 'POLICY.lt', 'CRITIC.lt']:
        if not os.path.exists(os.path.join(checkpoint_dir, name)):
            print(f"Error: {name} not found in {checkpoint_dir}")
            sys.exit(1)

    print(f"Loading checkpoint from {checkpoint_dir}...")

    # Load old models
    shared_old = torch.jit.load(os.path.join(checkpoint_dir, 'SHARED_HEAD.lt'), map_location='cpu')
    policy_old = torch.jit.load(os.path.join(checkpoint_dir, 'POLICY.lt'), map_location='cpu')
    critic_old = torch.jit.load(os.path.join(checkpoint_dir, 'CRITIC.lt'), map_location='cpu')

    print("Old model shapes:")
    for name, model in [('SHARED_HEAD', shared_old), ('POLICY', policy_old), ('CRITIC', critic_old)]:
        for k, v in model.state_dict().items():
            print(f"  {name}/{k}: {v.shape}")

    # Resize
    print(f"\nResizing {OLD_HIDDEN} -> {NEW_HIDDEN}...")
    new_shared = resize_shared_head(shared_old.state_dict())
    new_policy = resize_policy(policy_old.state_dict())
    new_critic = resize_critic(critic_old.state_dict())

    print("\nNew model shapes:")
    for name, model in [('SHARED_HEAD', new_shared), ('POLICY', new_policy), ('CRITIC', new_critic)]:
        for k, v in model.state_dict().items():
            print(f"  {name}/{k}: {v.shape}")

    # Save as TorchScript — GigaLearn's C++ torch::load expects TorchScript format
    # (archive/code/, archive/constants.pkl, etc.)
    print(f"\nSaving resized checkpoint to {checkpoint_dir}...")

    for name, model in [('SHARED_HEAD', new_shared), ('POLICY', new_policy), ('CRITIC', new_critic)]:
        model.eval()
        scripted = torch.jit.script(model)
        save_path = os.path.join(checkpoint_dir, f'{name}.lt')
        torch.jit.save(scripted, save_path)
        print(f"  Saved {name}.lt")

    # Delete optimizer states (ADAM must restart)
    for optim_name in ['SHARED_HEAD_OPTIM.lt', 'POLICY_OPTIM.lt', 'CRITIC_OPTIM.lt']:
        optim_path = os.path.join(checkpoint_dir, optim_name)
        if os.path.exists(optim_path):
            os.remove(optim_path)
            print(f"  Deleted {optim_name} (optimizer will restart)")

    # RUNNING_STATS.json stays as-is
    print("\n  RUNNING_STATS.json unchanged")

    print("\nDone! Checkpoint resized from 256 to 512.")
    print("Remember to update ExampleMain.cpp and Inference.h to use 512.")
    print("The first few hundred iterations may be rough while the optimizer rebuilds.")


if __name__ == '__main__':
    main()
