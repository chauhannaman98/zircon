// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "nandpart.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/driver.h>
#include <ddk/metadata.h>
#include <ddk/protocol/bad-block.h>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>
#include <fbl/unique_ptr.h>
#include <sync/completion.h>
#include <zircon/boot/image.h>
#include <zircon/hw/gpt.h>
#include <zircon/types.h>

namespace nand {
namespace {

constexpr uint8_t fvm_guid[] = GUID_FVM_VALUE;

// Checks that the partition map is valid, sorts it in partition order, and
// ensures blocks are on erase block boundaries.
zx_status_t SanitizePartitionMap(zbi_partition_map_t* pmap, const nand_info_t& nand_info) {
    if (pmap->partition_count == 0) {
        zxlogf(ERROR, "nandpart: partition count is zero\n");
        return ZX_ERR_INTERNAL;
    }

    // 1) Partitions should not be overlapping.
    qsort(pmap->partitions, pmap->partition_count, sizeof(zbi_partition_t),
          [](const void* left, const void* right) {
              const auto* left_ = static_cast<const zbi_partition_t*>(left);
              const auto* right_ = static_cast<const zbi_partition_t*>(right);
              if (left_->first_block < right_->first_block) {
                  return -1;
              }
              if (left_->first_block > right_->first_block) {
                  return 1;
              }
              return 0;
          });
    auto* const begin = &pmap->partitions[0];
    const auto* const end = &pmap->partitions[pmap->partition_count];

    for (auto *part = begin, *next = begin + 1; next != end; part++, next++) {
        if (part->last_block >= next->first_block) {
            zxlogf(ERROR, "nandpart: partition %s size is not a multiple of erase_block_size",
                   part->name);
            return ZX_ERR_INTERNAL;
        }
    }

    // 2) All partitions must start at an erase block boundary.
    const size_t erase_block_size = nand_info.page_size * nand_info.pages_per_block;
    ZX_DEBUG_ASSERT(fbl::is_pow2(erase_block_size));
    const int block_shift = ffs(static_cast<int>(erase_block_size)) - 1;

    if (pmap->block_size != erase_block_size) {
        for (auto* part = begin; part != end; part++) {
            uint64_t first_byte_offset = part->first_block * pmap->block_size;
            uint64_t last_byte_offset = part->last_block + 1 * pmap->block_size;

            if (fbl::round_down(first_byte_offset, erase_block_size) != first_byte_offset ||
                fbl::round_down(last_byte_offset, erase_block_size) != last_byte_offset) {
                zxlogf(ERROR, "nandpart: partition %s size is not a multiple of erase_block_size",
                       part->name);
                return ZX_ERR_INTERNAL;
            }
            part->first_block = first_byte_offset >> block_shift;
            part->last_block = (last_byte_offset >> block_shift) - 1;
        }
    }
    // 3) Partitions should exist within NAND.
    if ((end - 1)->last_block >= nand_info.num_blocks) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    return ZX_OK;
}

// Shim for calling sub-partition's callback.
void CompletionCallback(nand_op_t* op, zx_status_t status) {
    op = static_cast<nand_op_t*>(op->cookie);
    op->completion_cb(op, status);
}

} // namespace

zx_status_t NandPartDevice::Create(zx_device_t* parent) {
    zxlogf(INFO, "NandPartDevice::Create: Starting...!\n");

    nand_protocol_t nand_proto;
    if (device_get_protocol(parent, ZX_PROTOCOL_NAND, &nand_proto) != ZX_OK) {
        zxlogf(ERROR, "nandpart: parent device '%s': does not support nand protocol\n",
               device_get_name(parent));
        return ZX_ERR_NOT_SUPPORTED;
    }

    // Query parent to get its nand_info_t and size for nand_op_t.
    nand_info_t nand_info;
    size_t parent_op_size;
    nand_proto.ops->query(nand_proto.ctx, &nand_info, &parent_op_size);
    // Make sure parent_op_size is aligned, so we can safely add our data at the end.
    parent_op_size = fbl::round_up(parent_op_size, 8u);

    // Query parent for bad block configuration info.
    size_t actual;
    bad_block_config_t bad_block_config;
    zx_status_t status = device_get_metadata(parent, DEVICE_METADATA_DRIVER_DATA, &bad_block_config,
                                             sizeof(bad_block_config), &actual);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: parent device '%s' has no device metadata\n",
               device_get_name(parent));
        return status;
    }
    if (actual != sizeof(bad_block_config)) {
        zxlogf(ERROR, "nandpart: Expected metadata of size %zu, got %zu\n",
               sizeof(bad_block_config), actual);
        return ZX_ERR_INTERNAL;
    }

    // Create a bad block instance.
    BadBlock::Config config = {
        .bad_block_config = bad_block_config,
        .nand_proto = nand_proto,
    };
    fbl::RefPtr<BadBlock> bad_block;
    status = BadBlock::Create(config, &bad_block);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: Failed to create BadBlock object\n");
        return status;
    }

    // Query parent for partition map.
    uint8_t buffer[METADATA_PARTITION_MAP_MAX];
    status = device_get_metadata(parent, DEVICE_METADATA_PARTITION_MAP, buffer, sizeof(buffer),
                                 &actual);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: parent device '%s' has no parititon map\n",
               device_get_name(parent));
        return status;
    }
    if (actual < sizeof(zbi_partition_map_t)) {
        zxlogf(ERROR, "nandpart: Partition map is of size %zu, needs to at least be %zu\n", actual,
               sizeof(zbi_partition_t));
        return ZX_ERR_INTERNAL;
    }

    zbi_partition_map_t* pmap = (zbi_partition_map_t*)buffer;

    const size_t minimum_size =
        sizeof(zbi_partition_map_t) + (sizeof(zbi_partition_t) * (pmap->partition_count));
    if (actual < minimum_size) {
        zxlogf(ERROR, "nandpart: Partition map is of size %zu, needs to at least be %zu\n", actual,
               minimum_size);
        return ZX_ERR_INTERNAL;
    }

    // Sanity check partition map and transform into expected form.
    status = SanitizePartitionMap(pmap, nand_info);
    if (status != ZX_OK) {
        return status;
    }

    // Create a device for each partition.
    for (unsigned i = 0; i < pmap->partition_count; i++) {
        const auto* part = &pmap->partitions[i];
        char name[10];

        snprintf(name, sizeof(name), "part-%03u", i);

        nand_info.num_blocks = static_cast<uint32_t>(part->last_block - part->first_block + 1);
        memcpy(&nand_info.partition_guid, &part->type_guid, sizeof(nand_info.partition_guid));
        // We only use FTL for the FVM partition.
        if (memcmp(part->type_guid, fvm_guid, sizeof(fvm_guid)) == 0) {
            nand_info.nand_class = NAND_CLASS_FTL;
        } else {
            nand_info.nand_class = NAND_CLASS_BBS;
        }

        fbl::AllocChecker ac;
        fbl::unique_ptr<NandPartDevice> device(new (&ac) NandPartDevice(
            parent, nand_proto, bad_block, parent_op_size, nand_info,
            static_cast<uint32_t>(part->first_block)));
        if (!ac.check()) {
            continue;
        }
        status = device->Bind(name);
        if (status != ZX_OK) {
            zxlogf(ERROR, "Failed to bind %s with error %d\n", name, status);
            continue;
        }
        // devmgr is now in charge of the device.
        __UNUSED auto* dummy = device.release();
    }

    return ZX_OK;
}

