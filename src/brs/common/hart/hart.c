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
#include <acpi.h>
#include <elf.h>
#include <libfdt.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

struct hart_brs_chk_priv {
	fwts_framework *fw;
	bool is_acpi;
	int failed;
	uint32_t id;
	const char *isa;
	const char **required;
	const char *passed_msg;
	const char *warning_msg;
	const char *failed_msg;
	enum acpi_rhct_mmu_type mmu_type;
};

static char *mmu_names[] = { "sv39", "sv48", "sv57" };

static const char *rva20s64_required[] = {
	/* RVA20U64 Mandatory */
	"m",
	"a",
	"f",
	"d",
	"c",
	"zicsr",
	"zicntr",
	"ziccif",
	"ziccrse",
	"ziccamoa",
	"za128rs",
	"zicclsm",

	/* RVA20S64 Mandatory */
	"i",
	"zifencei",
	//"ss1p11",     /* Cannot be detected in U-mode */
	//"svbare",     /* Cannot be detected in U-mode */
	"sv39",
	"svade",
	"ssccptr",
	"sstvecd",
	"sstvala",
	NULL
};

static const char *rva20s64_warning_msg =
	"detectable ISA extensions comply with RVA20S64, "
	"but ss1p11 and svbare cannot be verified in U-mode.";

static const char *rva20s64_failed_msg =
	"ISA does not comply with the mandatory requirements of RVA20S64.";

static const char *hart_brs_owner(const struct hart_brs_chk_priv *chk)
{
	return chk->is_acpi ? "ACPI UID" : "hart";
}

static enum acpi_rhct_mmu_type hart_brs_mmu_name_to_type(const char *s)
{
	int i;
	for (i = 0; i < sizeof(mmu_names) / sizeof(mmu_names[0]); i++) {
		if (!strcasecmp(s, mmu_names[i]))
			return i;
	}
	return -1;
}

static int hart_brs_xlen(const char *isa)
{
	if (!isa)
		return -1;
	if (strncasecmp(isa, "rv32", 4) == 0)
		return 32;
	if (strncasecmp(isa, "rv64", 4) == 0)
		return 64;
	return -1;
}

static bool hart_brs_has_base_extension(const char *isa, char ext)
{
	const char *p = isa;

	if (strncasecmp(p, "rv64", 4) == 0 || strncasecmp(p, "rv32", 4) == 0)
		p += 4;
	while (*p != '_' && *p != '\0') {
		if (*p == ext)
			return true;
		p++;
	}
	return false;
}

static bool hart_brs_has_extension(const char *isa, const char *ext)
{
	bool found = false;
	char *isa_copy;
	char *p;
	char *saveptr;
	char *token;
	size_t ext_len = strlen(ext);

	if (!isa)
		return false;

	if (ext_len == 1)
		return hart_brs_has_base_extension(isa, *ext);

	isa_copy = strdup(isa);
	if (!isa_copy)
		return false;

	p = isa_copy;
	if (strncasecmp(p, "rv64", 4) == 0 || strncasecmp(p, "rv32", 4) == 0)
		p += 4;

	token = strtok_r(p, "_", &saveptr);
	while (token) {
		if (strcasecmp(token, ext) == 0) {
			found = true;
			break;
		}
		token = strtok_r(NULL, "_", &saveptr);
	}

	free(isa_copy);

	return found;
}

