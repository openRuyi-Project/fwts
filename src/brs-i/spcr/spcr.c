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

#if defined(FWTS_HAS_ACPI) && defined(FWTS_ARCH_RISCV)

static fwts_acpi_table_info *table;

static int spcr_brsi_init(fwts_framework *fw)
{
	int rc;

	rc = acpi_table_generic_init(fw, "SPCR", &table);
	if (table == NULL || table->length == 0)
		return FWTS_OK;

	return rc;
}

/*
 * Best-effort check that graphics were exposed to an OS loader through
 * EFI_GRAPHICS_OUTPUT_PROTOCOL.  After ExitBootServices the protocol itself
 * is gone, so use surviving firmware artifacts:
 *   - BGRT is produced from GOP boot graphics
 *   - Linux binds remaining GOP framebuffers as efi-framebuffer
 */
static bool graphics_gop_available(fwts_framework *fw)
{
	fwts_acpi_table_info *bgrt = NULL;
	static const char *const paths[] = {
		"/sys/bus/platform/devices/efi-framebuffer.0",
		"/sys/bus/platform/drivers/efi-framebuffer/efi-framebuffer.0",
		"/sys/firmware/efi/gop",
		NULL
	};
	const char *const *path;

	if (fwts_acpi_find_table(fw, "BGRT", 0, &bgrt) == FWTS_OK &&
	    bgrt != NULL && bgrt->length > 0) {
		fwts_log_info(fw, "BGRT table present; treating GOP as available.");
		return true;
	}

	for (path = paths; *path != NULL; path++) {
		if (access(*path, F_OK) == 0) {
			fwts_log_info(fw,
				"%s exists; treating EFI GOP framebuffer as available.",
				*path);
			return true;
		}
	}

	fwts_log_info(fw,
		"No BGRT table or EFI GOP framebuffer found; "
		"graphics are treated as unavailable to the OS loader.");
	return false;
}

static int spcr_brsi_test1(fwts_framework *fw)
{
	bool have_spcr = (table != NULL && table->length > 0);
	bool have_gop;

	if (have_spcr) {
		fwts_passed(fw,
			"The Serial Port Console Redirection Table (SPCR) is present "
			"on this RISC-V BRS system.");
		return FWTS_OK;
	}

	have_gop = graphics_gop_available(fw);
	if (have_gop) {
		fwts_passed(fw,
			"SPCR is not present, but graphics hardware appears available "
			"to an OS loader via EFI_GRAPHICS_OUTPUT_PROTOCOL; "
			"ACPI_050 does not require SPCR in this case.");
		return FWTS_OK;
	}

	fwts_failed(fw, LOG_LEVEL_CRITICAL, "ACPI_050",
		"A Serial Port Console Redirection Table MUST be present on "
		"systems where the graphics hardware is not present or not made "
		"available to an OS loader via the standard UEFI "
		"EFI_GRAPHICS_OUTPUT_PROTOCOL interface (per ACPI_050).");

	return FWTS_OK;
}

static fwts_framework_minor_test spcr_brsi_tests[] = {
	{ spcr_brsi_test1, "Check SPCR table presence when GOP is unavailable." },
	{ NULL, NULL }
};

static fwts_framework_ops spcr_brsi_ops = {
	.description = "RISC-V BRS-I SPCR Serial Port Console Redirection Table test.",
	.init        = spcr_brsi_init,
	.minor_tests = spcr_brsi_tests
};

FWTS_REGISTER("spcr_brsi", &spcr_brsi_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI)

#endif
