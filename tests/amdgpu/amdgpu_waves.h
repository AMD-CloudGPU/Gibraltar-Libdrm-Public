#ifndef AMDGPU_WAVES_H
#define AMDGPU_WAVES_H

struct amdgpu_waves_handle {
	struct {
		int mmio_reg;
		int waves;
		int gpr;
	} fd;
};

int amdgpu_waves_create(struct amdgpu_waves_handle *waves);
int amdgpu_waves_print(struct amdgpu_waves_handle *w_handle);
int amdgpu_waves_destroy(struct amdgpu_waves_handle *w_handle);

#endif