static void hart_brs_isa_check(void *priv)
{
	const char *compatible_ext;
	const char **ext;
	bool has_ext;
	int isa_failed = 0;
	int mmu_need;
	int xlen;
	struct hart_brs_chk_priv *chk = priv;
	fwts_framework *fw = chk->fw;

	xlen = hart_brs_xlen(chk->isa);
	if (xlen == 64)
		fwts_passed(fw, "HR_010: %s %u: ISA is RV64.", hart_brs_owner(chk), chk->id);
	else {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "HR_010", "%s %u: ISA is not RV64 (xlen = %d).",
			    hart_brs_owner(chk), chk->id, xlen);
		isa_failed++;
	}

	for (ext = chk->required; *ext != NULL; ext++) {
		compatible_ext = NULL;
		if (!strcasecmp(*ext, "za128rs")) {
			/* Za64rs implies Za128rs because its maximum
			 * reservation set size is smaller. */
			has_ext = hart_brs_has_extension(chk->isa, "za128rs");
			if (!has_ext) {
				has_ext = hart_brs_has_extension(chk->isa, "za64rs");
				if (has_ext)
					compatible_ext = "za64rs";
			}
		} else if ((mmu_need = hart_brs_mmu_name_to_type(*ext)) >= 0) {
			has_ext = chk->mmu_type >= mmu_need;
			if (chk->mmu_type > mmu_need)
				compatible_ext = mmu_names[chk->mmu_type];
		} else
			has_ext = hart_brs_has_extension(chk->isa, *ext);

		if (has_ext) {
			if (compatible_ext)
				fwts_passed(
					fw,
					"HR_010: %s %u: has '%s', which satisfies mandatory '%s'.",
					hart_brs_owner(chk), chk->id, compatible_ext, *ext);
			else
				fwts_passed(fw, "HR_010: %s %u: has mandatory extension '%s'.",
					    hart_brs_owner(chk), chk->id, *ext);
		} else {
			fwts_failed(fw, LOG_LEVEL_CRITICAL, "HR_010",
				    "%s %u: missing mandatory extension '%s'.", hart_brs_owner(chk),
				    chk->id, *ext);
			isa_failed++;
		}
	}

	if (chk->failed + isa_failed == 0) {
		if (chk->passed_msg)
			fwts_passed(fw, "HR_010: %s %u: %s", hart_brs_owner(chk), chk->id,
				    chk->passed_msg);
		if (chk->warning_msg)
			fwts_warning(fw, "HR_010: %s %u: %s", hart_brs_owner(chk), chk->id,
				     chk->warning_msg);
	} else if (chk->failed_msg) {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "HR_010", "%s %u: %s", hart_brs_owner(chk),
			    chk->id, chk->failed_msg);
	}
}

static sigjmp_buf jmpbuf;

static void hart_brs_sigill_handler(int sig)
{
	FWTS_UNUSED(sig);
	siglongjmp(jmpbuf, 1);
}

static int hart_brs_probe_fence_tso(struct hart_brs_chk_priv *chk)
{
	bool supported;
	fwts_framework *fw = chk->fw;
	struct sigaction sa, old_sa;

	sa.sa_handler = hart_brs_sigill_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGILL, &sa, &old_sa) < 0) {
		fwts_log_error(fw, "HR_010: failed to install SIGILL handler.");
		return -1;
	}

	supported = true;
	if (sigsetjmp(jmpbuf, 1) == 0) {
		asm volatile(".word 0x8330000f" ::: "memory");
	} else {
		supported = false;
	}

	if (sigaction(SIGILL, &old_sa, NULL) < 0) {
		fwts_log_error(fw, "HR_010: failed to restore SIGILL handler.");
		return -1;
	}

	if (supported)
		fwts_passed(fw, "HR_010: fence.tso instruction is supported.");
	else {
		chk->failed++;
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "HR_010",
			    "fence.tso instruction is not supported.");
	}

	return 0;
}

static uint32_t hart_brs_read_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint64_t hart_brs_read_le64(const unsigned char *p)
{
	return (uint64_t)hart_brs_read_le32(p) | ((uint64_t)hart_brs_read_le32(p + 4) << 32);
}

