/*
 * Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <tee/entry_fast.h>
#include <optee_msg.h>
#include <sm/optee_smc.h>
#include <kernel/generic_boot.h>
#include <kernel/tee_l2cc_mutex.h>
#include <kernel/misc.h>
#include <mm/core_mmu.h>
#include "mbed_lib/mbedtls/aes.h"
#include "mbed_lib/mbedtls/entropy.h"
#include "mbed_lib/mbedtls/bignum.h"
#include <tee_internal_api.h>
#include <string.h> /* for memcpy() */
#include <types_ext.h>
#include <mm/core_memprot.h>


static void tee_entry_get_shm_config(struct thread_smc_args *args)
{
	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = default_nsec_shm_paddr;
	args->a2 = default_nsec_shm_size;
	/* Should this be TEESMC cache attributes instead? */
	args->a3 = core_mmu_is_shm_cached();
}

static void tee_entry_fastcall_l2cc_mutex(struct thread_smc_args *args)
{
	TEE_Result ret;
#ifdef ARM32
	paddr_t pa = 0;

	switch (args->a1) {
	case OPTEE_SMC_L2CC_MUTEX_GET_ADDR:
		ret = tee_get_l2cc_mutex(&pa);
		reg_pair_from_64(pa, &args->a2, &args->a3);
		break;
	case OPTEE_SMC_L2CC_MUTEX_SET_ADDR:
		pa = reg_pair_to_64(args->a2, args->a3);
		ret = tee_set_l2cc_mutex(&pa);
		break;
	case OPTEE_SMC_L2CC_MUTEX_ENABLE:
		ret = tee_enable_l2cc_mutex();
		break;
	case OPTEE_SMC_L2CC_MUTEX_DISABLE:
		ret = tee_disable_l2cc_mutex();
		break;
	default:
		args->a0 = OPTEE_SMC_RETURN_EBADCMD;
		return;
	}
#else
	ret = TEE_ERROR_NOT_SUPPORTED;
#endif
	if (ret == TEE_ERROR_NOT_SUPPORTED)
		args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
	else if (ret)
		args->a0 = OPTEE_SMC_RETURN_EBADADDR;
	else
		args->a0 = OPTEE_SMC_RETURN_OK;
}

static void tee_entry_exchange_capabilities(struct thread_smc_args *args)
{
	bool dyn_shm_en = false;

	/*
	 * Currently we ignore OPTEE_SMC_NSEC_CAP_UNIPROCESSOR.
	 *
	 * The memory mapping of shared memory is defined as normal
	 * shared memory for SMP systems and normal memory for UP
	 * systems. Currently we map all memory as shared in secure
	 * world.
	 *
	 * When translation tables are created with shared bit cleared for
	 * uniprocessor systems we'll need to check
	 * OPTEE_SMC_NSEC_CAP_UNIPROCESSOR.
	 */

	if (args->a1 & ~OPTEE_SMC_NSEC_CAP_UNIPROCESSOR) {
		/* Unknown capability. */
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
		return;
	}

	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM;

#if defined(CFG_DYN_SHM_CAP)
	dyn_shm_en = core_mmu_nsec_ddr_is_defined();
	if (dyn_shm_en)
		args->a1 |= OPTEE_SMC_SEC_CAP_DYNAMIC_SHM;
#endif

	IMSG("Dynamic shared memory is %sabled", dyn_shm_en ? "en" : "dis");
}

static void tee_entry_disable_shm_cache(struct thread_smc_args *args)
{
	uint64_t cookie;

	if (!thread_disable_prealloc_rpc_cache(&cookie)) {
		args->a0 = OPTEE_SMC_RETURN_EBUSY;
		return;
	}

	if (!cookie) {
		args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
		return;
	}

	args->a0 = OPTEE_SMC_RETURN_OK;
	args->a1 = cookie >> 32;
	args->a2 = cookie;
}

static void tee_entry_enable_shm_cache(struct thread_smc_args *args)
{
	if (thread_enable_prealloc_rpc_cache())
		args->a0 = OPTEE_SMC_RETURN_OK;
	else
		args->a0 = OPTEE_SMC_RETURN_EBUSY;
}

static void tee_entry_boot_secondary(struct thread_smc_args *args)
{
#if defined(CFG_BOOT_SECONDARY_REQUEST)
	if (!generic_boot_core_release(args->a1, (paddr_t)(args->a3)))
		args->a0 = OPTEE_SMC_RETURN_OK;
	else
		args->a0 = OPTEE_SMC_RETURN_EBADCMD;
#else
	args->a0 = OPTEE_SMC_RETURN_ENOTAVAIL;
#endif
}

// Hardcoded example 256-bit AES key for SchrodinText
static const unsigned char aes_key[] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
};

