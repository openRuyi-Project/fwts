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
#define HID_CPU			"ACPI0007"

/* Generic Register Descriptor (ACPI 6.4.3.7): 0x82 + length word + 12-byte GAS. */
#define AML_GENERIC_REG_DESC		0x82
#define AML_GENERIC_REG_MIN_LEN		15
#define ACPI_ADR_SPACE_FFH		0x7F
#define CPPC_FFH_TYPE_SBI		0x1
#define CPPC_FFH_TYPE_CSR		0x2

#define CPPC_V2_REV			2
#define CPPC_V3_REV			3
#define CPPC_V4_REV			4
#define CPPC_V2_NUM_ENT			21
#define CPPC_V3_NUM_ENT			23
#define CPPC_V4_NUM_ENT			25

/* Indices into the _CPC package (ACPI 8.4.6.1). */
#define CPC_IDX_NUM_ENTRIES		0
#define CPC_IDX_REVISION		1
#define CPC_IDX_HIGHEST_PERF		2
#define CPC_IDX_NOMINAL_PERF		3
#define CPC_IDX_LOW_NONLINEAR_PERF	4
#define CPC_IDX_LOWEST_PERF		5
#define CPC_IDX_DESIRED_PERF		7
#define CPC_IDX_REF_CTR			13
#define CPC_IDX_DELIVERED_CTR		14

static bool no_os_perf_ctrl;

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
	char device_path[128];
	int io_num;
	bool walk_failed;
} method_brsi_rc;

typedef struct {
	fwts_framework *fw;
	unsigned int found;
	fwts_list rc_list;
} method_brsi_rc_ctx;