zx_status_t NandPartDevice::Bind(const char* name) {
    zxlogf(INFO, "nandpart: Binding %s to %s\n", name, device_get_name(parent()));

    zx_device_prop_t props[] = {
        {BIND_PROTOCOL, 0, ZX_PROTOCOL_NAND},
        {BIND_NAND_CLASS, 0, nand_info_.nand_class},
    };

    zx_status_t status = DdkAdd(name, DEVICE_ADD_INVISIBLE, props, countof(props));
    if (status != ZX_OK) {
        return status;
    }

    // Add empty partition map metadata to prevent this driver from binding to its child devices
    status = DdkAddMetadata(DEVICE_METADATA_PARTITION_MAP, NULL, 0);
    if (status != ZX_OK) {
        DdkRemove();
        return status;
    }

    DdkMakeVisible();
    return ZX_OK;
}

void NandPartDevice::Query(nand_info_t* info_out, size_t* nand_op_size_out) {
    memcpy(info_out, &nand_info_, sizeof(*info_out));
    // Add size of translated_op.
    *nand_op_size_out = parent_op_size_ + sizeof(nand_op_t);
}

void NandPartDevice::Queue(nand_op_t* op) {
    auto* translated_op =
        reinterpret_cast<nand_op_t*>(reinterpret_cast<uint8_t*>(op) + parent_op_size_);
    uint32_t command = op->command;

    // Copy client's op to translated op
    memcpy(translated_op, op, sizeof(*translated_op));

    // Make offset relative to full underlying device
    // TODO(surajamlhotra): Also support old commands.
    if (command == NAND_OP_READ || command == NAND_OP_WRITE) {
        translated_op->rw.offset_nand += (erase_block_start_ * nand_info_.pages_per_block);
    } else if (command == NAND_OP_ERASE) {
        translated_op->erase.first_block += erase_block_start_;
    } else {
        op->completion_cb(op, ZX_ERR_NOT_SUPPORTED);
    }

    translated_op->completion_cb = CompletionCallback;
    translated_op->cookie = op;

    // Call parent's queue
    nand_.Queue(translated_op);
}