static int hart_brs_find_boot_image_in_fstab(fwts_framework *fw, const char *boot_image,
					     char **result)
{
	char *candidate;
	char *line = NULL;
	char *mountdir;
	char *p;
	char *saveptr = NULL;
	char *source;
	int ret = 0;
	size_t cap = 0;
	size_t need;
	struct stat st;

	*result = NULL;

	FILE *fp = fopen("/etc/fstab", "r");
	if (!fp) {
		fwts_log_error(fw, "HR_010: failed to open /etc/fstab.");
		return -1;
	}

	while (getline(&line, &cap, fp) >= 0) {
		source = strtok_r(line, " \t\r\n", &saveptr);
		if (source == NULL || source[0] == '#')
			continue;

		mountdir = strtok_r(NULL, " \t\r\n", &saveptr);
		if (mountdir == NULL)
			continue;

		need = strlen(mountdir) + strlen(boot_image) + 1;
		candidate = malloc(need);
		if (!candidate) {
			fwts_log_error(
				fw, "HR_010: out of memory while searching for the kernel image.");
			ret = -1;
			break;
		}
		snprintf(candidate, need, "%s%s", mountdir, boot_image);

		while ((p = strstr(candidate, "//")) != NULL)
			memmove(p, p + 1, strlen(p + 1) + 1);

		if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
			*result = candidate;
			break;
		}
		free(candidate);
	}

	free(line);
	fclose(fp);
	return ret;
}

static int hart_brs_resolve_boot_image_path(fwts_framework *fw, char **result)
{
	char *line = NULL;
	char *saveptr = NULL;
	char *token;
	FILE *fp;
	int ret = 0;
	size_t cap = 0;

	*result = NULL;

	fp = fopen("/proc/cmdline", "r");
	if (!fp) {
		fwts_log_error(fw, "HR_010: failed to open /proc/cmdline.");
		return -1;
	}

	if (getline(&line, &cap, fp) >= 0) {
		token = strtok_r(line, " \t\n", &saveptr);
		while (token) {
			if (strncmp(token, "BOOT_IMAGE=", 11) == 0) {
				ret = hart_brs_find_boot_image_in_fstab(fw, token + 11, result);
				if (ret < 0 || *result)
					break;
			}
			token = strtok_r(NULL, " \t\n", &saveptr);
		}
	}

	free(line);
	fclose(fp);
	return ret;
}

static int hart_brs_detect_kernel_endian(fwts_framework *fw)
{
	char *path = NULL;
	FILE *fp;
	int ret = -1;
	size_t n;
	unsigned char h[64] = { 0 };

	if (hart_brs_resolve_boot_image_path(fw, &path) < 0)
		return ret;

	if (!path) {
		fwts_log_error(fw, "HR_010: failed to get the kernel image path.");
		return ret;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		fwts_log_error(fw, "HR_010: failed to open %s.", path);
		goto out;
	}

	n = fread(h, 1, sizeof(h), fp);
	fclose(fp);

	if (n != sizeof(h)) {
		fwts_log_error(fw, "HR_010: read %zu bytes from the kernel image, expected %zu.", n,
			       sizeof(h));
		goto out;
	}

	if (n >= EI_NIDENT && memcmp(h, ELFMAG, SELFMAG) == 0) {
		if (h[EI_DATA] == ELFDATA2LSB) {
			ret = 0;
			goto out;
		}
		if (h[EI_DATA] == ELFDATA2MSB) {
			ret = 1;
			goto out;
		}
	}

	if (h[0x38] == 'R' && h[0x39] == 'S' && h[0x3a] == 'C' && h[0x3b] == 0x05) {
		uint64_t flags = hart_brs_read_le64(h + 0x18);
		ret = flags & 1;
		goto out;
	}

	fwts_log_error(fw, "HR_010: unknown or invalid kernel image format: %s.", path);
out:
	free(path);
	return ret;
}

