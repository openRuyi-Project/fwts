/*
 * Copyright (C) 2026 Xiang W <wangxiang@iscas.ac.cn>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "fwts.h"

#if defined(FWTS_ARCH_RISCV)

#if FWTS_HAS_DEVICETREE
#include <libfdt.h>
#else
#error "Missing libfdt: RISC-V BRS Device Tree tests cannot be built"
#endif

static bool dt_brs_dt_is_dummy(fwts_framework * fw)
{
	int offset;
	const char *name;
	const void *fdt = fw->fdt;

	if (!fdt)
		return false;

	for (offset = fdt_first_subnode(fdt, 0);
	     offset >= 0;
	     offset = fdt_next_subnode(fdt, offset)) {
		name = fdt_get_name(fdt, offset, NULL);
		if (name && strcmp(name, "chosen") == 0)
			continue;
		return false;
	}
	return true;
}

static int dt_brs_check_hw_desc(fwts_framework *fw)
{
	bool has_acpi = fwts_firmware_has_features(FWTS_FW_FEATURE_ACPI);
	bool dt_valid = (fw->fdt != NULL) && !dt_brs_dt_is_dummy(fw);

	if (!has_acpi && !dt_valid) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "BRSNoHardwareDescription",
			"No hardware description is available");
		return FWTS_OK;
	}

	if (fw->flags & FWTS_FLAG_BRSI) {
		if (dt_valid)
			fwts_failed(fw, LOG_LEVEL_CRITICAL, "UEFI_080",
				"A Device Tree MUST only be exposed to the OS if no actual "
				"hardware description is included in the DT.");
		else
			fwts_passed(fw,
				"UEFI_080: no hardware Device Tree (ACPI describes hardware)");
		return FWTS_OK;
	}

	fwts_passed(fw, "Hardware description is present via %s%s%s",
		    has_acpi ? "ACPI" : "",
		    (has_acpi && dt_valid) ? " and " : "",
		    dt_valid ? "Device Tree" : "");
	return FWTS_OK;
}

static int dt_brs_check_dtb_format(fwts_framework *fw)
{
	int rc;
	uint32_t version;
	void *fdt = fw->fdt;

	if (!fdt) {
		fwts_skipped(fw, "No device tree present, skip DTB format check");
		return FWTS_OK;
	}

	rc = fdt_check_header(fdt);
	if (rc != 0) {
		fwts_failed(fw, LOG_LEVEL_HIGH, "DeviceTreeBaseInvalid",
			"Device tree data is invalid");
		return FWTS_OK;
	}

	/*
	 * BRS requires Devicetree Specification >= v0.3, which uses DTB
	 * format version 17 or higher.
	 */
	version = fdt_version(fdt);
	if (version < 17) {
		fwts_failed(fw, LOG_LEVEL_HIGH, "DeviceTreeVersion",
			"Device Tree blob version %" PRIu32 " is too old; "
			"BRS requires Devicetree Specification >= v0.3 "
			"(DTB version 17 or higher)",
			version);
		return FWTS_ERROR;
	}

	fwts_passed(fw, "DTB binary format is valid (version %" PRIu32 ")", version);
	return FWTS_OK;
}

static int dt_brs_check_dtc(fwts_framework *fw)
{
	const char *command;
	char *output = NULL;
	int fd;
	int rc;
	int status;
	pid_t pid;
	ssize_t len;

	if (!fwts_firmware_has_features(FWTS_FW_FEATURE_DEVICETREE)) {
		fwts_skipped(fw, "No device tree present, skip dtc DTB validity check");
		return FWTS_OK;
	}

	command = "dtc -I fs -O dtb -o /dev/null " DT_FS_PATH " 2>&1";
	rc = fwts_pipe_open_ro(command, &pid, &fd);
	if (rc < 0) {
		fwts_failed(fw, LOG_LEVEL_HIGH, "DeviceTreeDTCPipe", "failed to run dtc");
		return FWTS_OK;
	}

	rc = fwts_pipe_read(fd, &output, &len);
	if (rc) {
		fwts_pipe_close(fd, pid);
		fwts_failed(fw, LOG_LEVEL_HIGH, "DeviceTreeDTCPipe", "failed to read dtc output");
		return FWTS_OK;
	}

	status = fwts_pipe_close(fd, pid);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fwts_failed(fw, LOG_LEVEL_HIGH, "DeviceTreeDTCFailed", "dtc reported faults");
		if (output) {
			fwts_log_info_verbatim(fw, "%s", output);
			free(output);
		}
		return FWTS_OK;
	}

	if (output) {
		fwts_warning(fw, "dtc reported warnings");
		fwts_log_info_verbatim(fw, "%s", output);
		free(output);
		return FWTS_OK;
	}

	fwts_passed(fw, "dtc successfully validated the Device Tree");
	free(output);
	return FWTS_OK;
}

static fwts_framework_minor_test dt_brs_tests[] = {
	{ dt_brs_check_hw_desc, "Check hardware description (ACPI or Device Tree)" },
	{ dt_brs_check_dtb_format, "Check DTB binary format" },
	{ dt_brs_check_dtc, "Check DTB validity with dtc" },
	{ NULL, NULL },
};

static fwts_framework_ops dt_brs_ops = {
	.description	= "RISC-V BRS Device Tree tests.",
	.minor_tests	= dt_brs_tests,
};

FWTS_REGISTER("dt_brs", &dt_brs_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI | FWTS_FLAG_BRSB)

#endif
