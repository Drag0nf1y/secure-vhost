/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <fcntl.h>
#include <linux/pci_regs.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdbool.h>

#include <rte_log.h>
#include <rte_pci.h>
#include <rte_bus_pci.h>
#include <rte_eal_memconfig.h>
#include <rte_malloc.h>
#include <rte_vfio.h>

#include "eal_filesystem.h"

#include "pci_init.h"
#include "private.h"

/**
 * @file
 * PCI probing under linux (VFIO version)
 *
 * This code tries to determine if the PCI device is bound to VFIO driver,
 * and initialize it (map BARs, set up interrupts) if that's the case.
 *
 * This file is only compiled if CONFIG_RTE_EAL_VFIO is set to "y".
 */

#ifdef VFIO_PRESENT

#define PAGE_SIZE   (sysconf(_SC_PAGESIZE))
#define PAGE_MASK   (~(PAGE_SIZE - 1))

static struct rte_tailq_elem rte_vfio_tailq = {
	.name = "VFIO_RESOURCE_LIST",
};
EAL_REGISTER_TAILQ(rte_vfio_tailq)

int
pci_vfio_read_config(const struct rte_intr_handle *intr_handle,
		    void *buf, size_t len, off_t offs)
{
	return pread64(intr_handle->vfio_dev_fd, buf, len,
	       VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) + offs);
}

int
pci_vfio_write_config(const struct rte_intr_handle *intr_handle,
		    const void *buf, size_t len, off_t offs)
{
	return pwrite64(intr_handle->vfio_dev_fd, buf, len,
	       VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) + offs);
}

/* get PCI BAR number where MSI-X interrupts are */
static int
pci_vfio_get_msix_bar(int fd, struct pci_msix_table *msix_table)
{
	int ret;
	uint32_t reg;
	uint16_t flags;
	uint8_t cap_id, cap_offset;

	/* read PCI capability pointer from config space */
	ret = pread64(fd, &reg, sizeof(reg),
			VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
			PCI_CAPABILITY_LIST);
	if (ret != sizeof(reg)) {
		RTE_LOG(ERR, EAL, "Cannot read capability pointer from PCI "
				"config space!\n");
		return -1;
	}

	/* we need first byte */
	cap_offset = reg & 0xFF;

	while (cap_offset) {

		/* read PCI capability ID */
		ret = pread64(fd, &reg, sizeof(reg),
				VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
				cap_offset);
		if (ret != sizeof(reg)) {
			RTE_LOG(ERR, EAL, "Cannot read capability ID from PCI "
					"config space!\n");
			return -1;
		}

		/* we need first byte */
		cap_id = reg & 0xFF;

		/* if we haven't reached MSI-X, check next capability */
		if (cap_id != PCI_CAP_ID_MSIX) {
			ret = pread64(fd, &reg, sizeof(reg),
					VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
					cap_offset);
			if (ret != sizeof(reg)) {
				RTE_LOG(ERR, EAL, "Cannot read capability pointer from PCI "
						"config space!\n");
				return -1;
			}

			/* we need second byte */
			cap_offset = (reg & 0xFF00) >> 8;

			continue;
		}
		/* else, read table offset */
		else {
			/* table offset resides in the next 4 bytes */
			ret = pread64(fd, &reg, sizeof(reg),
					VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
					cap_offset + 4);
			if (ret != sizeof(reg)) {
				RTE_LOG(ERR, EAL, "Cannot read table offset from PCI config "
						"space!\n");
				return -1;
			}

			ret = pread64(fd, &flags, sizeof(flags),
					VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
					cap_offset + 2);
			if (ret != sizeof(flags)) {
				RTE_LOG(ERR, EAL, "Cannot read table flags from PCI config "
						"space!\n");
				return -1;
			}

			msix_table->bar_index = reg & RTE_PCI_MSIX_TABLE_BIR;
			msix_table->offset = reg & RTE_PCI_MSIX_TABLE_OFFSET;
			msix_table->size =
				16 * (1 + (flags & RTE_PCI_MSIX_FLAGS_QSIZE));

			return 0;
		}
	}
	return 0;
}

