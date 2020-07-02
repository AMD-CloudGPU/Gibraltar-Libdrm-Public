#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "amdgpu.h"
#include "amdgpu_drm.h"
#include "amdgpu_waves.h"

#define mmSQ_IND_INDEX 0x8de0
#define mmSQ_IND_DATA 0x8de4

#define AMDGPU_MAX_SE 4
#define AMDGPU_SH_PER_SE 1
#define AMDGPU_CU_PER_SH 16

#define AMDGPU_WAVE_STATUS_INDEX 1
#define AMDGPU_WAVE_PC_LOW_INDEX 2
#define AMDGPU_WAVE_PC_HI_INDEX 3
#define AMDGPU_WAVE_EXEC_LOW_INDEX 4
#define AMDGPU_WAVE_EXEC_HI_INDEX 5
#define AMDGPU_WAVE_HW_ID_INDEX 6
#define AMDGPU_WAVE_GPR_ALLOC_INDEX 8
#define AMDGPU_WAVE_LDS_ALLOC_INDEX 9
#define AMDGPU_WAVE_TRAPSTS_INDEX 10
#define AMDGPU_WAVE_IB_STS_INDEX 11

#define AMDGPU_WAVE_STATUS_VALID_MASK (1 << 16)

#define AMDGPU_WAVE_HW_ID_WAVE_ID_SHIFT 0
#define AMDGPU_WAVE_HW_ID_SIMD_ID_SHIFT 4
#define AMDGPU_WAVE_HW_ID_PIPE_ID_SHIFT 6
#define AMDGPU_WAVE_HW_ID_CU_ID_SHIFT 8
#define AMDGPU_WAVE_HW_ID_SH_ID_SHIFT 12
#define AMDGPU_WAVE_HW_ID_SE_ID_SHIFT 13
#define AMDGPU_WAVE_HW_ID_WAVE_ID_MASK 0xF
#define AMDGPU_WAVE_HW_ID_SIMD_ID_MASK 0x3
#define AMDGPU_WAVE_HW_ID_PIPE_ID_MASK 0x3
#define AMDGPU_WAVE_HW_ID_CU_ID_MASK 0xF
#define AMDGPU_WAVE_HW_ID_SH_ID_MASK 0x1
#define AMDGPU_WAVE_HW_ID_SE_ID_MASK 0x3

#define AMDGPU_WAVE_GPR_ALLOC_VGPR_SIZE_SHIFT 8
#define AMDGPU_WAVE_GPR_ALLOC_SGPR_SIZE_SHIFT 24
#define AMDGPU_WAVE_GPR_ALLOC_VGPR_SIZE_MASK 0x3F
#define AMDGPU_WAVE_GPR_ALLOC_SGPR_SIZE_MASK 0xF

#define AMDGPU_FAMILY_AI_DEV_ID_MASK 0x686
#define AMDGPU_FAMILY_MATCH(x, y) (((x) >> 4) == (y))

#define AMDGPU_MMIO_SE_OR_ME_SHIFT 24
#define AMDGPU_MMIO_SH_OR_PIPE_SHIFT 34
#define AMDGPU_MMIO_CU_OR_QUEUE_SHIFT 44
#define AMDGPU_MMIO_USE_RING 61 /* ME_PIPE=1 */
#define AMDGPU_MMIO_USE_BANK 62 /* SE_SH_CU=1 */

#define AMDGPU_GPR_BANK_SEL_SHIFT 60 /* VGPR=0, SGPR=1 */
#define AMDGPU_GPR_THREAD_SEL_SHIFT 52
#define AMDGPU_GPR_SIMD_SEL_SHIFT 44
#define AMDGPU_GPR_WAVE_ID_SEL_SHIFT 36
#define AMDGPU_GPR_CU_SEL_SHIFT 28
#define AMDGPU_GPR_SH_SEL_SHIFT 20
#define AMDGPU_GPR_SE_SEL_SHIFT 12

static inline uint32_t amdgpu_read_bits(uint32_t value, int shift,
					uint32_t mask)
{
	return ((value >> shift) & mask);
}

