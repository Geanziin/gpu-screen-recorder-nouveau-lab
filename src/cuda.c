#include "../include/cuda.h"
#include "../include/log.h"
#include "../include/library_loader.h"
#include "../include/utils.h"
#include <string.h>
#include <dlfcn.h>

static bool gsr_cuda_get_device_by_card_path(gsr_cuda *self, const char *card_path, CUdevice *cu_dev) {
    if(!card_path || card_path[0] == '\0')
        return false;

    char pci_bus_id[GSR_PCI_BUS_ID_MAX_SIZE];
    if(!gsr_card_path_get_pci_bus_id(card_path, pci_bus_id)) {
        gsr_log(GSR_LOG_LEVEL_WARNING, "gsr_cuda_load: failed to get the pci bus id of %s", card_path);
        return false;
    }

    const CUresult res = self->cuDeviceGetByPCIBusId(cu_dev, pci_bus_id);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        self->cuGetErrorString(res, &err_str);
        gsr_log(GSR_LOG_LEVEL_WARNING, "gsr_cuda_load: failed to get the cuda device of %s (%s), error: %s (result: %d)", card_path, pci_bus_id, err_str, res);
        return false;
    }

    return true;
}

bool gsr_cuda_load(gsr_cuda *self, const char *card_path) {
    memset(self, 0, sizeof(gsr_cuda));

    dlerror(); /* clear */
    void *lib = dlopen("libcuda.so.1", RTLD_LAZY);
    if(!lib) {
        lib = dlopen("libcuda.so", RTLD_LAZY);
        if(!lib) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_cuda_load failed: failed to load libcuda.so/libcuda.so.1, error: %s", dlerror());
            return false;
        }
    }

    const dlsym_assign required_dlsym[] = {
        { (void**)&self->cuInit, "cuInit" },
        { (void**)&self->cuDeviceGetCount, "cuDeviceGetCount" },
        { (void**)&self->cuDeviceGet, "cuDeviceGet" },
        { (void**)&self->cuDeviceGetByPCIBusId, "cuDeviceGetByPCIBusId" },
        { (void**)&self->cuCtxCreate_v2, "cuCtxCreate_v2" },
        { (void**)&self->cuCtxDestroy_v2, "cuCtxDestroy_v2" },
        { (void**)&self->cuCtxPushCurrent_v2, "cuCtxPushCurrent_v2" },
        { (void**)&self->cuCtxPopCurrent_v2, "cuCtxPopCurrent_v2" },
        { (void**)&self->cuGetErrorString, "cuGetErrorString" },
        { (void**)&self->cuMemcpy2D_v2, "cuMemcpy2D_v2" },
        { (void**)&self->cuMemcpy2DAsync_v2, "cuMemcpy2DAsync_v2" },
        { (void**)&self->cuStreamSynchronize, "cuStreamSynchronize" },

        { (void**)&self->cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage" },
        { (void**)&self->cuGraphicsEGLRegisterImage, "cuGraphicsEGLRegisterImage" },
        { (void**)&self->cuGraphicsResourceSetMapFlags, "cuGraphicsResourceSetMapFlags" },
        { (void**)&self->cuGraphicsMapResources, "cuGraphicsMapResources" },
        { (void**)&self->cuGraphicsUnmapResources, "cuGraphicsUnmapResources" },
        { (void**)&self->cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource" },
        { (void**)&self->cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray" },

        { NULL, NULL }
    };

    CUresult res;

    if(!dlsym_load_list(lib, required_dlsym)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_cuda_load failed: missing required symbols in libcuda.so/libcuda.so.1");
        goto fail;
    }

    res = self->cuInit(0);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        self->cuGetErrorString(res, &err_str);
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_cuda_load failed: cuInit failed, error: %s (result: %d)", err_str, res);
        goto fail;
    }

    int nGpu = 0;
    self->cuDeviceGetCount(&nGpu);
    if(nGpu <= 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_cuda_load failed: no cuda supported devices found");
        goto fail;
    }

    CUdevice cu_dev;
    if(!gsr_cuda_get_device_by_card_path(self, card_path, &cu_dev)) {
        res = self->cuDeviceGet(&cu_dev, 0);
        if(res != CUDA_SUCCESS) {
            const char *err_str = "unknown";
            self->cuGetErrorString(res, &err_str);
            gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_cuda_load failed: unable to get CUDA device, error: %s (result: %d)", err_str, res);
            goto fail;
        }
    }

    res = self->cuCtxCreate_v2(&self->cu_ctx, CU_CTX_SCHED_AUTO, cu_dev);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        self->cuGetErrorString(res, &err_str);
        gsr_log(GSR_LOG_LEVEL_ERROR, "gsr_cuda_load failed: unable to create CUDA context, error: %s (result: %d)", err_str, res);
        goto fail;
    }

    self->library = lib;
    return true;

    fail:
    dlclose(lib);
    memset(self, 0, sizeof(gsr_cuda));
    return false;
}

void gsr_cuda_unload(gsr_cuda *self) {
    if(self->library) {
        if(self->cu_ctx) {
            self->cuCtxDestroy_v2(self->cu_ctx);
            self->cu_ctx = 0;
        }
        dlclose(self->library);
    }

    memset(self, 0, sizeof(gsr_cuda));
}