/* set PCI bus mastering */
static int
pci_vfio_set_bus_master(int dev_fd, bool op)
{
	uint16_t reg;
	int ret;

	ret = pread64(dev_fd, &reg, sizeof(reg),
			VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
			PCI_COMMAND);
	if (ret != sizeof(reg)) {
		RTE_LOG(ERR, EAL, "Cannot read command from PCI config space!\n");
		return -1;
	}

	if (op)
		/* set the master bit */
		reg |= PCI_COMMAND_MASTER;
	else
		reg &= ~(PCI_COMMAND_MASTER);

	ret = pwrite64(dev_fd, &reg, sizeof(reg),
			VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX) +
			PCI_COMMAND);

	if (ret != sizeof(reg)) {
		RTE_LOG(ERR, EAL, "Cannot write command to PCI config space!\n");
		return -1;
	}

	return 0;
}

/* set up interrupt support (but not enable interrupts) */
static int
pci_vfio_setup_interrupts(struct rte_pci_device *dev, int vfio_dev_fd)
{
	int i, ret, intr_idx;
	enum rte_intr_mode intr_mode;

	/* default to invalid index */
	intr_idx = VFIO_PCI_NUM_IRQS;

	/* Get default / configured intr_mode */
	intr_mode = rte_eal_vfio_intr_mode();

	/* get interrupt type from internal config (MSI-X by default, can be
	 * overridden from the command line
	 */
	switch (intr_mode) {
	case RTE_INTR_MODE_MSIX:
		intr_idx = VFIO_PCI_MSIX_IRQ_INDEX;
		break;
	case RTE_INTR_MODE_MSI:
		intr_idx = VFIO_PCI_MSI_IRQ_INDEX;
		break;
	case RTE_INTR_MODE_LEGACY:
		intr_idx = VFIO_PCI_INTX_IRQ_INDEX;
		break;
	/* don't do anything if we want to automatically determine interrupt type */
	case RTE_INTR_MODE_NONE:
		break;
	default:
		RTE_LOG(ERR, EAL, "  unknown default interrupt type!\n");
		return -1;
	}

	/* start from MSI-X interrupt type */
	for (i = VFIO_PCI_MSIX_IRQ_INDEX; i >= 0; i--) {
		struct vfio_irq_info irq = { .argsz = sizeof(irq) };
		int fd = -1;

		/* skip interrupt modes we don't want */
		if (intr_mode != RTE_INTR_MODE_NONE &&
				i != intr_idx)
			continue;

		irq.index = i;

		ret = ioctl(vfio_dev_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq);
		if (ret < 0) {
			RTE_LOG(ERR, EAL, "  cannot get IRQ info, "
					"error %i (%s)\n", errno, strerror(errno));
			return -1;
		}

		/* if this vector cannot be used with eventfd, fail if we explicitly
		 * specified interrupt type, otherwise continue */
		if ((irq.flags & VFIO_IRQ_INFO_EVENTFD) == 0) {
			if (intr_mode != RTE_INTR_MODE_NONE) {
				RTE_LOG(ERR, EAL,
						"  interrupt vector does not support eventfd!\n");
				return -1;
			} else
				continue;
		}

		/* set up an eventfd for interrupts */
		fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (fd < 0) {
			RTE_LOG(ERR, EAL, "  cannot set up eventfd, "
					"error %i (%s)\n", errno, strerror(errno));
			return -1;
		}

		dev->intr_handle.fd = fd;
		dev->intr_handle.vfio_dev_fd = vfio_dev_fd;

		switch (i) {
		case VFIO_PCI_MSIX_IRQ_INDEX:
			intr_mode = RTE_INTR_MODE_MSIX;
			dev->intr_handle.type = RTE_INTR_HANDLE_VFIO_MSIX;
			break;
		case VFIO_PCI_MSI_IRQ_INDEX:
			intr_mode = RTE_INTR_MODE_MSI;
			dev->intr_handle.type = RTE_INTR_HANDLE_VFIO_MSI;
			break;
		case VFIO_PCI_INTX_IRQ_INDEX:
			intr_mode = RTE_INTR_MODE_LEGACY;
			dev->intr_handle.type = RTE_INTR_HANDLE_VFIO_LEGACY;
			break;
		default:
			RTE_LOG(ERR, EAL, "  unknown interrupt type!\n");
			return -1;
		}

		return 0;
	}

	/* if we're here, we haven't found a suitable interrupt vector */
	return -1;
}