static int amdgpu_open_debugfs(const char *fmt, uint32_t instance)
{
	int fd;
	char filepath[128] = { 0 };

	if (snprintf(filepath, 128, fmt, instance) <= 0) {
		printf("Failed to prepare debugfs path: %s.\n",
		       strerror(errno));
		return -1;
	}

	fd = open(filepath, O_RDWR);
	if (fd <= 0) {
		printf("Failed to open %s: %s.\n", filepath, strerror(errno));
		return -1;
	}

	return fd;
}

static int amdgpu_read_regu32(struct amdgpu_waves_handle *wh,
			      const uint64_t addr, const uint32_t *value)
{
	int ret;
	ssize_t sz;
	off_t offset;

	offset = addr;
	offset = lseek(wh->fd.mmio_reg, offset, SEEK_SET);
	if (offset < 0)
		return -1;

	sz = read(wh->fd.mmio_reg, value, sizeof(uint32_t));
	if (sz < 0) {
		return -1;
	}

	if (sz != sizeof(uint32_t)) {
		return -1;
	}

	return 0;
}

static int amdgpu_write_regu32(struct amdgpu_waves_handle *wh,
			       const uint64_t addr, const uint32_t value)
{
	int ret;
	ssize_t sz;
	off_t offset;

	offset = addr;
	offset = lseek(wh->fd.mmio_reg, offset, SEEK_SET);
	if (offset < 0)
		return -1;

	sz = write(wh->fd.mmio_reg, &value, sizeof(uint32_t));
	if (sz < 0) {
		return -1;
	}

	if (sz != sizeof(uint32_t)) {
		return -1;
	}

	return 0;
}

static int amdgpu_is_cu_active(struct amdgpu_waves_handle *wh, uint32_t se,
			       uint32_t sh, uint32_t cu)
{
	int ret;
	uint64_t addr_msbs;
	uint32_t value = 0;

	addr_msbs = ((uint64_t)cu << AMDGPU_MMIO_CU_OR_QUEUE_SHIFT) |
		    ((uint64_t)sh << AMDGPU_MMIO_SH_OR_PIPE_SHIFT) |
		    ((uint64_t)se << AMDGPU_MMIO_SE_OR_ME_SHIFT) |
		    ((uint64_t)1 << AMDGPU_MMIO_USE_BANK);

	ret = amdgpu_write_regu32(wh, addr_msbs | mmSQ_IND_INDEX, (1 << 19));
	if (ret)
		return ret;

	ret = amdgpu_read_regu32(wh, addr_msbs | mmSQ_IND_DATA, &value);
	if (ret)
		return ret;

	if (value == 0xbebebeef)
		return -EINVAL;

	if (value & 1)
		return 0;

	return -EBADF;
}

static int amdgpu_waves_check_device(uint32_t instance)
{
	FILE *file;
	char filepath[256] = { 0 };
	char name[64] = { 0 };
	char *dev;
	uint32_t dev_id;

	if (snprintf(filepath, 256, "/sys/kernel/debug/dri/%d/name",
		     instance) <= 0) {
		printf("Failed to prepare debugfs path.\n");
		return -1;
	}

	file = fopen(filepath, "r");
	if (!file) {
		printf("Failed to open: %s: %s.\n", filepath, strerror(errno));
		return -1;
	}

	if (fscanf(file, "%*s %s", name) != 1) {
		printf("Failed to read device name\n");
		fclose(file);
		return -1;
	}

	fclose(file);
	dev = strstr(name, "dev=");
	if (!dev)
		return -1;

	/* skip 'dev=' */
	dev += 4;
	if (snprintf(filepath, 256, "/sys/bus/pci/devices/%s/device", dev) <=
	    0) {
		printf("Failed to prepare pci device path.\n");
		return -1;
	}

	file = fopen(filepath, "r");
	if (!file) {
		printf("Failed to open: %s: %s.\n", filepath, strerror(errno));
		return -1;
	}

	if (fscanf(file, "%x", &dev_id) != 1) {
		printf("Failed to read device name\n");
		fclose(file);
		return -1;
	}

	fclose(file);
	if (AMDGPU_FAMILY_MATCH(dev_id, AMDGPU_FAMILY_AI_DEV_ID_MASK))
		return 0;

	return -1;
}