static ACPI_STATUS method_brsi_crs_resource(ACPI_RESOURCE *resource, void *context)
{
	method_brsi_rc *rc = context;
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
		rc->io_num++;
		fwts_log_info(rc->fw,
			"PCIe Root Complex %s _CRS has I/O range descriptor %s "
			"(min=0x%" PRIx64 ", length=0x%" PRIx64 ").",
			rc->device_path[0] ? rc->device_path : "(unknown)",
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
	method_brsi_rc_ctx *ctx = context;
	method_brsi_rc *rc;
	ACPI_STATUS status;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	rc = calloc(1, sizeof(*rc));
	if (rc == NULL)
		return AE_NO_MEMORY;
	rc->fw = ctx->fw;
	method_brsi_acpi_fullname(handle, rc->device_path, sizeof(rc->device_path));

	fwts_log_info(ctx->fw,
		"Checking _CRS of PCIe Root Complex %s (HID %s / CID %s).",
		rc->device_path, HID_ECAM, CID_PCI);

	status = AcpiWalkResources(handle, METHOD_NAME__CRS,
		method_brsi_crs_resource, rc);
	if (ACPI_FAILURE(status)) {
		rc->walk_failed = true;
		fwts_log_warning(ctx->fw,
			"Failed to walk _CRS of %s: %s.",
			rc->device_path, AcpiFormatException(status));
	}
	fwts_list_append(&ctx->rc_list, rc);
	ctx->found++;

	return AE_OK;
}

static int method_brsi_aml010(fwts_framework *fw)
{
	fwts_list_link *item;
	method_brsi_rc_ctx ctx;
	int walk_failed = 0;
	int has_io = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	fwts_list_init(&ctx.rc_list);

	/*
	 * AcpiGetDevices() matches the given ID against both _HID and _CID,
	 * so PNP0A08 covers ECAM PCI Express host bridges.
	 */
	AcpiGetDevices(HID_ECAM, method_brsi_pci_host_walk, &ctx, NULL);

	fwts_list_foreach(item, &ctx.rc_list) {
		method_brsi_rc *rc = fwts_list_data(method_brsi_rc *, item);
		if (rc->walk_failed)
			walk_failed++;
		else if (rc->io_num)
			has_io++;
	}

	if (ctx.found == 0) {
		fwts_skipped(fw,
			"AML_010: no PCIe Root Complex with _HID/_CID %s found; "
			"skipping I/O range check.",
			HID_ECAM);
	} else if (has_io) {
		fwts_warning(fw,
			"AML_010: %u of %u PCIe Root Complex _CRS return I/O "
			"range descriptors. BRS-I says _CRS SHOULD NOT return "
			"I/O ranges (WordIO, DWordIO, QWordIO, IO, FixedIO or "
			"ExtendedIO).",
			has_io, ctx.found);
		fwts_advice(fw,
			"Legacy PCI I/O BARs are uncommon on modern PCIe devices "
			"and describing I/O space can complicate Root Complex "
			"configuration. Remove I/O descriptors from the Root "
			"Complex _CRS unless a specific device requires them.");
	} else if (walk_failed) {
		fwts_warning(fw,
			"AML_010: failed to inspect _CRS of %u of %u PCIe "
			"Root Complex device(s); not treated as "
			"\"does not return I/O range descriptors\".",
			walk_failed, ctx.found);
	} else {
		fwts_passed(fw,
			"AML_010: _CRS of %u PCIe Root Complex device(s) "
			"does not return I/O range descriptors.",
			ctx.found);
	}

	fwts_list_free_items(&ctx.rc_list, free);
	return FWTS_OK;
}

static int method_brsi_aml020(fwts_framework *fw)
{
	fwts_list *methods;
	fwts_list_link *item;
	bool found = false;
	bool lookup_failed = false;
	unsigned int candidates = 0;

	if ((methods = fwts_acpi_object_get_names()) != NULL) {
		fwts_list_foreach(item, methods) {
			char *name = fwts_list_data(char *, item);
			size_t len;
			ACPI_HANDLE handle;
			ACPI_OBJECT_TYPE type;
			ACPI_STATUS status;
			const char *which;

			if (name == NULL)
				continue;

			len = strlen(name);
			if (len < 4)
				continue;

			if (strncmp(name + len - 4, "_PRS", 4) == 0)
				which = "_PRS";
			else if (strncmp(name + len - 4, "_SRS", 4) == 0)
				which = "_SRS";
			else
				continue;

			status = AcpiGetHandle(NULL, name, &handle);
			if (ACPI_FAILURE(status)) {
				lookup_failed = true;
				candidates++;
				fwts_log_warning(fw, "AML_020: AcpiGetHandle(%s) failed: %s.",
					name, AcpiFormatException(status));
				continue;
			}

			status = AcpiGetType(handle, &type);
			if (ACPI_FAILURE(status)) {
				lookup_failed = true;
				candidates++;
				fwts_log_warning(fw, "AML_020: AcpiGetType(%s) failed: %s.",
					name, AcpiFormatException(status));
				continue;
			}

			/* Skip namespace scopes that happen to end in the same suffix. */
			if (type == ACPI_TYPE_LOCAL_SCOPE)
				continue;

			found = true;
			fwts_log_info(fw, "AML_020: found %s method %s.", which, name);
		}
	}

	if (found) {
		fwts_warning(fw,
			"AML_020: _PRS and/or _SRS methods are implemented. "
			"BRS-I says these methods SHOULD NOT be implemented.");
		fwts_advice(fw,
			"ACPI resource descriptors are typically used for "
			"devices with fixed resource ranges. Flexible resource "
			"assignment via _PRS/_SRS is not supported by most "
			"modern ACPI operating systems. Remove these methods "
			"unless a device truly requires runtime rebalancing.");
	} else if (lookup_failed) {
		fwts_warning(fw,
			"AML_020: found %u name(s) ending in _PRS/_SRS, but "
			"AcpiGetHandle/AcpiGetType failed; not treated as "
			"\"no methods are implemented\".", candidates);
	} else {
		fwts_passed(fw,
			"AML_020: no _PRS or _SRS methods are implemented.");
	}

	return FWTS_OK;
}

typedef struct {
	fwts_framework *fw;
	unsigned int under_sb;
	unsigned int under_pr;
	unsigned int elsewhere;
} method_brsi_cpu_ctx;

static ACPI_STATUS method_brsi_cpu_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_cpu_ctx *ctx = context;
	char path[128];

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	method_brsi_acpi_fullname(handle, path, sizeof(path));
	fwts_log_info(ctx->fw, "AML_030: found %s at %s.", HID_CPU, path);

	if (strncmp(path, "\\_SB", 4) == 0)
		ctx->under_sb++;
	else if (strncmp(path, "\\_PR", 4) == 0)
		ctx->under_pr++;
	else
		ctx->elsewhere++;

	return AE_OK;
}

