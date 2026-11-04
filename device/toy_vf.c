/* VF 只提供資料 queue 與狀態寄存器；關閉 fence 和管理配額的權限留在 PF。 */
#include "qemu/osdep.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_sriov.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "toy.h"

static uint64_t read_vf(void *opaque, hwaddr addr, unsigned size)
{
    ToyVF *vf = opaque;
    ToyPF *pf = vf->pf;
    ToySlot *s = &pf->slots[vf->index];
    if (size != 4 || (addr & 3)) {
        toy_saturate(&pf->bad_mmio);
        return 0;
    }
    switch (addr) {
    case TOY_VF_MAGIC: return TOY_VF_MAGIC_VALUE;
    case TOY_VF_ABI: return TOY_ABI_VERSION;
    case TOY_VF_STATUS: return s->latch_status;
    case TOY_VF_EPOCH_LO: return (uint32_t)s->latch_epoch;
    case TOY_VF_EPOCH_HI: return s->latch_epoch >> 32;
    case TOY_VF_SESSION_LO: return (uint32_t)s->latch_session;
    case TOY_VF_SESSION_HI: return s->latch_session >> 32;
    case TOY_VF_SQ_BASE_LO: return (uint32_t)s->staging.sq_base;
    case TOY_VF_SQ_BASE_HI: return s->staging.sq_base >> 32;
    case TOY_VF_SQ_ENTRIES: return s->staging.sq_entries;
    case TOY_VF_CQ_BASE_LO: return (uint32_t)s->staging.cq_base;
    case TOY_VF_CQ_BASE_HI: return s->staging.cq_base >> 32;
    case TOY_VF_CQ_ENTRIES: return s->staging.cq_entries;
    case TOY_VF_CONFIG_EPOCH_LO: return (uint32_t)s->staging.epoch;
    case TOY_VF_CONFIG_EPOCH_HI: return s->staging.epoch >> 32;
    case TOY_VF_CONFIG_SESSION_LO: return (uint32_t)s->staging.session;
    case TOY_VF_CONFIG_SESSION_HI: return s->staging.session >> 32;
    case TOY_VF_SQ_HEAD: return s->sq_head;
    case TOY_VF_SQ_TAIL: return s->sq_tail;
    case TOY_VF_CQ_HEAD: return s->cq_head;
    case TOY_VF_CQ_TAIL: return s->cq_tail;
    case TOY_VF_QUEUE_STATUS: return s->queue_status;
    case TOY_VF_QUEUE_ERROR: return s->queue_error;
    case TOY_VF_IRQ_CAUSE: return s->irq_cause;
    default: return 0;
    }
}

static void set_half(uint64_t *target, uint32_t value, bool high)
{
    if (high) {
        *target = (*target & UINT32_MAX) | ((uint64_t)value << 32);
    } else {
        *target = (*target & ~((uint64_t)UINT32_MAX)) | value;
    }
}

/* Staging 寄存器可分次寫入；COMMIT 驗證完整設定後才生效，並停在 READY_PENDING 等 PF REARM。 */
static void config_commit(ToyPF *pf, unsigned index)
{
    ToySlot *s = &pf->slots[index];
    fw_status result;
    if (s->killed || !s->fenced || s->queue_status == TOY_QUEUE_STATUS_RUNNING ||
        (pf->core.vf[index].state != FW_ACTIVE && pf->core.vf[index].state != FW_RECOVERING)) {
        s->queue_error = FW_STATE;
        return;
    }
    result = fw_queue_validate(&s->staging, pf->session, s->epoch);
    if (result != FW_OK || s->ready_gen == UINT32_MAX) {
        s->queue_error = result == FW_OK ? FW_OVERFLOW : result;
        return;
    }
    s->config = s->staging;
    s->sq_head = s->sq_tail = s->cq_head = s->cq_tail = 0;
    s->ready_gen++;
    s->config_valid = s->ready_pending = true;
    s->queue_status = TOY_QUEUE_STATUS_READY_PENDING;
    s->queue_error = FW_OK;
}