int amdgpu_waves_create(struct amdgpu_waves_handle *w_handle)
{
	uint32_t instance = 0;

	if (!w_handle)
		return -1;

	if (amdgpu_waves_check_device(instance) < 0)
		return -1;

	w_handle->fd.mmio_reg = amdgpu_open_debugfs(
		"/sys/kernel/debug/dri/%d/amdgpu_regs", instance);
	if (w_handle->fd.mmio_reg <= 0)
		return w_handle->fd.mmio_reg;

	w_handle->fd.waves = amdgpu_open_debugfs(
		"/sys/kernel/debug/dri/%d/amdgpu_wave", instance);
	if (w_handle->fd.waves <= 0) {
		close(w_handle->fd.mmio_reg);
		return w_handle->fd.waves;
	}

	w_handle->fd.gpr = amdgpu_open_debugfs(
		"/sys/kernel/debug/dri/%d/amdgpu_gpr", instance);
	if (w_handle->fd.gpr <= 0) {
		close(w_handle->fd.mmio_reg);
		close(w_handle->fd.waves);
		return w_handle->fd.gpr;
	}

	return 0;
}

static int amdgpu_print_vgprs(struct amdgpu_waves_handle *w_handle,
			      uint32_t se_id, uint32_t sh_id, uint32_t cu_id,
			      uint32_t simd_id, uint32_t wave_id,
			      uint32_t thread, uint32_t vgpr_count,
			      uint64_t exec)
{
	int ret;
	uint32_t i;
	uint32_t j;
	ssize_t sz;
	uint32_t buffer[256 * 16] = { 0 };
	off_t addr_msbs;

	addr_msbs = ((uint64_t)se_id << AMDGPU_GPR_SE_SEL_SHIFT) |
		    ((uint64_t)sh_id << AMDGPU_GPR_SH_SEL_SHIFT) |
		    ((uint64_t)cu_id << AMDGPU_GPR_CU_SEL_SHIFT) |
		    ((uint64_t)wave_id << AMDGPU_GPR_WAVE_ID_SEL_SHIFT) |
		    ((uint64_t)simd_id << AMDGPU_GPR_SIMD_SEL_SHIFT) |
		    ((uint64_t)thread << AMDGPU_GPR_THREAD_SEL_SHIFT) |
		    ((uint64_t)0 << AMDGPU_GPR_BANK_SEL_SHIFT);

	addr_msbs = lseek(w_handle->fd.gpr, addr_msbs, SEEK_SET);
	if (addr_msbs < 0)
		return -1;

	sz = read(w_handle->fd.gpr, &buffer, sizeof(uint32_t) * vgpr_count);
	if (sz < 0)
		return -1;

	printf("Thread[%02x]: %s\n", thread,
	       (exec & (1 << thread)) ? "Executing" : "Not Executing");
	for (i = 0; i < vgpr_count;) {
		if (i == 0) {
			printf("         ");
			for (j = 0; j < 16 && (i + j) < vgpr_count; ++j) {
				printf(" %08x", j);
			}
			printf("\n");
		}
		printf("VGPR %3x:", i);
		for (j = 0; j < 16 && (i + j) < vgpr_count; ++j) {
			printf(" %08x", buffer[i + j]);
		}

		i += j;
		printf("\n");
	}

	printf("\n");

	return 0;
}

