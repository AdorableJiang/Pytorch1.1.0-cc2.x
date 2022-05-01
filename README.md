# Pytorch v1.1.0

This repo makes it possible to use cuda-capable pytorch v1.1.0 on old `Fermi` architecture GPU with cuda compute capability (cc) `2.x`.

## Motivation

  1. Have fun.
  2. Learn more about the source code of `Pytorch` and tools e.g. `ninja`, `gcc`, etc.

![PyTorch Logo](https://github.com/pytorch/pytorch/raw/master/docs/source/_static/img/pytorch-logo-dark.png)

--------------------------------------------------------------------------------

PyTorch is a Python package that provides two high-level features:
- Tensor computation (like NumPy) with strong GPU acceleration
- Deep neural networks built on a tape-based autograd system

You can reuse your favorite Python packages such as NumPy, SciPy and Cython to extend PyTorch when needed.

- [Installation](#installation)
  - [From Source](#from-source)
  - [Building the Documentation](#building-the-documentation)
  - [Previous Versions](#previous-versions)
- [Getting Started](#getting-started)

| System | 2.7 | 3.5 | 3.6 |
| :---: | :---: | :---: | :--: |
| Linux GPU | [![Build Status](https://ci.pytorch.org/jenkins/job/pytorch-master/badge/icon)](https://ci.pytorch.org/jenkins/job/pytorch-master/) | [![Build Status](https://ci.pytorch.org/jenkins/job/pytorch-master/badge/icon)](https://ci.pytorch.org/jenkins/job/pytorch-master/) | <center>—</center> |

See also the [ci.pytorch.org HUD](https://ezyang.github.io/pytorch-ci-hud/build/pytorch-master).


## Installation

### Anaconda

If you are installing from source, we highly recommend installing an [Anaconda](https://www.anaconda.com/distribution/#download-section) environment.
You will get a high-quality BLAS library (MKL) and you get a controlled compiler version regardless of your Linux distro.

Once you have [Anaconda](https://www.anaconda.com/distribution/#download-section) installed, here are the instructions.

```bash
conda create --name torch110 python=3.6
conda activate torch110
```

### gcc

As state in the [next section](#cuda), `cuda 8.0` which requires `gcc<=5` is necessary. At the time of writing, the default `apt` installed gcc-5 by `Ubuntu 20.04 LTS` is `5.5.0`. But this version of gcc triggers error when compiling `AVX512` related libs, as discussed [here](https://stackoverflow.com/questions/50815093/avx512-intrinsics-header-produces-many-errors-after-distro-upgrades-gcc-to-5-5-0). Solution could be installing `gcc 5.4.0` from source, which is also not a pleasant experience because of compiling errors when using [original source code](https://github.com/gcc-mirror/gcc/tree/releases/gcc-5.4.0) provided by GNU. Instead, a bug fixed version of `gcc 5.4.0` is provided with installing instructions [AdorableJiang/gcc-5.4.0](https://github.com/AdorableJiang/gcc-5.4.0).

### CUDA

If you want to compile with CUDA support, install

- [NVIDIA CUDA](https://developer.nvidia.com/cuda-downloads) 7.5 or above

As announced in release note of [cuda 8.0](https://docs.nvidia.com/cuda/archive/8.0/cuda-toolkit-release-notes/index.html#deprecated-features) and [cuda 9.0](https://docs.nvidia.com/cuda/archive/9.0/cuda-toolkit-release-notes/index.html#unsupported-features), `cuda 8.0` is the latest version usable for Fermi GPUs, although you would find that your driver version meets the requirement of `cuda 9.0`.

> Fermi architecture support is being deprecated in the CUDA 8.0 Toolkit, which will be the last toolkit release to support it. Note that support for Fermi is being deprecated in the CUDA Toolkit but not in the driver. Applications compiled with CUDA 8.0 or older will continue to work on Fermi with newer NVIDIA drivers.

Use `-override` to suppress cuda requirements check if necessary. Make sure verifying cuda installation by compiling and running [cuda samples](https://docs.nvidia.com/cuda/archive/8.0/cuda-installation-guide-linux/index.html#verify-installation).

### cuDNN

cuDNN requires compute capability >= 3.0. Install it if possible.

- [NVIDIA cuDNN](https://developer.nvidia.com/cudnn) v6.x or above

If you want to disable CUDA support, export environment variable `NO_CUDA=1`.
Other potentially useful environment variables may be found in `setup.py`.

If you are building for NVIDIA's Jetson platforms (Jetson Nano, TX1, TX2, AGX Xavier), Instructions to [are available here](https://devtalk.nvidia.com/default/topic/1049071/jetson-nano/pytorch-for-jetson-nano/)


### Install Dependencies

Common
```
conda install numpy pyyaml mkl mkl-include setuptools cmake cffi typing ninja
```

On Linux
```bash
# Add LAPACK support for the GPU if needed
conda install -c pytorch magma-cuda80
```

### Get the PyTorch Source
```bash
git clone --recursive https://github.com/pytorch/pytorch
cd pytorch
```

### Install PyTorch
On Linux
```bash
./build.sh
```

In `build.sh`, make sure `CC`, `CXX` and `CUDAHOSTCXX` points to the right `gcc&g++ 5.4.0` compiler

## Building the Documentation

To build documentation in various formats, you will need [Sphinx](http://www.sphinx-doc.org) and the
readthedocs theme.

```
cd docs/
pip install -r requirements.txt
```
You can then build the documentation by running ``make <format>`` from the
``docs/`` folder. Run ``make`` to get a list of all available output formats.

## Previous Versions

Installation instructions and binaries for previous PyTorch versions may be found
on [our website](https://pytorch.org/previous-versions).


## Getting Started

Three pointers to get you started:
- [Tutorials: get you started with understanding and using PyTorch](https://pytorch.org/tutorials/)
- [Examples: easy to understand pytorch code across all domains](https://github.com/pytorch/examples)
- [The API Reference](https://pytorch.org/docs/)

## License

PyTorch is BSD-style licensed, as found in the LICENSE file.
