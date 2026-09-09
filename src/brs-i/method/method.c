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
#define HID_CONTAINER		"ACPI0010"
/* ACPI Time and Alarm Device, same HID as src/acpi/devices/time/time.c. */
#define HID_TAD			"ACPI000E"
#define HID_PLIC		"RSCV0001"
#define HID_APLIC		"RSCV0002"

/* RISC-V FFH LPI entry method types (bits[63:60]). */
#define LPI_FFH_TYPE_WFI		0x0
#define LPI_FFH_TYPE_SBI_HSM		0x1

/* Indices into the _LPI package (ACPI 8.4.4.3). */
#define LPI_IDX_REVISION		0
#define LPI_IDX_LEVELID			1
#define LPI_IDX_COUNT			2
#define LPI_STATE_FIRST			3
#define LPI_STATE_MIN_ENT		7
#define LPI_ST_MIN_RESIDENCY		0
#define LPI_ST_WAKE_LATENCY		1
#define LPI_ST_FLAGS			2
#define LPI_ST_ARCH_FLAGS		3
#define LPI_ST_RES_FREQ			4
#define LPI_ST_PARENT_STATE		5
#define LPI_ST_ENTRY_METHOD		6
#define LPI_ST_RES_COUNTER		7
#define LPI_ST_USAGE_COUNTER		8
#define LPI_ST_NAME			9
#define LPI_ARCH_FLAGS_RESERVED_MASK	(~0x1ULL)

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

static bool no_os_idle_states;
static bool no_os_perf_ctrl;
static bool no_osbus_rtc;

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

static int method_brsi_aml020(fwts_framework *fw)
{
	fwts_list *methods;
	fwts_list_link *item;
	bool found = false;

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
			if (ACPI_FAILURE(status))
				continue;

			status = AcpiGetType(handle, &type);
			if (ACPI_FAILURE(status))
				continue;

			/* Skip namespace scopes that happen to end in the same suffix. */
			if (type == ACPI_TYPE_LOCAL_SCOPE)
				continue;

			found = true;
			fwts_log_info(fw, "AML_020: found %s method %s.", which, name);
		}
	}

	if (!found) {
		fwts_passed(fw,
			"AML_020: no _PRS or _SRS methods are implemented.");
	} else {
		fwts_warning(fw,
			"AML_020: _PRS and/or _SRS methods are implemented. "
			"BRS-I says these methods SHOULD NOT be implemented.");
		fwts_advice(fw,
			"ACPI resource descriptors are typically used for "
			"devices with fixed resource ranges. Flexible resource "
			"assignment via _PRS/_SRS is not supported by most "
			"modern ACPI operating systems. Remove these methods "
			"unless a device truly requires runtime rebalancing.");
	}

	return FWTS_OK;
}

