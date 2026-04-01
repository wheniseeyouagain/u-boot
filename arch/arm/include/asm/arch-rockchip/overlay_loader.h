/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Dynamic Device Tree Overlay Loader for Rockchip Platforms
 *
 * Copyright (C) 2024 TaishanPi
 */

#ifndef __ASM_ARCH_OVERLAY_LOADER_H
#define __ASM_ARCH_OVERLAY_LOADER_H

/**
 * rockchip_load_dtbo_overlays() - Load and apply device tree overlays
 *
 * This function reads overlay configuration from /boot/ubootEnv.txt on the
 * rootfs partition and applies the specified .dtbo files to the device tree.
 *
 * Configuration file format (ubootEnv.txt):
 *   overlays=overlay1.dtbo overlay2.dtbo overlay3.dtbo
 *   # Comments are supported (lines starting with #)
 *
 * Overlay files should be placed in /boot/overlays/ directory on rootfs.
 *
 * @fdt_addr:	Pointer to the main device tree blob (will be modified)
 *
 * Return: 0 on success (even if no overlays are applied), negative on error
 */
int rockchip_load_dtbo_overlays(void *fdt_addr);

#endif /* __ASM_ARCH_OVERLAY_LOADER_H */