static int
pci_vfio_is_ioport_bar(int vfio_dev_fd, int bar_index)
{
	uint32_t ioport_bar;
	int ret;

	ret = pread64(vfio_dev_fd, &ioport_bar, sizeof(ioport_bar),
			  VFIO_GET_REGION_ADDR(VFIO_PCI_CONFIG_REGION_INDEX)
			  + PCI_BASE_ADDRESS_0 + bar_index*4);
	if (ret != sizeof(ioport_bar)) {
		RTE_LOG(ERR, EAL, "Cannot read command (%x) from config space!\n",
			PCI_BASE_ADDRESS_0 + bar_index*4);
		return -1;
	}

	return (ioport_bar & PCI_BASE_ADDRESS_SPACE_IO) != 0;
}

static int
pci_rte_vfio_setup_device(struct rte_pci_device *dev, int vfio_dev_fd)
{
	if (pci_vfio_setup_interrupts(dev, vfio_dev_fd) != 0) {
		RTE_LOG(ERR, EAL, "Error setting up interrupts!\n");
		return -1;
	}

	/* set bus mastering for the device */
	if (pci_vfio_set_bus_master(vfio_dev_fd, true)) {
		RTE_LOG(ERR, EAL, "Cannot set up bus mastering!\n");
		return -1;
	}

	/*
	 * Reset the device. If the device is not capable of resetting,
	 * then it updates errno as EINVAL.
	 */
	if (ioctl(vfio_dev_fd, VFIO_DEVICE_RESET) && errno != EINVAL) {
		RTE_LOG(ERR, EAL, "Unable to reset device! Error: %d (%s)\n",
				errno, strerror(errno));
		return -1;
	}

	return 0;
}

static int
pci_vfio_mmap_bar(int vfio_dev_fd, struct mapped_pci_resource *vfio_res,
		int bar_index, int additional_flags)
{
	struct memreg {
		unsigned long offset, size;
	} memreg[2] = {};
	void *bar_addr;
	struct pci_msix_table *msix_table = &vfio_res->msix_table;
	struct pci_map *bar = &vfio_res->maps[bar_index];

	if (bar->size == 0)
		/* Skip this BAR */
		return 0;

	if (msix_table->bar_index == bar_index) {
		/*
		 * VFIO will not let us map the MSI-X table,
		 * but we can map around it.
		 */
		uint32_t table_start = msix_table->offset;
		uint32_t table_end = table_start + msix_table->size;
		table_end = (table_end + ~PAGE_MASK) & PAGE_MASK;
		table_start &= PAGE_MASK;

		if (table_start == 0 && table_end >= bar->size) {
			/* Cannot map this BAR */
			RTE_LOG(DEBUG, EAL, "Skipping BAR%d\n", bar_index);
			bar->size = 0;
			bar->addr = 0;
			return 0;
		}

		memreg[0].offset = bar->offset;
		memreg[0].size = table_start;
		memreg[1].offset = bar->offset + table_end;
		memreg[1].size = bar->size - table_end;

		RTE_LOG(DEBUG, EAL,
			"Trying to map BAR%d that contains the MSI-X "
			"table. Trying offsets: "
			"0x%04lx:0x%04lx, 0x%04lx:0x%04lx\n", bar_index,
			memreg[0].offset, memreg[0].size,
			memreg[1].offset, memreg[1].size);
	} else {
		memreg[0].offset = bar->offset;
		memreg[0].size = bar->size;
	}

	/* reserve the address using an inaccessible mapping */
	bar_addr = mmap(bar->addr, bar->size, 0, MAP_PRIVATE |
			MAP_ANONYMOUS | additional_flags, -1, 0);
	if (bar_addr != MAP_FAILED) {
		void *map_addr = NULL;
		if (memreg[0].size) {
			/* actual map of first part */
			map_addr = pci_map_resource(bar_addr, vfio_dev_fd,
							memreg[0].offset,
							memreg[0].size,
							MAP_FIXED);
		}

		/* if there's a second part, try to map it */
		if (map_addr != MAP_FAILED
			&& memreg[1].offset && memreg[1].size) {
			void *second_addr = RTE_PTR_ADD(bar_addr,
							memreg[1].offset -
							(uintptr_t)bar->offset);
			map_addr = pci_map_resource(second_addr,
							vfio_dev_fd,
							memreg[1].offset,
							memreg[1].size,
							MAP_FIXED);
		}

		if (map_addr == MAP_FAILED || !map_addr) {
			munmap(bar_addr, bar->size);
			bar_addr = MAP_FAILED;
			RTE_LOG(ERR, EAL, "Failed to map pci BAR%d\n",
					bar_index);
			return -1;
		}
	} else {
		RTE_LOG(ERR, EAL,
				"Failed to create inaccessible mapping for BAR%d\n",
				bar_index);
		return -1;
	}

	bar->addr = bar_addr;
	return 0;
}