static int hart_brs_probe_endian(struct hart_brs_chk_priv *chk)
{
	fwts_framework *fw = chk->fw;
	int r;
	volatile unsigned int x;

	x = 1;
	if (*(volatile unsigned char *)&x == 1) {
		fwts_passed(fw, "HR_010: U-mode is little-endian.");
	} else {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "HR_010", "U-mode is not little-endian.");
		chk->failed++;
	}

	r = hart_brs_detect_kernel_endian(fw);
	if (r < 0)
		return -1;
	else if (r == 0) {
		fwts_passed(fw, "HR_010: S-mode is little-endian.");
	} else {
		fwts_failed(fw, LOG_LEVEL_CRITICAL, "HR_010", "S-mode is not little-endian.");
		chk->failed++;
	}

	return 0;
}

static int hart_brs_dt_get_mmu_type(fwts_framework *fw, const void *fdt, int cpu_node,
				    enum acpi_rhct_mmu_type *mmu_type)
{
	const char *p;
	int len;

	FWTS_UNUSED(fw);

	p = fdt_getprop(fdt, cpu_node, "mmu-type", &len);
	if (p == NULL || len == 0)
		return 0;

	*mmu_type = -1;
	if (!strcasecmp(p, "riscv,sv39")) {
		*mmu_type = ACPI_RHCT_MMU_TYPE_SV39;
	} else if (!strcasecmp(p, "riscv,sv48")) {
		*mmu_type = ACPI_RHCT_MMU_TYPE_SV48;
	} else if (!strcasecmp(p, "riscv,sv57")) {
		*mmu_type = ACPI_RHCT_MMU_TYPE_SV57;
	}

	return 0;
}

static int hart_brs_normalize_isa(fwts_framework *fw, const void *fdt, int cpu_node, char **out)
{
	const char *base = NULL;
	const char *exts = NULL;
	const char *implies_isa = "_zicntr_zicsr_zifencei_zihpm";
	const char *isa = NULL;
	const char *p;
	char *multi = NULL;
	char single[32] = { 0 };
	int base_len;
	int elen;
	int exts_len;
	int isa_len;
	int remaining;
	size_t multi_pos = 0;
	size_t need;
	size_t single_pos = 0;

	base = fdt_getprop(fdt, cpu_node, "riscv,isa-base", &base_len);
	exts = fdt_getprop(fdt, cpu_node, "riscv,isa-extensions", &exts_len);

	if (base && base_len > 0 && exts && exts_len > 0) {
		multi = malloc(exts_len + 1);
		if (!multi) {
			fwts_log_error(fw,
				       "HR_010: out of memory while normalizing the ISA string.");
			return -1;
		}

		p = exts;
		remaining = exts_len;

		while (remaining > 0 && *p) {
			elen = strnlen(p, remaining);
			if (elen == 0)
				break;

			if (elen == 1) {
				if (*p != 'i')
					single[single_pos++] = *p;
			} else {
				multi[multi_pos++] = '_';
				memcpy(multi + multi_pos, p, elen);
				multi_pos += elen;
			}

			p += elen + 1;
			remaining -= elen + 1;
		}

		single[single_pos] = '\0';
		multi[multi_pos] = '\0';

		need = base_len + single_pos + multi_pos + 1;
		*out = malloc(need);
		if (*out == NULL) {
			fwts_log_error(fw,
				       "HR_010: out of memory while normalizing the ISA string.");
			free(multi);
			return -1;
		}
		sprintf(*out, "%s%s%s", base, single, multi);
		free(multi);
		return 0;
	}

	isa = fdt_getprop(fdt, cpu_node, "riscv,isa", &isa_len);
	if (!isa || isa_len <= 0) {
		fwts_log_error(fw, "HR_010: CPU node lacks a valid ISA property.");
		return -1;
	}

	need = isa_len + strlen(implies_isa) + 1;
	*out = malloc(need);
	if (*out == NULL) {
		fwts_log_error(fw, "HR_010: out of memory while normalizing the ISA string.");
		return -1;
	}
	sprintf(*out, "%s%s", isa, implies_isa);
	return 0;
}