static void write_vf(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ToyVF *vf = opaque;
    ToyPF *pf = vf->pf;
    ToySlot *s = &pf->slots[vf->index];
    uint32_t val = value;
    if (size != 4 || (addr & 3)) {
        toy_saturate(&pf->bad_mmio);
        return;
    }
    switch (addr) {
    /* 先 latch 再分次讀 32-bit halves，避免讀到不同時刻的 session/epoch。 */
    case TOY_VF_EPOCH_LATCH:
        if (val != 1) { break; }
        s->latch_epoch = s->epoch;
        s->latch_session = pf->session;
        s->latch_status = toy_hw_flags(pf, vf->index);
        return;
    case TOY_VF_SQ_BASE_LO: set_half(&s->staging.sq_base, val, false); return;
    case TOY_VF_SQ_BASE_HI: set_half(&s->staging.sq_base, val, true); return;
    case TOY_VF_CQ_BASE_LO: set_half(&s->staging.cq_base, val, false); return;
    case TOY_VF_CQ_BASE_HI: set_half(&s->staging.cq_base, val, true); return;
    case TOY_VF_CONFIG_EPOCH_LO: set_half(&s->staging.epoch, val, false); return;
    case TOY_VF_CONFIG_EPOCH_HI: set_half(&s->staging.epoch, val, true); return;
    case TOY_VF_CONFIG_SESSION_LO: set_half(&s->staging.session, val, false); return;
    case TOY_VF_CONFIG_SESSION_HI: set_half(&s->staging.session, val, true); return;
    case TOY_VF_SQ_ENTRIES: s->staging.sq_entries = val; return;
    case TOY_VF_CQ_ENTRIES: s->staging.cq_entries = val; return;
    case TOY_VF_CONFIG_COMMIT:
        if (val != 1) { break; }
        config_commit(pf, vf->index);
        return;
    case TOY_VF_SQ_TAIL:
        if (!s->config_valid || s->queue_status != TOY_QUEUE_STATUS_RUNNING) {
            s->queue_error = FW_STATE;
            return;
        }
        if ((uint32_t)(val - s->sq_head) > s->config.sq_entries ||
            (uint32_t)(val - s->sq_tail) > s->config.sq_entries) {
            toy_fail(pf, vf->index, TOY_LAST_FAULT_BAD_QUEUE);
        } else {
            s->sq_tail = val;
        }
        return;
    case TOY_VF_CQ_HEAD:
        if ((uint32_t)(val - s->cq_head) > (uint32_t)(s->cq_tail - s->cq_head)) {
            toy_fail(pf, vf->index, TOY_LAST_FAULT_BAD_QUEUE);
        } else {
            s->cq_head = val;
            toy_schedule(pf);
        }
        return;
    case TOY_VF_DOORBELL:
        if (val != 1) { break; }
        if (s->fenced || s->killed) {
            toy_saturate(&s->fence_blocked);
        } else {
            toy_schedule(pf);
        }
        return;
    case TOY_VF_IRQ_CAUSE:
        if (val & ~3u) { break; }
        s->irq_cause &= ~val;
        return;
    default: break;
    }
    toy_saturate(&pf->bad_mmio);
}