static int method_brsi_aml030(fwts_framework *fw)
{
	fwts_list *methods;
	fwts_list_link *item;
	unsigned int under_sb = 0, under_pr = 0, elsewhere = 0;

	if ((methods = fwts_acpi_object_get_names()) != NULL) {
		fwts_list_foreach(item, methods) {
			char *name = fwts_list_data(char *, item);
			ACPI_HANDLE handle;
			ACPI_DEVICE_INFO *info;
			ACPI_STATUS status;

			if (name == NULL)
				continue;

			status = AcpiGetHandle(NULL, name, &handle);
			if (ACPI_FAILURE(status))
				continue;

			status = AcpiGetObjectInfo(handle, &info);
			if (ACPI_FAILURE(status))
				continue;

			if (!(info->Valid & ACPI_VALID_HID) ||
			    info->HardwareId.String == NULL ||
			    strcmp(info->HardwareId.String, HID_CPU) != 0) {
				ACPI_FREE(info);
				continue;
			}
			ACPI_FREE(info);

			fwts_log_info(fw, "AML_030: found "HID_CPU" at %s.", name);

			if (strncmp(name, "\\_SB", 4) == 0)
				under_sb++;
			else if (strncmp(name, "\\_PR", 4) == 0)
				under_pr++;
			else
				elsewhere++;
		}
	}

	if (under_pr || elsewhere) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_030",
			"Per-hart "HID_CPU" objects must be under \\_SB, "
			"not deprecated \\_PR (%u under \\_PR, %u elsewhere, "
			"%u under \\_SB).",
			under_pr, elsewhere, under_sb);
	} else if (under_sb) {
		fwts_passed(fw,
			"AML_030: "HID_CPU" per-hart objects are under \\_SB.");
	} else {
		fwts_skipped(fw,
			"AML_030: no "HID_CPU" per-hart objects found.");
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
		ACPI_FREE(buf.Pointer);
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

	ACPI_FREE(buf.Pointer);
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

typedef struct {
	fwts_framework *fw;
	unsigned int harts;
	unsigned int containers;
	unsigned int with_lpi;
	unsigned int failed;
} method_brsi_lpi_ctx;

static bool method_brsi_lpi_entry_ok(
	fwts_framework *fw,
	const char *path,
	unsigned int state,
	const ACPI_OBJECT *obj)
{
	fwts_acpi_gas gas;
	bool is_null = true;
	uint64_t addr;
	uint8_t type;
	uint32_t mid;

	/*
	 * ACPI allows Entry Method to be an Integer (OS-initiated
	 * composition) or a Generic Register Descriptor Buffer.
	 * RISC-V FFH describes WFI / SBI HSM via FFixedHW buffers.
	 */
	if (obj->Type == ACPI_TYPE_INTEGER) {
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u Entry Method = "
			"Integer 0x%" PRIx64 ".",
			path, state, (uint64_t)obj->Integer.Value);
		return true;
	}

	if (!method_brsi_cpc_parse_reg(obj, &gas, &is_null) || is_null) {
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u Entry Method is missing, "
			"malformed or a NULL register.",
			path, state);
		return false;
	}

	fwts_log_info(fw,
		"AML_050: %s._LPI state %u Entry Method space=0x%" PRIx8
		" width=%u addr=0x%" PRIx64 ".",
		path, state, gas.address_space_id,
		gas.register_bit_width, (uint64_t)gas.address);

	if (gas.address_space_id != ACPI_ADR_SPACE_FFH)
		return true;

	addr = gas.address;
	type = (addr >> 60) & 0xf;
	mid = (addr >> 32) & 0xfffffff;

	if (gas.register_bit_width != 64 ||
	    gas.register_bit_offset != 0 ||
	    gas.access_width != 4) {
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u FFixedHW Entry Method "
			"must be 64-bit, offset 0, AccessSize QWord.",
			path, state);
		return false;
	}
	if (mid != 0) {
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u FFixedHW bits[59:32] "
			"must be 0.",
			path, state);
		return false;
	}
	if (type != LPI_FFH_TYPE_WFI && type != LPI_FFH_TYPE_SBI_HSM) {
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u FFixedHW type 0x%x is "
			"reserved (want 0x0 WFI or 0x1 SBI HSM).",
			path, state, type);
		return false;
	}
	if (type == LPI_FFH_TYPE_WFI && (addr & 0xffffffffULL) != 0) {
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u WFI Entry Method "
			"bits[31:0] must be 0.",
			path, state);
		return false;
	}

	return true;
}

