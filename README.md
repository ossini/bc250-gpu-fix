# bc250-gpu-fix

Fixes the broken GPU utilization readout (permanently stuck at 655%) on AMD APUs where the `amdgpu` driver doesn't populate the `gpu_metrics` utilization field. Confirmed on the ASRock BC-250 (PS5 Oberon APU) running Bazzite, should work on other affected AMD APUs too.

## What's the problem?

The kernel exposes GPU telemetry at `/sys/class/drm/cardX/device/gpu_metrics` as a binary struct. MangoHud and similar tools read this directly. On certain APUs, the utilization field at offset `0x1C` is permanently `0xFFFF` (65535) AMD's "not available" sentinel. MangoHud doesn't check for that and just displays it as 655%.

You can verify if you're affected:

```bash
xxd -l 32 /sys/class/drm/card0/device/gpu_metrics | tail -1
xxd -l 32 /sys/class/drm/card1/device/gpu_metrics | tail -1
# ffff at bytes 1c-1d = broken
```

## How does the fix work?

The daemon opens the real `gpu_metrics` file, then bind-mounts a patched copy over it. Every second it:

1. Reads the real 128-byte metrics table through the saved file descriptor
2. Computes actual GPU utilization from DRM engine times (`/proc/*/fdinfo/*`)
3. Overwrites the broken 2-byte field with the computed value
4. Writes the patched table out

Everything else (temps, clocks, VRAM, power) passes through untouched. On shutdown the bind mount is removed and everything goes back to normal. A reboot also clears it.

The utilization is calculated from `drm-engine-gfx` nanoseconds across all processes this is engine busy time, not shader occupancy. Good enough to tell if your GPU is bottlenecked or idle, not meant to replace a profiler. Expect around 5-15% deviation from what a hardware counter would show under load.

## Install

```bash
git clone https://github.com/ossini/bc250-gpu-fix.git && cd bc250-gpu-fix
chmod +x install.sh
sudo ./install.sh
```

The install script checks for `gcc`/`make`, installs them if needed, builds the binary, and starts the service. To remove everything:

```bash
sudo ./install.sh --uninstall
```

### Manual install

If you'd rather do it yourself:

```bash
make
sudo make install
sudo make enable
```

## Usage

```
gpu-metrics-fix [OPTIONS]

  -c, --card NAME    DRM card to patch (default: card1)
  -d, --dry-run      Print usage % to stdout, don't patch anything
  -h, --help
  -v, --version
```

Test before installing:

```bash
sudo ./gpu-metrics-fix --dry-run
```

If your GPU shows up as `card0` instead of `card1`:

```bash
sudo systemctl edit gpu-metrics-fix.service
```
```ini
[Service]
ExecStart=
ExecStart=/usr/local/bin/gpu-metrics-fix --card card0
```

## Why C?

I found a python file that did the same. It worked but burned 20-50ms per scan cycle plus 15-25MB RSS just for the interpreter, pointless overhead for something that reads procfs in a loop. C does the same scan in 1-5ms and uses under 1MB total.

Rust and Go were considered but don't add anything here. There's no dynamic allocation, no concurrency, no user input to sanitize. The whole program is ~200 lines of sequential reads and writes against virtual filesystems. A garbage-collected runtime (Go) or a heavy toolchain for borrow checking (Rust) would be solving problems that don't exist in this codebase. C with libc is the right tool.

## Performance impact

Negligible. The fdinfo scan costs 1-5ms of CPU per second (<0.5% of one core), the binary uses <1MB RAM, and it doesn't interact with the render pipeline at all. Your GPU won't notice.

## Disclaimer

This runs as root and bind-mounts over a kernel sysfs path. It doesn't touch hardware registers, firmware, or kernel modules, and everything reverts on stop/reboot, but use at your own risk. Test with `--dry-run` first. This is a community workaround, not an official fix from AMD or ASRock.

## Affected hardware

| Board | APU | Status |
|---|---|---|
| ASRock BC-250 | PS5 Oberon | Working |

If it works (or doesn't) on your hardware, open an issue with your `lspci | grep VGA` output and a `xxd` dump of your gpu_metrics file.

## License

MIT