static int method_brsi_aml030(fwts_framework *fw)
{
	method_brsi_cpu_ctx ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;

	AcpiGetDevices(HID_CPU, method_brsi_cpu_walk, &ctx, NULL);

	if (ctx.under_pr || ctx.elsewhere) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_030",
			"Per-hart %s objects must be under \\_SB, "
			"not deprecated \\_PR (%u under \\_PR, %u elsewhere, "
			"%u under \\_SB).",
			HID_CPU, ctx.under_pr, ctx.elsewhere, ctx.under_sb);
	} else if (ctx.under_sb) {
		fwts_passed(fw,
			"AML_030: %s per-hart objects are under \\_SB.",
			HID_CPU);
	} else {
		fwts_skipped(fw,
			"AML_030: no %s per-hart objects found.",
			HID_CPU);
	}

	return FWTS_OK;
}


typedef struct {
	fwts_framework *fw;
	unsigned int harts;
	unsigned int with_cpc;
	unsigned int failed;
} method_brsi_cpc_ctx;

/*
 * Parse an AML Generic Register Descriptor Buffer into a GAS.
 * A NULL register is SystemMemory with width/offset/access/address all 0.
 */
static bool method_brsi_cpc_parse_reg(
	const ACPI_OBJECT *obj,
	fwts_acpi_gas *gas,
	bool *is_null)
{
	uint8_t *p;

	if (obj == NULL || obj->Type != ACPI_TYPE_BUFFER ||
	    obj->Buffer.Pointer == NULL ||
	    obj->Buffer.Length < AML_GENERIC_REG_MIN_LEN)
		return false;

	p = obj->Buffer.Pointer;
	if (p[0] != AML_GENERIC_REG_DESC)
		return false;

	memcpy(gas, p + 3, sizeof(*gas));
	*is_null = (gas->address_space_id == 0 &&
		    gas->register_bit_width == 0 &&
		    gas->register_bit_offset == 0 &&
		    gas->access_width == 0 &&
		    gas->address == 0);
	return true;
}

static bool method_brsi_cpc_perf_usable(
	fwts_framework *fw,
	const char *path,
	const char *field,
	const ACPI_OBJECT *obj)
{
	fwts_acpi_gas gas;
	bool is_null = true;

	if (obj->Type == ACPI_TYPE_INTEGER) {
		if (obj->Integer.Value == 0) {
			fwts_log_info(fw,
				"AML_040: %s.%s is Integer 0 (unsupported).",
				path, field);
			return false;
		}
		fwts_log_info(fw,
			"AML_040: %s.%s = Integer 0x%" PRIx64 ".",
			path, field, (uint64_t)obj->Integer.Value);
		return true;
	}

	if (!method_brsi_cpc_parse_reg(obj, &gas, &is_null) || is_null) {
		fwts_log_info(fw,
			"AML_040: %s.%s is missing, malformed or a NULL "
			"register.",
			path, field);
		return false;
	}

	fwts_log_info(fw,
		"AML_040: %s.%s register space=0x%" PRIx8
		" width=%u addr=0x%" PRIx64 ".",
		path, field, gas.address_space_id,
		gas.register_bit_width, (uint64_t)gas.address);
	return true;
}