static bool method_brsi_lpi_state_ok(
	fwts_framework *fw,
	const char *path,
	unsigned int state,
	const ACPI_OBJECT *pkg)
{
	bool rc = true;

	/*
	 * State package type and element types (Integer/Buffer/String) are
	 * already checked by src/acpi/method/method.c (method_test_LPI_return).
	 * Only RISC-V BRS-I field semantics remain here.
	 */
	if (pkg->Type != ACPI_TYPE_PACKAGE ||
	    pkg->Package.Count < LPI_STATE_MIN_ENT)
		return false;

	if (pkg->Package.Count > LPI_ST_ARCH_FLAGS &&
	    pkg->Package.Elements[LPI_ST_ARCH_FLAGS].Type == ACPI_TYPE_INTEGER) {
		uint64_t arch =
			pkg->Package.Elements[LPI_ST_ARCH_FLAGS].Integer.Value;

		fwts_log_info(fw,
			"AML_050: %s._LPI state %u MinResidency=%" PRIu64
			" WakeLatency=%" PRIu64 " Flags=0x%" PRIx64
			" ArchFlags=0x%" PRIx64 ".",
			path, state,
			(uint64_t)pkg->Package.Elements[LPI_ST_MIN_RESIDENCY].Integer.Value,
			(uint64_t)pkg->Package.Elements[LPI_ST_WAKE_LATENCY].Integer.Value,
			(uint64_t)pkg->Package.Elements[LPI_ST_FLAGS].Integer.Value,
			arch);
		if (arch & LPI_ARCH_FLAGS_RESERVED_MASK) {
			fwts_log_info(fw,
				"AML_050: %s._LPI state %u Arch. Context Lost "
				"Flags 0x%" PRIx64 " has reserved bits set "
				"(RISC-V FFH only allows bit 0).",
				path, state, arch);
			rc = false;
		}
	}

	if (pkg->Package.Count > LPI_ST_ENTRY_METHOD &&
	    !method_brsi_lpi_entry_ok(fw, path, state,
			&pkg->Package.Elements[LPI_ST_ENTRY_METHOD]))
		rc = false;

	if (pkg->Package.Count > LPI_ST_NAME &&
	    pkg->Package.Elements[LPI_ST_NAME].Type == ACPI_TYPE_STRING &&
	    pkg->Package.Elements[LPI_ST_NAME].String.Pointer)
		fwts_log_info(fw,
			"AML_050: %s._LPI state %u Name=\"%s\".",
			path, state,
			pkg->Package.Elements[LPI_ST_NAME].String.Pointer);

	return rc;
}

typedef enum {
	METHOD_BRSI_LPI_ABSENT = 0,
	METHOD_BRSI_LPI_BAD,
	METHOD_BRSI_LPI_OK
} method_brsi_lpi_rc;

static method_brsi_lpi_rc method_brsi_lpi_eval(
	fwts_framework *fw,
	ACPI_HANDLE handle,
	const char *path)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT *obj;
	ACPI_STATUS status;
	uint64_t rev, count;
	unsigned int i;
	bool rc = true;

	status = AcpiEvaluateObject(handle, "_LPI", NULL, &buf);
	if (ACPI_FAILURE(status) || buf.Pointer == NULL)
		return METHOD_BRSI_LPI_ABSENT;

	obj = buf.Pointer;
	/*
	 * Package type, Revision/LevelID/Count types, revision==0, Count vs
	 * number of state packages, and per-state element types are already
	 * validated by src/acpi/method/method.c (method_test_LPI_return).
	 */
	if (obj->Type != ACPI_TYPE_PACKAGE ||
	    obj->Package.Count < LPI_STATE_FIRST + 1) {
		fwts_log_info(fw,
			"AML_050: %s._LPI is not a usable Package; see the "
			"generic method _LPI test.",
			path);
		ACPI_FREE(buf.Pointer);
		return METHOD_BRSI_LPI_BAD;
	}

	rev = (obj->Package.Elements[LPI_IDX_REVISION].Type ==
		ACPI_TYPE_INTEGER) ?
		obj->Package.Elements[LPI_IDX_REVISION].Integer.Value : 0;
	count = (obj->Package.Elements[LPI_IDX_COUNT].Type ==
		ACPI_TYPE_INTEGER) ?
		obj->Package.Elements[LPI_IDX_COUNT].Integer.Value : 0;

	fwts_log_info(fw,
		"AML_050: %s._LPI revision=%" PRIu64 " LevelID=0x%" PRIx64
		" Count=%" PRIu64 " (package count %u).",
		path, rev,
		obj->Package.Elements[LPI_IDX_LEVELID].Type == ACPI_TYPE_INTEGER ?
			(uint64_t)obj->Package.Elements[LPI_IDX_LEVELID].Integer.Value : 0,
		count, obj->Package.Count);

	for (i = LPI_STATE_FIRST; i < obj->Package.Count; i++) {
		if (!method_brsi_lpi_state_ok(fw, path,
				i - LPI_STATE_FIRST + 1,
				&obj->Package.Elements[i]))
			rc = false;
	}

	ACPI_FREE(buf.Pointer);
	return rc ? METHOD_BRSI_LPI_OK : METHOD_BRSI_LPI_BAD;
}

