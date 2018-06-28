// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "aml-bad-block.h"

#include <stdlib.h>

#include <ddk/debug.h>
#include <ddk/protocol/nand.h>

#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <pretty/hexdump.h>
#include <sync/completion.h>

#define OLD_NAND_PROTO 1

namespace nand {

namespace {

constexpr uint32_t kBadBlockTableMagic = 0x7462626E; // "nbbt"

struct BlockOperationContext {
    completion_t* completion_event;
    zx_status_t status;
};

void CompletionCallback(nand_op_t* op, zx_status_t status) {
    auto* ctx = static_cast<BlockOperationContext*>(op->cookie);

    zxlogf(TRACE, "Completion status: %d\n", status);
    ctx->status = status;
    completion_signal(ctx->completion_event);
    return;
}

} // namespace

zx_status_t AmlBadBlock::Create(Config config, fbl::RefPtr<BadBlock>* out) {
    // Query parent to get its nand_info_t and size for nand_op_t.
    nand_info_t nand_info;
    size_t parent_op_size;
    config.nand_proto.ops->query(config.nand_proto.ctx, &nand_info, &parent_op_size);

    // Allocate nand_op
    fbl::AllocChecker ac;
    fbl::Array<uint8_t> nand_op(new (&ac) uint8_t[parent_op_size], parent_op_size);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    // Allocate VMOs.
    const uint32_t table_len = nand_info.num_blocks;
    zx::vmo data_vmo;
    zx_status_t status = zx::vmo::create(table_len, 0, &data_vmo);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: Failed to create VMO for bad block table\n");
        return status;
    }

    zx::vmo oob_vmo;
    status = zx::vmo::create(sizeof(OobMetadata), 0, &oob_vmo);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: Failed to create VMO for oob metadata\n");
        return status;
    }

    // Map them.
    constexpr uint32_t kPermissions = ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE;
    uintptr_t vaddr_table;
    status = zx::vmar::root_self().map(0, data_vmo, 0, table_len, kPermissions, &vaddr_table);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: Failed to map VMO for bad block table\n");
        return status;
    }

    uintptr_t vaddr_oob;
    status = zx::vmar::root_self().map(0, oob_vmo, 0, sizeof(OobMetadata), kPermissions,
                                       &vaddr_oob);
    if (status != ZX_OK) {
        zxlogf(ERROR, "nandpart: Failed to map VMO for oob metadata\n");
        return status;
    }

    // Construct all the things.
    *out = fbl::MakeRefCountedChecked<AmlBadBlock>(&ac, fbl::move(data_vmo), fbl::move(oob_vmo),
                                                   fbl::move(nand_op), config, nand_info,
                                                   reinterpret_cast<BlockStatus*>(vaddr_table),
                                                   table_len,
                                                   reinterpret_cast<OobMetadata*>(vaddr_oob));
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }
    return ZX_OK;
}

zx_status_t AmlBadBlock::EraseBlock(uint32_t block) {
    completion_t completion;
    BlockOperationContext op_ctx = {.completion_event = &completion,
                                    .status = ZX_ERR_INTERNAL};
    auto* nand_op = reinterpret_cast<nand_op_t*>(nand_op_.get());
    nand_op->erase.command = NAND_OP_ERASE;
    nand_op->erase.first_block = block;
    nand_op->erase.num_blocks = 1;
    nand_op->completion_cb = CompletionCallback;
    nand_op->cookie = &op_ctx;
    nand_.Queue(nand_op);

    // Wait on completion.
    completion_wait(&completion, ZX_TIME_INFINITE);
    return op_ctx.status;
}

zx_status_t AmlBadBlock::GetNewBlock() {
    for (;;) {
        // Find a block with the least number of PE cycles.
        uint16_t least_pe_cycles = UINT16_MAX;
        uint32_t index = kBlockListMax;
        for (uint32_t i = 0; i < kBlockListMax; i++) {
            if (block_list_[i].valid &&
                &block_list_[i] != block_entry_ &&
                block_list_[i].program_erase_cycles < least_pe_cycles) {
                least_pe_cycles = block_list_[i].program_erase_cycles;
                index = i;
            }
        }
        if (index == kBlockListMax) {
            zxlogf(ERROR, "nandpart: Unable to find a valid block to store BBT into\n");
            return ZX_ERR_NOT_FOUND;
        }

        // Make sure we aren't trying to write to a bad block.
        const uint32_t block = block_list_[index].block;
        if (table_[block] != kNandBlockGood) {
            // Try again.
            block_list_[index].valid = false;
            continue;
        }

        // Erase the block before using it.
        const zx_status_t status = EraseBlock(block);
        if (status != ZX_OK) {
            zxlogf(ERROR, "nandpart: Failed to erase block %u, marking bad\n", block);
            // Mark the block as bad and try again.
            table_[block] = kNandBlockBad;
            block_list_[index].valid = false;
            continue;
        }

        zxlogf(INFO, "nandpart: Moving BBT to block %u\n", block);
        block_entry_ = &block_list_[index];
        block_list_[index].program_erase_cycles++;
        page_ = 0;
        return ZX_OK;
    }
}