static bool method_brsi_cpc_reg_usable(
	fwts_framework *fw,
	const char *path,
	const char *field,
	const ACPI_OBJECT *obj)
{
	fwts_acpi_gas gas;
	bool is_null = true;

	if (!method_brsi_cpc_parse_reg(obj, &gas, &is_null)) {
		fwts_log_info(fw,
			"AML_040: %s.%s is not a Generic Register Descriptor.",
			path, field);
		return false;
	}
	if (is_null) {
		fwts_log_info(fw,
			"AML_040: %s.%s is a NULL register; OSPM cannot use it.",
			path, field);
		return false;
	}

	fwts_log_info(fw,
		"AML_040: %s.%s register space=0x%" PRIx8
		" width=%u addr=0x%" PRIx64 ".",
		path, field, gas.address_space_id,
		gas.register_bit_width, (uint64_t)gas.address);

	/*
	 * RISC-V FFH: FFixedHW descriptors must use Table 5/6 encoding
	 * (SBI CPPC type 0x1 or CSR type 0x2). Other spaces (SystemMemory)
	 * are allowed by ACPI and not forbidden by BRS AML_040.
	 */
	if (gas.address_space_id == ACPI_ADR_SPACE_FFH) {
		uint64_t addr = gas.address;
		uint8_t type = (addr >> 60) & 0xf;
		uint32_t mid = (addr >> 32) & 0xfffffff;

		if (gas.register_bit_width != 64 ||
		    gas.register_bit_offset != 0 ||
		    gas.access_width != 4) {
			fwts_log_info(fw,
				"AML_040: %s.%s FFixedHW descriptor must be "
				"64-bit, offset 0, AccessSize QWord.",
				path, field);
			return false;
		}
		if (mid != 0) {
			fwts_log_info(fw,
				"AML_040: %s.%s FFixedHW bits[59:32] must be 0.",
				path, field);
			return false;
		}
		if (type != CPPC_FFH_TYPE_SBI && type != CPPC_FFH_TYPE_CSR) {
			fwts_log_info(fw,
				"AML_040: %s.%s FFixedHW type 0x%x is reserved "
				"(want 0x1 SBI CPPC or 0x2 CSR).",
				path, field, type);
			return false;
		}
		if (type == CPPC_FFH_TYPE_CSR && ((addr >> 12) & 0xfffff) != 0) {
			fwts_log_info(fw,
				"AML_040: %s.%s CSR FFixedHW bits[31:12] "
				"must be 0.",
				path, field);
			return false;
		}
	}

	return true;
}