/*
 * Idle states may be declared on the hart (ACPI0007) or on an ancestor
 * Processor Container (ACPI0010). Walk parents until a usable _LPI is
 * found or the root is reached.
 */
static bool method_brsi_lpi_on_hierarchy(
	fwts_framework *fw,
	ACPI_HANDLE handle,
	const char *path)
{
	ACPI_HANDLE cur = handle;
	char cur_path[128];
	unsigned int depth = 0;

	strncpy(cur_path, path, sizeof(cur_path) - 1);
	cur_path[sizeof(cur_path) - 1] = '\0';

	while (cur && depth < 16) {
		ACPI_HANDLE parent;
		ACPI_DEVICE_INFO *info;

		switch (method_brsi_lpi_eval(fw, cur, cur_path)) {
		case METHOD_BRSI_LPI_OK:
			if (cur != handle)
				fwts_log_info(fw,
					"AML_050: %s inherits _LPI from "
					"ancestor %s.",
					path, cur_path);
			return true;
		case METHOD_BRSI_LPI_BAD:
			return false;
		case METHOD_BRSI_LPI_ABSENT:
		default:
			break;
		}

		if (ACPI_FAILURE(AcpiGetParent(cur, &parent)) ||
		    parent == NULL || parent == cur)
			break;

		info = NULL;
		if (ACPI_SUCCESS(AcpiGetObjectInfo(parent, &info))) {
			bool is_container = (info->Valid & ACPI_VALID_HID) &&
				info->HardwareId.String &&
				strcmp(info->HardwareId.String,
					HID_CONTAINER) == 0;
			ACPI_FREE(info);
			if (!is_container)
				break;
		} else {
			break;
		}

		cur = parent;
		method_brsi_acpi_fullname(cur, cur_path, sizeof(cur_path));
		depth++;
	}

	fwts_log_info(fw,
		"AML_050: %s has no usable _LPI on the device or an "
		HID_CONTAINER" ancestor.",
		path);
	return false;
}

static ACPI_STATUS method_brsi_lpi_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_lpi_ctx *ctx = context;
	char device_path[128];

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->harts++;
	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "AML_050: found per-hart device %s.",
		device_path);

	if (method_brsi_lpi_on_hierarchy(ctx->fw, handle, device_path))
		ctx->with_lpi++;
	else
		ctx->failed++;

	return AE_OK;
}

static ACPI_STATUS method_brsi_lpi_container_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_lpi_ctx *ctx = context;
	char device_path[128];

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->containers++;
	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw,
		"AML_050: found Processor Container %s.", device_path);

	/*
	 * Containers are optional LPI nodes. Validate _LPI when present;
	 * absence is not a failure (the leaf hart may carry the states).
	 */
	if (method_brsi_lpi_eval(ctx->fw, handle, device_path) ==
	    METHOD_BRSI_LPI_OK)
		fwts_log_info(ctx->fw,
			"AML_050: %s exposes a usable container _LPI.",
			device_path);

	return AE_OK;
}

static unsigned int method_brsi_count_cstate(fwts_framework *fw)
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
		if (strncmp(name + len - 4, "_CST", 4) != 0 &&
		    strncmp(name + len - 4, "_CSD", 4) != 0)
			continue;

		fwts_log_info(fw,
			"AML_050: found legacy C-state method %s.", name);
		n++;
	}

	return n;
}

