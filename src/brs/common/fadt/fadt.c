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

#define BRS_VERSION(major, minor)	 ((((uint16_t)(major)) << 8) | (minor))

static int fadt_brs_revision(fwts_framework *fw)
{
	const uint8_t BRS_LEAST_MAJOR = 6;
	const uint8_t BRS_LEAST_MINOR = 6;
	uint8_t major = 0;
	uint8_t minor = 0;

	if (!fwts_firmware_has_features(FWTS_FW_FEATURE_ACPI)) {
		if (fw->flags & FWTS_FLAG_BRSI) {
			fwts_failed(fw, LOG_LEVEL_CRITICAL, "fadt_revision:",
				"ACPI is required for RISC-V BRS-I.");
		} else if (fw->flags & FWTS_FLAG_BRSB) {
			fwts_skipped(fw,
				"ACPI is optional for RISC-V BRS-B; skip FADT revision check.");
		}
		return FWTS_OK;
	}

	fwts_get_fadt_version(fw, &major, &minor);

	fwts_log_info(fw, "FADT revision: %" PRIu8 ".%" PRIu8, major, minor);

	if (BRS_VERSION(major, minor) >=
	    BRS_VERSION(BRS_LEAST_MAJOR, BRS_LEAST_MINOR))
		fwts_passed(fw, "FADT revision is up to date.");
	else {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "fadt_revision:",
			"FADT revision is outdated, at least revision is %" PRIu8 ".%" PRIu8,
			BRS_LEAST_MAJOR, BRS_LEAST_MINOR);
	}

	return FWTS_OK;
}

static fwts_framework_minor_test fadt_brs_tests[] = {
	{ fadt_brs_revision, "FADT Revision Test." },
	{ NULL, NULL }
};

static fwts_framework_ops fadt_brs_ops = {
	.description = "RISC-V BRS FADT Fixed ACPI Description Table tests.",
	.minor_tests = fadt_brs_tests
};

FWTS_REGISTER("fadt_brs", &fadt_brs_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI | FWTS_FLAG_BRSB)
#endif