static int
pci_vfio_map_resource_primary(struct rte_pci_device *dev)
{
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	char pci_addr[PATH_MAX] = {0};
	int vfio_dev_fd;
	struct rte_pci_addr *loc = &dev->addr;
	int i, ret;
	struct mapped_pci_resource *vfio_res = NULL;
	struct mapped_pci_res_list *vfio_res_list =
		RTE_TAILQ_CAST(rte_vfio_tailq.head, mapped_pci_res_list);

	struct pci_map *maps;

	dev->intr_handle.fd = -1;

	/* store PCI address string */
	snprintf(pci_addr, sizeof(pci_addr), PCI_PRI_FMT,
			loc->domain, loc->bus, loc->devid, loc->function);

	ret = rte_vfio_setup_device(rte_pci_get_sysfs_path(), pci_addr,
					&vfio_dev_fd, &device_info);
	if (ret)
		return ret;

	/* allocate vfio_res and get region info */
	vfio_res = rte_zmalloc("VFIO_RES", sizeof(*vfio_res), 0);
	if (vfio_res == NULL) {
		RTE_LOG(ERR, EAL,
			"%s(): cannot store uio mmap details\n", __func__);
		goto err_vfio_dev_fd;
	}
	memcpy(&vfio_res->pci_addr, &dev->addr, sizeof(vfio_res->pci_addr));

	/* get number of registers (up to BAR5) */
	vfio_res->nb_maps = RTE_MIN((int) device_info.num_regions,
			VFIO_PCI_BAR5_REGION_INDEX + 1);

	/* map BARs */
	maps = vfio_res->maps;

	vfio_res->msix_table.bar_index = -1;
	/* get MSI-X BAR, if any (we have to know where it is because we can't
	 * easily mmap it when using VFIO)
	 */
	ret = pci_vfio_get_msix_bar(vfio_dev_fd, &vfio_res->msix_table);
	if (ret < 0) {
		RTE_LOG(ERR, EAL, "  %s cannot get MSI-X BAR number!\n",
				pci_addr);
		goto err_vfio_dev_fd;
	}

	for (i = 0; i < (int) vfio_res->nb_maps; i++) {
		struct vfio_region_info reg = { .argsz = sizeof(reg) };
		void *bar_addr;

		reg.index = i;

		ret = ioctl(vfio_dev_fd, VFIO_DEVICE_GET_REGION_INFO, &reg);
		if (ret) {
			RTE_LOG(ERR, EAL, "  %s cannot get device region info "
					"error %i (%s)\n", pci_addr, errno, strerror(errno));
			goto err_vfio_res;
		}

		/* chk for io port region */
		ret = pci_vfio_is_ioport_bar(vfio_dev_fd, i);
		if (ret < 0)
			goto err_vfio_res;
		else if (ret) {
			RTE_LOG(INFO, EAL, "Ignore mapping IO port bar(%d)\n",
					i);
			continue;
		}

		/* skip non-mmapable BARs */
		if ((reg.flags & VFIO_REGION_INFO_FLAG_MMAP) == 0)
			continue;

		/* try mapping somewhere close to the end of hugepages */
		if (pci_map_addr == NULL)
			pci_map_addr = pci_find_max_end_va();

		bar_addr = pci_map_addr;
		pci_map_addr = RTE_PTR_ADD(bar_addr, (size_t) reg.size);

		maps[i].addr = bar_addr;
		maps[i].offset = reg.offset;
		maps[i].size = reg.size;
		maps[i].path = NULL; /* vfio doesn't have per-resource paths */

		ret = pci_vfio_mmap_bar(vfio_dev_fd, vfio_res, i, 0);
		if (ret < 0) {
			RTE_LOG(ERR, EAL, "  %s mapping BAR%i failed: %s\n",
					pci_addr, i, strerror(errno));
			goto err_vfio_res;
		}

		dev->mem_resource[i].addr = maps[i].addr;
	}

	if (pci_rte_vfio_setup_device(dev, vfio_dev_fd) < 0) {
		RTE_LOG(ERR, EAL, "  %s setup device failed\n", pci_addr);
		goto err_vfio_res;
	}

	TAILQ_INSERT_TAIL(vfio_res_list, vfio_res, next);

	return 0;
err_vfio_res:
	rte_free(vfio_res);
err_vfio_dev_fd:
	close(vfio_dev_fd);
	return -1;
}

