/* SPDX-License-Identifier: MIT
 * Trimmed NVKM device descriptor for NVIDIA GeForce MX250 (GP108 / nv138_chipset)
 * Headless 3D controller (0x030200): omits .disp, .gpio, .i2c, .therm.
 */
#include <engine/device/priv.h>
#include <engine/device/acpi.h>

#include <core/enum.h>
#include <core/option.h>
#include <subdev/bios.h>
#include <subdev/therm.h>

static DEFINE_MUTEX(nv_devices_mutex);
static LIST_HEAD(nv_devices);

/* Stubs for omitted optional subdevs (therm, clk, gsp) */
void nvkm_therm_clkgate_init(struct nvkm_therm *therm,
                             const struct nvkm_therm_clkgate_pack *p)
{
    (void)therm; (void)p;
}

void nvkm_therm_clkgate_enable(struct nvkm_therm *therm)
{
    (void)therm;
}

void nvkm_therm_clkgate_fini(struct nvkm_therm *therm, bool suspend)
{
    (void)therm; (void)suspend;
}

int nvkm_therm_temp_get(struct nvkm_therm *therm)
{
    (void)therm;
    return -ENODEV;
}

int nvkm_clk_read(struct nvkm_clk *clk, enum nv_clk_src src)
{
    (void)clk; (void)src;
    return 0;
}

int nvkm_clk_ustate(struct nvkm_clk *clk, int req, int pwr)
{
    (void)clk; (void)req; (void)pwr;
    return -ENOSYS;
}

int nvkm_clk_pwrsrc(struct nvkm_device *device)
{
    (void)device;
    return 0;
}

void nvkm_gpio_reset(struct nvkm_gpio *gpio, u8 func)
{
    (void)gpio; (void)func;
}

int nvkm_gpio_find(struct nvkm_gpio *gpio, int idx, u8 tag, u8 line,
                   struct dcb_gpio_func *func)
{
    (void)gpio; (void)idx; (void)tag; (void)line; (void)func;
    return -ENODEV;
}

struct nvkm_i2c_bus *nvkm_i2c_bus_find(struct nvkm_i2c *i2c, int id)
{
    (void)i2c; (void)id;
    return NULL;
}

struct nvkm_i2c_aux *nvkm_i2c_aux_find(struct nvkm_i2c *i2c, int id)
{
    (void)i2c; (void)id;
    return NULL;
}

int nvkm_i2c_aux_acquire(struct nvkm_i2c_aux *aux)
{
    (void)aux;
    return -ENODEV;
}

void nvkm_i2c_aux_release(struct nvkm_i2c_aux *aux)
{
    (void)aux;
}

int nvkm_i2c_aux_xfer(struct nvkm_i2c_aux *aux, bool retry, u8 type,
                      u32 addr, u8 *data, u8 *size)
{
    (void)aux; (void)retry; (void)type; (void)addr; (void)data; (void)size;
    return -ENODEV;
}

struct nvkm_instmem_func;
int r535_instmem_new(const struct nvkm_instmem_func *func,
                     struct nvkm_device *device, enum nvkm_subdev_type type,
                     int inst, struct nvkm_instmem **pimem)
{
    (void)func; (void)device; (void)type; (void)inst; (void)pimem;
    return -ENODEV;
}

void r535_mmu_vaspace_del(struct nvkm_vmm *vmm)
{
    (void)vmm;
}

const struct nvkm_enum nv50_data_error_names[] = {
    { 0x00000003, "INVALID_OPERATION", NULL },
    { 0x00000004, "INVALID_VALUE", NULL },
    { 0x00000005, "INVALID_ENUM", NULL },
    { 0x00000008, "INVALID_OBJECT", NULL },
    {}
};

static struct nvkm_device *
nvkm_device_find_locked(u64 handle)
{
    struct nvkm_device *device;
    list_for_each_entry(device, &nv_devices, head) {
        if (device->handle == handle)
            return device;
    }
    return NULL;
}