zx_status_t AmlBadBlock::WritePage(uint32_t nand_page, uint64_t data_offset) {
    completion_t completion;
    BlockOperationContext op_ctx = {.completion_event = &completion,
                                    .status = ZX_ERR_INTERNAL};

    auto* nand_op = reinterpret_cast<nand_op_t*>(nand_op_.get());
#if OLD_NAND_PROTO
    nand_op->rw_data_oob.command = NAND_OP_WRITE_PAGE_DATA_OOB;
    nand_op->rw_data_oob.page_num = nand_page;
    nand_op->rw_data_oob.data.vmo = data_vmo_.get();
    nand_op->rw_data_oob.data.length = 1;
    nand_op->rw_data_oob.data.offset_vmo = data_offset;
    nand_op->rw_data_oob.oob.vmo = oob_vmo_.get();
    nand_op->rw_data_oob.oob.length = sizeof(*oob_);
    nand_op->rw_data_oob.oob.offset_vmo = 0;
#else
    nand_op->rw.command = NAND_OP_WRITE_PAGE_DATA;
    nand_op->rw.data_vmo = data_vmo_.get();
    nand_op->rw.oob_vmo = oob_vmo_.get();
    nand_op->rw.length = 1;
    nand_op->rw.offset_nand = nand_page;
    nand_op->rw.offset_data_vmo = data_offset;
    nand_op->rw.offset_oob_vmo = 0;
#endif
    nand_op->completion_cb = CompletionCallback;
    nand_op->cookie = &op_ctx;
    nand_.Queue(nand_op);

    // Wait on completion.
    completion_wait(&completion, ZX_TIME_INFINITE);
    return op_ctx.status;
}

zx_status_t AmlBadBlock::WriteBadBlockTable(bool use_new_block) {
    const uint32_t bbt_page_count =
        (table_len_ + nand_info_.page_size - 1) / nand_info_.page_size;

    bool successful;
    do {
        if (use_new_block ||
            table_[block_entry_->block] != kNandBlockGood ||
            page_ + bbt_page_count > nand_info_.pages_per_block) {
            // Current BBT is in a bad block, or it is full, so we must find a new one.
            use_new_block = false;
            zxlogf(INFO, "nandpart: Finding a new block to store BBT into\n");
            zx_status_t status = GetNewBlock();
            if (status != ZX_OK) {
                return status;
            }
        }

        // Perform write
        oob_->magic = kBadBlockTableMagic;
        oob_->program_erase_cycles = block_entry_->program_erase_cycles;
        oob_->generation = generation_;

        const uint32_t block = block_entry_->block;

        successful = true;
        for (uint32_t i = 0; i < bbt_page_count; i++) {
            const uint32_t nand_page = block * nand_info_.pages_per_block + page_ + i;

            const zx_status_t status = WritePage(nand_page, i);
            if (status != ZX_OK) {
                zxlogf(ERROR, "nandpart: BBT write failed. Marking %u bad and trying again\n",
                       block);
                table_[block] = kNandBlockBad;
                successful = false;
                break;
            }
            zxlogf(TRACE, "nandpart: BBT write to block %u page %u successful\n", block,
                   page_ + i);
        }
    } while (!successful);

    page_ += bbt_page_count;
    generation_++;
    return ZX_OK;
}