void NandPartDevice::GetBadBlockList(uint32_t* bad_blocks, uint32_t bad_block_len,
                                     uint32_t* num_bad_blocks) {
    // TODO implement this
    *num_bad_blocks = 0;
}

zx_status_t NandPartDevice::GetBadBlockList2(uint32_t* bad_block_list, uint32_t bad_block_list_len,
                                             uint32_t* bad_block_count) {

    if (!bad_block_list_) {
        const zx_status_t status = bad_block_->GetBadBlockList(
            &bad_block_list_, erase_block_start_, erase_block_start_ + nand_info_.num_blocks);
        if (status != ZX_OK) {
            return status;
        }
    }

    *bad_block_count = static_cast<uint32_t>(bad_block_list_.size());
    zxlogf(TRACE, "nandpart: %s: Bad block count: %u\n", name(), *bad_block_count);

    if (bad_block_list_len == 0 || bad_block_list_.size() == 0) {
        return ZX_OK;
    }
    if (bad_block_list == NULL) {
        return ZX_ERR_INVALID_ARGS;
    }

    if (bad_block_list_.size() > 0) {
        const size_t size = sizeof(uint32_t) * bad_block_list_.size();
        memcpy(bad_block_list, bad_block_list_.get(), size);
    }
    return ZX_OK;
}

zx_status_t NandPartDevice::IsBlockBad(uint32_t block, bool* is_bad) {
    if (block >= nand_info_.num_blocks) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    if (!bad_block_list_) {
        const zx_status_t status = bad_block_->GetBadBlockList(
            &bad_block_list_, erase_block_start_, erase_block_start_ + nand_info_.num_blocks);
        if (status != ZX_OK) {
            return status;
        }
    }

    *is_bad = false;
    // bad_block_list should be relatively small and we don't keep the
    // bad_block_list sorted, so we simply iterate through entire list.
    for (const auto& bad_block : bad_block_list_) {
        if (bad_block == block) {
            *is_bad = true;
            break;
        }
    }
    return ZX_OK;
}

zx_status_t NandPartDevice::MarkBlockBad(uint32_t block) {
    if (block >= nand_info_.num_blocks) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    if (!bad_block_list_) {
        const zx_status_t status = bad_block_->GetBadBlockList(
            &bad_block_list_, erase_block_start_, erase_block_start_ + nand_info_.num_blocks);
        if (status != ZX_OK) {
            return status;
        }
    }

    // First, update our cached copy of the bad block list.
    fbl::AllocChecker ac;
    const size_t new_size = bad_block_list_.size() + 1;
    fbl::Array<uint32_t> new_bad_block_list(new (&ac) uint32_t[new_size], new_size);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    memcpy(new_bad_block_list.get(), bad_block_list_.get(),
           bad_block_list_.size() * sizeof(uint32_t));
    new_bad_block_list[bad_block_list_.size()] = block;
    bad_block_list_ = fbl::move(new_bad_block_list);

    // Second, "write-through" to actually persist.
    block += erase_block_start_;
    return bad_block_->MarkBlockBad(block);
}

zx_status_t NandPartDevice::DdkGetProtocol(uint32_t proto_id, void* protocol) {
    auto* proto = static_cast<ddk::AnyProtocol*>(protocol);
    proto->ctx = this;
    switch (proto_id) {
    case ZX_PROTOCOL_NAND:
        proto->ops = &nand_proto_ops_;
        break;
    case ZX_PROTOCOL_BAD_BLOCK:
        proto->ops = &bad_block_proto_ops_;
        break;
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
    return ZX_OK;
}

} // namespace nand

extern "C" zx_status_t nandpart_bind(void* ctx, zx_device_t* parent) {
    return nand::NandPartDevice::Create(parent);
}