const MemoryRegionOps toy_vf_mmio_ops = {
    .read = read_vf, .write = write_vf, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void vf_config_write(PCIDevice *dev, uint32_t address, uint32_t value, int length)
{
    pci_default_write_config(dev, address, value, length);
    pcie_cap_flr_write_config(dev, address, value, length);
}

static void vf_realize(PCIDevice *dev, Error **errp)
{
    ToyVF *vf = TOY_VF(dev);
    int result;
    vf->pf = TOY_PF(pcie_sriov_get_pf(dev));
    vf->index = pcie_sriov_vf_number(dev);
    if (vf->index >= FW_VFS) {
        error_setg(errp, "Invalid VF index");
        return;
    }
    dev->config_write = vf_config_write;
    memory_region_init_io(&vf->bar0, OBJECT(vf), &toy_vf_mmio_ops, vf,
                          "toy-vf-mmio", TOY_LIMITS_VF_BAR0_BYTES);
    memory_region_init(&vf->bar2, OBJECT(vf), "toy-vf-msix", TOY_LIMITS_BAR2_BYTES);
    pcie_sriov_vf_register_bar(dev, 0, &vf->bar0);
    pcie_sriov_vf_register_bar(dev, 2, &vf->bar2);
    result = pcie_endpoint_cap_init(dev, 0x80);
    if (result < 0) {
        error_setg(errp, "VF PCIe capability initialization failed");
        return;
    }
    result = msix_init(dev, 2, &vf->bar2, 2, 0, &vf->bar2, 2, 0x1000, 0x50, errp);
    if (result) {
        pcie_cap_exit(dev);
        return;
    }
    msix_vector_use(dev, 0);
    msix_vector_use(dev, 1);
    pcie_cap_flr_init(dev);
    pcie_ari_init(dev, 0x100);
    pcie_acs_init(dev, 0x120);
    vf->pf->slots[vf->index].vf = vf;
    vf->pf->slots[vf->index].fenced = true;
    toy_clear_queue(vf->pf, vf->index);
    fprintf(stderr, "TOY_VF_REALIZE vf=%u rid=%04x reserve=%u\n", vf->index, pci_get_bdf(dev),
            vf->pf->core.vf[vf->index].reserve);
}

static void vf_exit(PCIDevice *dev)
{
    ToyVF *vf = TOY_VF(dev);
    toy_fence(vf->pf, vf->index, 0, false);
    toy_clear_queue(vf->pf, vf->index);
    vf->pf->slots[vf->index].vf = NULL;
    msix_unuse_all_vectors(dev);
    msix_uninit(dev, &vf->bar2, &vf->bar2);
    pcie_cap_exit(dev);
    fprintf(stderr, "TOY_VF_UNREALIZE vf=%u reserve=%u\n", vf->index, vf->pf->core.vf[vf->index].reserve);
}

static void vf_reset(Object *obj, ResetType type)
{
    ToyVF *vf = TOY_VF(obj);
    if (vf->pf && vf->pf->initialized) {
        /* pinned qdev 在 realized=true 之前重設熱插入的新物件；這不是 PCI FLR。 */
        if (!DEVICE(vf)->realized) {
            fw_status status = toy_fence(vf->pf, vf->index, 0, false);
            toy_clear_queue(vf->pf, vf->index);
            if (status != FW_OK) {
                toy_kill(vf->pf, vf->index);
            }
            fprintf(stderr, "TOY_VF_POWER_ON vf=%u epoch=%" PRIu64 " status=%u\n",
                    vf->index, vf->pf->slots[vf->index].epoch, status);
            return;
        }
        toy_vf_flr(vf->pf, vf->index);
    }
}

static void vf_instance_init(Object *obj)
{
    qdev_prop_set_bit(DEVICE(obj), "multifunction", true);
}

static void vf_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    pc->realize = vf_realize;
    pc->exit = vf_exit;
    pc->vendor_id = TOY_PCI_VENDOR;
    pc->device_id = TOY_PCI_VF;
    pc->revision = TOY_PCI_REVISION;
    pc->class_id = TOY_PCI_CLASS_CODE >> 8;
    RESETTABLE_CLASS(klass)->phases.hold = vf_reset;
    dc->user_creatable = false;
    dc->desc = "SR-IOV firmware lab VF";
}

static const TypeInfo vf_type = {
    .name = TYPE_TOY_VF, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(ToyVF), .instance_init = vf_instance_init, .class_init = vf_class_init,
    .interfaces = (InterfaceInfo[]) { { INTERFACE_PCIE_DEVICE }, { } },
};

static void register_vf(void)
{
    type_register_static(&vf_type);
}
type_init(register_vf)