zx_status_t AmlBadBlock::ReadPage(uint32_t nand_page, uint64_t data_offset) {
    completion_t completion;
    BlockOperationContext op_ctx = {.completion_event = &completion,
                                    .status = ZX_ERR_INTERNAL};
    auto* nand_op = reinterpret_cast<nand_op_t*>(nand_op_.get());
#if OLD_NAND_PROTO
    nand_op->rw_data_oob.command = NAND_OP_READ_PAGE_DATA_OOB;
    nand_op->rw_data_oob.page_num = nand_page;
    nand_op->rw_data_oob.data.vmo = data_vmo_.get();
    nand_op->rw_data_oob.data.length = 1;
    nand_op->rw_data_oob.data.offset_vmo = data_offset;
    nand_op->rw_data_oob.oob.vmo = oob_vmo_.get();
    nand_op->rw_data_oob.oob.length = sizeof(*oob_);
    nand_op->rw_data_oob.oob.offset_vmo = 0;
#else
    nand_op->rw.command = NAND_OP_READ_PAGE_DATA;
    nand_op->rw.data_vmo = data_vmo_.get();
    nand_op->rw.oob_vmo = oob_vmo_.get();
    nand_op->rw.length = 1;
    nand_op->rw.offset_nand = nand_page;
    nand_op->rw.offset_data_vmo = data_offset;
    nand_op->rw.offset_oob_vmo = 0;
#endif
    nand_op->completion_cb = CompletionCallback;
    nand_op->cookie = &op_ctx;
    nand_.Queue(nand_op);

    // Wait on completion.
    completion_wait(&completion, ZX_TIME_INFINITE);
    return op_ctx.status;
}

zx_status_t AmlBadBlock::FindBadBlockTable() {
    zxlogf(TRACE, "nandpart: Finding bad block table\n");

    if (sizeof(OobMetadata) > nand_info_.oob_size) {
        zxlogf(ERROR, "nandpart: OOB is too small. Need %zu, found %u\n", sizeof(OobMetadata),
               nand_info_.oob_size);
        return ZX_ERR_NOT_SUPPORTED;
    }

    zxlogf(TRACE, "nandpart: Starting in block %u. Ending in block %u.\n",
           config_.aml.table_start_block, config_.aml.table_end_block);

    const uint32_t blocks = config_.aml.table_end_block - config_.aml.table_start_block;
    if (blocks == 0 || blocks > kBlockListMax) {
        // Driver assumption that no more than 8 blocks will be dedicated for BBT use.
        zxlogf(ERROR, "Unsupported number of blocks used for BBT.\n");
        return ZX_ERR_NOT_SUPPORTED;
    }

    // First find the block the BBT lives in.
    const uint32_t page_stride = (table_len_ + nand_info_.page_size - 1) /
                                 (nand_info_.page_size);
    ZX_DEBUG_ASSERT(page_stride > 0);

    int8_t valid_blocks = 0;
    block_entry_ = NULL;
    uint32_t block = config_.aml.table_start_block;
    for (; block <= config_.aml.table_end_block; block++) {
        //  Attempt to read up to 6 entries to see if block is valid.
        uint32_t nand_page = block * nand_info_.pages_per_block;
        zx_status_t status = ZX_ERR_INTERNAL;
        for (uint32_t i = 0; i < 6 && status != ZX_OK; i++, nand_page += page_stride) {
            status = ReadPage(nand_page, 0);
        }
        if (status != ZX_OK) {
            // This block is untrustworthy. Do not add it to the block list.
            // TODO(surajmalhotra): Should we somehow mark this block as bad or
            // try erasing it?
            zxlogf(ERROR, "nandpart: Unable to read any pages in block %u\n", block);
            continue;
        }

        zxlogf(TRACE, "Successfully read block %u.\n", block);

        block_list_[valid_blocks].block = block;
        block_list_[valid_blocks].program_erase_cycles = oob_->program_erase_cycles;
        block_list_[valid_blocks].valid = true;

        // If block has valid BBT entries, see if it has the latest entries.
        if (oob_->magic == kBadBlockTableMagic && oob_->generation >= generation_) {
            zxlogf(TRACE, "Block %u has valid BBT entries!\n", block);
            block_entry_ = &block_list_[valid_blocks];
            generation_ = oob_->generation;
        }

        valid_blocks++;
    }

    if (block_entry_ == NULL) {
        zxlogf(ERROR, "nandpart: No valid BBT entries found!\n");
        return ZX_ERR_INTERNAL;
    }

    zxlogf(TRACE, "nandpart: Finding last BBT in block %u\n", block_entry_->block);

    // Next find the last valid BBT entry in block.
    bool found_one = false;
    bool latest_entry_bad = true;
    uint32_t page = 0;
    for (; page + page_stride <= nand_info_.pages_per_block; page += page_stride) {
        zx_status_t status = ZX_OK;
        // Check that all pages in current page_stride are valid.
        zxlogf(TRACE, "Reading page %u\n", page);
        for (uint32_t i = 0; i < page_stride; i++) {
            const uint32_t nand_page = block_entry_->block * nand_info_.pages_per_block + page + i;
            status = ReadPage(nand_page, i);
            if (status != ZX_OK || oob_->magic != kBadBlockTableMagic) {
                break;
            }
        }
        if (status != ZX_OK) {
            // It's fine for entries to be unreadable as long as future ones are
            // readable.
            zxlogf(TRACE, "nandpart: Unable to read page %u\n", page);
            latest_entry_bad = true;
            continue;
        }
        if (oob_->magic != kBadBlockTableMagic) {
            // Last BBT entry in table was found, so quit looking at future
            // entries.
            zxlogf(TRACE, "nandpart: Page %u does not contain valid BBT entry\n", page);
            break;
        }
        // Store latest complete BBT.
        zxlogf(TRACE, "BBT entry in page %u is valid\n", page);
        latest_entry_bad = false;
        found_one = true;
        page_ = page;
        generation_ = static_cast<uint16_t>(oob_->generation + 1);
    }

    if (!found_one) {
        zxlogf(ERROR, "nandpart: Unable to find a valid copy of the bad block table\n");
        return ZX_ERR_NOT_FOUND;
    }

    // Last iteration failed to read valid copy of BBT (that's how loop exited),
    // so we need to reread the BBT.
    for (uint32_t i = 0; i < page_stride; i++) {
        const uint32_t nand_page = block_entry_->block * nand_info_.pages_per_block + page_ + i;
        const zx_status_t status = ReadPage(nand_page, i);
        if (status != ZX_OK || oob_->magic != kBadBlockTableMagic) {
            zxlogf(ERROR, "nandpart: Unable to re-read latest copy of bad block table\n");
            return status;
        }
    }

    if (latest_entry_bad) {
        zxlogf(ERROR, "nandpart: Latest entry in block %u is invalid. Moving bad block file.\n",
               block_entry_->block);
        const zx_status_t status = WriteBadBlockTable(true);
        if (status != ZX_OK) {
            return ZX_ERR_NOT_SUPPORTED;
        }
    } else {
        // Page needs to point to next available slot.
        zxlogf(INFO, "nandpart: Latest BBT entry found in page %u\n", page_);
        page_ += page_stride;
    }

    found_ = true;
    return ZX_OK;
}