static int method_brsi_aml050(fwts_framework *fw)
{
	method_brsi_lpi_ctx ctx;
	unsigned int cstate;

	/*
	 * AML_050: processor idle states MUST be described using _LPI
	 * (ACPI 8.4.3 / 8.4.4), not legacy _CST/_CSD C-states.
	 * --brs-i-no-os-idle-states declares that the platform exposes
	 * no OS-directed hart idle states.
	 */
	if (no_os_idle_states) {
		fwts_skipped(fw,
			"AML_050: --brs-i-no-os-idle-states specified; "
			"platform does not describe OS-directed hart idle "
			"states, _LPI is not required.");
		return FWTS_OK;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;

	AcpiGetDevices(HID_CPU, method_brsi_lpi_walk, &ctx, NULL);
	AcpiGetDevices(HID_CONTAINER,
		method_brsi_lpi_container_walk, &ctx, NULL);

	cstate = method_brsi_count_cstate(fw);

	if (ctx.harts == 0) {
		if (cstate) {
			fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_050",
				"Found %u legacy C-state method(s) (_CST/"
				"_CSD) but no per-hart "HID_CPU" objects "
				"with _LPI. BRS-I requires processor idle "
				"states via Low Power Idle (_LPI), not "
				"C-states.",
				cstate);
		} else {
			fwts_skipped(fw,
				"AML_050: no "HID_CPU" per-hart objects "
				"found; skipping _LPI check. Re-run with "
				"--brs-i-no-os-idle-states if this platform "
				"does not describe processor idle states.");
		}
		return FWTS_OK;
	}

	if (ctx.failed || cstate) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_050",
			"%u of %u per-hart object(s) failed Low Power Idle "
			"(_LPI)%s. Processor idle states MUST be described "
			"using _LPI (ACPI 8.4.3). Re-run with --brs-i-no-os-idle-states "
			"if this platform has no OS-directed idle states.",
			ctx.failed, ctx.harts,
			cstate ? "; legacy C-state methods are present" : "");
		if (cstate)
			fwts_advice(fw,
				"Replace _CST/_CSD C-state objects with "
				"per-hart or Processor Container _LPI "
				"packages. On RISC-V the Entry Method "
				"typically uses an FFixedHW descriptor "
				"encoding WFI (type 0) or SBI HSM suspend "
				"(type 1).");
	} else {
		fwts_passed(fw,
			"AML_050: %u per-hart object(s) describe idle "
			"states via _LPI (%u Processor Container(s) "
			"examined).",
			ctx.with_lpi, ctx.containers);
	}

	return FWTS_OK;
}

typedef struct {
	fwts_framework *fw;
	const char *id;
	unsigned int found;
	unsigned int failed;
} method_brsi_tad_ctx;

/*
 * Evaluate `name` under `handle`.
 * Optional `args` / `arg_count` are passed through (used by _SRT).
 *
 * On success:
 *   Integer             -> *value = Integer.Value
 *   Buffer named "_GRT" -> *value = Buffer.Length
 */
static bool method_brsi_eval(
	ACPI_HANDLE handle,
	char *name,
	ACPI_OBJECT *args,
	UINT32 arg_count,
	uint64_t *value)
{
	ACPI_BUFFER buf = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT_LIST arg_list;
	ACPI_OBJECT *obj;
	ACPI_STATUS status;
	bool rc = false;

	if (args && arg_count) {
		arg_list.Count = arg_count;
		arg_list.Pointer = args;
		status = AcpiEvaluateObject(handle, name, &arg_list, &buf);
	} else {
		status = AcpiEvaluateObject(handle, name, NULL, &buf);
	}
	if (ACPI_FAILURE(status) || buf.Pointer == NULL)
		return false;

	obj = buf.Pointer;
	if (obj->Type == ACPI_TYPE_INTEGER) {
		rc = true;
		if (value)
			*value = obj->Integer.Value;
	} else if (obj->Type == ACPI_TYPE_BUFFER && !strcmp(name, "_GRT")) {
		rc = true;
		if (value)
			*value = obj->Buffer.Length;
	}

	ACPI_FREE(buf.Pointer);
	return rc;
}