struct nvkm_device *
nvkm_device_find(u64 handle)
{
    struct nvkm_device *device;
    mutex_lock(&nv_devices_mutex);
    device = nvkm_device_find_locked(handle);
    mutex_unlock(&nv_devices_mutex);
    return device;
}

static const struct nvkm_device_chip
nv138_chipset = {
    .name     = "GP108",
    .acr      = { 0x00000001, gp108_acr_new },
    .bar      = { 0x00000001, gm107_bar_new },
    .bios     = { 0x00000001, nvkm_bios_new },
    .bus      = { 0x00000001, gf100_bus_new },
    .devinit  = { 0x00000001, gm200_devinit_new },
    .fault    = { 0x00000001, gp100_fault_new },
    .fb       = { 0x00000001, gp102_fb_new },
    .fuse     = { 0x00000001, gm107_fuse_new },
    .imem     = { 0x00000001, nv50_instmem_new },
    .ltc      = { 0x00000001, gp102_ltc_new },
    .mc       = { 0x00000001, gp100_mc_new },
    .mmu      = { 0x00000001, gp100_mmu_new },
    .pci      = { 0x00000001, gp100_pci_new },
    .pmu      = { 0x00000001, gp102_pmu_new },
    .privring = { 0x00000001, gm200_privring_new },
    .timer    = { 0x00000001, gk20a_timer_new },
    .top      = { 0x00000001, gk104_top_new },
    .ce       = { 0x0000000f, gp102_ce_new },
    .dma      = { 0x00000001, gf119_dma_new },
    .fifo     = { 0x00000001, gp100_fifo_new },
    .gr       = { 0x00000001, gp108_gr_new },
    .nvdec    = { 0x00000001, gm107_nvdec_new },
    .sec2     = { 0x00000001, gp108_sec2_new },
    .sw       = { 0x00000001, gf100_sw_new },
};

struct nvkm_subdev *
nvkm_device_subdev(struct nvkm_device *device, int type, int inst)
{
    struct nvkm_subdev *subdev;

    list_for_each_entry(subdev, &device->subdev, head) {
        if (subdev->type == type && subdev->inst == inst)
            return subdev;
    }

    return NULL;
}

struct nvkm_engine *
nvkm_device_engine(struct nvkm_device *device, int type, int inst)
{
    struct nvkm_subdev *subdev = nvkm_device_subdev(device, type, inst);
    if (subdev && subdev->func == &nvkm_engine)
        return container_of(subdev, struct nvkm_engine, subdev);
    return NULL;
}

int
nvkm_device_fini(struct nvkm_device *device, enum nvkm_suspend_state suspend)
{
    const char *action;
    struct nvkm_subdev *subdev;
    int ret;
    s64 time;

    switch (suspend) {
    case NVKM_POWEROFF:
    default:
        action = "fini";
        break;
    case NVKM_SUSPEND:
        action = "suspend";
        break;
    case NVKM_RUNTIME_SUSPEND:
        action = "runtime";
        break;
    }
    nvdev_trace(device, "%s running...\n", action);
    time = ktime_to_us(ktime_get());

    nvkm_acpi_fini(device);

    list_for_each_entry_reverse(subdev, &device->subdev, head) {
        ret = nvkm_subdev_fini(subdev, suspend);
        if (ret && suspend)
            goto fail;
    }

    nvkm_therm_clkgate_fini(device->therm, suspend);

    if (device->func->fini)
        device->func->fini(device, suspend);

    nvkm_intr_unarm(device);

    time = ktime_to_us(ktime_get()) - time;
    nvdev_trace(device, "%s completed in %lldus...\n", action, time);
    return 0;

fail:
    list_for_each_entry_from(subdev, &device->subdev, head) {
        int rret = nvkm_subdev_init(subdev);
        if (rret)
            nvkm_fatal(subdev, "failed restart, %d\n", ret);
    }

    nvdev_trace(device, "%s failed with %d\n", action, ret);
    return ret;
}

