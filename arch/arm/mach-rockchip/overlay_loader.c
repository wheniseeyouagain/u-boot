// SPDX-License-Identifier: GPL-2.0+
/*
 * Dynamic Device Tree Overlay Loader for Rockchip Platforms
 *
 * Copyright (C) 2024 TaishanPi
 *
 * This module implements dynamic device tree overlay loading similar to
 * Raspberry Pi/LubanCat. It reads overlay configuration from ubootEnv.txt
 * on the rootfs partition and applies specified .dtbo files.
 *
 * Configuration file format (ubootEnv.txt):
 *   overlays=overlay1.dtbo overlay2.dtbo overlay3.dtbo
 *   # Comments are supported
 *   # Each overlay file should be placed in /boot/overlays/ directory
 */

#include <common.h>
#include <blk.h>
#include <dm.h>
#include <fs.h>
#include <malloc.h>
#include <part.h>
#include <linux/libfdt.h>
#include <fdt_support.h>
#include <boot_rkimg.h>

/* Default paths for overlay files */
#define OVERLAY_ENV_FILE	"/boot/ubootEnv.txt"
#define OVERLAY_DIR		"/boot/overlays/"

/* Maximum configuration */
#define MAX_OVERLAYS		128             /* Support up to 128 overlays */
#define MAX_OVERLAY_NAME_LEN	64
#define MAX_ENV_SIZE		8192            /* 8KB for env file */
#define MAX_DTBO_SIZE		(256 * 1024)	/* 256KB per overlay */

/*
 * Parse overlay names from environment string
 * Format: overlays=name1.dtbo name2.dtbo name3.dtbo
 * 
 * Parameters:
 *   env_data: null-terminated environment file content
 *   overlay_names: dynamically allocated array of name buffers
 *   max_overlays: maximum number of overlays to parse
 *
 * Returns: number of overlays parsed
 */
static int parse_overlay_names(const char *env_data, char **overlay_names, int max_overlays)
{
	const char *line_start = env_data;
	const char *line_end;
	int count = 0;

	while (line_start && *line_start && count < max_overlays) {
		/* Skip leading whitespace */
		while (*line_start == ' ' || *line_start == '\t')
			line_start++;

		/* Skip empty lines and comments */
		if (*line_start == '\0')
			break;
		if (*line_start == '#' || *line_start == '\n') {
			line_start = strchr(line_start, '\n');
			if (line_start)
				line_start++;
			continue;
		}

		/* Find end of line */
		line_end = strchr(line_start, '\n');
		if (!line_end)
			line_end = line_start + strlen(line_start);

		/* Check for overlays= prefix */
		if (strncmp(line_start, "overlays=", 9) == 0) {
			const char *value_start = line_start + 9;
			const char *value_end = line_end;
			const char *token_start = value_start;
			const char *token_end;

			/* Parse space-separated overlay names */
			while (token_start < value_end && count < max_overlays) {
				/* Skip whitespace */
				while (token_start < value_end && 
				       (*token_start == ' ' || *token_start == '\t'))
					token_start++;

				if (token_start >= value_end)
					break;

				/* Find end of token */
				token_end = token_start;
				while (token_end < value_end && 
				       *token_end != ' ' && *token_end != '\t' &&
				       *token_end != '\n' && *token_end != '\0')
					token_end++;

				/* Copy overlay name */
				if (token_end > token_start) {
					int name_len = token_end - token_start;
					if (name_len >= MAX_OVERLAY_NAME_LEN)
						name_len = MAX_OVERLAY_NAME_LEN - 1;
					strncpy(overlay_names[count], token_start, name_len);
					overlay_names[count][name_len] = '\0';

					/* Add .dtbo extension if missing */
					if (!strstr(overlay_names[count], ".dtbo")) {
						int len = strlen(overlay_names[count]);
						if (len + 5 < MAX_OVERLAY_NAME_LEN) {
							strcat(overlay_names[count], ".dtbo");
						}
					}

					printf("  Found overlay: %s\n", overlay_names[count]);
					count++;
				}

				token_start = token_end;
			}
		}

		line_start = line_end;
		if (*line_start == '\n')
			line_start++;
	}

	return count;
}

/*
 * Apply device tree overlay to the main FDT
 * Returns: 0 on success, negative on failure
 */
static int apply_single_overlay(void *fdt, void *overlay, const char *name)
{
	int ret;

	/* Check overlay header */
	ret = fdt_check_header(overlay);
	if (ret) {
		printf("  [%s] Invalid overlay header: %s\n", name, fdt_strerror(ret));
		return ret;
	}

	/* Increase FDT size to accommodate overlay */
	ret = fdt_increase_size(fdt, fdt_totalsize(overlay) + 1024);
	if (ret) {
		printf("  [%s] Failed to increase FDT size: %s\n", name, fdt_strerror(ret));
		return ret;
	}

	/* Apply overlay */
	ret = fdt_overlay_apply_verbose(fdt, overlay);
	if (ret) {
		printf("  [%s] Failed to apply overlay: %s\n", name, fdt_strerror(ret));
		return ret;
	}

	printf("  [%s] Applied successfully\n", name);
	return 0;
}

/*
 * Main function to load and apply device tree overlays
 * This should be called after the main DTB is loaded but before booting kernel
 *
 * Parameters:
 *   fdt_addr: pointer to the main device tree blob
 *
 * Returns: 0 on success (even if no overlays are applied), negative on error
 */