static ACPI_STATUS method_brsi_tad_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_tad_ctx *ctx = context;
	char device_path[128];
	fwts_acpi_time_buffer real_time;
	ACPI_OBJECT arg0;
	uint64_t gcp = 0;
	uint64_t grt_len = 0;
	uint64_t srt = 0;
	bool failed = false;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found++;

	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "%s: found TAD %s (HID %s).",
		ctx->id, device_path, HID_TAD);

	if (!method_brsi_eval(handle, "_GCP", NULL, 0, &gcp)) {
		fwts_log_info(ctx->fw,
			"%s: %s._GCP is mandatory but missing, failed "
			"to evaluate, or did not return an Integer.",
			ctx->id, device_path);
		failed = true;
	} else {
		fwts_log_info(ctx->fw, "%s: %s._GCP returned 0x%" PRIx64 ".",
			ctx->id, device_path, gcp);
		if (gcp & ~0x1ff) {
			fwts_log_info(ctx->fw,
				"%s: %s._GCP reserved bits 9..31 are set.",
				ctx->id, device_path);
			failed = true;
		} else if (!(gcp & 0x4)) {
			fwts_log_info(ctx->fw,
				"%s: %s._GCP bit 2 (get/set real time) is not set.",
				ctx->id, device_path);
			failed = true;
		}
	}

	if (!method_brsi_eval(handle, "_GRT", NULL, 0, &grt_len)) {
		fwts_log_info(ctx->fw,
			"%s: %s._GRT is mandatory but missing, failed "
			"to evaluate, or did not return a Buffer.",
			ctx->id, device_path);
		failed = true;
	} else if (grt_len != sizeof(fwts_acpi_time_buffer)) {
		fwts_log_info(ctx->fw,
			"%s: %s._GRT returned a Buffer of %" PRIu64
			" bytes, expected %zu.",
			ctx->id, device_path, grt_len,
			sizeof(fwts_acpi_time_buffer));
		failed = true;
	} else {
		fwts_log_info(ctx->fw,
			"%s: %s._GRT returned a %" PRIu64 "-byte time buffer.",
			ctx->id, device_path, grt_len);
	}

	memset(&real_time, 0, sizeof(real_time));
	real_time.year = 2000;
	real_time.month = 1;
	real_time.day = 1;
	real_time.hour = 0;
	real_time.minute = 0;
	real_time.milliseconds = 1;
	real_time.timezone = 0;

	arg0.Type = ACPI_TYPE_BUFFER;
	arg0.Buffer.Length = sizeof(real_time);
	arg0.Buffer.Pointer = (void *)&real_time;

	if (!method_brsi_eval(handle, "_SRT", &arg0, 1, &srt)) {
		fwts_log_info(ctx->fw,
			"%s: %s._SRT is mandatory but missing, failed "
			"to evaluate, or did not return an Integer.",
			ctx->id, device_path);
		failed = true;
	} else {
		fwts_log_info(ctx->fw, "%s: %s._SRT returned 0x%" PRIx64 ".",
			ctx->id, device_path, srt);
	}

	if (failed)
		ctx->failed++;

	return AE_OK;
}

static int method_brsi_aml060(fwts_framework *fw)
{
	method_brsi_tad_ctx ctx;

	/*
	 * AML_060 applies only when the platform has an RTC on a bus the
	 * OS manages (I2C, SPI, ...). That cannot be discovered from ACPI,
	 * so --brs-i-no-osbus-rtc declares that TAD is not required.
	 */
	if (no_osbus_rtc) {
		fwts_skipped(fw,
			"AML_060: --brs-i-no-osbus-rtc specified; no RTC on "
			"an OS-managed bus, Time and Alarm Device is not "
			"required.");
		return FWTS_OK;
	}

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	ctx.id = "AML_060";

	AcpiGetDevices(HID_TAD, method_brsi_tad_walk, &ctx, NULL);

	if (ctx.found == 0)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_060",
			"No Time and Alarm Device (HID %s) found. Systems "
			"with an RTC on an OS-managed bus MUST implement a "
			"TAD with functioning _GCP (bit 2 set), _GRT and "
			"_SRT. Re-run with --brs-i-no-osbus-rtc if this "
			"system has no such RTC.",
			HID_TAD);
	else if (ctx.failed)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_060",
			"%u of %u Time and Alarm Device(s) failed _GCP bit 2, "
			"_GRT or _SRT.",
			ctx.failed, ctx.found);
	else
		fwts_passed(fw,
			"AML_060: %u Time and Alarm Device(s) implement "
			"functioning _GCP (bit 2 set), _GRT and _SRT.",
			ctx.found);

	return FWTS_OK;
}