static int amdgpu_print_sgprs(struct amdgpu_waves_handle *w_handle,
			      uint32_t se_id, uint32_t sh_id, uint32_t cu_id,
			      uint32_t simd_id, uint32_t wave_id,
			      uint32_t sgpr_count)
{
	int ret;
	uint32_t i;
	uint32_t j;
	ssize_t sz;
	uint32_t buffer[256 * 4] = { 0 };
	off_t addr_msbs;

	addr_msbs = ((uint64_t)se_id << AMDGPU_GPR_SE_SEL_SHIFT) |
		    ((uint64_t)sh_id << AMDGPU_GPR_SH_SEL_SHIFT) |
		    ((uint64_t)cu_id << AMDGPU_GPR_CU_SEL_SHIFT) |
		    ((uint64_t)wave_id << AMDGPU_GPR_WAVE_ID_SEL_SHIFT) |
		    ((uint64_t)simd_id << AMDGPU_GPR_SIMD_SEL_SHIFT) |
		    ((uint64_t)1 << AMDGPU_GPR_BANK_SEL_SHIFT);

	addr_msbs = lseek(w_handle->fd.gpr, addr_msbs, SEEK_SET);
	if (addr_msbs < 0)
		return -1;

	sz = read(w_handle->fd.gpr, &buffer, sizeof(uint32_t) * sgpr_count);
	if (sz < 0)
		return -1;

	for (i = 0; i < sgpr_count;) {
		if (i == 0) {
			printf("\n         ");
			for (j = 0; j < 16 && (i + j) < sgpr_count; ++j) {
				printf(" %08x", j);
			}
			printf("\n");
		}
		printf("SGPR %3x:", i);
		for (j = 0; j < 16 && (i + j) < sgpr_count; ++j) {
			printf(" %08x", buffer[i + j]);
		}

		i += j;
		printf("\n");
	}

	printf("\n");

	return 0;
}

static int amdgpu_print_wavedata(struct amdgpu_waves_handle *w_handle,
				 uint32_t se, uint32_t sh, uint32_t cu,
				 uint32_t simd, uint32_t wave)
{
	int i;
	int ret;
	ssize_t sz;
	uint32_t buffer[32] = { 0 };
	off_t addr_msbs;
	uint32_t value = 0, se_id, sh_id, cu_id, wave_id, simd_id, sgpr_size,
		 vgpr_size;

	addr_msbs = ((uint64_t)se << 7) | ((uint64_t)sh << 15) |
		    ((uint64_t)cu << 23) | ((uint64_t)wave << 31) |
		    ((uint64_t)simd << 37);

	addr_msbs = lseek(w_handle->fd.waves, addr_msbs, SEEK_SET);
	if (addr_msbs < 0)
		return -1;

	sz = read(w_handle->fd.waves, &buffer, sizeof(uint32_t) * 32);
	if (sz < 0)
		return -1;

	if (buffer[0] != 1)
		return -1;

	if ((buffer[AMDGPU_WAVE_STATUS_INDEX] &
	     AMDGPU_WAVE_STATUS_VALID_MASK) == 0)
		return -1;

	printf("%2u %2u %2u %4u %4u    "
	       "%08x %08x %08x %08x "
	       "%08x   %08x  %08x  %08x"
	       "     %08x    %08x\n",
	       se, sh, cu, simd, wave, buffer[AMDGPU_WAVE_STATUS_INDEX],
	       buffer[AMDGPU_WAVE_PC_LOW_INDEX],
	       buffer[AMDGPU_WAVE_PC_HI_INDEX],
	       buffer[AMDGPU_WAVE_EXEC_LOW_INDEX],
	       buffer[AMDGPU_WAVE_EXEC_HI_INDEX],
	       buffer[AMDGPU_WAVE_HW_ID_INDEX],
	       buffer[AMDGPU_WAVE_GPR_ALLOC_INDEX],
	       buffer[AMDGPU_WAVE_LDS_ALLOC_INDEX],
	       buffer[AMDGPU_WAVE_TRAPSTS_INDEX],
	       buffer[AMDGPU_WAVE_IB_STS_INDEX]);

	se_id = amdgpu_read_bits(buffer[AMDGPU_WAVE_HW_ID_INDEX],
				 AMDGPU_WAVE_HW_ID_SE_ID_SHIFT,
				 AMDGPU_WAVE_HW_ID_SE_ID_MASK);

	sh_id = amdgpu_read_bits(buffer[AMDGPU_WAVE_HW_ID_INDEX],
				 AMDGPU_WAVE_HW_ID_SH_ID_SHIFT,
				 AMDGPU_WAVE_HW_ID_SH_ID_MASK);

	cu_id = amdgpu_read_bits(buffer[AMDGPU_WAVE_HW_ID_INDEX],
				 AMDGPU_WAVE_HW_ID_CU_ID_SHIFT,
				 AMDGPU_WAVE_HW_ID_CU_ID_MASK);

	wave_id = amdgpu_read_bits(buffer[AMDGPU_WAVE_HW_ID_INDEX],
				   AMDGPU_WAVE_HW_ID_WAVE_ID_SHIFT,
				   AMDGPU_WAVE_HW_ID_WAVE_ID_MASK);

	simd_id = amdgpu_read_bits(buffer[AMDGPU_WAVE_HW_ID_INDEX],
				   AMDGPU_WAVE_HW_ID_SIMD_ID_SHIFT,
				   AMDGPU_WAVE_HW_ID_SIMD_ID_MASK);

	sgpr_size = amdgpu_read_bits(buffer[AMDGPU_WAVE_GPR_ALLOC_INDEX],
				     AMDGPU_WAVE_GPR_ALLOC_SGPR_SIZE_SHIFT,
				     AMDGPU_WAVE_GPR_ALLOC_SGPR_SIZE_MASK);

	vgpr_size = amdgpu_read_bits(buffer[AMDGPU_WAVE_GPR_ALLOC_INDEX],
				     AMDGPU_WAVE_GPR_ALLOC_VGPR_SIZE_SHIFT,
				     AMDGPU_WAVE_GPR_ALLOC_VGPR_SIZE_MASK);

	/* AI family SGPR allocation are in blocks of 16 */
	sgpr_size = (sgpr_size + 1) << 4;

	/* AI family VGPR allocation are in blocks of 16 */
	vgpr_size = (vgpr_size + 1) << 2;

	amdgpu_print_sgprs(w_handle, se_id, sh_id, cu_id, simd_id, wave_id,
			   sgpr_size);

	/* AI family has 64 threads */
	for (i = 0; i < 64; ++i) {
		amdgpu_print_vgprs(
			w_handle, se_id, sh_id, cu_id, simd_id, wave_id, i,
			vgpr_size,
			(((uint64_t)buffer[AMDGPU_WAVE_EXEC_HI_INDEX] << 32) |
			 buffer[AMDGPU_WAVE_EXEC_LOW_INDEX]));
	}

	return 0;
}