static int hart_brs_dt_get_hartid(const void *fdt, int cpu_node, uint32_t *hartid)
{
	const uint32_t *prop;
	const uint32_t *reg;
	int cells = 1;
	int len;
	int parent;
	uint64_t id;

	parent = fdt_parent_offset(fdt, cpu_node);
	if (parent < 0)
		return -1;

	prop = fdt_getprop(fdt, parent, "#address-cells", &len);
	if (prop && len >= (int)sizeof(uint32_t))
		cells = fdt32_to_cpu(*prop);

	reg = fdt_getprop(fdt, cpu_node, "reg", &len);
	if (!reg || len < (int)(cells * sizeof(uint32_t)))
		return -2;

	if (cells == 1) {
		id = fdt32_to_cpu(reg[0]);
	} else if (cells == 2) {
		id = ((uint64_t)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1]);
	} else {
		return -3;
	}

	if (id > UINT32_MAX)
		return -4;

	*hartid = (uint32_t)id;
	return 0;
}

static int hart_brs_dt_for_each_isa(struct hart_brs_chk_priv *chk)
{
	const char *prop;
	const void *fdt = chk->fw->fdt;
	char *isa;
	fwts_framework *fw = chk->fw;
	int count = 0;
	int offset;

	for (offset = fdt_next_node(fdt, -1, NULL); offset >= 0;
	     offset = fdt_next_node(fdt, offset, NULL)) {
		prop = fdt_getprop(fdt, offset, "device_type", NULL);
		if (prop && strcmp(prop, "cpu") != 0)
			continue;

		prop = fdt_getprop(fdt, offset, "status", NULL);
		if (prop && strcmp(prop, "okay") != 0)
			continue;

		if (hart_brs_normalize_isa(fw, fdt, offset, &isa) == 0) {
			if (hart_brs_dt_get_hartid(fdt, offset, &chk->id) != 0) {
				fwts_log_error(fw,
					       "HR_010: failed to get hart ID from DeviceTree.");
				return -1;
			}
			if (hart_brs_dt_get_mmu_type(fw, fdt, offset, &chk->mmu_type) < 0)
				return -1;
			chk->isa = isa;
			hart_brs_isa_check(chk);
			free(isa);
			count++;
		}
	}

	return count;
}

static int hart_brs_acpi_collect_enabled_harts(fwts_framework *fw, fwts_list *enabled)
{
	fwts_acpi_madt_rintc *rintc;
	fwts_acpi_madt_sub_table_header *hdr;
	fwts_acpi_table_info *table = NULL;
	ssize_t len;
	uint32_t *uid;
	uint8_t *data;

	if (fwts_acpi_find_table(fw, "APIC", 0, &table) != FWTS_OK || !table ||
	    table->length < sizeof(fwts_acpi_table_madt)) {
		fwts_log_error(fw, "HR_010: no valid MADT table found.");
		return -1;
	}

	data = (uint8_t *)table->data + sizeof(fwts_acpi_table_madt);
	len = table->length - sizeof(fwts_acpi_table_madt);

	while (len > (ssize_t)sizeof(fwts_acpi_madt_sub_table_header)) {
		hdr = (fwts_acpi_madt_sub_table_header *)data;

		if (hdr->type != FWTS_MADT_RINTC)
			goto next;

		if (hdr->length <
		    sizeof(fwts_acpi_madt_sub_table_header) + sizeof(fwts_acpi_madt_rintc)) {
			fwts_log_error(fw, "HR_010: MADT contains an invalid RINTC subtable.");
			return -1;
		}
		rintc = (fwts_acpi_madt_rintc *)(data + sizeof(fwts_acpi_madt_sub_table_header));

		if (rintc->flags & ACPI_MADT_ENABLED) {
			uid = malloc(sizeof(uint32_t));
			if (!uid) {
				fwts_log_error(fw, "HR_010: out of memory while collecting MADT "
						   "RINTC entries.");
				return -1;
			}
			*uid = rintc->uid;
			fwts_list_append(enabled, uid);
		}
	next:
		data += hdr->length;
		len -= hdr->length;
	}

	return 0;
}