static int method_brsi_aml070(fwts_framework *fw)
{
	method_brsi_tad_ctx ctx;

	/*
	 * AML_070: a TAD must work with no vendor OS driver loaded.
	 * fwts evaluates AML with its own ACPICA instance, which does not
	 * use kernel I2C/SPI/UART drivers or GenericSerialBus handlers
	 * installed in the in-kernel interpreter.
	 */
	fwts_log_info(fw,
		"AML_070: evaluate TAD _GCP/_GRT/_SRT with fwts ACPICA. "
		"This interpreter does not use kernel-loaded I2C/SPI/UART "
		"drivers or in-kernel GenericSerialBus handlers.");
	fwts_log_info(fw,
		"AML_070: SystemMemory/SystemIO OperationRegions can work "
		"here (same physical address as the kernel). GenericSerialBus "
		"Field accesses usually fail in fwts even if a mainline bus "
		"driver is bound and the same AML works in the kernel.");
	fwts_log_info(fw,
		"AML_070: a pass is stricter than \"works after the OS loads "
		"a bus driver\". The driver-loaded OperationRegion switch "
		"is not tested.");

	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;
	ctx.id = "AML_070";

	AcpiGetDevices(HID_TAD, method_brsi_tad_walk, &ctx, NULL);

	if (ctx.found == 0) {
		fwts_skipped(fw,
			"AML_070: no Time and Alarm Device (HID %s) found; "
			"requirement applies only when a TAD is implemented.",
			HID_TAD);
		return FWTS_OK;
	}

	if (ctx.failed) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_070",
			"%u of %u Time and Alarm Device(s) failed _GCP, _GRT "
			"or _SRT under fwts ACPICA.",
			ctx.failed, ctx.found);
		fwts_advice(fw,
			"AML_070 requires a TAD to work without extra "
			"system-specific OS drivers. fwts uses a private "
			"ACPICA instance: kernel I2C/SPI/UART drivers do "
			"not install GenericSerialBus handlers for this "
			"evaluator. A failure often means _GRT/_SRT only "
			"work through a driver-backed GenericSerialBus "
			"OperationRegion. Confirm whether a SystemMemory "
			"fallback exists for the no-driver path. A kernel "
			"success with drivers loaded does not satisfy this "
			"fwts check. The AML switch onto a driver-backed "
			"region after the driver loads is not tested.");
	} else {
		fwts_passed(fw,
			"AML_070: %u Time and Alarm Device(s) are functional "
			"under fwts ACPICA without kernel bus drivers.",
			ctx.found);
		fwts_advice(fw,
			"This pass means _GCP/_GRT/_SRT ran in fwts ACPICA "
			"with no kernel GenericSerialBus handler, which is "
			"stricter than \"the TAD works once a mainline I2C/"
			"SPI/UART driver is bound\". It does not verify that "
			"AML later switches to a driver-backed OperationRegion "
			"when that driver is loaded.");
	}

	return FWTS_OK;
}

typedef struct {
	fwts_framework *fw;
	const char *hid;
	const char *kind;
	unsigned int found;
	unsigned int failed;
} method_brsi_gsb_ctx;