static void tee_entry_schrodtext_decrypt(struct thread_smc_args *args)
{
	mbedtls_aes_context ctx;
	int status = 0;
	unsigned char etext[16];
	unsigned char dtext[16];
	unsigned char *shared_buf = NULL;
	
	// Set up shared memory
	if (!core_mmu_add_mapping(MEM_AREA_NSEC_SHM, 0xfee00000, 0x1000)) {
		EMSG("Error: could not map shared memory\n");
		goto error;
	}
	
	shared_buf = (unsigned char*) phys_to_virt(0xfee00000, MEM_AREA_NSEC_SHM);
	if (!shared_buf) {
		EMSG("Error: shared_buf failed to get virtual address\n");
		goto error;
	}
	
	memcpy(etext, shared_buf, 16);

	// Decrypt
	mbedtls_aes_init(&ctx);
	status = mbedtls_aes_setkey_dec(&ctx, aes_key, 256);
	if (status) goto error;
	
	status = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, etext, dtext);
	if (status) goto error;
	
	memcpy(shared_buf, dtext, 16);

	mbedtls_aes_free(&ctx);
	args->a0 = OPTEE_SMC_RETURN_OK;
	return;

error:
	EMSG("%s Something went wrong: %d\n", __func__, status);
	mbedtls_aes_free(&ctx);
	args->a0 = OPTEE_SMC_RETURN_EBADCMD;
}

void tee_entry_fast(struct thread_smc_args *args)
{
	switch (args->a0) {

	/* Generic functions */
	case OPTEE_SMC_CALLS_COUNT:
		tee_entry_get_api_call_count(args);
		break;
	case OPTEE_SMC_CALLS_UID:
		tee_entry_get_api_uuid(args);
		break;
	case OPTEE_SMC_CALLS_REVISION:
		tee_entry_get_api_revision(args);
		break;
	case OPTEE_SMC_CALL_GET_OS_UUID:
		tee_entry_get_os_uuid(args);
		break;
	case OPTEE_SMC_CALL_GET_OS_REVISION:
		tee_entry_get_os_revision(args);
		break;

	/* OP-TEE specific SMC functions */
	case OPTEE_SMC_GET_SHM_CONFIG:
		tee_entry_get_shm_config(args);
		break;
	case OPTEE_SMC_L2CC_MUTEX:
		tee_entry_fastcall_l2cc_mutex(args);
		break;
	case OPTEE_SMC_EXCHANGE_CAPABILITIES:
		tee_entry_exchange_capabilities(args);
		break;
	case OPTEE_SMC_DISABLE_SHM_CACHE:
		tee_entry_disable_shm_cache(args);
		break;
	case OPTEE_SMC_ENABLE_SHM_CACHE:
		tee_entry_enable_shm_cache(args);
		break;
	case OPTEE_SMC_BOOT_SECONDARY:
		tee_entry_boot_secondary(args);
		break;
		
	/* SchrodinText SMC */
	case OPTEE_SMC_SCHRODTEXT_DECRYPT:
		tee_entry_schrodtext_decrypt(args);
		break;

	default:
		args->a0 = OPTEE_SMC_RETURN_UNKNOWN_FUNCTION;
		break;
	}
}

size_t tee_entry_generic_get_api_call_count(void)
{
	/*
	 * All the different calls handled in this file. If the specific
	 * target has additional calls it will call this function and
	 * add the number of calls the target has added.
	 */
	return 9;
}

void __weak tee_entry_get_api_call_count(struct thread_smc_args *args)
{
	args->a0 = tee_entry_generic_get_api_call_count();
}

void __weak tee_entry_get_api_uuid(struct thread_smc_args *args)
{
	args->a0 = OPTEE_MSG_UID_0;
	args->a1 = OPTEE_MSG_UID_1;
	args->a2 = OPTEE_MSG_UID_2;
	args->a3 = OPTEE_MSG_UID_3;
}

void __weak tee_entry_get_api_revision(struct thread_smc_args *args)
{
	args->a0 = OPTEE_MSG_REVISION_MAJOR;
	args->a1 = OPTEE_MSG_REVISION_MINOR;
}

void __weak tee_entry_get_os_uuid(struct thread_smc_args *args)
{
	args->a0 = OPTEE_MSG_OS_OPTEE_UUID_0;
	args->a1 = OPTEE_MSG_OS_OPTEE_UUID_1;
	args->a2 = OPTEE_MSG_OS_OPTEE_UUID_2;
	args->a3 = OPTEE_MSG_OS_OPTEE_UUID_3;
}

void __weak tee_entry_get_os_revision(struct thread_smc_args *args)
{
	args->a0 = CFG_OPTEE_REVISION_MAJOR;
	args->a1 = CFG_OPTEE_REVISION_MINOR;
}