static int
pci_vfio_map_resource_secondary(struct rte_pci_device *dev)
{
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	char pci_addr[PATH_MAX] = {0};
	int vfio_dev_fd;
	struct rte_pci_addr *loc = &dev->addr;
	int i, ret;
	struct mapped_pci_resource *vfio_res = NULL;
	struct mapped_pci_res_list *vfio_res_list =
		RTE_TAILQ_CAST(rte_vfio_tailq.head, mapped_pci_res_list);

	struct pci_map *maps;

	dev->intr_handle.fd = -1;

	/* store PCI address string */
	snprintf(pci_addr, sizeof(pci_addr), PCI_PRI_FMT,
			loc->domain, loc->bus, loc->devid, loc->function);

	ret = rte_vfio_setup_device(rte_pci_get_sysfs_path(), pci_addr,
					&vfio_dev_fd, &device_info);
	if (ret)
		return ret;

	/* if we're in a secondary process, just find our tailq entry */
	TAILQ_FOREACH(vfio_res, vfio_res_list, next) {
		if (rte_pci_addr_cmp(&vfio_res->pci_addr,
						 &dev->addr))
			continue;
		break;
	}
	/* if we haven't found our tailq entry, something's wrong */
	if (vfio_res == NULL) {
		RTE_LOG(ERR, EAL, "  %s cannot find TAILQ entry for PCI device!\n",
				pci_addr);
		goto err_vfio_dev_fd;
	}

	/* map BARs */
	maps = vfio_res->maps;

	for (i = 0; i < (int) vfio_res->nb_maps; i++) {
		ret = pci_vfio_mmap_bar(vfio_dev_fd, vfio_res, i, MAP_FIXED);
		if (ret < 0) {
			RTE_LOG(ERR, EAL, "  %s mapping BAR%i failed: %s\n",
					pci_addr, i, strerror(errno));
			goto err_vfio_dev_fd;
		}

		dev->mem_resource[i].addr = maps[i].addr;
	}

	return 0;
err_vfio_dev_fd:
	close(vfio_dev_fd);
	return -1;
}

/*
 * map the PCI resources of a PCI device in virtual memory (VFIO version).
 * primary and secondary processes follow almost exactly the same path
 */
int
pci_vfio_map_resource(struct rte_pci_device *dev)
{
	if (rte_eal_process_type() == RTE_PROC_PRIMARY || strcmp(dev->driver->driver.name,"spdk_ioat")==0)
		return pci_vfio_map_resource_primary(dev);
	else
		return pci_vfio_map_resource_secondary(dev);
}