static ACPI_STATUS method_brsi_gsb_walk(
	ACPI_HANDLE handle,
	UINT32 nesting_level,
	void *context,
	void **return_value)
{
	method_brsi_gsb_ctx *ctx = context;
	char device_path[128];
	uint64_t gsb = 0;

	FWTS_UNUSED(nesting_level);
	FWTS_UNUSED(return_value);

	ctx->found++;

	method_brsi_acpi_fullname(handle, device_path, sizeof(device_path));

	fwts_log_info(ctx->fw, "AML_080: found %s %s (HID %s).",
		ctx->kind, device_path, ctx->hid);

	if (!method_brsi_eval(handle, "_GSB", NULL, 0, &gsb)) {
		fwts_log_info(ctx->fw,
			"AML_080: %s._GSB is mandatory but missing, failed "
			"to evaluate, or did not return an Integer.",
			device_path);
		ctx->failed++;
	} else {
		fwts_log_info(ctx->fw,
			"AML_080: %s._GSB returned GSI base 0x%" PRIx64 ".",
			device_path, gsb);
	}

	return AE_OK;
}

static int method_brsi_aml080(fwts_framework *fw)
{
	method_brsi_gsb_ctx ctx;

	/*
	 * AML_080: every PLIC (RSCV0001) and APLIC (RSCV0002) namespace
	 * device must implement _GSB returning the GSI base as an Integer.
	 * Device presence when MADT has matching entries is AML_100.
	 */
	memset(&ctx, 0, sizeof(ctx));
	ctx.fw = fw;

	ctx.hid = HID_PLIC;
	ctx.kind = "PLIC";
	AcpiGetDevices(HID_PLIC, method_brsi_gsb_walk, &ctx, NULL);

	ctx.hid = HID_APLIC;
	ctx.kind = "APLIC";
	AcpiGetDevices(HID_APLIC, method_brsi_gsb_walk, &ctx, NULL);

	if (ctx.found == 0) {
		fwts_skipped(fw,
			"AML_080: no PLIC (HID %s) or APLIC (HID %s) device "
			"found; _GSB is required on those objects when they "
			"exist.",
			HID_PLIC, HID_APLIC);
		return FWTS_OK;
	}

	if (ctx.failed)
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "AML_080",
			"%u of %u PLIC/APLIC device(s) failed _GSB.",
			ctx.failed, ctx.found);
	else
		fwts_passed(fw,
			"AML_080: %u PLIC/APLIC device(s) implement _GSB "
			"returning an Integer GSI base.",
			ctx.found);

	return FWTS_OK;
}

static int options_handler(
	fwts_framework *fw,
	int argc,
	char * const argv[],
	int option_char,
	int long_index)
{
	FWTS_UNUSED(argc);
	FWTS_UNUSED(argv);

	if (option_char == 0) {
		switch (long_index) {
		case 0:	/* --brs-i-no-os-perf-ctrl */
			no_os_perf_ctrl = true;
			fwts_log_info(fw,
				"BRS-I: no OS-directed hart performance control, skip AML_040 check");
			break;
		case 1:	/* --brs-i-no-os-idle-states */
			no_os_idle_states = true;
			fwts_log_info(fw,
				"BRS-I: no OS-directed hart idle states, skip AML_050 check");
			break;
		case 2:	/* --brs-i-no-osbus-rtc */
			no_osbus_rtc = true;
			fwts_log_info(fw,
				"BRS-I: no RTC on OS-managed bus, skip AML_060 check");
			break;

		}
	}
	return FWTS_OK;
}

static fwts_option options[] = {
	{ "brs-i-no-os-perf-ctrl", "", 0,
	  "Platform has no OS-directed hart performance control (skip AML_040)" },
	{ "brs-i-no-os-idle-states", "", 0,
	  "Platform has no OS-directed hart idle states (skip AML_050)" },
	{ "brs-i-no-osbus-rtc", "", 0,
	  "Platform has no RTC on an OS-managed bus (skip AML_060)" },
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
	{ method_brsi_aml050,
	  "AML_050: processor idle states MUST be described using _LPI." },
	{ method_brsi_aml060,
	  "AML_060: TAD with _GCP bit 2, _GRT and _SRT if RTC is on an OS-managed bus." },
	{ method_brsi_aml070,
	  "AML_070: TAD MUST work in fwts ACPICA without kernel bus drivers." },
	{ method_brsi_aml080,
	  "AML_080: PLIC and APLIC devices MUST implement _GSB." },
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
