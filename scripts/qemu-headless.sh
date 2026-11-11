#!/usr/bin/env bash

QEMU_HEADLESS_PROBE_OPTS=(
    -no-user-config
    -nodefaults
    -nographic
    -display none
    -vga none
    -monitor none
    -serial none
)

QEMU_HEADLESS_ENV=(
    env
    -u DISPLAY
    -u WAYLAND_DISPLAY
    -u QEMU_DISPLAY
    -u GDK_BACKEND
    -u QT_QPA_PLATFORM
    SDL_VIDEODRIVER=dummy
    SDL_AUDIODRIVER=dummy
    QEMU_AUDIO_DRV=none
    QEMU_AUDIO_BACKEND=none
)

QEMU_HEADLESS_RUN_OPTS=(
    -nographic
    -display none
    -vga none
    -chardev stdio,id=rtmux-stdio,signal=off,mux=on
    -serial chardev:rtmux-stdio
    -mon chardev=rtmux-stdio,mode=readline
)

qemu_enforce_headless_env()
{
    export SDL_VIDEODRIVER=dummy
    export SDL_AUDIODRIVER=dummy
    export QEMU_AUDIO_DRV=none
    export QEMU_AUDIO_BACKEND=none
    unset DISPLAY WAYLAND_DISPLAY QEMU_DISPLAY GDK_BACKEND QT_QPA_PLATFORM
}

qemu_opt_value()
{
    printf '%s\n' "${1%%,*}"
}

qemu_is_graphics_device()
{
    case "$(qemu_opt_value "$1")" in
        VGA|ati-vga|bochs-display|cirrus-vga|qxl*|ramfb|secondary-vga|\
        virtio-gpu*|virtio-vga*)
            return 0
            ;;
    esac
    return 1
}

qemu_is_headless_option()
{
    case "$1" in
        -display|--display|-vga|--vga|-monitor|--monitor|-serial|--serial|\
        -parallel|--parallel|-spice|--spice|-vnc|--vnc)
            return 0
            ;;
    esac
    return 1
}

qemu_is_headless_option_assignment()
{
    case "$1" in
        -display=*|--display=*|-vga=*|--vga=*|-monitor=*|--monitor=*|\
        -serial=*|--serial=*|-parallel=*|--parallel=*|-spice=*|--spice=*|\
        -vnc=*|--vnc=*)
            return 0
            ;;
    esac
    return 1
}

qemu_sanitize_headless_opts()
{
    local opts=("$@")
    local out=()
    local i opt dev

    for ((i = 0; i < ${#opts[@]}; i++)); do
        opt="${opts[$i]}"
        case "$opt" in
            -nographic|-full-screen|--full-screen|-curses|--curses)
                continue
                ;;
            -device|--device)
                if ((i + 1 < ${#opts[@]})); then
                    dev="${opts[$((i + 1))]}"
                    if qemu_is_graphics_device "$dev"; then
                        ((i++))
                        continue
                    fi
                fi
                ;;
            -device=*|--device=*)
                dev="${opt#*=}"
                if qemu_is_graphics_device "$dev"; then
                    continue
                fi
                ;;
        esac

        if qemu_is_headless_option "$opt"; then
            ((i++))
            continue
        fi
        if qemu_is_headless_option_assignment "$opt"; then
            continue
        fi
        out+=("$opt")
    done

    QEMU_HEADLESS_SANITIZED_OPTS=("${out[@]}" "${QEMU_HEADLESS_RUN_OPTS[@]}")
    qemu_enforce_headless_env
}