int rockchip_load_dtbo_overlays(void *fdt_addr)
{
	struct blk_desc *dev_desc;
	disk_partition_t part_info;
	char **overlay_names = NULL;
	char *names_buf = NULL;
	char path[128];
	char *env_buf;
	void *dtbo_buf;
	loff_t bytes_read;
	int overlay_count = 0;
	int applied_count = 0;
	int part_num;
	int ret, i;

	printf("\n[TaishanPi] Dynamic Device Tree Overlay Loader\n");
	printf("================================================\n");

	/* Get boot device */
	dev_desc = rockchip_get_bootdev();
	if (!dev_desc) {
		printf("  Error: Cannot get boot device\n");
		return -ENODEV;
	}

	/* Find rootfs partition */
	part_num = part_get_info_by_name(dev_desc, PART_ROOTFS, &part_info);
	if (part_num < 0) {
		/* Try 'system' partition name as fallback */
		part_num = part_get_info_by_name(dev_desc, PART_SYSTEM, &part_info);
		if (part_num < 0) {
			printf("  Info: No rootfs/system partition found, skipping overlays\n");
			return 0;
		}
	}

	printf("  Using partition: %s (part %d)\n", part_info.name, part_num);

	/* Set up filesystem access */
	ret = fs_set_blk_dev_with_part(dev_desc, part_num);
	if (ret) {
		printf("  Error: Cannot set up filesystem, ret=%d\n", ret);
		return 0;
	}

	/* Allocate buffer for environment file */
	env_buf = malloc(MAX_ENV_SIZE);
	if (!env_buf) {
		printf("  Error: Cannot allocate memory for env file\n");
		return -ENOMEM;
	}
	memset(env_buf, 0, MAX_ENV_SIZE);

	/* Read ubootEnv.txt */
	ret = fs_read(OVERLAY_ENV_FILE, (ulong)env_buf, 0, MAX_ENV_SIZE - 1, &bytes_read);
	if (ret || bytes_read == 0) {
		printf("  Info: %s not found or empty, skipping overlays\n", OVERLAY_ENV_FILE);
		free(env_buf);
		return 0;
	}

	env_buf[bytes_read] = '\0';
	printf("  Read %lld bytes from %s\n", bytes_read, OVERLAY_ENV_FILE);

	/* Allocate memory for overlay names array (dynamically) */
	overlay_names = malloc(MAX_OVERLAYS * sizeof(char *));
	if (!overlay_names) {
		printf("  Error: Cannot allocate memory for overlay names array\n");
		free(env_buf);
		return -ENOMEM;
	}

	/* Allocate contiguous buffer for all overlay names */
	names_buf = malloc(MAX_OVERLAYS * MAX_OVERLAY_NAME_LEN);
	if (!names_buf) {
		printf("  Error: Cannot allocate memory for overlay names buffer\n");
		free(overlay_names);
		free(env_buf);
		return -ENOMEM;
	}
	memset(names_buf, 0, MAX_OVERLAYS * MAX_OVERLAY_NAME_LEN);

	/* Set up pointer array to point into the contiguous buffer */
	for (i = 0; i < MAX_OVERLAYS; i++)
		overlay_names[i] = names_buf + (i * MAX_OVERLAY_NAME_LEN);

	/* Parse overlay names from environment */
	overlay_count = parse_overlay_names(env_buf, overlay_names, MAX_OVERLAYS);
	free(env_buf);

	if (overlay_count == 0) {
		printf("  Info: No overlays specified in %s\n", OVERLAY_ENV_FILE);
		free(names_buf);
		free(overlay_names);
		return 0;
	}

	printf("  Found %d overlay(s) to apply\n", overlay_count);

	/* Allocate buffer for loading overlays */
	dtbo_buf = malloc(MAX_DTBO_SIZE);
	if (!dtbo_buf) {
		printf("  Error: Cannot allocate memory for overlay\n");
		free(names_buf);
		free(overlay_names);
		return -ENOMEM;
	}

	/* Resize main FDT to accommodate all overlays */
	ret = fdt_increase_size(fdt_addr, overlay_count * 32 * 1024);
	if (ret) {
		printf("  Error: Cannot increase FDT size: %s\n", fdt_strerror(ret));
		free(dtbo_buf);
		free(names_buf);
		free(overlay_names);
		return ret;
	}

	/* Load and apply each overlay */
	for (i = 0; i < overlay_count; i++) {
		/* Construct full path */
		snprintf(path, sizeof(path), "%s%s", OVERLAY_DIR, overlay_names[i]);

		/* Re-setup filesystem for each file read */
		ret = fs_set_blk_dev_with_part(dev_desc, part_num);
		if (ret) {
			printf("  [%s] Cannot access filesystem\n", overlay_names[i]);
			continue;
		}

		/* Read overlay file */
		memset(dtbo_buf, 0, MAX_DTBO_SIZE);
		ret = fs_read(path, (ulong)dtbo_buf, 0, MAX_DTBO_SIZE, &bytes_read);
		if (ret || bytes_read == 0) {
			printf("  [%s] File not found: %s\n", overlay_names[i], path);
			continue;
		}

		printf("  Loading %s (%lld bytes)...\n", overlay_names[i], bytes_read);

		/* Apply overlay */
		ret = apply_single_overlay(fdt_addr, dtbo_buf, overlay_names[i]);
		if (!ret)
			applied_count++;
	}

	free(dtbo_buf);
	free(names_buf);
	free(overlay_names);

	printf("================================================\n");
	printf("  Summary: Applied %d of %d overlay(s)\n", applied_count, overlay_count);
	printf("================================================\n\n");

	return 0;
}
