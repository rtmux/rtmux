Overview
--------
RTMux is a lightweight open source para-virtualization layer, using
resource-multiplexing techniques to provide a highly deterministic real-time
environment for Linux/ARM. Typically, less than 500 lines modication against
Linux kernel are required to allow additional RTOS executives on the same ARM
core concurrently with real-time performance guarantees.

Build Instructions
------------------
* Build RTMux-friendly RTOS
  - [RT-Thread Lite](https://github.com/rtmux/rt-thread-lite) is the only RTOS supporting RTMux.
  - [SCons](http://www.scons.org/) is required to build [RT-Thread Lite](https://github.com/rtmux/rt-thread-lite).
  - Fetch the source
    * `git clone https://github.com/rtmux/rt-thread-lite.git rt-thread && cd rt-thread`
  - Configure and Build for the machines. Take Realview for ARM Cortex-A8 for example:
    * `cd bsp/realview-a8 && scons`
  - Copy the generated file `rtthread.bin` to the path `/tmp/rtos.bin`
* Modifying Linux kernel is vital, and get the kernel source in advance. The version that RTMux supports is v3.8.13
  - Apply the patches under directory patches/ against Linux source
  - `patch -p1 < patches/0001-RTMux-implement-dual-kernel-approach-in-minimal-chan.patch`
  - `patch -p1 < patches/0002-RTMux-support-Realview-Platform-Baseboard-for-Cortex.patch`
  - `patch -p1 < patches/0003-RTMux-support-AM33x-based-platforms.patch`
  - `patch -p1 < patches/0004-arm-gic-correct-the-cpu-map-on-gic_raise_softirq-for.patch`
* Enable `ARM_VMM` in kernel configurations.
  - `make menuconfig ARCH=arm`
  - At present, both Realview for ARM Cortex-A8 and Beaglebone Black (based on TI AM33x) are supported.
* Build the kernel image
  - `make ARCH=arm CROSS_COMPILE=arm-none-eabi- zImage`
* Build the virtual machime monitor module for Linux
  - `make -C <kernel-source-path> M=$PWD ARCH=arm CROSS_COMPILE=arm-none-eabi-`
* Repack the root filesystem and ensure the following changes:
  - Copy the generated rtvmm.ko
  - Put the RTMux-friendly RTOS image (that is, `/tmp/rtos.bin`) under the path `/vmm/rtos.bin`.

Usage
-----
* Insert vmm kernel module when you are about to launch the RTOS
  - `insmod rtvmm.ko`
* Linux domain would continue executing, and the real-time domain starts as expected.