static int hart_brs_acpi_rhct_find_isa(fwts_framework *fw, const void *base, size_t len,
				       const uint32_t *offsets, uint16_t num, const char **isa)
{
	const fwts_acpi_rhct_node_header *hdr;
	const fwts_acpi_rhct_node_isa_string *isa_node;
	uint16_t i;
	uint32_t off;

	for (i = 0; i < num; i++) {
		off = offsets[i];

		if (off + sizeof(fwts_acpi_rhct_node_header) > len) {
			fwts_log_error(fw, "HR_010: RHCT contains an invalid node offset.");
			return -1;
		}

		hdr = (const fwts_acpi_rhct_node_header *)(base + off);

		if (off + hdr->length > len) {
			fwts_log_error(fw, "HR_010: RHCT contains an invalid node offset.");
			return -1;
		}

		if (hdr->type != FWTS_RHCT_NODE_ISA_STRING)
			continue;

		if (hdr->length <
		    sizeof(fwts_acpi_rhct_node_header) + sizeof(fwts_acpi_rhct_node_isa_string)) {
			fwts_log_error(fw, "HR_010: RHCT contains an invalid ISA string node.");
			return -1;
		}

		isa_node = (const fwts_acpi_rhct_node_isa_string
				    *)((const void *)hdr + sizeof(fwts_acpi_rhct_node_header));

		*isa = isa_node->isa;
		return 0;
	}
	*isa = NULL;
	fwts_log_error(fw, "HR_010: RHCT hart info has no ISA string node.");
	return 1;
}

static int hart_brs_acpi_get_mmu_type(fwts_framework *fw, const void *base, size_t len,
				      const uint32_t *offsets, uint16_t num,
				      enum acpi_rhct_mmu_type *mmu_type)
{
	const fwts_acpi_rhct_node_header *hdr;
	const fwts_acpi_rhct_node_mmu *mmu_node;
	uint16_t i;
	uint32_t off;

	*mmu_type = -1;
	for (i = 0; i < num; i++) {
		off = offsets[i];

		if (off + sizeof(fwts_acpi_rhct_node_header) > len) {
			fwts_log_error(fw, "HR_010: RHCT contains an invalid node offset.");
			return -1;
		}

		hdr = (const fwts_acpi_rhct_node_header *)(base + off);

		if (off + hdr->length > len) {
			fwts_log_error(fw, "HR_010: RHCT contains an invalid node offset.");
			return -1;
		}

		if (hdr->type == FWTS_RHCT_NODE_MMU) {
			if (hdr->length <
			    sizeof(fwts_acpi_rhct_node_header) + sizeof(fwts_acpi_rhct_node_mmu)) {
				fwts_log_error(fw, "HR_010: RHCT contains an invalid MMU node.");
				return -1;
			}

			mmu_node =
				(const fwts_acpi_rhct_node_mmu
					 *)((const void *)hdr + sizeof(fwts_acpi_rhct_node_header));

			*mmu_type = mmu_node->mmu_type;
			return 0;
		}
	}
	return 0;
}

