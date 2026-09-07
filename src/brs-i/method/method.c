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

#if defined(FWTS_HAS_ACPI) && (FWTS_ARCH_RISCV)

#include "fwts_acpi_object_eval.h"

/* Standard AML IDs for an ECAM-capable PCI host bridge. */
#define CID_PCI			"PNP0A03"
#define HID_ECAM		"PNP0A08"

static int method_brsi_init(fwts_framework *fw)
{
	if (fwts_acpi_init(fw) != FWTS_OK) {
		fwts_log_error(fw, "Cannot initialise ACPI.");
		return FWTS_ERROR;
	}

	return FWTS_OK;
}

static int method_brsi_deinit(fwts_framework *fw)
{
	return fwts_acpi_deinit(fw);
}

typedef struct {
	fwts_framework *fw;
	bool found;
	bool passed;
	char device_path[128];
} method_brsi_crs_ctx;

static ACPI_STATUS method_brsi_crs_resource(ACPI_RESOURCE *resource, void *context)
{
	method_brsi_crs_ctx *ctx = context;
	const char *desc = NULL;
	uint64_t min;
	uint64_t length;

	switch (resource->Type) {
	case ACPI_RESOURCE_TYPE_IO:
		desc = "IO";
		min = resource->Data.Io.Minimum;
		length = resource->Data.Io.AddressLength;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		desc = "FixedIO";
		min = resource->Data.FixedIo.Address;
		length = resource->Data.FixedIo.AddressLength;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS16:
		if (resource->Data.Address16.ResourceType == ACPI_IO_RANGE) {
			desc = "WordIO";
			min = resource->Data.Address16.Address.Minimum;
			length = resource->Data.Address16.Address.AddressLength;
		}
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS32:
		if (resource->Data.Address32.ResourceType == ACPI_IO_RANGE) {
			desc = "DWordIO";
			min = resource->Data.Address32.Address.Minimum;
			length = resource->Data.Address32.Address.AddressLength;
		}
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		if (resource->Data.Address64.ResourceType == ACPI_IO_RANGE) {
			desc = "QWordIO";
			min = resource->Data.Address64.Address.Minimum;
			length = resource->Data.Address64.Address.AddressLength;
		}
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_ADDRESS64:
		if (resource->Data.ExtAddress64.ResourceType == ACPI_IO_RANGE) {
			desc = "ExtendedIO";
			min = resource->Data.ExtAddress64.Address.Minimum;
			length = resource->Data.ExtAddress64.Address.AddressLength;
		}
		break;
	default:
		break;
	}

	if (desc) {
		ctx->passed = false;
		fwts_log_info(ctx->fw,
			"PCIe Root Complex %s _CRS has I/O range descriptor %s "
			"(min=0x%" PRIx64 ", length=0x%" PRIx64 ").",
			ctx->device_path[0] ? ctx->device_path : "(unknown)",
			desc, min, length);
	}

	return AE_OK;
}

static void method_brsi_acpi_fullname(ACPI_HANDLE handle, char *out, size_t out_size)
{
	ACPI_BUFFER buf;

	if (!out || !out_size)
		return;

	buf.Pointer = out;
	buf.Length = out_size;
	out[0] = '\0';
	if (ACPI_FAILURE(AcpiGetName(handle, ACPI_FULL_PATHNAME, &buf)))
		strncpy(out, "(unknown)", out_size - 1);
}

static ACPI_STATUS method_brsi_pci_host_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_crs_ctx *ctx = context;
	ACPI_STATUS status;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found = true;
	method_brsi_acpi_fullname(handle, ctx->device_path, sizeof(ctx->device_path));

	fwts_log_info(ctx->fw,
		"Checking _CRS of PCIe Root Complex %s (HID %s / CID %s).",
		ctx->device_path, HID_ECAM, CID_PCI);

	status = AcpiWalkResources(handle, METHOD_NAME__CRS,
		method_brsi_crs_resource, ctx);
	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND)
		fwts_log_warning(ctx->fw,
			"Failed to walk _CRS of %s: %s.",
			ctx->device_path, AcpiFormatException(status));

	return AE_OK;
}

static int method_brsi_aml010(fwts_framework *fw)
{
	method_brsi_crs_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	ctx.found = false;
	ctx.passed = true;

	/*
	 * AcpiGetDevices() matches the given ID against both _HID and _CID,
	 * so PNP0A08 covers ECAM PCI Express host bridges.
	 */
	AcpiGetDevices(HID_ECAM, method_brsi_pci_host_walk, &ctx, NULL);

	if (!ctx.found) {
		fwts_skipped(fw,
			"AML_010: no PCIe Root Complex with _HID/_CID %s found; "
			"skipping I/O range check.",
			HID_ECAM);
		return FWTS_OK;
	}

	if (ctx.passed) {
		fwts_passed(fw,
			"AML_010: _CRS of PCIe Root Complex device(s) does not "
			"return I/O range descriptors.");
	} else {
		fwts_warning(fw,
			"AML_010: PCIe Root Complex _CRS returns I/O range "
			"descriptors. BRS-I says _CRS SHOULD NOT return I/O "
			"ranges (WordIO, DWordIO, QWordIO, IO, FixedIO or "
			"ExtendedIO).");
		fwts_advice(fw,
			"Legacy PCI I/O BARs are uncommon on modern PCIe devices "
			"and describing I/O space can complicate Root Complex "
			"configuration. Remove I/O descriptors from the Root "
			"Complex _CRS unless a specific device requires them.");
	}

	return FWTS_OK;
}

static fwts_framework_minor_test method_brsi_tests[] = {
	{ method_brsi_aml010,
	  "AML_010: PCIe Root Complex _CRS SHOULD NOT return I/O ranges." },
	{ NULL, NULL }
};

static fwts_framework_ops method_brsi_ops = {
	.description = "RISC-V BRS-I ACPI Methods and Objects test.",
	.init        = method_brsi_init,
	.deinit      = method_brsi_deinit,
	.minor_tests = method_brsi_tests
};

FWTS_REGISTER("method_brsi", &method_brsi_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI)

#endif