static int
nvkm_device_preinit(struct nvkm_device *device)
{
    struct nvkm_subdev *subdev;
    int ret;
    s64 time;

    nvdev_trace(device, "preinit running...\n");
    time = ktime_to_us(ktime_get());

    nvkm_intr_unarm(device);

    if (device->func->preinit) {
        ret = device->func->preinit(device);
        if (ret)
            goto fail;
    }

    list_for_each_entry(subdev, &device->subdev, head) {
        ret = nvkm_subdev_preinit(subdev);
        if (ret)
            goto fail;
    }

    ret = nvkm_devinit_post(device->devinit);
    if (ret)
        goto fail;

    ret = nvkm_top_parse(device);
    if (ret)
        goto fail;

    ret = nvkm_fb_mem_unlock(device->fb);
    if (ret)
        goto fail;

    time = ktime_to_us(ktime_get()) - time;
    nvdev_trace(device, "preinit completed in %lldus\n", time);
    return 0;

fail:
    nvdev_error(device, "preinit failed with %d\n", ret);
    return ret;
}

int
nvkm_device_init(struct nvkm_device *device)
{
    struct nvkm_subdev *subdev;
    int ret;
    s64 time;

    ret = nvkm_device_preinit(device);
    if (ret)
        return ret;

    nvkm_device_fini(device, NVKM_POWEROFF);

    nvdev_trace(device, "init running...\n");
    time = ktime_to_us(ktime_get());

    nvkm_intr_rearm(device);

    if (device->func->init) {
        ret = device->func->init(device);
        if (ret)
            goto fail;
    }

    list_for_each_entry(subdev, &device->subdev, head) {
        ret = nvkm_subdev_init(subdev);
        if (ret)
            goto fail_subdev;
    }

    nvkm_acpi_init(device);
    nvkm_therm_clkgate_enable(device->therm);

    time = ktime_to_us(ktime_get()) - time;
    nvdev_trace(device, "init completed in %lldus\n", time);
    return 0;

fail_subdev:
    list_for_each_entry_from(subdev, &device->subdev, head)
        nvkm_subdev_fini(subdev, NVKM_POWEROFF);
fail:
    nvkm_device_fini(device, NVKM_POWEROFF);

    nvdev_error(device, "init failed with %d\n", ret);
    return ret;
}

void
nvkm_device_del(struct nvkm_device **pdevice)
{
    struct nvkm_device *device = *pdevice;
    struct nvkm_subdev *subdev, *subtmp;
    if (device) {
        mutex_lock(&nv_devices_mutex);

        nvkm_intr_dtor(device);

        list_for_each_entry_reverse(subdev, &device->subdev, head) {
            (void)subtmp;
        }
        while (!list_empty(&device->subdev)) {
            subdev = list_last_entry(&device->subdev, typeof(*subdev), head);
            nvkm_subdev_del(&subdev);
        }

        if (device->pri)
            iounmap(device->pri);
        list_del(&device->head);

        if (device->func->dtor)
            *pdevice = device->func->dtor(device);
        mutex_unlock(&nv_devices_mutex);

        kfree(*pdevice);
        *pdevice = NULL;
    }
}

static inline bool
nvkm_device_endianness(struct nvkm_device *device)
{
    u32 pmc_boot_1 = nvkm_rd32(device, 0x000004);
    if (pmc_boot_1 && pmc_boot_1 != 0x01000001)
        return true;
    if (pmc_boot_1) {
        nvkm_wr32(device, 0x000004, 0x01000001);
        nvkm_rd32(device, 0x000000);
    }
    return true;
}

int
nvkm_device_ctor(const struct nvkm_device_func *func,
                 const struct nvkm_device_quirk *quirk,
                 struct device *dev, enum nvkm_device_type type, u64 handle,
                 const char *name, const char *cfg, const char *dbg,
                 struct nvkm_device *device)
{
    struct nvkm_subdev *subdev;
    u64 mmio_base, mmio_size;
    u32 boot0, strap;
    int ret = -EEXIST, j;

    mutex_lock(&nv_devices_mutex);
    if (nvkm_device_find_locked(handle))
        goto done;