zx_status_t AmlBadBlock::GetBadBlockList(fbl::Array<uint32_t>* bad_blocks, uint32_t first_block,
                                         uint32_t last_block) {
    fbl::AutoLock al(&lock_);
    if (!found_) {
        const zx_status_t status = FindBadBlockTable();
        if (status != ZX_OK) {
            return status;
        }
    }

    if (first_block >= table_len_ || last_block > table_len_) {
        return ZX_ERR_INVALID_ARGS;
    }

    // Scan BBT for bad block list.
    size_t bad_block_count = 0;
    for (uint32_t block = first_block; block < last_block; block++) {
        if (table_[block] != kNandBlockGood) {
            bad_block_count += 1;
        }
    }

    // Early return if no bad blocks found.
    if (bad_block_count == 0) {
        bad_blocks->reset();
        return ZX_OK;
    }

    // Allocate array and copy list.
    fbl::AllocChecker ac;
    bad_blocks->reset(new (&ac) uint32_t[bad_block_count], bad_block_count);
    if (!ac.check()) {
        return ZX_ERR_NO_MEMORY;
    }

    bad_block_count = 0;
    for (uint32_t block = first_block; block < last_block; block++) {
        if (table_[block] != kNandBlockGood) {
            (*bad_blocks)[bad_block_count++] = block;
        }
    }

    return ZX_OK;
}

zx_status_t AmlBadBlock::IsBlockBad(uint32_t block, bool* is_bad) {
    fbl::AutoLock al(&lock_);
    if (!found_) {
        const zx_status_t status = FindBadBlockTable();
        if (status != ZX_OK) {
            return status;
        }
    }

    if (block > table_len_) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    *is_bad = table_[block] != kNandBlockGood;

    return ZX_OK;
}

zx_status_t AmlBadBlock::MarkBlockBad(uint32_t block) {
    fbl::AutoLock al(&lock_);
    if (!found_) {
        const zx_status_t status = FindBadBlockTable();
        if (status != ZX_OK) {
            return status;
        }
    }

    if (block > table_len_) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    // Early return if block is already marked bad.
    if (table_[block] != kNandBlockGood) {
        return ZX_OK;
    }
    table_[block] = kNandBlockBad;

    return WriteBadBlockTable(false);
}

} // namespace nand