static bool method_brsi_cpc_check(
	fwts_framework *fw,
	ACPI_HANDLE handle,
	const char *path)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT *obj;
	ACPI_STATUS status;
	uint64_t nent, rev;
	bool rc = true;

	status = AcpiEvaluateObject(handle, "_CPC", NULL, &buf);
	if (ACPI_FAILURE(status) || buf.Pointer == NULL) {
		fwts_log_info(fw,
			"AML_040: %s._CPC is missing or failed to evaluate "
			"(%s).",
			path, AcpiFormatException(status));
		return false;
	}

	obj = buf.Pointer;
	/*
	 * Package type, Revision/NumEntries types, and per-field Integer/Buffer
	 * types are already validated by src/acpi/method/method.c
	 * (method_test_CPC_return). Only BRS-I semantic checks remain here.
	 */
	if (obj->Type != ACPI_TYPE_PACKAGE || obj->Package.Count < 2) {
		fwts_log_info(fw,
			"AML_040: %s._CPC is not a usable Package (type %u, "
			"count=%u); see the generic method _CPC test.",
			path, obj->Type, obj->Type == ACPI_TYPE_PACKAGE ?
				obj->Package.Count : 0);
		free(buf.Pointer);
		return false;
	}

	nent = (obj->Package.Elements[CPC_IDX_NUM_ENTRIES].Type ==
		ACPI_TYPE_INTEGER) ?
		obj->Package.Elements[CPC_IDX_NUM_ENTRIES].Integer.Value : 0;
	rev = (obj->Package.Elements[CPC_IDX_REVISION].Type ==
		ACPI_TYPE_INTEGER) ?
		obj->Package.Elements[CPC_IDX_REVISION].Integer.Value : 0;

	fwts_log_info(fw,
		"AML_040: %s._CPC revision=%" PRIu64 " entries=%" PRIu64
		" (package count %u).",
		path, rev, nent, obj->Package.Count);

	/* BRS AML_040 requires CPPC; ACPI allows revision 1, BRS does not. */
	if (rev != CPPC_V2_REV && rev != CPPC_V3_REV && rev != CPPC_V4_REV) {
		fwts_log_info(fw,
			"AML_040: %s._CPC revision %" PRIu64
			" is not 2, 3 or 4.",
			path, rev);
		rc = false;
	}

	if (obj->Package.Count > CPC_IDX_LOWEST_PERF) {
		if (!method_brsi_cpc_perf_usable(fw, path, "HighestPerformance",
				&obj->Package.Elements[CPC_IDX_HIGHEST_PERF]))
			rc = false;
		if (!method_brsi_cpc_perf_usable(fw, path, "NominalPerformance",
				&obj->Package.Elements[CPC_IDX_NOMINAL_PERF]))
			rc = false;
		if (!method_brsi_cpc_perf_usable(fw, path,
				"LowestNonlinearPerformance",
				&obj->Package.Elements[CPC_IDX_LOW_NONLINEAR_PERF]))
			rc = false;
		if (!method_brsi_cpc_perf_usable(fw, path, "LowestPerformance",
				&obj->Package.Elements[CPC_IDX_LOWEST_PERF]))
			rc = false;
	} else {
		rc = false;
	}

	if (obj->Package.Count > CPC_IDX_DELIVERED_CTR) {
		if (!method_brsi_cpc_reg_usable(fw, path,
				"DesiredPerformanceRegister",
				&obj->Package.Elements[CPC_IDX_DESIRED_PERF]))
			rc = false;
		if (!method_brsi_cpc_reg_usable(fw, path,
				"ReferencePerformanceCounterRegister",
				&obj->Package.Elements[CPC_IDX_REF_CTR]))
			rc = false;
		if (!method_brsi_cpc_reg_usable(fw, path,
				"DeliveredPerformanceCounterRegister",
				&obj->Package.Elements[CPC_IDX_DELIVERED_CTR]))
			rc = false;
	} else {
		rc = false;
	}

	free(buf.Pointer);
	return rc;
}

static ACPI_STATUS method_brsi_cpc_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_cpc_ctx *ctx = context;
	char device_path[128];

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->harts++;
	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "AML_040: found per-hart device %s.",
		device_path);

	if (method_brsi_cpc_check(ctx->fw, handle, device_path))
		ctx->with_cpc++;
	else
		ctx->failed++;

	return AE_OK;
}

static unsigned int method_brsi_count_pstate(fwts_framework *fw)
{
	fwts_list *methods;
	fwts_list_link *item;
	unsigned int n = 0;

	methods = fwts_acpi_object_get_names();
	if (methods == NULL)
		return 0;

	fwts_list_foreach(item, methods) {
		char *name = fwts_list_data(char *, item);
		size_t len;

		if (name == NULL)
			continue;
		len = strlen(name);
		if (len < 4)
			continue;
		if (strncmp(name + len - 4, "_PCT", 4) != 0 &&
		    strncmp(name + len - 4, "_PSS", 4) != 0 &&
		    strncmp(name + len - 4, "_PPC", 4) != 0)
			continue;

		fwts_log_info(fw,
			"AML_040: found legacy P-state method %s.", name);
		n++;
	}

	return n;
}