    device->func = func;
    device->quirk = quirk;
    device->dev = dev;
    device->type = type;
    device->handle = handle;
    device->cfgopt = cfg;
    device->dbgopt = dbg;
    device->name = name;
    list_add_tail(&device->head, &nv_devices);
    device->debug = nvkm_dbgopt(device->dbgopt, "device");
    INIT_LIST_HEAD(&device->subdev);

    mmio_base = device->func->resource_addr(device, NVKM_BAR0_PRI);
    mmio_size = device->func->resource_size(device, NVKM_BAR0_PRI);

    device->pri = ioremap(mmio_base, mmio_size);
    if (device->pri == NULL) {
        nvdev_error(device, "unable to map PRI\n");
        ret = -ENOMEM;
        goto done;
    }

    if (!nvkm_device_endianness(device)) {
        ret = -ENOSYS;
        goto done;
    }

    boot0 = nvkm_rd32(device, 0x000000);
    device->chipset = (boot0 & 0x1ff00000) >> 20;
    device->chiprev = (boot0 & 0x000000ff);
    device->card_type = GP100;

    if (device->chipset != 0x138) {
        nvdev_error(device, "expected GP108 (0x138), got %08x\n", boot0);
        ret = -ENODEV;
        goto done;
    }
    device->chip = &nv138_chipset;

    nvdev_info(device, "NVIDIA %s (%08x)\n", device->chip->name, boot0);

    strap = nvkm_rd32(device, 0x101000) & 0x00400040;
    switch (strap) {
    case 0x00000000: device->crystal = 13500; break;
    case 0x00000040: device->crystal = 14318; break;
    case 0x00400000: device->crystal = 27000; break;
    case 0x00400040: device->crystal = 25000; break;
    }

    if (!device->name)
        device->name = device->chip->name;

    mutex_init(&device->mutex);
    nvkm_intr_ctor(device);

#define NVKM_LAYOUT_ONCE(type,data,ptr)                                                      \
    if (device->chip->ptr.inst) {                                                            \
        WARN_ON(device->chip->ptr.inst != 0x00000001);                                       \
        ret = device->chip->ptr.ctor(device, (type), -1, &device->ptr);                      \
        subdev = nvkm_device_subdev(device, (type), 0);                                      \
        if (ret) {                                                                           \
            nvkm_subdev_del(&subdev);                                                        \
            device->ptr = NULL;                                                              \
            if (ret != -ENODEV) {                                                            \
                nvdev_error(device, "%s ctor failed: %d\n",                                  \
                            nvkm_subdev_type[(type)], ret);                                  \
                goto done;                                                                   \
            }                                                                                \
        } else {                                                                             \
            subdev->pself = (void **)&device->ptr;                                           \
        }                                                                                    \
    }
#define NVKM_LAYOUT_INST(type,data,ptr,cnt)                                                  \
    WARN_ON(device->chip->ptr.inst & ~((1 << ARRAY_SIZE(device->ptr)) - 1));                 \
    for (j = 0; device->chip->ptr.inst && j < (int)ARRAY_SIZE(device->ptr); j++) {           \
        if (device->chip->ptr.inst & BIT(j)) {                                               \
            ret = device->chip->ptr.ctor(device, (type), (j), &device->ptr[j]);              \
            subdev = nvkm_device_subdev(device, (type), (j));                                \
            if (ret) {                                                                       \
                nvkm_subdev_del(&subdev);                                                    \
                device->ptr[j] = NULL;                                                       \
                if (ret != -ENODEV) {                                                        \
                    nvdev_error(device, "%s%d ctor failed: %d\n",                            \
                                nvkm_subdev_type[(type)], (j), ret);                         \
                    goto done;                                                               \
                }                                                                            \
            } else {                                                                         \
                subdev->pself = (void **)&device->ptr[j];                                    \
            }                                                                                \
        }                                                                                    \
    }
#include <core/layout.h>
#undef NVKM_LAYOUT_INST
#undef NVKM_LAYOUT_ONCE

    ret = nvkm_intr_install(device);
done:
    if (ret && device->pri) {
        iounmap(device->pri);
        device->pri = NULL;
    }
    mutex_unlock(&nv_devices_mutex);
    return ret;
}