static int amdgpu_print_cu(struct amdgpu_waves_handle *w_handle, uint32_t se,
			   uint32_t sh, uint32_t cu)
{
	int ret;
	int active = 1;
	uint32_t simd, wave;

	for (simd = 0; simd < 4; ++simd) {
		for (wave = 0; wave < 10; ++wave) {
			ret = amdgpu_print_wavedata(w_handle, se, sh, cu, simd,
						    wave);
			if (!ret)
				active = 0;
		}
	}

	return active;
}

int amdgpu_waves_print(struct amdgpu_waves_handle *w_handle)
{
	int ret;
	int active = -1;
	uint32_t sh, se, cu;
	if (!w_handle || w_handle->fd.mmio_reg <= 0 || w_handle->fd.waves <= 0)
		return -EINVAL;

	for (se = 0; se < AMDGPU_MAX_SE; ++se) {
		for (sh = 0; sh < AMDGPU_SH_PER_SE; ++sh) {
			for (cu = 0; cu < AMDGPU_CU_PER_SH; ++cu) {
				ret = amdgpu_is_cu_active(w_handle, se, sh, cu);
				if (!ret) {
					active = 0;
					printf("SE SH CU SIMD WAVE WAVE_STATUS"
					       "   PC_LOW    PC_HI  EXEC_LO  EXEC_HI"
					       " WAVE_HW_ID GPR_ALLOC LDS_ALLOC"
					       " WAVE_TRAPSTS WAVE_IB_STS\n");
					ret = amdgpu_print_cu(w_handle, se, sh,
							      cu);
					if (ret)
						return ret;
				}
			}
		}
	}

	return active;
}

int amdgpu_waves_destroy(struct amdgpu_waves_handle *w_handle)
{
	if (!w_handle || w_handle->fd.mmio_reg <= 0 || w_handle->fd.waves <= 0)
		return -EINVAL;

	close(w_handle->fd.mmio_reg);
	close(w_handle->fd.waves);
	close(w_handle->fd.gpr);
	return 0;
}