static int method_brsi_aml040(fwts_framework *fw)
{
	method_brsi_cpc_ctx ctx;
	unsigned int pstate;

	/*
	 * AML_040: systems that support OS-directed hart performance
	 * control and power management MUST expose it via CPPC (_CPC).
	 * Whether the platform supports that cannot be discovered from
	 * ACPI, so --brs-i-no-os-perf-ctrl declares that CPPC is not
	 * required.
	 */
	if (no_os_perf_ctrl) {
		fwts_skipped(fw,
			"AML_040: --brs-i-no-os-perf-ctrl specified; "
			"platform does not support OS-directed hart "
			"performance control, CPPC is not required.");
		return FWTS_OK;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;

	AcpiGetDevices(HID_CPU, method_brsi_cpc_walk, &ctx, NULL);

	pstate = method_brsi_count_pstate(fw);

	if (ctx.harts == 0) {
		if (pstate) {
			fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_040",
				"Found %u legacy P-state method(s) (_PCT/"
				"_PSS/_PPC) but no per-hart "HID_CPU" or "
				"Processor() objects with CPPC. BRS-I "
				"requires OS-directed performance control "
				"via CPPC, not P-states.",
				pstate);
		} else {
			fwts_skipped(fw,
				"AML_040: no "HID_CPU" or Processor() per-hart "
				"objects found; skipping CPPC check. Re-run "
				"with --brs-i-no-os-perf-ctrl if this platform "
				"does not support OS-directed hart performance "
				"control.");
		}
		return FWTS_OK;
	}

	if (ctx.failed || pstate) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_040",
			"%u of %u per-hart object(s) failed CPPC (_CPC)%s. "
			"Systems supporting OS-directed hart performance "
			"control MUST expose it via CPPC (ACPI 8.4.6). "
			"Re-run with --brs-i-no-os-perf-ctrl if this "
			"platform has no such support.",
			ctx.failed, ctx.harts,
			pstate ? "; legacy P-state methods are present" : "");
		if (pstate)
			fwts_advice(fw,
				"Replace _PCT/_PSS/_PPC P-state objects with "
				"a per-hart _CPC package. On RISC-V the "
				"register fields typically use FFixedHW "
				"descriptors that encode SBI CPPC or a CSR.");
	} else {
		fwts_passed(fw,
			"AML_040: %u per-hart object(s) expose CPPC via a "
			"usable _CPC package.",
			ctx.with_cpc);
	}

	return FWTS_OK;
}

static int options_handler(
	fwts_framework *fw,
	int argc,
	char * const argv[],
	int option_char,
	int long_index)
{
	FWTS_UNUSED(fw);
	FWTS_UNUSED(argc);
	FWTS_UNUSED(argv);

	if (option_char == 0) {
		switch (long_index) {
		case 0:	/* --brs-i-no-os-perf-ctrl */
			no_os_perf_ctrl = true;
			break;
		}
	}
	return FWTS_OK;
}

static fwts_option options[] = {
	{ "brs-i-no-os-perf-ctrl", "", 0,
	  "Platform has no OS-directed hart performance control (skip AML_040)" },
	{ NULL, NULL, 0, NULL }
};

static fwts_framework_minor_test method_brsi_tests[] = {
	{ method_brsi_aml010,
	  "AML_010: PCIe Root Complex _CRS SHOULD NOT return I/O ranges." },
	{ method_brsi_aml020,
	  "AML_020: _PRS and _SRS methods SHOULD NOT be implemented." },
	{ method_brsi_aml030,
	  "AML_030: per-hart devices MUST be under \\_SB, not \\_PR." },
	{ method_brsi_aml040,
	  "AML_040: OS-directed hart performance control MUST use CPPC (_CPC)." },
	{ NULL, NULL }
};

static fwts_framework_ops method_brsi_ops = {
	.description     = "RISC-V BRS-I ACPI Methods and Objects test.",
	.init            = method_brsi_init,
	.deinit          = method_brsi_deinit,
	.minor_tests     = method_brsi_tests,
	.options         = options,
	.options_handler = options_handler
};

FWTS_REGISTER("method_brsi", &method_brsi_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI)

#endif