int
pci_vfio_unmap_resource(struct rte_pci_device *dev)
{
	char pci_addr[PATH_MAX] = {0};
	struct rte_pci_addr *loc = &dev->addr;
	int i, ret;
	struct mapped_pci_resource *vfio_res = NULL;
	struct mapped_pci_res_list *vfio_res_list;

	struct pci_map *maps;

	/* store PCI address string */
	snprintf(pci_addr, sizeof(pci_addr), PCI_PRI_FMT,
			loc->domain, loc->bus, loc->devid, loc->function);


	if (close(dev->intr_handle.fd) < 0) {
		RTE_LOG(INFO, EAL, "Error when closing eventfd file descriptor for %s\n",
			pci_addr);
		return -1;
	}

	if (pci_vfio_set_bus_master(dev->intr_handle.vfio_dev_fd, false)) {
		RTE_LOG(ERR, EAL, "  %s cannot unset bus mastering for PCI device!\n",
				pci_addr);
		return -1;
	}

	ret = rte_vfio_release_device(rte_pci_get_sysfs_path(), pci_addr,
				  dev->intr_handle.vfio_dev_fd);
	if (ret < 0) {
		RTE_LOG(ERR, EAL,
			"%s(): cannot release device\n", __func__);
		return ret;
	}

	vfio_res_list = RTE_TAILQ_CAST(rte_vfio_tailq.head, mapped_pci_res_list);
	/* Get vfio_res */
	TAILQ_FOREACH(vfio_res, vfio_res_list, next) {
		if (memcmp(&vfio_res->pci_addr, &dev->addr, sizeof(dev->addr)))
			continue;
		break;
	}
	/* if we haven't found our tailq entry, something's wrong */
	if (vfio_res == NULL) {
		RTE_LOG(ERR, EAL, "  %s cannot find TAILQ entry for PCI device!\n",
				pci_addr);
		return -1;
	}

	/* unmap BARs */
	maps = vfio_res->maps;

	RTE_LOG(INFO, EAL, "Releasing pci mapped resource for %s\n",
		pci_addr);
	for (i = 0; i < (int) vfio_res->nb_maps; i++) {

		/*
		 * We do not need to be aware of MSI-X table BAR mappings as
		 * when mapping. Just using current maps array is enough
		 */
		if (maps[i].addr) {
			RTE_LOG(INFO, EAL, "Calling pci_unmap_resource for %s at %p\n",
				pci_addr, maps[i].addr);
			pci_unmap_resource(maps[i].addr, maps[i].size);
		}
	}

	TAILQ_REMOVE(vfio_res_list, vfio_res, next);

	return 0;
}

int
pci_vfio_ioport_map(struct rte_pci_device *dev, int bar,
		    struct rte_pci_ioport *p)
{
	if (bar < VFIO_PCI_BAR0_REGION_INDEX ||
	    bar > VFIO_PCI_BAR5_REGION_INDEX) {
		RTE_LOG(ERR, EAL, "invalid bar (%d)!\n", bar);
		return -1;
	}

	p->dev = dev;
	p->base = VFIO_GET_REGION_ADDR(bar);
	return 0;
}

void
pci_vfio_ioport_read(struct rte_pci_ioport *p,
		     void *data, size_t len, off_t offset)
{
	const struct rte_intr_handle *intr_handle = &p->dev->intr_handle;

	if (pread64(intr_handle->vfio_dev_fd, data,
		    len, p->base + offset) <= 0)
		RTE_LOG(ERR, EAL,
			"Can't read from PCI bar (%" PRIu64 ") : offset (%x)\n",
			VFIO_GET_REGION_IDX(p->base), (int)offset);
}

void
pci_vfio_ioport_write(struct rte_pci_ioport *p,
		      const void *data, size_t len, off_t offset)
{
	const struct rte_intr_handle *intr_handle = &p->dev->intr_handle;

	if (pwrite64(intr_handle->vfio_dev_fd, data,
		     len, p->base + offset) <= 0)
		RTE_LOG(ERR, EAL,
			"Can't write to PCI bar (%" PRIu64 ") : offset (%x)\n",
			VFIO_GET_REGION_IDX(p->base), (int)offset);
}

int
pci_vfio_ioport_unmap(struct rte_pci_ioport *p)
{
	RTE_SET_USED(p);
	return -1;
}

int
pci_vfio_is_enabled(void)
{
	return rte_vfio_is_enabled("vfio_pci");
}
#endif