static int hart_brs_acpi_iterate_rhct(struct hart_brs_chk_priv *chk, fwts_list *enabled)
{
	const fwts_acpi_rhct_node_hart_info *hart;
	const fwts_acpi_rhct_node_header *hdr;
	const fwts_acpi_table_rhct *rhct;
	const uint32_t *offsets;
	const void *data;
	bool found;
	fwts_acpi_table_info *table = NULL;
	fwts_framework *fw = chk->fw;
	fwts_list_link *item;
	int count = 0;
	ssize_t length;

	if (fwts_acpi_find_table(fw, "RHCT", 0, &table) != FWTS_OK || !table ||
	    table->length < sizeof(fwts_acpi_table_rhct)) {
		fwts_log_error(fw, "HR_010: no valid RHCT table found.");
		return -1;
	}

	rhct = (const fwts_acpi_table_rhct *)table->data;
	data = (const void *)rhct + rhct->node_offset;
	length = table->length - rhct->node_offset;

	while (length > (ssize_t)sizeof(fwts_acpi_rhct_node_header)) {
		hdr = (const fwts_acpi_rhct_node_header *)data;

		if (hdr->type == FWTS_RHCT_NODE_HART_INFO) {
			hart = (const fwts_acpi_rhct_node_hart_info
					*)(data + sizeof(fwts_acpi_rhct_node_header));

			found = false;
			fwts_list_foreach(item, enabled) {
				if (*(uint32_t *)item->data == hart->uid) {
					found = true;
					break;
				}
			}

			if (found) {
				offsets = (const uint32_t *)((const void *)hart +
							     sizeof(fwts_acpi_rhct_node_hart_info));
				chk->id = hart->uid;
				if (hart_brs_acpi_rhct_find_isa(fw, rhct, table->length, offsets,
								hart->num_offsets, &chk->isa) < 0)
					return -1;
				if (hart_brs_acpi_get_mmu_type(fw, rhct, table->length, offsets,
							       hart->num_offsets,
							       &chk->mmu_type) < 0)
					return -1;
				hart_brs_isa_check(chk);
				count++;
			}
		}

		data += hdr->length;
		length -= hdr->length;
	}

	return count;
}

static int hart_brs_acpi_for_each_isa(struct hart_brs_chk_priv *chk)
{
	fwts_framework *fw = chk->fw;
	fwts_list enabled = FWTS_LIST_INIT;
	int count;

	if (hart_brs_acpi_collect_enabled_harts(fw, &enabled) < 0) {
		fwts_list_free_items(&enabled, free);
		return -1;
	}

	count = hart_brs_acpi_iterate_rhct(chk, &enabled);

	fwts_list_free_items(&enabled, free);
	return count;
}

static int hart_brs_test1(fwts_framework *fw)
{
	struct hart_brs_chk_priv chk = {
		.fw = fw,
		.is_acpi = false,
		.failed = 0,
		.id = 0,
		.isa = NULL,
		.required = rva20s64_required,
		.passed_msg = NULL,
		.warning_msg = rva20s64_warning_msg,
		.failed_msg = rva20s64_failed_msg,
		.mmu_type = -1
	};

	if (hart_brs_probe_fence_tso(&chk) < 0)
		return FWTS_OK;

	if (hart_brs_probe_endian(&chk) < 0)
		return FWTS_OK;

	if (fwts_firmware_has_features(FWTS_FW_FEATURE_ACPI)) {
		chk.is_acpi = true;
		hart_brs_acpi_for_each_isa(&chk);
		return FWTS_OK;
	}
	if (fwts_firmware_has_features(FWTS_FW_FEATURE_DEVICETREE)) {
		chk.is_acpi = false;
		hart_brs_dt_for_each_isa(&chk);
		return FWTS_OK;
	}

	fwts_failed(
		fw, LOG_LEVEL_CRITICAL, "HR_010",
		"Neither ACPI nor Device Tree is available to enumerate harts and check the ISA.");

	return FWTS_OK;
}

static fwts_framework_minor_test hart_brs_tests[] = {
	{ hart_brs_test1, "HR_010: application processor harts MUST be compliant to RVA20S64." },
	{ NULL, NULL }
};

static fwts_framework_ops hart_brs_ops = {
	.description = "RISC-V BRS Hart Requirements test.",
	.minor_tests = hart_brs_tests
};

FWTS_REGISTER("hart_brs", &hart_brs_ops, FWTS_TEST_ANYTIME, FWTS_FLAG_BRSI | FWTS_FLAG_BRSB)
#endif
